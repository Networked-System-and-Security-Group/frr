// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR topology/event interface skeleton.
 */

#include <zebra.h>

#include <errno.h>

#include "hash.h"
#include "jhash.h"
#include "linklist.h"
#include "log.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_route.h"

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR, "BGP MIDR instance");
DEFINE_MTYPE_STATIC(BGPD, MIDR_EVENT, "MIDR topology event");
DEFINE_MTYPE_STATIC(BGPD, MIDR_NODE_ENTRY, "MIDR node entry");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LINK_ENTRY, "MIDR link entry");

enum midr_event_type {
	MIDR_EVENT_NODE_UPSERT,
	MIDR_EVENT_NODE_WITHDRAW,
	MIDR_EVENT_LINK_UPSERT,
	MIDR_EVENT_LINK_WITHDRAW,
};

struct midr_event {
	enum midr_event_type type;
	union {
		struct midr_node_update node;
		struct {
			uint32_t node_id;
			uint64_t version;
		} node_withdraw;
		struct midr_link_update link;
		struct {
			struct midr_link_key key;
			uint64_t version;
		} link_withdraw;
	} u;
};

struct midr_node_entry {
	struct midr_node_update data;
	bool active;
};

struct midr_link_entry {
	struct midr_link_update data;
	bool active;
};

struct bgp_midr {
	struct bgp *bgp;
	struct midr_context ctx;
	struct list *event_queue;
	struct event *t_process;
	struct hash *node_table;
	struct hash *link_table;
	uint64_t event_enqueued;
	uint64_t event_processed;
	uint64_t event_ignored_old;
	uint64_t peer_hook_events;
	uint64_t route_hook_events;
	struct midr_remote_view_callbacks remote_callbacks;
	bool remote_callbacks_registered;
};

static bool midr_hooks_registered;

static unsigned int midr_node_hash_key(const void *data)
{
	const struct midr_node_entry *entry = data;

	return jhash_1word(entry->data.node_id, 0);
}

static bool midr_node_hash_cmp(const void *data1, const void *data2)
{
	const struct midr_node_entry *a = data1;
	const struct midr_node_entry *b = data2;

	return a->data.node_id == b->data.node_id;
}

static unsigned int midr_link_hash_key(const void *data)
{
	const struct midr_link_entry *entry = data;
	const struct midr_link_key *key = &entry->data.key;
	uint32_t words[4];

	words[0] = key->local_node_id;
	words[1] = key->remote_node_id;
	words[2] = (uint32_t)(key->link_id >> 32);
	words[3] = (uint32_t)key->link_id;

	return jhash2(words, array_size(words), 0);
}

static bool midr_link_key_same(const struct midr_link_key *a, const struct midr_link_key *b)
{
	return a->local_node_id == b->local_node_id && a->remote_node_id == b->remote_node_id &&
	       a->link_id == b->link_id;
}

static bool midr_link_hash_cmp(const void *data1, const void *data2)
{
	const struct midr_link_entry *a = data1;
	const struct midr_link_entry *b = data2;

	return midr_link_key_same(&a->data.key, &b->data.key);
}

static void midr_event_free(void *data)
{
	XFREE(MTYPE_MIDR_EVENT, data);
}

static void midr_node_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_NODE_ENTRY, data);
}

static void midr_link_entry_free(void *data)
{
	XFREE(MTYPE_MIDR_LINK_ENTRY, data);
}

static bool midr_policy_valid(enum midr_policy_state state)
{
	return state == MIDR_POLICY_ALLOWED || state == MIDR_POLICY_BLOCKED;
}

static bool midr_ipaddr_present(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 || address->ipa_type == IPADDR_V6;
}

int midr_validate_node_update(uint32_t local_node_id, const struct midr_node_update *node)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node || !node->node_id || node->node_id != local_node_id)
		return -EINVAL;
	if (!midr_policy_valid(node->policy_state))
		return -EINVAL;
	if (node->has_transport_address) {
		if (!midr_ipaddr_present(&node->transport_address))
			return -EINVAL;
	} else if (node->transport_address.ipa_type != IPADDR_NONE) {
		return -EINVAL;
	}

	return 0;
}

int midr_validate_node_withdraw(uint32_t local_node_id, uint32_t node_id)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node_id || node_id != local_node_id)
		return -EINVAL;

	return 0;
}

