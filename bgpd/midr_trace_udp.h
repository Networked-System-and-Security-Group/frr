// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_MIDR_TRACE_UDP_H
#define _FRR_MIDR_TRACE_UDP_H

#include "bgpd/midr_trace_types.h"

/* Internal transport API. Return errno values, never terminate the daemon. */
struct midr_trace_udp_reply {
	struct prefix offender;
	bool visible;
	bool terminal;
	bool reached;
	uint8_t type;
	uint8_t code;
	int local_errno;
};

bool midr_trace_udp_supported(int family);
int midr_trace_udp_open(int family,
			const struct midr_trace_net_context *context, int *fd);
int midr_trace_udp_send(int fd, const struct prefix *target, uint16_t port,
			uint8_t ttl, const void *payload, size_t len);
/* 1 matched reply, 0 unrelated/malformed, -1 syscall error (errno set).
 * recvmsg storage and ancillary lengths are reset on every invocation.
 */
int midr_trace_udp_receive(int fd, const struct prefix *target, uint16_t port,
			   const void *payload, size_t len,
			   struct midr_trace_udp_reply *reply);

#endif
