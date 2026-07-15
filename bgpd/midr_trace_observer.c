// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR traceroute observer.
 *
 * This MVP intentionally performs a synchronous active measurement.  Later
 * work can move execution to a scheduler/cache layer without changing the
 * midr_tier1_observer contract.
 */

#include <zebra.h>

#include <ctype.h>

#include "prefix.h"

#include "bgpd/midr_ip2asn.h"
#include "bgpd/midr_trace_observer.h"

#define MIDR_TRACE_LINE_MAX 1024
#define MIDR_TRACE_CMD_MAX 256
#define MIDR_TRACE_TOKEN_MAX 128

static bool midr_trace_prefix_to_addrstr(const struct prefix *prefix,
					 char *buf, size_t buflen)
{
	const void *addr;

	if (prefix->family == AF_INET)
		addr = &prefix->u.prefix4;
	else if (prefix->family == AF_INET6)
		addr = &prefix->u.prefix6;
	else
		return false;

	return inet_ntop(prefix->family, addr, buf, buflen) != NULL;
}

static void midr_trace_token_clean(char *token)
{
	size_t len;

	while (*token && (unsigned char)*token <= ' ')
		memmove(token, token + 1, strlen(token));

	while (*token == '(' || *token == '[')
		memmove(token, token + 1, strlen(token));

	len = strlen(token);
	while (len && (token[len - 1] == ')' || token[len - 1] == ']'
		       || token[len - 1] == ',' || token[len - 1] == ';')) {
		token[len - 1] = '\0';
		len--;
	}
}

static bool midr_trace_token_to_prefix(const char *token,
				       struct prefix *prefix)
{
	struct in_addr addr4;
	struct in6_addr addr6;

	if (inet_pton(AF_INET, token, &addr4) == 1) {
		memset(prefix, 0, sizeof(*prefix));
		prefix->family = AF_INET;
		prefix->prefixlen = IPV4_MAX_BITLEN;
		prefix->u.prefix4 = addr4;
		return true;
	}

	if (inet_pton(AF_INET6, token, &addr6) == 1) {
		memset(prefix, 0, sizeof(*prefix));
		prefix->family = AF_INET6;
		prefix->prefixlen = IPV6_MAX_BITLEN;
		prefix->u.prefix6 = addr6;
		return true;
	}

	return false;
}

static bool midr_trace_line_is_hop(const char *line)
{
	char *endp;

	while (*line && isspace((unsigned char)*line))
		line++;

	if (!isdigit((unsigned char)*line))
		return false;

	(void)strtoul(line, &endp, 10);
	return endp > line && (!*endp || isspace((unsigned char)*endp));
}

static bool midr_trace_line_first_hop(const char *line, struct prefix *hop)
{
	char copy[MIDR_TRACE_LINE_MAX];
	char *saveptr = NULL;
	char *token;

	strlcpy(copy, line, sizeof(copy));

	for (token = strtok_r(copy, " \t\r\n,", &saveptr); token;
	     token = strtok_r(NULL, " \t\r\n,", &saveptr)) {
		char clean[MIDR_TRACE_TOKEN_MAX];

		strlcpy(clean, token, sizeof(clean));
		midr_trace_token_clean(clean);
		if (!clean[0] || !strcmp(clean, "*"))
			continue;

		if (midr_trace_token_to_prefix(clean, hop))
			return true;
	}

	return false;
}

static bool midr_trace_observation_add_asn(
	struct midr_tier1_observation *observation, as_t asn)
{
	if (observation->observed_asn_count >= MIDR_TIER1_MAX_OBSERVED_ASNS)
		return false;

	observation->observed_asns[observation->observed_asn_count++] = asn;
	return true;
}

static int midr_trace_command_build(const struct prefix *target, char *cmd,
				    size_t cmdlen)
{
	char target_buf[INET6_ADDRSTRLEN];
	const char *program;
	int ret;

	if (!midr_trace_prefix_to_addrstr(target, target_buf,
					  sizeof(target_buf)))
		return -1;

	program = target->family == AF_INET6 ? "traceroute -6" : "traceroute";
	ret = snprintf(cmd, cmdlen, "%s -n %s", program, target_buf);
	if (ret < 0 || (size_t)ret >= cmdlen)
		return -1;

	return 0;
}

int midr_trace_observe_path(const struct prefix *target,
			    struct midr_tier1_observation *observation)
{
	FILE *fp;
	char cmd[MIDR_TRACE_CMD_MAX];
	char line[MIDR_TRACE_LINE_MAX];

	if (!target || !observation)
		return -1;

	if (!midr_ip2asn_is_loaded())
		return -2;

	if (midr_trace_command_build(target, cmd, sizeof(cmd)))
		return -1;

	memset(observation, 0, sizeof(*observation));
	observation->target = *target;
	observation->source = "traceroute-ip2asn";

	fp = popen(cmd, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		struct prefix hop;
		as_t asn = 0;

		if (!midr_trace_line_is_hop(line))
			continue;

		if (!midr_trace_line_first_hop(line, &hop))
			goto add_asn;

		if (!midr_ip2asn_lookup(&hop, &asn, NULL))
			asn = 0;

add_asn:
		if (!midr_trace_observation_add_asn(observation, asn)) {
			pclose(fp);
			return -1;
		}
	}

	if (pclose(fp) == -1)
		return -1;

	return observation->observed_asn_count ? 0 : -1;
}

static int midr_trace_observe_cb(
	const struct prefix *target,
	struct midr_tier1_observation *observation, void *arg)
{
	(void)arg;
	return midr_trace_observe_path(target, observation);
}

static const struct midr_tier1_observer midr_trace_observer = {
	.name = "traceroute-ip2asn",
	.observe = midr_trace_observe_cb,
	.arg = NULL,
};

const struct midr_tier1_observer *midr_trace_observer_get(void)
{
	return &midr_trace_observer;
}
