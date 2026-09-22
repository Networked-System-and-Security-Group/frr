// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR node directory for group 1 inside midrd.
 *
 * In bgpd, group 2 flooded every node's group, locator and role bits and
 * handed them to NDS through the remote-view callbacks.  midrd's Membership
 * object carries only the group, so group 1 floods this directory itself:
 *
 *   - every node advertises its own Node fact (exactly what it hands to
 *     midr_topology_node_upsert/withdraw) in a NODE_ADV control datagram;
 *   - a node accepts an advertisement with a higher sequence number than it
 *     holds, reports it through the remote-view callbacks and relays it to
 *     every other established overlay session;
 *   - advertisements are refreshed periodically and expire when a node stops
 *     refreshing, and a new session receives the whole directory.
 *
 * Wire format (48 bytes, UDP control port):
 *   0   version (MIDR_CTRL_MSG_VERSION)   1  type (MIDR_CTRL_NODE_ADV)
 *   2   flags (bit 0: withdrawn)          4  node id (BGP Identifier)
 *   8   group id                         12  capability bits
 *   16  locator (MIDR_CTRL_LOCATOR_LEN)  36  sequence number
 *   44  lifetime in seconds              46  reserved
 */
#include <zebra.h>

#include "command.h"
#include "log.h"
#include "monotime.h"
#include "vty.h"

#include "midrd/group1/midr_g1.h"
#include "midrd/group1/midr_nds.h"
#include "midrd/group1/midr_ctrl.h"

DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_NODEDIR_ENTRY, "MIDR node directory entry");
DEFINE_MTYPE_STATIC(MIDR_G1, MIDR_NODEDIR_SNAPSHOT,
		    "MIDR node directory snapshot");

#define NODEDIR_FLAG_WITHDRAWN 0x0001
#define NODEDIR_REFRESH_SECS 10
#define NODEDIR_LIFETIME_SECS 45
/* A withdrawal is kept (and re-sent) this long so late copies of the old
 * advertisement cannot resurrect the node. */
#define NODEDIR_TOMBSTONE_SECS NODEDIR_LIFETIME_SECS

struct nodedir_entry {
	struct in_addr node_id;
	uint32_t group_id;
	uint32_t cap_flags;
	struct ipaddr transport;
	uint64_t sequence;
	bool withdrawn;
	time_t expires;
	struct ipaddr learned_from;
};

struct nodedir {
	/* struct nodedir_entry *, remote nodes only */
	struct list *entries;
	/* Our own advertisement. */
	struct nodedir_entry self;
	bool self_valid;
	time_t self_withdrawn_at;
	const struct midr_remote_view_callbacks *callbacks;
	struct event *t_refresh;
	uint64_t rx_accepted, rx_stale, rx_invalid, tx_frames;
};

static struct nodedir nodedir;

static void put_u16(uint8_t *p, uint16_t v)
{
	v = htons(v);
	memcpy(p, &v, sizeof(v));
}

static void put_u32(uint8_t *p, uint32_t v)
{
	v = htonl(v);
	memcpy(p, &v, sizeof(v));
}

static uint16_t get_u16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return ntohs(v);
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return ntohl(v);
}

static void put_u64(uint8_t *p, uint64_t v)
{
	put_u32(p, (uint32_t)(v >> 32));
	put_u32(p + 4, (uint32_t)v);
}

