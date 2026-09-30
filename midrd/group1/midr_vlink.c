// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR group 1: virtual links (see midr_vlink.h).
 */
#include <zebra.h>

#include <netinet/icmp6.h>

#include "command.h"
#include "frrevent.h"
#include "linklist.h"
#include "memory.h"
#include "network.h"
#include "vty.h"

#include "midrd/midr-dp-backend.h"
#include "midrd/midr-virtual-link.h"
#include "midrd/group1/midr_g1.h"
#include "midrd/group1/midr_nds.h"
#include "midrd/group1/midr_nds_facts.h"
#include "midrd/group1/midr_vlink.h"

DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_G1_VLINK, "MIDR group 1 virtual link");

#define MIDR_G1_VLINK_PREFIX_LEN 64

struct midr_g1_vlink {
	uint32_t remote_rid;
	struct midr_virtual_link_desc desc;
	/* Last state the service reported. */
	enum midr_virtual_link_state state;
	enum midr_virtual_link_stage stage;
	int last_error;
	ifindex_t ifindex;
	/* The service reports READY for the local end. */
	bool ready;
	/* The peer answers on the overlay; only then is the Link reported. */
	bool reachable;
	uint32_t failures;
	/* Overlay probing: sequence of the last probe, whether it was
	 * answered, and unanswered probes in a row. */
	uint16_t probe_seq;
	bool probe_answered;
	uint32_t probe_misses;
	/* Re-request after FAILED. */
	struct event *t_retry;
	/* Report or withdraw the Link after the state changed. */
	struct event *t_sync;
	struct event *t_probe;
};

static struct midr_g1 *vlink_g1;
/* struct midr_g1_vlink * */
static struct list *vlinks;

/* ICMPv6 echo socket shared by all tunnels. */
static int probe_fd = -1;
static uint16_t probe_ident;
static struct event *t_probe_read;

bool midr_g1_vlink_enabled(void)
{
	return midr_dp_backend_get() != NULL;
}

void midr_g1_vlink_overlay_addr(uint32_t a_rid, uint32_t b_rid,
				struct ipaddr *out)
{
	memset(out, 0, sizeof(*out));
	out->ipa_type = IPADDR_V6;
	out->ipaddr_v6.s6_addr[0] = 0xfe;
	out->ipaddr_v6.s6_addr[1] = 0x80;
	/* s_addr is in network order, so its bytes are already a.b.c.d. */
	memcpy(&out->ipaddr_v6.s6_addr[8], &a_rid, sizeof(a_rid));
	memcpy(&out->ipaddr_v6.s6_addr[12], &b_rid, sizeof(b_rid));
}

static void midr_g1_vlink_desc_build(const struct midr_g1 *g1,
				     uint32_t remote_rid,
				     const struct ipaddr *local_locator,
				     const struct ipaddr *remote_locator,
				     struct midr_virtual_link_desc *desc)
{
	memset(desc, 0, sizeof(*desc));
	snprintf(desc->ifname, sizeof(desc->ifname), "mgre-%08x",
		 ntohl(remote_rid));
	desc->vrf_id = g1->vrf_id;
	desc->outer_local = *local_locator;
	desc->outer_remote = *remote_locator;
	midr_g1_vlink_overlay_addr(g1->router_id.s_addr, remote_rid,
				   &desc->overlay_local);
	midr_g1_vlink_overlay_addr(remote_rid, g1->router_id.s_addr,
				   &desc->overlay_remote);
	desc->overlay_prefix_len = MIDR_G1_VLINK_PREFIX_LEN;
}

static bool midr_g1_vlink_desc_same(const struct midr_virtual_link_desc *a,
				    const struct midr_virtual_link_desc *b)
{
	return !strcmp(a->ifname, b->ifname) && a->vrf_id == b->vrf_id &&
	       !ipaddr_cmp(&a->outer_local, &b->outer_local) &&
	       !ipaddr_cmp(&a->outer_remote, &b->outer_remote) &&
	       !ipaddr_cmp(&a->overlay_local, &b->overlay_local) &&
	       !ipaddr_cmp(&a->overlay_remote, &b->overlay_remote) &&
	       a->overlay_prefix_len == b->overlay_prefix_len;
}

static struct midr_g1_vlink *midr_g1_vlink_lookup(uint32_t remote_rid)
{
	struct listnode *node;
	struct midr_g1_vlink *v;