int midr_validate_link_update(uint32_t local_node_id, const struct midr_link_update *link)
{
	if (!local_node_id)
		return -ENOENT;
	if (!link || !link->key.local_node_id || link->key.local_node_id != local_node_id ||
	    !link->key.remote_node_id || link->key.remote_node_id == link->key.local_node_id)
		return -EINVAL;
	if (link->local_ifindex < 0 || !midr_ipaddr_present(&link->link_local_address) ||
	    !midr_ipaddr_present(&link->link_remote_address) ||
	    link->link_local_address.ipa_type != link->link_remote_address.ipa_type)
		return -EINVAL;
	if (!link->metrics.has_rtt_us || !link->metrics.has_loss_ppm ||
	    !link->metrics.has_available_bandwidth_kbps)
		return -EINVAL;
	if (!link->metrics.rtt_us || !link->metrics.available_bandwidth_kbps ||
	    link->metrics.loss_ppm >= 1000000)
		return -EINVAL;
	if (!midr_policy_valid(link->policy_state))
		return -EINVAL;

	return 0;
}

int midr_validate_link_withdraw(uint32_t local_node_id, const struct midr_link_key *key)
{
	if (!local_node_id)
		return -ENOENT;
	if (!key || !key->local_node_id || key->local_node_id != local_node_id ||
	    !key->remote_node_id || key->remote_node_id == key->local_node_id)
		return -EINVAL;

	return 0;
}

static struct midr_context *midr_context_from_bgp(struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return NULL;

	return &bgp->midr_info->ctx;
}

static void midr_apply_node_upsert(struct bgp_midr *midr, const struct midr_node_update *node)
{
	struct midr_node_entry lookup = { .data.node_id = node->node_id };
	struct midr_node_entry *entry;

	entry = hash_lookup(midr->node_table, &lookup);
	if (entry && node->version <= entry->data.version) {
		midr->event_ignored_old++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->data.node_id = node->node_id;
		(void)hash_get(midr->node_table, entry, hash_alloc_intern);
	}

	entry->data = *node;
	entry->active = true;
}

static void midr_apply_node_withdraw(struct bgp_midr *midr, uint32_t node_id, uint64_t version)
{
	struct midr_node_entry lookup = { .data.node_id = node_id };
	struct midr_node_entry *entry;

	entry = hash_lookup(midr->node_table, &lookup);
	if (entry && version <= entry->data.version) {
		midr->event_ignored_old++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_NODE_ENTRY, sizeof(*entry));
		entry->data.node_id = node_id;
		(void)hash_get(midr->node_table, entry, hash_alloc_intern);
	}

	memset(&entry->data, 0, sizeof(entry->data));
	entry->data.node_id = node_id;
	entry->data.version = version;
	entry->active = false;
}

static void midr_apply_link_upsert(struct bgp_midr *midr, const struct midr_link_update *link)
{
	struct midr_link_entry lookup = { .data.key = link->key };
	struct midr_link_entry *entry;

	entry = hash_lookup(midr->link_table, &lookup);
	if (entry && link->version <= entry->data.version) {
		midr->event_ignored_old++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*entry));
		entry->data.key = link->key;
		(void)hash_get(midr->link_table, entry, hash_alloc_intern);
	}

	entry->data = *link;
	entry->active = true;
}

static void midr_apply_link_withdraw(struct bgp_midr *midr, const struct midr_link_key *key,
				     uint64_t version)
{
	struct midr_link_entry lookup = { .data.key = *key };
	struct midr_link_entry *entry;

	entry = hash_lookup(midr->link_table, &lookup);
	if (entry && version <= entry->data.version) {
		midr->event_ignored_old++;
		return;
	}

	if (!entry) {
		entry = XCALLOC(MTYPE_MIDR_LINK_ENTRY, sizeof(*entry));
		entry->data.key = *key;
		(void)hash_get(midr->link_table, entry, hash_alloc_intern);
	}

	memset(&entry->data, 0, sizeof(entry->data));
	entry->data.key = *key;
	entry->data.version = version;
	entry->active = false;
}

void midr_topology_process_pending(struct midr_context *ctx)
{
	struct bgp_midr *midr;
	struct midr_event *event;

	if (!ctx || !ctx->midr)
		return;

	midr = ctx->midr;

	while ((event = listnode_head(midr->event_queue)) != NULL) {
		listnode_delete(midr->event_queue, event);

		switch (event->type) {
		case MIDR_EVENT_NODE_UPSERT:
			midr_apply_node_upsert(midr, &event->u.node);
			break;
		case MIDR_EVENT_NODE_WITHDRAW:
			midr_apply_node_withdraw(midr, event->u.node_withdraw.node_id,
						 event->u.node_withdraw.version);
			break;
		case MIDR_EVENT_LINK_UPSERT:
			midr_apply_link_upsert(midr, &event->u.link);
			break;
		case MIDR_EVENT_LINK_WITHDRAW:
			midr_apply_link_withdraw(midr, &event->u.link_withdraw.key,
						 event->u.link_withdraw.version);
			break;
		}

		midr->event_processed++;
		midr_event_free(event);
	}
}

