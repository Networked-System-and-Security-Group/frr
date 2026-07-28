// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR node liveness confirmation and failure gossip.
 *
 * The existing BGP-LS Node NLRI refresh remains the keepalive transport.
 * This module adds the receive-side SUSPECT/confirmation state machine and
 * small UDP control messages used for indirect confirmation and failure
 * gossip.  Node-table ownership remains in bgp_midr.c (NDS).
 */

#ifndef _FRR_BGP_MIDR_LIVENESS_H
#define _FRR_BGP_MIDR_LIVENESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

struct bgp;
struct midr_node_entry;
struct prefix;
struct vty;

#define MIDR_LIVENESS_DEFAULT_VOTER_SAMPLE_SIZE 3
#define MIDR_LIVENESS_DEFAULT_QUORUM           2
#define MIDR_LIVENESS_DEFAULT_CONFIRM_TIMEOUT  3
#define MIDR_LIVENESS_DEFAULT_RETRY_BACKOFF    5
#define MIDR_LIVENESS_DEFAULT_HOP_LIMIT        16
#define MIDR_LIVENESS_DEFAULT_CACHE_TTL        60

#define MIDR_LIVENESS_MAX_VOTERS   16
#define MIDR_LIVENESS_MAX_WIRE_SIZE 128

/* Values 1-5 are owned by enum midr_ctrl_msg_type. */
enum midr_liveness_msg_type {
	MIDR_LIVENESS_PROBE_REQ = 6,
	MIDR_LIVENESS_PROBE_RESP = 7,
	MIDR_LIVENESS_DEAD = 8,
	MIDR_LIVENESS_GRACEFUL_LEAVE = 9,
};

enum midr_liveness_probe_result {
	MIDR_LIVENESS_RESULT_UNKNOWN = 0,
	MIDR_LIVENESS_RESULT_ALIVE = 1,
	MIDR_LIVENESS_RESULT_STALE = 2,
};

struct midr_liveness_config {
	uint32_t keepalive_interval;
	uint32_t suspect_timeout;
	uint32_t scan_interval;
	uint32_t voter_sample_size;
	uint32_t quorum;
	uint32_t confirm_timeout;
	uint32_t retry_backoff;
	uint32_t hop_limit;
	uint32_t cache_ttl;
};

/* Module lifecycle; initialized before and finished after shared UDP ingress. */
extern void midr_liveness_init(struct bgp *bgp);
extern void midr_liveness_finish(struct bgp *bgp);
extern void midr_liveness_schedule_self_advertisement(struct bgp *bgp);

/* Evidence/state hooks called by NDS.  on_alive is reserved for a real
 * Node-NLRI/self refresh; indirect confirmation is handled internally and
 * never refreshes the transferable last_seen timestamp.
 */
extern void midr_liveness_on_alive(struct bgp *bgp,
				   struct midr_node_entry *entry);
extern void midr_liveness_on_withdraw(struct bgp *bgp,
				      const struct prefix *node_id);
extern void midr_liveness_on_node_removed(struct bgp *bgp,
					  const struct prefix *node_id);

/* Emit the explicit, trusted graceful-leave rumor before MP_UNREACH. */
extern void midr_liveness_publish_leave(struct bgp *bgp);

/* Dispatch liveness datagrams received on the shared MIDR UDP socket. */
extern bool midr_liveness_handle_ctrl(struct bgp *bgp, const uint8_t *buf,
				      size_t len,
				      const struct sockaddr_in *source);

/* Common soft-quarantine predicates for node-table consumers. */
extern bool
midr_liveness_node_usable(const struct midr_node_entry *entry);
extern bool midr_liveness_transport_usable(struct bgp *bgp,
					   struct in_addr transport);
extern bool midr_liveness_endpoint_usable(struct bgp *bgp,
					  const struct prefix *endpoint);
extern bool midr_liveness_indirect_discovery_allowed(
	struct bgp *bgp, const struct prefix *node_id);
extern bool midr_liveness_indirect_endpoint_usable(
	struct bgp *bgp, const struct prefix *node_id,
	const struct in_addr *transport);
extern const char *
midr_liveness_state_name(const struct midr_node_entry *entry);

/* Runtime configuration and timer accessors. */
extern bool midr_liveness_get_config(const struct bgp *bgp,
				     struct midr_liveness_config *config);
extern void
midr_liveness_config_defaults(struct midr_liveness_config *config);
extern bool midr_liveness_set_config(struct bgp *bgp,
				     const struct midr_liveness_config *config);

/* VTY helpers; config writer matches bgp_inst_config_write hook signature. */
extern void midr_liveness_show(struct vty *vty, struct bgp *bgp);
extern int midr_liveness_config_write(struct bgp *bgp, struct vty *vty);

#endif /* _FRR_BGP_MIDR_LIVENESS_H */