	if (!vlinks)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(vlinks, node, v))
		if (v->remote_rid == remote_rid)
			return v;
	return NULL;
}

static struct midr_g1_vlink *midr_g1_vlink_lookup_name(const char *ifname)
{
	struct listnode *node;
	struct midr_g1_vlink *v;

	if (!vlinks || !ifname)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(vlinks, node, v))
		if (!strcmp(v->desc.ifname, ifname))
			return v;
	return NULL;
}

static void midr_g1_vlink_sync_cb(struct event *event)
{
	struct midr_g1_vlink *v = EVENT_ARG(event);
	struct prefix remote = { .family = AF_INET,
				 .prefixlen = IPV4_MAX_BITLEN };

	if (!vlink_g1)
		return;
	if (v->ready && v->reachable) {
		midr_nds_report_link_by_rid(vlink_g1, v->remote_rid);
		return;
	}
	remote.u.prefix4.s_addr = v->remote_rid;
	midr_nds_report_link_withdraw(vlink_g1, &remote);
}

static void midr_g1_vlink_sync(struct midr_g1_vlink *v)
{
	event_add_event(midr_g1_master(), midr_g1_vlink_sync_cb, v, 0,
			&v->t_sync);
}

/* ------------------------------------------------------------------------
 * Overlay probing
 * ---------------------------------------------------------------------- */

struct midr_g1_vlink_probe {
	struct icmp6_hdr hdr;
	uint32_t remote_rid;
};

static void midr_g1_vlink_probe_reply(struct midr_g1_vlink *v)
{
	v->probe_answered = true;
	v->probe_misses = 0;
	if (v->reachable)
		return;
	v->reachable = true;
	zlog_info("MIDR vlink: %s to %pI4 answers on the overlay",
		  v->desc.ifname, (struct in_addr *)&v->remote_rid);
	midr_g1_vlink_sync(v);
}

static void midr_g1_vlink_probe_miss(struct midr_g1_vlink *v)
{
	if (++v->probe_misses < MIDR_G1_VLINK_PROBE_MISSES || !v->reachable)
		return;
	v->reachable = false;
	zlog_warn("MIDR vlink: %s to %pI4 unanswered on the overlay %u times, withdrawing the Link",
		  v->desc.ifname, (struct in_addr *)&v->remote_rid,
		  v->probe_misses);
	midr_g1_vlink_sync(v);
}

void midr_g1_vlink_probe_event(uint32_t remote_rid, bool replied)
{
	struct midr_g1_vlink *v = midr_g1_vlink_lookup(remote_rid);

	if (!v || !v->ready)
		return;
	if (replied)
		midr_g1_vlink_probe_reply(v);
	else
		midr_g1_vlink_probe_miss(v);
}

static void midr_g1_vlink_probe_send(struct midr_g1_vlink *v)
{
	struct midr_g1_vlink_probe pkt = {};
	struct sockaddr_in6 dst = {};

	if (probe_fd < 0)
		return;
	pkt.hdr.icmp6_type = ICMP6_ECHO_REQUEST;
	pkt.hdr.icmp6_id = htons(probe_ident);
	pkt.hdr.icmp6_seq = htons(v->probe_seq);
	pkt.remote_rid = v->remote_rid;
	/* The kernel fills in the ICMPv6 checksum on raw sockets. */
	dst.sin6_family = AF_INET6;
	dst.sin6_addr = v->desc.overlay_remote.ipaddr_v6;
	dst.sin6_scope_id = v->ifindex;
	if (sendto(probe_fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst,
		   sizeof(dst)) < 0)
		MIDR_G1_LOG("MIDR vlink: probe on %s failed: %s",
			    v->desc.ifname, safe_strerror(errno));
}

static void midr_g1_vlink_probe_cb(struct event *event)
{
	struct midr_g1_vlink *v = EVENT_ARG(event);

	if (!v->ready)
		return;
	if (v->probe_seq && !v->probe_answered)
		midr_g1_vlink_probe_miss(v);
	v->probe_answered = false;
	v->probe_seq++;
	midr_g1_vlink_probe_send(v);
	event_add_timer_msec(midr_g1_master(), midr_g1_vlink_probe_cb, v,
			     v->reachable ? MIDR_G1_VLINK_KEEPALIVE_MS
					  : MIDR_G1_VLINK_PROBE_MS,
			     &v->t_probe);
}