static void midr_process_event_cb(struct event *event)
{
	struct midr_context *ctx = EVENT_ARG(event);

	if (ctx && ctx->midr)
		ctx->midr->t_process = NULL;
	midr_topology_process_pending(ctx);
}

static void midr_schedule_process(struct midr_context *ctx)
{
	if (!ctx || !ctx->midr || ctx->midr->t_process)
		return;

	event_add_event(bm->master, midr_process_event_cb, ctx, 0, &ctx->midr->t_process);
}

static int midr_enqueue_event(struct midr_context *ctx, struct midr_event *event)
{
	if (!ctx || !ctx->midr || !event)
		return -EINVAL;

	listnode_add(ctx->midr->event_queue, event);
	ctx->midr->event_enqueued++;
	midr_schedule_process(ctx);

	return 0;
}

struct midr_context *midr_context_get_default(void)
{
	return midr_context_from_bgp(bgp_get_default());
}

int midr_peer_session_request(struct midr_context *ctx,
			      const struct midr_peer_session_request_info *req)
{
	struct peer *peer;
	as_t remote_as;
	char remote_as_str[ASN_STRING_MAX_SIZE];
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!req)
		return -EINVAL;
	if (sockunion_family(&req->remote_address) != AF_INET &&
	    sockunion_family(&req->remote_address) != AF_INET6)
		return -EINVAL;
	if (!req->remote_as || req->afi <= AFI_UNSPEC || req->afi >= AFI_MAX ||
	    req->safi <= SAFI_UNSPEC || req->safi >= SAFI_MAX)
		return -EINVAL;
	if (req->has_update_source || req->ebgp_multihop || (req->password && req->password[0]) ||
	    req->policy_tags)
		return -ENOTSUP;

	remote_as = req->remote_as;
	snprintf(remote_as_str, sizeof(remote_as_str), "%u", remote_as);
	ret = peer_remote_as(ctx->bgp, (union sockunion *)&req->remote_address, NULL, &remote_as,
			     AS_SPECIFIED, remote_as_str);
	if (ret)
		return -EINVAL;

	peer = peer_lookup(ctx->bgp, (union sockunion *)&req->remote_address);
	if (!peer)
		return -ENOENT;

	ret = peer_activate(peer, req->afi, req->safi);
	if (ret)
		return -EINVAL;

	return 0;
}

int midr_peer_session_release(struct midr_context *ctx, const union sockunion *remote_address,
			      afi_t afi, safi_t safi, enum midr_peer_release_reason reason)
{
	struct peer *peer;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!remote_address)
		return -EINVAL;
	if (sockunion_family(remote_address) != AF_INET &&
	    sockunion_family(remote_address) != AF_INET6)
		return -EINVAL;
	if (afi <= AFI_UNSPEC || afi >= AFI_MAX || safi <= SAFI_UNSPEC || safi >= SAFI_MAX)
		return -EINVAL;
	if (reason != MIDR_PEER_RELEASE_ADMIN && reason != MIDR_PEER_RELEASE_NODE_DOWN &&
	    reason != MIDR_PEER_RELEASE_POLICY)
		return -EINVAL;

	peer = peer_lookup(ctx->bgp, (union sockunion *)remote_address);
	if (!peer)
		return -ENOENT;

	ret = peer_deactivate(peer, afi, safi);
	if (ret)
		return -EINVAL;

	return 0;
}

int midr_topology_node_upsert(struct midr_context *ctx, const struct midr_node_update *node)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	ret = midr_validate_node_update(ctx->bgp->router_id.s_addr, node);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_NODE_UPSERT;
	event->u.node = *node;

	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_node_withdraw(struct midr_context *ctx, uint32_t node_id, uint64_t version)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	ret = midr_validate_node_withdraw(ctx->bgp->router_id.s_addr, node_id);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_NODE_WITHDRAW;
	event->u.node_withdraw.node_id = node_id;
	event->u.node_withdraw.version = version;

	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_link_upsert(struct midr_context *ctx, const struct midr_link_update *link)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	ret = midr_validate_link_update(ctx->bgp->router_id.s_addr, link);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_LINK_UPSERT;
	event->u.link = *link;

	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_link_withdraw(struct midr_context *ctx, const struct midr_link_key *key,
				uint64_t version)
{
	struct midr_event *event;
	int ret;

	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	ret = midr_validate_link_withdraw(ctx->bgp->router_id.s_addr, key);
	if (ret)
		return ret;

	event = XCALLOC(MTYPE_MIDR_EVENT, sizeof(*event));
	event->type = MIDR_EVENT_LINK_WITHDRAW;
	event->u.link_withdraw.key = *key;
	event->u.link_withdraw.version = version;

	ret = midr_enqueue_event(ctx, event);
	if (ret)
		midr_event_free(event);
	return ret;
}

