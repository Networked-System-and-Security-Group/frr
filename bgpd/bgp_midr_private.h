// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Private MIDR declarations used by bgpd, VTY, and unit tests.
 */

#ifndef _FRR_BGP_MIDR_PRIVATE_H
#define _FRR_BGP_MIDR_PRIVATE_H

#include "bgpd/bgp_midr.h"
struct bgp;
struct bgp_midr;
struct midr_ted_store;
struct vty;

struct midr_context {
	struct bgp *bgp;
	struct bgp_midr *midr;
	struct midr_ted_store *ted_store;
};

extern int midr_validate_node_update(uint32_t local_node_id, const struct midr_node_update *node);
extern int midr_validate_node_withdraw(uint32_t local_node_id, uint32_t node_id);
extern int midr_validate_link_update(uint32_t local_node_id, const struct midr_link_update *link);
extern int midr_validate_link_withdraw(uint32_t local_node_id, const struct midr_link_key *key);

extern void bgp_midr_init(struct bgp *bgp);
extern void bgp_midr_finish(struct bgp *bgp);

extern void midr_topology_process_pending(struct midr_context *ctx);
extern void midr_show_topology_nodes(struct vty *vty, struct midr_context *ctx);
extern void midr_show_topology_links(struct vty *vty, struct midr_context *ctx);
extern void midr_show_topology_tombstones(struct vty *vty, struct midr_context *ctx);
extern void midr_show_events(struct vty *vty, struct midr_context *ctx);

#endif /* _FRR_BGP_MIDR_PRIVATE_H */
