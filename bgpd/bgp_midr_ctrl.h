// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR Control Layer
 *
 * Bridges the MIDR node table (bgp_midr.c / NDS) and the BGP peer FSM.
 * Evaluates peering policy and drives peer_remote_as() / peer_delete()
 * based on node table events.
 */

#ifndef _FRR_BGP_MIDR_CTRL_H
#define _FRR_BGP_MIDR_CTRL_H

#include <stdint.h>
#include <netinet/in.h>

struct bgp;
struct midr_node_entry;

/* ===========================================================================
 * Peer-request UDP control channel
 *
 * A new node, after picking its group, unicasts a PEER_REQUEST to each group
 * member's transport address; the member then peers back (bidirectional).
 * This channel is INDEPENDENT of the PM probe channel (own socket / port /
 * format) so PM's design can change without affecting it.
 * =========================================================================*/

#define MIDR_CTRL_UDP_PORT    5859 /* control channel only (distinct from PM) */
#define MIDR_CTRL_MSG_VERSION 1

/* Max list entries in one (single-datagram) response; TODO: fragment beyond. */
#define MIDR_CTRL_LIST_MAX 80

enum midr_ctrl_msg_type {
	MIDR_CTRL_PEER_REQUEST = 1,	  /* "please peer back with me" */
	MIDR_CTRL_REP_LIST_REQ = 2,	  /* new node -> bootstrap: send rep list */
	MIDR_CTRL_REP_LIST_RESP = 3,	  /* bootstrap -> new node: rep directory */
	MIDR_CTRL_MEMBER_LIST_REQ = 4,	  /* new node -> rep: send group members */
	MIDR_CTRL_MEMBER_LIST_RESP = 5,	  /* rep -> new node: member list (table A) */
	MIDR_CTRL_ANNOUNCE = 6,	  /* new node -> each candidate member: "this is
					   * who I am" so it can validate my PM probes.
					   * One-way, no reply expected. */
};

/*
 * Request frame, fixed 20 bytes, network byte order.  Shared by PEER_REQUEST,
 * REP_LIST_REQ, MEMBER_LIST_REQ and ANNOUNCE: all carry the requester's
 * identity so the responder can reply / peer back / recognise it without a
 * node-table lookup.
 *   - PEER_REQUEST     : target_group = the group the requester joined
 *   - REP_LIST_REQ     : target_group = 0 (ignored)
 *   - MEMBER_LIST_REQ  : target_group = the group whose members are wanted
 *   - ANNOUNCE         : target_group = 0 (ignored)
 */
struct midr_ctrl_msg {
	uint8_t version;
	uint8_t type;
	uint16_t reserved;
	struct in_addr requester_rid;	    /* router-id (identity) */
	struct in_addr requester_transport; /* reachable locator to reply / peer back */
	uint32_t requester_asn;
	uint32_t target_group;
};

/* Variable-length response header, followed by `count` list items. 4 bytes. */
struct midr_ctrl_list_hdr {
	uint8_t version;
	uint8_t type;
	uint16_t count; /* number of items that follow (network order) */
};

/* REP_LIST_RESP item, 12 bytes, network byte order. */
struct midr_ctrl_rep_item {
	uint32_t group_id;
	struct in_addr rep_transport;
	uint32_t rep_asn;
};

/* MEMBER_LIST_RESP item, 16 bytes, network byte order. */
struct midr_ctrl_member_item {
	struct in_addr rid;
	struct in_addr transport;
	uint32_t asn;
	uint32_t group_id;
};

/* Open / close the control-channel UDP socket (called from bgp_midr_init/finish). */
extern void midr_ctrl_init(struct bgp *bgp);
extern void midr_ctrl_finish(struct bgp *bgp);

/*
 * Called by bgp_midr.c (NDS) to initiate a (multi-hop eBGP + BGP-LS) session to
 * a node, after NDS has decided to peer.  Dedups against an existing peer and
 * sends a reverse PEER_REQUEST so the far end peers back.
 */
extern void midr_ctrl_connect(struct bgp *bgp,
			      const struct midr_node_entry *entry);

/*
 * Called by bgp_midr.c (NDS) before a node entry is removed (withdraw or
 * expiry). Tears down the dynamically-created BGP session if one exists.
 */
extern void midr_ctrl_on_node_remove(struct bgp *bgp,
				     struct midr_node_entry *entry);

/*
 * Initiate BGP sessions to every non-self node in the given group (used by
 * the new-node join orchestration after a JOIN decision).  Returns the
 * number of members a session was initiated to.
 */
extern int midr_ctrl_connect_group(struct bgp *bgp, uint32_t group_id);

/*
 * Hierarchical-discovery requests over the UDP control channel (new node side):
 *   - send a REP_LIST_REQ to the bootstrap's transport address, then
 *   - send a MEMBER_LIST_REQ to the chosen group representative.
 * Both enqueue a retransmit until the matching response arrives.
 */
extern void midr_ctrl_send_rep_request(struct bgp *bgp,
				       struct in_addr bootstrap_transport);
extern void midr_ctrl_send_member_request(struct bgp *bgp,
					  struct in_addr rep_transport,
					  uint32_t group_id);

/*
 * New node -> an arbitrary candidate's transport address: one-way
 * MIDR_CTRL_ANNOUNCE self-identification, no retransmit/response tracking.
 * Lets the receiver recognise our subsequent PM probes as coming from a
 * known source even though it never sent us a REP_LIST_REQ/MEMBER_LIST_REQ
 * itself (e.g. a non-bootstrap rep in the representative directory).
 */
extern void midr_ctrl_send_announce(struct bgp *bgp, struct in_addr dst);

/*
 * Mark a qualifying connection into the topology graph.  Skeleton stub: the
 * real topology-graph bookkeeping lands in a later phase.
 */
extern void midr_mark_topology(struct bgp *bgp,
			       const struct midr_node_entry *entry);

#endif /* _FRR_BGP_MIDR_CTRL_H */