int midr_topology_resync_begin(struct midr_context *ctx, enum midr_topology_resync_reason reason)
{
	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (reason != MIDR_TOPOLOGY_RESYNC_VERSION_LOST &&
	    reason != MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART &&
	    reason != MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT)
		return -EINVAL;

	return -EAGAIN;
}

int midr_remote_view_snapshot_get(struct midr_context *ctx,
				  struct midr_remote_view_snapshot *snapshot)
{
	if (!snapshot)
		return -EINVAL;

	memset(snapshot, 0, sizeof(*snapshot));
	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;

	return -EAGAIN;
}

void midr_remote_view_snapshot_release(struct midr_context *ctx,
				       struct midr_remote_view_snapshot *snapshot)
{
	(void)ctx;

	if (!snapshot)
		return;

	memset(snapshot, 0, sizeof(*snapshot));
}

int midr_remote_view_callbacks_register(struct midr_context *ctx,
					const struct midr_remote_view_callbacks *callbacks)
{
	if (!ctx || !ctx->bgp || !ctx->midr)
		return -ENOENT;
	if (!callbacks)
		return -EINVAL;

	ctx->midr->remote_callbacks = *callbacks;
	ctx->midr->remote_callbacks_registered = true;
	return 0;
}

static void midr_show_node_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_node_entry *entry = bucket->data;
	struct in_addr addr = { .s_addr = entry->data.node_id };
	char transport[IPADDR_STRING_SIZE];

	if (!entry->active)
		return;

	vty_out(vty, "%pI4 group %u", &addr, entry->data.group_id);
	if (entry->data.has_transport_address)
		vty_out(vty, " transport %s %s",
			entry->data.transport_address.ipa_type == IPADDR_V4 ? "ipv4" : "ipv6",
			ipaddr2str(&entry->data.transport_address, transport, sizeof(transport)));
	vty_out(vty, " policy %s version %" PRIu64 " active\n",
		entry->data.policy_state == MIDR_POLICY_ALLOWED ? "allowed" : "blocked",
		entry->data.version);
}

void midr_show_topology_nodes(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->midr) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology nodes:\n");
	hash_iterate(ctx->midr->node_table, midr_show_node_iter, vty);
}

static void midr_show_link_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_link_entry *entry = bucket->data;
	struct in_addr local = { .s_addr = entry->data.key.local_node_id };
	struct in_addr remote = { .s_addr = entry->data.key.remote_node_id };
	char local_address[IPADDR_STRING_SIZE];
	char remote_address[IPADDR_STRING_SIZE];
	const char *address_type;

	if (!entry->active)
		return;

	address_type = entry->data.link_local_address.ipa_type == IPADDR_V4 ? "ipv4" : "ipv6";
	vty_out(vty,
		"%pI4 -> %pI4 id %" PRIu64 " local-address %s %s remote-address %s %s ifindex %d"
		" rtt-us %u loss-ppm %u available-bandwidth-kbps %u"
		" policy %s version %" PRIu64,
		&local, &remote, entry->data.key.link_id, address_type,
		ipaddr2str(&entry->data.link_local_address, local_address, sizeof(local_address)),
		address_type,
		ipaddr2str(&entry->data.link_remote_address, remote_address,
			   sizeof(remote_address)),
		entry->data.local_ifindex, entry->data.metrics.rtt_us,
		entry->data.metrics.loss_ppm, entry->data.metrics.available_bandwidth_kbps,
		entry->data.policy_state == MIDR_POLICY_ALLOWED ? "allowed" : "blocked",
		entry->data.version);
	if (entry->data.metrics.measurement_seqno)
		vty_out(vty, " seqno %" PRIu64, entry->data.metrics.measurement_seqno);
	if (entry->data.metrics.measurement_timestamp_ms)
		vty_out(vty, " timestamp-ms %" PRIu64,
			entry->data.metrics.measurement_timestamp_ms);
	vty_out(vty, " active\n");
}

void midr_show_topology_links(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->midr) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology links:\n");
	hash_iterate(ctx->midr->link_table, midr_show_link_iter, vty);
}