/* READY: probe the peer before the Link may be reported. */
static void midr_g1_vlink_probe_start(struct midr_g1_vlink *v)
{
	v->reachable = false;
	v->probe_answered = false;
	v->probe_misses = 0;
	event_cancel(&v->t_probe);
	event_add_timer_msec(midr_g1_master(), midr_g1_vlink_probe_cb, v, 0,
			     &v->t_probe);
}

static void midr_g1_vlink_probe_stop(struct midr_g1_vlink *v)
{
	v->reachable = false;
	event_cancel(&v->t_probe);
}

static void midr_g1_vlink_probe_read(struct event *event)
{
	uint8_t buf[256];
	struct sockaddr_in6 from;
	socklen_t len;
	ssize_t n;

	event_add_read(midr_g1_master(), midr_g1_vlink_probe_read, NULL,
		       probe_fd, &t_probe_read);
	for (;;) {
		struct midr_g1_vlink_probe pkt;
		struct midr_g1_vlink *v;

		len = sizeof(from);
		n = recvfrom(probe_fd, buf, sizeof(buf), 0,
			     (struct sockaddr *)&from, &len);
		if (n < 0)
			break;
		if ((size_t)n < sizeof(pkt))
			continue;
		memcpy(&pkt, buf, sizeof(pkt));
		if (pkt.hdr.icmp6_type != ICMP6_ECHO_REPLY ||
		    ntohs(pkt.hdr.icmp6_id) != probe_ident)
			continue;
		v = midr_g1_vlink_lookup(pkt.remote_rid);
		/* Only the answer to the current probe of this tunnel, from the
		 * peer's overlay address on this tunnel, counts. */
		if (!v || !v->ready ||
		    ntohs(pkt.hdr.icmp6_seq) != v->probe_seq ||
		    from.sin6_scope_id != (uint32_t)v->ifindex ||
		    !IN6_ARE_ADDR_EQUAL(&from.sin6_addr,
					&v->desc.overlay_remote.ipaddr_v6))
			continue;
		midr_g1_vlink_probe_reply(v);
	}
}

static void midr_g1_vlink_probe_open(void)
{
	struct icmp6_filter filter;

	probe_fd = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	if (probe_fd < 0) {
		zlog_warn("MIDR vlink: no ICMPv6 socket (%s); overlay reachability cannot be confirmed and virtual-link Links will not be reported",
			  safe_strerror(errno));
		return;
	}
	(void)set_nonblocking(probe_fd);
	ICMP6_FILTER_SETBLOCKALL(&filter);
	ICMP6_FILTER_SETPASS(ICMP6_ECHO_REPLY, &filter);
	(void)setsockopt(probe_fd, IPPROTO_ICMPV6, ICMP6_FILTER, &filter,
			 sizeof(filter));
	probe_ident = (uint16_t)getpid();
	event_add_read(midr_g1_master(), midr_g1_vlink_probe_read, NULL,
		       probe_fd, &t_probe_read);
}

static void midr_g1_vlink_probe_close(void)
{
	event_cancel(&t_probe_read);
	if (probe_fd >= 0)
		close(probe_fd);
	probe_fd = -1;
}

/* ------------------------------------------------------------------------
 * Tunnel requests and service notifications
 * ---------------------------------------------------------------------- */

static void midr_g1_vlink_request(struct midr_g1_vlink *v, uint32_t flags);

static void midr_g1_vlink_retry_cb(struct event *event)
{
	struct midr_g1_vlink *v = EVENT_ARG(event);

	/* The service tears a FAILED tunnel down before it rebuilds it. */
	midr_g1_vlink_request(v, 0);
}

static void midr_g1_vlink_failed(struct midr_g1_vlink *v, int err,
				 enum midr_virtual_link_stage stage)
{
	bool was_ready = v->ready;

	v->ready = false;
	midr_g1_vlink_probe_stop(v);
	v->ifindex = 0;
	v->state = MIDR_VLINK_FAILED;
	v->stage = stage;
	v->last_error = err;
	/* Warn once per outage; the retries that follow log at debug. */
	if (!v->failures++)
		zlog_warn("MIDR vlink: %s to %pI4 failed at stage %d: %s",
			  v->desc.ifname, (struct in_addr *)&v->remote_rid,
			  stage, safe_strerror(err));
	else
		MIDR_G1_LOG("MIDR vlink: %s to %pI4 failed again (%u) at stage %d: %s",
			    v->desc.ifname, (struct in_addr *)&v->remote_rid,
			    v->failures, stage, safe_strerror(err));
	if (was_ready)
		midr_g1_vlink_sync(v);
	event_add_timer_msec(midr_g1_master(), midr_g1_vlink_retry_cb, v,
			     MIDR_G1_VLINK_RETRY_MS, &v->t_retry);
}

