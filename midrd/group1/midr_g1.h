// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR group 1 (neighbor discovery, measurement, clustering) inside midrd.
 *
 * The group-1 modules were written against bgpd, where every function took
 * the BGP instance and BGP peers carried the overlay sessions.  In midrd the
 * instance is struct midr_g1 and an overlay session is a native MIDR
 * transport session requested through midrd/midr-session.h; struct
 * midr_g1_peer is group 1's view of one such session.
 */
#ifndef _MIDR_G1_H
#define _MIDR_G1_H

#include <zebra.h>

#include "asn.h"
#include "frrevent.h"
#include "hook.h"
#include "ipaddr.h"
#include "linklist.h"
#include "memory.h"
#include "prefix.h"
#include "vrf.h"

#include "midrd/midr-context.h"
#include "midrd/midr-session.h"
#include "midrd/midr-topology.h"

DECLARE_MGROUP(MIDR_G1);
DECLARE_MTYPE(MIDR_G1);

struct midr_nds;
struct vty;

/* One overlay session as group 1 sees it. */
struct midr_g1_peer {
	struct midr_g1 *g1;
	struct ipaddr transport;
	/* Identity of the remote node; 0 until known (manual sessions). */
	struct in_addr remote_id;
	as_t as;
	/* The session is currently requested from midrd.  Admission may hold a
	 * session back, which leaves the peer known but not requested. */
	bool requested;
	bool established;
	/* Admission permit the current attempt was started with. */
	uint64_t admission_permit;
	time_t uptime;
	uint32_t established_count;
	uint32_t dropped_count;
	int last_reset;
};

struct midr_g1 {
	struct midr_context *ctx;
	struct midr_nds *midr_nds_info;
	/* Local identity, fixed by the midrd command line. */
	struct in_addr router_id;
	/* Informational only: carried in control messages for display. */
	as_t as;
	const char *name_pretty;
	vrf_id_t vrf_id;
	/* struct midr_g1_peer * */
	struct list *peer;
};

/* Debug switches ("debug midr [discovery]"). */
extern unsigned long midr_g1_debug;
#define MIDR_G1_DEBUG_GENERAL 0x01
#define MIDR_G1_DEBUG_DISCOVERY 0x02
#define MIDR_G1_DEBUG_ON(flag) (midr_g1_debug & MIDR_G1_DEBUG_##flag)
/* Discovery flow logs follow either switch. */
#define MIDR_G1_DEBUG_FLOW                                                     \
	(MIDR_G1_DEBUG_ON(GENERAL) || MIDR_G1_DEBUG_ON(DISCOVERY))

#define MIDR_G1_LOG(...)                                                       \
	do {                                                                   \
		if (MIDR_G1_DEBUG_ON(GENERAL))                                 \
			zlog_debug(__VA_ARGS__);                               \
	} while (0)

/* Configuration commands act on the single group-1 instance. */
#define MIDR_G1_DECLVAR(vty, g1)                                               \
	struct midr_g1 *g1 = midr_g1_get();                                    \
	if (!g1) {                                                             \
		vty_out(vty, "%% MIDR group 1 is not running\n");               \
		return CMD_WARNING_CONFIG_FAILED;                              \
	}

struct event_loop *midr_g1_master(void);
struct midr_g1 *midr_g1_get(void);
/* True while a configuration is being applied and shortly after it ended. */
bool midr_g1_config_inprocess(void);

DECLARE_HOOK(midr_g1_config_end, (struct midr_g1 *g1), (g1));

/* Overlay sessions. */
struct midr_g1_peer *midr_g1_peer_lookup(struct midr_g1 *g1,
					 const struct ipaddr *transport);
struct midr_g1_peer *midr_g1_peer_lookup_rid(struct midr_g1 *g1,
					     struct in_addr remote_id);
/* Create the session state and request it unless admission holds it. */
struct midr_g1_peer *midr_g1_peer_create(struct midr_g1 *g1,
					 const struct ipaddr *transport,
					 struct in_addr remote_id, as_t as);
void midr_g1_peer_delete(struct midr_g1_peer *peer);
/* Ask midrd to (re)establish a session admission had held back. */
void midr_g1_peer_start(struct midr_g1_peer *peer);
/* Drop an attempt that is not established yet; the state is kept. */
void midr_g1_peer_stop(struct midr_g1_peer *peer);
const char *midr_g1_peer_state_str(const struct midr_g1_peer *peer);

/* ------------------------------------------------------------------------
 * Remote node directory.
 *
 * bgpd's group 2 flooded each node's group, locator and role bits and
 * delivered them through these callbacks.  midrd's Membership object only
 * carries the group, so group 1 distributes this directory itself over its
 * control channel (midr_g1_nodedir.c) and keeps the callback contract.
 * ---------------------------------------------------------------------- */
struct midr_remote_node_info {
	uint32_t node_id;
	uint32_t group_id;
	bool has_transport_address;
	struct ipaddr transport_address;
	uint64_t cap_flags;
	uint64_t policy_tags;
	uint64_t ls_sequence;
};

struct midr_remote_view_snapshot {
	const struct midr_remote_node_info *nodes;
	size_t node_count;
	uint64_t snapshot_version;
};

struct midr_remote_view_callbacks {
	void (*remote_node_update)(const struct midr_remote_node_info *node);
	void (*remote_node_withdraw)(uint32_t node_id, uint64_t ls_sequence);
};

int midr_remote_view_callbacks_register(
	struct midr_context *ctx,
	const struct midr_remote_view_callbacks *callbacks);
int midr_remote_view_snapshot_get(struct midr_context *ctx,
				  struct midr_remote_view_snapshot *snapshot);
void midr_remote_view_snapshot_release(
	struct midr_context *ctx, struct midr_remote_view_snapshot *snapshot);

/* Local node advertisement: called whenever the local Node fact changes. */
void midr_nodedir_local_update(struct midr_g1 *g1,
			       const struct midr_node_update *node,
			       bool withdraw);
/* Control-channel entry for a received directory frame. */
void midr_nodedir_receive(struct midr_g1 *g1, const struct ipaddr *from,
			  const uint8_t *buf, size_t len);
void midr_nodedir_session_up(struct midr_g1 *g1, const struct ipaddr *remote);
void midr_nodedir_init(struct midr_g1 *g1);
void midr_nodedir_finish(struct midr_g1 *g1);
void midr_nodedir_show(struct midr_g1 *g1, struct vty *vty);

/* midrd entry points (weak in midrd.c). */
void midr_group1_init(struct event_loop *master, struct midr_context *ctx);
void midr_group1_terminate(void);

#endif /* _MIDR_G1_H */
