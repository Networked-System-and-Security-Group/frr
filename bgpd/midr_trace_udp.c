// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux UDP error-queue transport; no raw sockets or external executable. */
#include <zebra.h>
#include <fcntl.h>
#ifdef HAVE_LINUX_ERRQUEUE_H
#include <linux/errqueue.h>
#endif
#include "network.h"
#include "sockopt.h"
#include "vrf.h"
#include "bgpd/midr_trace_udp.h"

#if defined(HAVE_LINUX_ERRQUEUE_H) && defined(MSG_ERRQUEUE) \
	&& defined(IP_RECVERR) && defined(IPV6_RECVERR)
#define MIDR_HAVE_ERRQUEUE 1
#endif

bool midr_trace_udp_supported(int family)
{
#ifdef MIDR_HAVE_ERRQUEUE
	return family == AF_INET || family == AF_INET6;
#else
	(void)family;
	return false;
#endif
}

static socklen_t midr_trace_sockaddr(const struct prefix *target,
				   uint16_t port, struct sockaddr_storage *ss)
{
	memset(ss, 0, sizeof(*ss));
	if (target->family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *)ss;

		sa->sin_family = AF_INET;
		sa->sin_addr = target->u.prefix4;
		sa->sin_port = htons(port);
		return sizeof(*sa);
	}
	if (target->family == AF_INET6) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *)ss;

		sa->sin6_family = AF_INET6;
		sa->sin6_addr = target->u.prefix6;
		sa->sin6_port = htons(port);
		return sizeof(*sa);
	}
	return 0;
}