static void midr_g1_vlink_request(struct midr_g1_vlink *v, uint32_t flags)
{
	struct midr_virtual_link_status st = {};
	int ret;

	event_cancel(&v->t_retry);
	v->desc.flags = flags;
	ret = midr_virtual_link_add(vlink_g1->ctx, &v->desc, &st);
	/* The notify callback may already have run; READY and FAILED are
	 * applied idempotently. */
	if (ret) {
		midr_g1_vlink_failed(v, st.last_error, st.stage);
		return;
	}
	v->state = st.state;
	if (st.state == MIDR_VLINK_READY && st.overlay_ready && !v->ready) {
		v->ready = true;
		v->ifindex = st.ifindex;
		v->failures = 0;
		v->stage = MIDR_VLINK_STAGE_NONE;
		v->last_error = 0;
		midr_g1_vlink_probe_start(v);
	}
	MIDR_G1_LOG("MIDR vlink: requested %s to %pI4, state %s",
		    v->desc.ifname, (struct in_addr *)&v->remote_rid,
		    midr_virtual_link_state_str(st.state));
}

static void midr_g1_vlink_notify(const struct midr_virtual_link_status *st,
				 void *arg)
{
	struct midr_g1_vlink *v = midr_g1_vlink_lookup_name(st->ifname);

	(void)arg;
	if (!v)
		return;
	v->state = st->state;
	switch (st->event) {
	case MIDR_VLINK_EV_READY:
		if (!st->overlay_ready || v->ready)
			break;
		v->ready = true;
		v->ifindex = st->ifindex;
		v->stage = MIDR_VLINK_STAGE_NONE;
		v->last_error = 0;
		v->failures = 0;
		event_cancel(&v->t_retry);
		zlog_info("MIDR vlink: %s to %pI4 ready, ifindex %d",
			  v->desc.ifname, (struct in_addr *)&v->remote_rid,
			  v->ifindex);
		midr_g1_vlink_probe_start(v);
		break;
	case MIDR_VLINK_EV_FAILED:
		midr_g1_vlink_failed(v, st->last_error, st->stage);
		break;
	case MIDR_VLINK_EV_DOWN:
		/* A retry makes the service tear the FAILED tunnel down first,
		 * which reports DOWN; only losing a READY tunnel is a failure.
		 * Group 1's own deletes forget the tunnel before they run. */
		if (v->ready)
			midr_g1_vlink_failed(v, ENODEV, st->stage);
		else
			v->ifindex = 0;
		break;
	case MIDR_VLINK_EV_DEVICE_UP:
	case MIDR_VLINK_EV_ADDRESS_SET:
		v->ifindex = st->ifindex;
		break;
	case MIDR_VLINK_EV_NONE:
		break;
	}
}

bool midr_g1_vlink_ready(struct midr_g1 *g1, uint32_t remote_rid,
			 const struct ipaddr *local_locator,
			 const struct ipaddr *remote_locator,
			 struct midr_g1_vlink_info *out)
{
	struct midr_virtual_link_desc desc;
	struct midr_g1_vlink *v;

	if (!g1 || !vlinks || !remote_rid)
		return false;
	midr_g1_vlink_desc_build(g1, remote_rid, local_locator,
				 remote_locator, &desc);
	v = midr_g1_vlink_lookup(remote_rid);
	if (!v) {
		v = XCALLOC(MTYPE_MIDR_G1_VLINK, sizeof(*v));
		v->remote_rid = remote_rid;
		v->desc = desc;
		listnode_add(vlinks, v);
		midr_g1_vlink_request(v, 0);
	} else if (!midr_g1_vlink_desc_same(&v->desc, &desc)) {
		/* A locator changed: rebuild the tunnel under the same name
		 * and stop reporting the old one meanwhile. */
		zlog_info("MIDR vlink: endpoints of %s changed, rebuilding",
			  v->desc.ifname);
		v->desc = desc;
		if (v->ready) {
			v->ready = false;
			midr_g1_vlink_probe_stop(v);
			v->ifindex = 0;
			midr_g1_vlink_sync(v);
		}
		midr_g1_vlink_request(v, MIDR_VLINK_F_REBIND);
	}
	return midr_g1_vlink_get(remote_rid, out);
}

