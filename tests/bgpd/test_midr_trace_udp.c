// SPDX-License-Identifier: GPL-2.0-or-later
/* Inject recvmsg control data; no socket or network is required. */
#include <zebra.h>
#ifdef HAVE_LINUX_ERRQUEUE_H
#include <linux/errqueue.h>
#endif
#include "privs.h"
#include "vrf.h"
#include "bgpd/midr_trace_udp.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

#if defined(HAVE_LINUX_ERRQUEUE_H) && defined(IP_RECVERR) \
	&& defined(IPV6_RECVERR) && defined(MSG_ERRQUEUE)
static int af;
static unsigned int variant;
static struct prefix target;
static const char payload[] = "MIDR probe test";

ssize_t __wrap_recvmsg(int fd, struct msghdr *msg, int flags);
ssize_t __wrap_recvmsg(int fd, struct msghdr *msg, int flags)
{
	struct cmsghdr *cm;
	struct sock_extended_err *err;
	size_t salen = af == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

	(void)fd;
	assert(flags & MSG_ERRQUEUE);
	assert(msg->msg_controllen >= CMSG_SPACE(sizeof(*err) + salen));
	msg->msg_flags = variant == 3 ? MSG_CTRUNC : 0;
	msg->msg_namelen = salen;
	memset(msg->msg_name, 0, salen);
	if (af == AF_INET) {
		struct sockaddr_in *sa = msg->msg_name;

		sa->sin_family = AF_INET;
		sa->sin_addr = target.u.prefix4;
		sa->sin_port = htons(variant == 1 ? 40001 : 40000);
	} else {
		struct sockaddr_in6 *sa = msg->msg_name;

		sa->sin6_family = AF_INET6;
		sa->sin6_addr = target.u.prefix6;
		sa->sin6_port = htons(variant == 1 ? 40001 : 40000);
	}
	cm = CMSG_FIRSTHDR(msg);
	cm->cmsg_level = af == AF_INET ? IPPROTO_IP : IPPROTO_IPV6;
	cm->cmsg_type = af == AF_INET ? IP_RECVERR : IPV6_RECVERR;
	cm->cmsg_len = CMSG_LEN(sizeof(*err) + salen);
	msg->msg_controllen = CMSG_SPACE(sizeof(*err) + salen);
	err = (void *)CMSG_DATA(cm);
	memset(err, 0, sizeof(*err) + salen);
	err->ee_origin = af == AF_INET ? SO_EE_ORIGIN_ICMP : SO_EE_ORIGIN_ICMP6;
	err->ee_type = af == AF_INET ? 11 : 3;
	memcpy(err + 1, msg->msg_name, salen);
	if (variant == 4) {
		err->ee_type = af == AF_INET ? 3 : 1;
		err->ee_code = af == AF_INET ? 3 : 4;
	}
	if (variant == 5)
		cm->cmsg_len = CMSG_LEN(sizeof(*err) - 1);
	if (variant == 6) {
		err->ee_origin = SO_EE_ORIGIN_LOCAL;
		err->ee_errno = ENETUNREACH;
	}
	if (variant == 7)
		return 0; /* Minimum quote, no UDP payload. */
	memcpy(msg->msg_iov[0].iov_base, payload, sizeof(payload));
	if (variant == 2)
		((char *)msg->msg_iov[0].iov_base)[0] = 'X';
	return sizeof(payload);
}
#endif

int main(void)
{
	struct midr_trace_net_context context = {.vrf_id = 1};
	int fd = 100;
	assert(midr_trace_udp_open(AF_INET, &context, &fd) == EOPNOTSUPP && fd == -1);
	context.vrf_id = VRF_DEFAULT;
	context.source.family = AF_INET6;
	assert(midr_trace_udp_open(AF_INET, &context, &fd) == EINVAL && fd == -1);
#if defined(HAVE_LINUX_ERRQUEUE_H) && defined(IP_RECVERR) \
	&& defined(IPV6_RECVERR) && defined(MSG_ERRQUEUE)
	struct midr_trace_udp_reply reply;
	int rc;
	unsigned int family;

	for (family = 0; family < 2; family++) {
		af = family ? AF_INET6 : AF_INET;
		assert(str2prefix(family ? "2001:db8::10" : "192.0.2.10", &target));
		for (variant = 0; variant < 8; variant++) {
			rc = midr_trace_udp_receive(1, &target, 40000,
				payload, sizeof(payload), &reply);
			if (variant == 1 || variant == 2 || variant == 3 || variant == 5) {
				assert(rc == 0);
				continue;
			}
			assert(rc == 1);
			if (variant == 6)
				assert(reply.local_errno == ENETUNREACH && !reply.visible);
			else {
				assert(reply.visible);
				assert(reply.reached == (variant == 4));
			}
		}
	}
	puts("MIDR UDP ancillary tests passed");
#else
	puts("MIDR UDP ancillary tests skipped: Linux error queue unavailable");
#endif
	return 0;
}