static uint64_t get_u64(const uint8_t *p)
{
	return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

static struct nodedir_entry *nodedir_find(struct in_addr node_id)
{
	struct listnode *node;
	struct nodedir_entry *e;

	for (ALL_LIST_ELEMENTS_RO(nodedir.entries, node, e))
		if (e->node_id.s_addr == node_id.s_addr)
			return e;
	return NULL;
}

static bool nodedir_encode(const struct nodedir_entry *e, uint8_t *buf)
{
	memset(buf, 0, MIDR_CTRL_NODE_ADV_LEN);
	buf[0] = MIDR_CTRL_MSG_VERSION;
	buf[1] = MIDR_CTRL_NODE_ADV;
	put_u16(buf + 2, e->withdrawn ? NODEDIR_FLAG_WITHDRAWN : 0);
	memcpy(buf + 4, &e->node_id, 4);
	put_u32(buf + 8, e->group_id);
	put_u32(buf + 12, e->cap_flags);
	if (!e->withdrawn && !midr_ctrl_encode_locator(buf + 16, &e->transport))
		return false;
	put_u64(buf + 36, e->sequence);
	put_u16(buf + 44, NODEDIR_LIFETIME_SECS);
	return true;
}

/* Send one entry to every established session except the one it came from. */
static void nodedir_flood(struct midr_g1 *g1, const struct nodedir_entry *e,
			  const struct ipaddr *except)
{
	uint8_t buf[MIDR_CTRL_NODE_ADV_LEN];
	struct listnode *node;
	struct midr_g1_peer *peer;

	if (!nodedir_encode(e, buf))
		return;
	for (ALL_LIST_ELEMENTS_RO(g1->peer, node, peer)) {
		if (!peer->established)
			continue;
		if (except && !ipaddr_cmp(except, &peer->transport))
			continue;
		if (midr_ctrl_send_datagram(g1, &peer->transport, buf,
					    sizeof(buf)))
			nodedir.tx_frames++;
	}
}

static void nodedir_send_to(struct midr_g1 *g1, const struct nodedir_entry *e,
			    const struct ipaddr *dst)
{
	uint8_t buf[MIDR_CTRL_NODE_ADV_LEN];

	if (nodedir_encode(e, buf) &&
	    midr_ctrl_send_datagram(g1, dst, buf, sizeof(buf)))
		nodedir.tx_frames++;
}

static void nodedir_report_update(const struct nodedir_entry *e)
{
	struct midr_remote_node_info info = {};

	if (!nodedir.callbacks || !nodedir.callbacks->remote_node_update)
		return;
	info.node_id = e->node_id.s_addr;
	info.group_id = e->group_id;
	info.has_transport_address = true;
	info.transport_address = e->transport;
	info.cap_flags = e->cap_flags;
	info.ls_sequence = e->sequence;
	nodedir.callbacks->remote_node_update(&info);
}

static void nodedir_report_withdraw(const struct nodedir_entry *e)
{
	if (nodedir.callbacks && nodedir.callbacks->remote_node_withdraw)
		nodedir.callbacks->remote_node_withdraw(e->node_id.s_addr,
							e->sequence);
}

/* Sequence numbers must keep growing across restarts. */
static uint64_t nodedir_next_sequence(uint64_t current)
{
	uint64_t base = (uint64_t)time(NULL) << 16;

	return base > current ? base : current + 1;
}

void midr_nodedir_local_update(struct midr_g1 *g1,
			       const struct midr_node_update *node,
			       bool withdraw)
{
	struct nodedir_entry *self = &nodedir.self;

	if (!g1 || !node || !node->node_id)
		return;
	if (withdraw && !nodedir.self_valid)
		return;
	self->node_id.s_addr = node->node_id;
	self->group_id = node->group_id;
	self->cap_flags = (uint32_t)node->cap_flags;
	if (node->has_transport_address)
		self->transport = node->transport_address;
	if (!withdraw && !midr_ipaddr_valid_locator(&self->transport))
		return;
	self->withdrawn = withdraw;
	self->sequence = nodedir_next_sequence(self->sequence);
	nodedir.self_valid = true;
	nodedir.self_withdrawn_at = withdraw ? monotime(NULL) : 0;
	MIDR_LOG("MIDR nodedir: advertise self %pI4 group %u seq %" PRIu64 "%s",
		 &self->node_id, self->group_id, self->sequence,
		 withdraw ? " (withdrawn)" : "");
	nodedir_flood(g1, self, NULL);
}

void midr_nodedir_receive(struct midr_g1 *g1, const struct ipaddr *from,
			  const uint8_t *buf, size_t len)
{
	struct nodedir_entry adv = {};
	struct nodedir_entry *e;
	struct midr_g1_peer *peer;
	uint16_t flags, lifetime;
	bool was_active;

	if (!g1 || !from || !buf)
		return;
	/* Only accept relays from our own established overlay sessions. */
	peer = midr_g1_peer_lookup(g1, from);
	if (len != MIDR_CTRL_NODE_ADV_LEN || buf[0] != MIDR_CTRL_MSG_VERSION ||
	    buf[1] != MIDR_CTRL_NODE_ADV || !peer || !peer->established) {
		nodedir.rx_invalid++;
		return;
	}
	flags = get_u16(buf + 2);
	memcpy(&adv.node_id, buf + 4, 4);
	adv.group_id = get_u32(buf + 8);
	adv.cap_flags = get_u32(buf + 12);
	adv.sequence = get_u64(buf + 36);
	lifetime = get_u16(buf + 44);
	adv.withdrawn = !!(flags & NODEDIR_FLAG_WITHDRAWN);
	if (adv.node_id.s_addr == INADDR_ANY || !adv.sequence ||
	    (!adv.withdrawn && !midr_ctrl_decode_locator(buf + 16, &adv.transport))) {
		nodedir.rx_invalid++;
		return;
	}
	if (!lifetime)
		lifetime = NODEDIR_LIFETIME_SECS;

	/* Our own advertisement came back.  Only a strictly newer copy means a
	 * previous run of this node is still remembered: jump past it and
	 * re-advertise.  Our current copy returning is normal flooding. */
	if (adv.node_id.s_addr == g1->router_id.s_addr) {
		if (nodedir.self_valid && adv.sequence > nodedir.self.sequence) {
			nodedir.self.sequence = adv.sequence;
			nodedir.self.sequence =
				nodedir_next_sequence(nodedir.self.sequence);
			nodedir_flood(g1, &nodedir.self, NULL);
		}
		return;
	}

	e = nodedir_find(adv.node_id);
	if (e && adv.sequence <= e->sequence) {
		nodedir.rx_stale++;
		return;
	}
	if (!e) {
		e = XCALLOC(MTYPE_MIDR_NODEDIR_ENTRY, sizeof(*e));
		listnode_add(nodedir.entries, e);
	}
	was_active = e->sequence && !e->withdrawn;
	*e = adv;
	if (adv.withdrawn)
		e->transport = midr_ipaddr_none();
	e->expires = monotime(NULL) +
		     (adv.withdrawn ? NODEDIR_TOMBSTONE_SECS : lifetime);
	e->learned_from = *from;
	nodedir.rx_accepted++;

	if (adv.withdrawn) {
		if (was_active)
			nodedir_report_withdraw(e);
	} else {
		nodedir_report_update(e);
	}
	nodedir_flood(g1, e, from);
}

void midr_nodedir_session_up(struct midr_g1 *g1, const struct ipaddr *remote)
{
	struct listnode *node;
	struct nodedir_entry *e;

	if (!g1 || !remote)
		return;
	if (nodedir.self_valid)
		nodedir_send_to(g1, &nodedir.self, remote);
	for (ALL_LIST_ELEMENTS_RO(nodedir.entries, node, e))
		nodedir_send_to(g1, e, remote);
}

static void nodedir_refresh_cb(struct event *event)
{
	struct midr_g1 *g1 = EVENT_ARG(event);
	struct listnode *node, *nnode;
	struct nodedir_entry *e;
	time_t now = monotime(NULL);

	event_add_timer(midr_g1_master(), nodedir_refresh_cb, g1,
			NODEDIR_REFRESH_SECS, &nodedir.t_refresh);

	if (nodedir.self_valid &&
	    (!nodedir.self.withdrawn ||
	     now - nodedir.self_withdrawn_at < NODEDIR_TOMBSTONE_SECS)) {
		nodedir.self.sequence =
			nodedir_next_sequence(nodedir.self.sequence);
		nodedir_flood(g1, &nodedir.self, NULL);
	}

	for (ALL_LIST_ELEMENTS(nodedir.entries, node, nnode, e)) {
		if (now < e->expires)
			continue;
		if (!e->withdrawn) {
			/* The node stopped refreshing: treat it as gone and keep
			 * a tombstone for the stale copies still in flight. */
			MIDR_LOG("MIDR nodedir: %pI4 expired", &e->node_id);
			e->withdrawn = true;
			e->expires = now + NODEDIR_TOMBSTONE_SECS;
			nodedir_report_withdraw(e);
			continue;
		}
		list_delete_node(nodedir.entries, node);
		XFREE(MTYPE_MIDR_NODEDIR_ENTRY, e);
	}
}

int midr_remote_view_callbacks_register(
	struct midr_context *ctx,
	const struct midr_remote_view_callbacks *callbacks)
{
	struct listnode *node;
	struct nodedir_entry *e;

	if (!callbacks)
		return -EINVAL;
	nodedir.callbacks = callbacks;
	/* Replay what is already known, as group 2's registration did. */
	for (ALL_LIST_ELEMENTS_RO(nodedir.entries, node, e))
		if (!e->withdrawn)
			nodedir_report_update(e);
	return 0;
}

int midr_remote_view_snapshot_get(struct midr_context *ctx,
				  struct midr_remote_view_snapshot *snapshot)
{
	struct midr_remote_node_info *nodes;
	struct listnode *node;
	struct nodedir_entry *e;
	size_t count = 0;

	if (!snapshot)
		return -EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	nodes = XCALLOC(MTYPE_MIDR_NODEDIR_SNAPSHOT,
			(listcount(nodedir.entries) + 1) * sizeof(*nodes));
	for (ALL_LIST_ELEMENTS_RO(nodedir.entries, node, e)) {
		if (e->withdrawn)
			continue;
		nodes[count].node_id = e->node_id.s_addr;
		nodes[count].group_id = e->group_id;
		nodes[count].has_transport_address = true;
		nodes[count].transport_address = e->transport;
		nodes[count].cap_flags = e->cap_flags;
		nodes[count].ls_sequence = e->sequence;
		count++;
	}
	snapshot->nodes = nodes;
	snapshot->node_count = count;
	snapshot->snapshot_version = nodedir.rx_accepted;
	return 0;
}

void midr_remote_view_snapshot_release(
	struct midr_context *ctx, struct midr_remote_view_snapshot *snapshot)
{
	void *nodes;

	if (!snapshot)
		return;
	nodes = (void *)snapshot->nodes;
	XFREE(MTYPE_MIDR_NODEDIR_SNAPSHOT, nodes);
	memset(snapshot, 0, sizeof(*snapshot));
}

void midr_nodedir_show(struct midr_g1 *g1, struct vty *vty)
{
	struct listnode *node;
	struct nodedir_entry *e;
	time_t now = monotime(NULL);

	vty_out(vty,
		"Node directory: rx accepted %" PRIu64 ", stale %" PRIu64
		", invalid %" PRIu64 ", tx %" PRIu64 "\n",
		nodedir.rx_accepted, nodedir.rx_stale, nodedir.rx_invalid,
		nodedir.tx_frames);
	if (nodedir.self_valid)
		vty_out(vty, "  self %pI4 group %u caps 0x%x locator %pIA seq %" PRIu64 "%s\n",
			&nodedir.self.node_id, nodedir.self.group_id,
			nodedir.self.cap_flags, &nodedir.self.transport,
			nodedir.self.sequence,
			nodedir.self.withdrawn ? " withdrawn" : "");
	else
		vty_out(vty, "  self not advertised\n");
	vty_out(vty, "  %-15s %-8s %-6s %-39s %-8s %s\n", "Node", "Group",
		"Caps", "Locator", "Expires", "From");
	for (ALL_LIST_ELEMENTS_RO(nodedir.entries, node, e))
		vty_out(vty, "  %-15pI4 %-8u 0x%-4x %-39pIA %-8lld %pIA%s\n",
			&e->node_id, e->group_id, e->cap_flags, &e->transport,
			(long long)(e->expires - now), &e->learned_from,
			e->withdrawn ? " withdrawn" : "");
}

DEFUN(show_midr_directory, show_midr_directory_cmd, "show midr directory",
      SHOW_STR "MIDR information\n" "Show the group-1 node directory\n")
{
	struct midr_g1 *g1 = midr_g1_get();

	if (!g1)
		return CMD_SUCCESS;
	midr_nodedir_show(g1, vty);
	return CMD_SUCCESS;
}

void midr_nodedir_init(struct midr_g1 *g1)
{
	nodedir.entries = list_new();
	install_element(VIEW_NODE, &show_midr_directory_cmd);
	event_add_timer(midr_g1_master(), nodedir_refresh_cb, g1,
			NODEDIR_REFRESH_SECS, &nodedir.t_refresh);
}

void midr_nodedir_finish(struct midr_g1 *g1)
{
	struct nodedir_entry *e;

	event_cancel(&nodedir.t_refresh);
	nodedir.callbacks = NULL;
	if (!nodedir.entries)
		return;
	while ((e = listnode_head(nodedir.entries))) {
		listnode_delete(nodedir.entries, e);
		XFREE(MTYPE_MIDR_NODEDIR_ENTRY, e);
	}
	list_delete(&nodedir.entries);
}