bool midr_g1_vlink_get(uint32_t remote_rid, struct midr_g1_vlink_info *out)
{
	struct midr_g1_vlink *v = midr_g1_vlink_lookup(remote_rid);

	if (!v || !v->ready || !v->reachable)
		return false;
	if (out) {
		out->ifindex = v->ifindex;
		out->overlay_local = v->desc.overlay_local;
		out->overlay_remote = v->desc.overlay_remote;
		out->outer_local = v->desc.outer_local;
		out->outer_remote = v->desc.outer_remote;
	}
	return true;
}

static void midr_g1_vlink_delete(struct midr_g1_vlink *v)
{
	/* Forget the tunnel before deleting it, so the DOWN notification the
	 * delete causes finds nothing. */
	listnode_delete(vlinks, v);
	event_cancel(&v->t_retry);
	event_cancel(&v->t_sync);
	event_cancel(&v->t_probe);
	if (midr_virtual_link_del(vlink_g1->ctx, v->desc.ifname, NULL))
		zlog_warn("MIDR vlink: deleting %s failed", v->desc.ifname);
	else
		MIDR_G1_LOG("MIDR vlink: deleted %s", v->desc.ifname);
	XFREE(MTYPE_MIDR_G1_VLINK, v);
}

void midr_g1_vlink_release(struct midr_g1 *g1, uint32_t remote_rid)
{
	struct midr_g1_vlink *v = midr_g1_vlink_lookup(remote_rid);

	(void)g1;
	if (v)
		midr_g1_vlink_delete(v);
}

DEFUN(show_midr_virtual_links, show_midr_virtual_links_cmd,
      "show midr virtual-links",
      SHOW_STR "MIDR information\n" "Group 1 GRE virtual links\n")
{
	struct listnode *node;
	struct midr_g1_vlink *v;

	if (!midr_g1_vlink_enabled()) {
		vty_out(vty, "Virtual links off: midrd runs without zebra\n");
		return CMD_SUCCESS;
	}
	vty_out(vty, "Virtual links: %u\n", vlinks ? listcount(vlinks) : 0);
	for (ALL_LIST_ELEMENTS_RO(vlinks, node, v)) {
		vty_out(vty, "  %s  node %pI4  %s  ifindex %d  overlay %s\n",
			v->desc.ifname, (struct in_addr *)&v->remote_rid,
			v->ready ? "READY"
				 : midr_virtual_link_state_str(v->state),
			v->ifindex,
			!v->ready       ? "-"
			: v->reachable ? "reachable"
				       : "unreachable");
		vty_out(vty, "    outer    %pIA -> %pIA\n",
			&v->desc.outer_local, &v->desc.outer_remote);
		vty_out(vty, "    overlay  %pIA/%u -> %pIA\n",
			&v->desc.overlay_local, v->desc.overlay_prefix_len,
			&v->desc.overlay_remote);
		if (v->failures)
			vty_out(vty, "    failures %u, last stage %d, error %d\n",
				v->failures, v->stage, v->last_error);
		if (v->ready)
			vty_out(vty, "    probe seq %u, unanswered %u\n",
				v->probe_seq, v->probe_misses);
	}
	return CMD_SUCCESS;
}

void midr_g1_vlink_init(struct midr_g1 *g1)
{
	vlink_g1 = g1;
	vlinks = list_new();
	install_element(VIEW_NODE, &show_midr_virtual_links_cmd);
	if (midr_g1_vlink_enabled()) {
		midr_virtual_link_register_notify(midr_g1_vlink_notify, g1);
		midr_g1_vlink_probe_open();
	}
}

void midr_g1_vlink_finish(struct midr_g1 *g1)
{
	struct midr_g1_vlink *v;

	(void)g1;
	if (!vlinks)
		return;
	if (midr_g1_vlink_enabled())
		midr_virtual_link_unregister_notify(midr_g1_vlink_notify);
	while ((v = listnode_head(vlinks)))
		midr_g1_vlink_delete(v);
	list_delete(&vlinks);
	midr_g1_vlink_probe_close();
	vlink_g1 = NULL;
}