int midr_trace_udp_open(int family,
			const struct midr_trace_net_context *context, int *fd)
{
	struct prefix local = {.family = family};
	struct sockaddr_storage ss;
	socklen_t len;
	int sock, rc, flags;
#ifdef MIDR_HAVE_ERRQUEUE
	int on = 1;
#endif

	if (!fd)
		return EINVAL;
	*fd = -1;
	if (context && context->vrf_id != VRF_DEFAULT)
		return EOPNOTSUPP;
	if (context && context->source.family) {
		if (context->source.family != family)
			return EINVAL;
		local = context->source;
	}
	if (!midr_trace_udp_supported(family))
		return EAFNOSUPPORT;
	sock = vrf_socket(family, SOCK_DGRAM, IPPROTO_UDP, VRF_DEFAULT, NULL);
	if (sock < 0)
		return errno;
	flags = fcntl(sock, F_GETFD);
	if (flags < 0 || fcntl(sock, F_SETFD, flags | FD_CLOEXEC) < 0
	    || set_nonblocking(sock) < 0)
		goto fail;
#ifdef MIDR_HAVE_ERRQUEUE
	if (family == AF_INET6
	    && setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
		goto fail;
	if (setsockopt(sock, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6,
		       family == AF_INET ? IP_RECVERR : IPV6_RECVERR,
		       &on, sizeof(on)) < 0)
		goto fail;
#endif
	/* Unique ephemeral source port; never share PM or BGP sockets. */
	len = midr_trace_sockaddr(&local, 0, &ss);
	if (bind(sock, (struct sockaddr *)&ss, len) < 0)
		goto fail;
	*fd = sock;
	return 0;
fail:
	rc = errno;
	close(sock);
	return rc;
}

int midr_trace_udp_send(int fd, const struct prefix *target, uint16_t port,
			uint8_t ttl, const void *payload, size_t len)
{
	struct sockaddr_storage ss;
	socklen_t salen = midr_trace_sockaddr(target, port, &ss);
	ssize_t n;

	if (!salen || !ttl)
		return EINVAL;
	if (sockopt_ttl(target->family, fd, ttl) < 0)
		return errno;
	n = sendto(fd, payload, len, MSG_DONTWAIT, (struct sockaddr *)&ss, salen);
	if (n < 0)
		return errno;
	return (size_t)n == len ? 0 : EIO;
}

int midr_trace_udp_receive(int fd, const struct prefix *target, uint16_t port,
			   const void *payload, size_t len,
			   struct midr_trace_udp_reply *reply)
{
#ifdef MIDR_HAVE_ERRQUEUE
	struct sockaddr_storage destination = {};
	union {
		struct cmsghdr alignment;
		unsigned char data[512];
	} control;
	unsigned char data[128];
	struct iovec iov = {.iov_base = data, .iov_len = sizeof(data)};
	struct msghdr msg = {
		.msg_name = &destination,
		.msg_namelen = sizeof(destination),
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control.data,
		.msg_controllen = sizeof(control.data),
	};
	struct cmsghdr *cmsg;
	ssize_t n = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);

	if (n < 0)
		return -1;
	memset(reply, 0, sizeof(*reply));
	if (msg.msg_flags & MSG_CTRUNC)
		return 0;
	if (target->family == AF_INET) {
		const struct sockaddr_in *sa = (const void *)&destination;

		if (msg.msg_namelen < sizeof(*sa) || sa->sin_family != AF_INET
		    || sa->sin_port != htons(port)
		    || sa->sin_addr.s_addr != target->u.prefix4.s_addr)
			return 0;
	} else {
		const struct sockaddr_in6 *sa = (const void *)&destination;

		if (msg.msg_namelen < sizeof(*sa) || sa->sin6_family != AF_INET6
		    || sa->sin6_port != htons(port)
		    || memcmp(&sa->sin6_addr, &target->u.prefix6, sizeof(sa->sin6_addr)))
			return 0;
	}
	/* A short ICMP quote need not contain payload. Check every available byte. */
	if (n > 0 && memcmp(data, payload, MIN((size_t)n, len)))
		return 0;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		struct sock_extended_err err;
		const unsigned char *offender;
		size_t available;
		bool ipv4 = target->family == AF_INET;

		if (cmsg->cmsg_len < CMSG_LEN(0)
		    || cmsg->cmsg_len > (size_t)((unsigned char *)msg.msg_control
			+ msg.msg_controllen - (unsigned char *)cmsg))
			return 0;
		if (cmsg->cmsg_level != (ipv4 ? IPPROTO_IP : IPPROTO_IPV6)
		    || cmsg->cmsg_type != (ipv4 ? IP_RECVERR : IPV6_RECVERR))
			continue;
		if (cmsg->cmsg_len < CMSG_LEN(sizeof(err)))
			return 0;
		memcpy(&err, CMSG_DATA(cmsg), sizeof(err));
		if (err.ee_origin == SO_EE_ORIGIN_LOCAL) {
			reply->local_errno = err.ee_errno ? (int)err.ee_errno : EIO;
			return 1;
		}
		if (err.ee_origin != (ipv4 ? SO_EE_ORIGIN_ICMP : SO_EE_ORIGIN_ICMP6))
			return 0;
		/* Same location as SO_EE_OFFENDER, with explicit length checks. */
		offender = (const unsigned char *)CMSG_DATA(cmsg) + sizeof(err);
		available = cmsg->cmsg_len - CMSG_LEN(sizeof(err));
		if (ipv4 && available >= sizeof(struct sockaddr_in)) {
			struct sockaddr_in sa;

			memcpy(&sa, offender, sizeof(sa));
			if (sa.sin_family == AF_INET && sa.sin_addr.s_addr != INADDR_ANY) {
				reply->visible = true;
				reply->offender.family = AF_INET;
				reply->offender.prefixlen = IPV4_MAX_BITLEN;
				reply->offender.u.prefix4 = sa.sin_addr;
			}
		} else if (!ipv4 && available >= sizeof(struct sockaddr_in6)) {
			struct sockaddr_in6 sa;

			memcpy(&sa, offender, sizeof(sa));
			if (sa.sin6_family == AF_INET6 && !IN6_IS_ADDR_UNSPECIFIED(&sa.sin6_addr)) {
				reply->visible = true;
				reply->offender.family = AF_INET6;
				reply->offender.prefixlen = IPV6_MAX_BITLEN;
				reply->offender.u.prefix6 = sa.sin6_addr;
			}
		}
		reply->type = err.ee_type;
		reply->code = err.ee_code;
		/* ICMPv4 type 11 / ICMPv6 type 3, code 0: TTL exceeded. */
		if (err.ee_type == (ipv4 ? 11 : 3) && err.ee_code == 0)
			return 1;
		/* Destination unreachable; v6 Packet Too Big / Parameter Problem. */
		if (err.ee_type != (ipv4 ? 3 : 1)
		    && (ipv4 || (err.ee_type != 2 && err.ee_type != 4)))
			return 0;
		reply->terminal = true;
		reply->reached = err.ee_type == (ipv4 ? 3 : 1)
			&& err.ee_code == (ipv4 ? 3 : 4) && reply->visible
			&& prefix_same(&reply->offender, target);
		return 1;
	}
	return 0;
#else
	(void)fd;
	(void)target;
	(void)port;
	(void)payload;
	(void)len;
	(void)reply;
	errno = ENOTSUP;
	return -1;
#endif
}