static void midr_show_node_tombstone_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_node_entry *entry = bucket->data;
	struct in_addr addr = { .s_addr = entry->data.node_id };

	if (entry->active)
		return;

	vty_out(vty, "    %pI4 version %" PRIu64 "\n", &addr, entry->data.version);
}

static void midr_show_link_tombstone_iter(struct hash_bucket *bucket, void *arg)
{
	struct vty *vty = arg;
	struct midr_link_entry *entry = bucket->data;
	struct in_addr local = { .s_addr = entry->data.key.local_node_id };
	struct in_addr remote = { .s_addr = entry->data.key.remote_node_id };

	if (entry->active)
		return;

	vty_out(vty, "    %pI4 -> %pI4 id %" PRIu64 " version %" PRIu64 "\n", &local, &remote,
		entry->data.key.link_id, entry->data.version);
}

void midr_show_topology_tombstones(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->midr) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR topology tombstones:\n");
	vty_out(vty, "  nodes:\n");
	hash_iterate(ctx->midr->node_table, midr_show_node_tombstone_iter, vty);
	vty_out(vty, "  links:\n");
	hash_iterate(ctx->midr->link_table, midr_show_link_tombstone_iter, vty);
}

void midr_show_events(struct vty *vty, struct midr_context *ctx)
{
	if (!ctx || !ctx->midr) {
		vty_out(vty, "MIDR is not initialized\n");
		return;
	}

	midr_topology_process_pending(ctx);
	vty_out(vty, "MIDR events:\n");
	vty_out(vty, "  enqueued:       %" PRIu64 "\n", ctx->midr->event_enqueued);
	vty_out(vty, "  processed:      %" PRIu64 "\n", ctx->midr->event_processed);
	vty_out(vty, "  ignored-old:    %" PRIu64 "\n", ctx->midr->event_ignored_old);
	vty_out(vty, "  peer hooks:     %" PRIu64 "\n", ctx->midr->peer_hook_events);
	vty_out(vty, "  route hooks:    %" PRIu64 "\n", ctx->midr->route_hook_events);
	vty_out(vty, "  pending queue:  %u\n", listcount(ctx->midr->event_queue));
}

static int midr_peer_status_changed(struct peer *peer)
{
	struct midr_context *ctx;

	if (!peer || !peer->bgp)
		return 0;

	ctx = midr_context_from_bgp(peer->bgp);
	if (ctx && ctx->midr)
		ctx->midr->peer_hook_events++;

	return 0;
}

static int midr_bgp_route_update(struct bgp *bgp, afi_t afi, safi_t safi, struct bgp_dest *bn,
				 struct bgp_path_info *old_route, struct bgp_path_info *new_route)
{
	struct midr_context *ctx = midr_context_from_bgp(bgp);

	(void)afi;
	(void)safi;
	(void)bn;
	(void)old_route;
	(void)new_route;

	if (ctx && ctx->midr)
		ctx->midr->route_hook_events++;

	return 0;
}

static void midr_hooks_register_once(void)
{
	if (midr_hooks_registered)
		return;

	hook_register(peer_status_changed, midr_peer_status_changed);
	hook_register(bgp_route_update, midr_bgp_route_update);
	midr_hooks_registered = true;
}

void bgp_midr_init(struct bgp *bgp)
{
	struct bgp_midr *midr;
	int ret;

	if (!bgp || bgp->midr_info)
		return;

	midr = XCALLOC(MTYPE_BGP_MIDR, sizeof(*midr));
	midr->bgp = bgp;
	midr->ctx.bgp = bgp;
	midr->ctx.midr = midr;
	ret = midr_ted_context_init(&midr->ctx);
	if (ret) {
		XFREE(MTYPE_BGP_MIDR, midr);
		return;
	}
	midr->event_queue = list_new();
	midr->event_queue->del = midr_event_free;
	midr->node_table = hash_create(midr_node_hash_key, midr_node_hash_cmp, "MIDR node table");
	midr->link_table = hash_create(midr_link_hash_key, midr_link_hash_cmp, "MIDR link table");

	bgp->midr_info = midr;
	midr_hooks_register_once();
}

void bgp_midr_finish(struct bgp *bgp)
{
	struct bgp_midr *midr;

	if (!bgp || !bgp->midr_info)
		return;

	midr = bgp->midr_info;
	event_cancel(&midr->t_process);
	midr_ted_context_finish(&midr->ctx);
	list_delete(&midr->event_queue);
	hash_clean_and_free(&midr->node_table, midr_node_entry_free);
	hash_clean_and_free(&midr->link_table, midr_link_entry_free);
	bgp->midr_info = NULL;
	XFREE(MTYPE_BGP_MIDR, midr);
}
