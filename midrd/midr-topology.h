/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_TOPOLOGY_H
#define MIDRD_TOPOLOGY_H

#include <zebra.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "if.h"
#include "ipaddr.h"
#include "midr-context.h"

enum midr_policy_state {
	MIDR_POLICY_ALLOWED = 0,
	MIDR_POLICY_BLOCKED,
};

enum midr_topology_resync_reason {
	MIDR_TOPOLOGY_RESYNC_VERSION_LOST = 0,
	MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART,
	MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT,
};

struct midr_node_update {
	uint32_t node_id;
	uint32_t group_id;
	bool has_transport_address;
	struct ipaddr transport_address;
	uint64_t cap_flags;
	enum midr_policy_state policy_state;
	uint64_t policy_tags;
	uint64_t version;
};

struct midr_link_key {
	uint32_t local_node_id;
	uint32_t remote_node_id;
	uint64_t link_id;
};

struct midr_link_metrics {
	bool has_rtt_us;
	uint32_t rtt_us;
	bool has_loss_ppm;
	uint32_t loss_ppm;
	bool has_available_bandwidth_kbps;
	uint32_t available_bandwidth_kbps;
	uint64_t measurement_seqno;
	uint64_t measurement_timestamp_ms;
};

struct midr_link_update {
	struct midr_link_key key;
	ifindex_t local_ifindex;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	struct midr_link_metrics metrics;
	enum midr_policy_state policy_state;
	uint64_t policy_tags;
	uint64_t version;
};

struct midr_topology_snapshot {
	const struct midr_node_update *nodes;
	size_t node_count;
	const struct midr_link_update *links;
	size_t link_count;
	uint64_t snapshot_version;
};

typedef int (*midr_topology_snapshot_get_cb)(
	struct midr_context *ctx, struct midr_topology_snapshot *snapshot);
typedef void (*midr_topology_snapshot_release_cb)(
	struct midr_context *ctx, struct midr_topology_snapshot *snapshot);

int midr_topology_node_upsert(struct midr_context *ctx,
			      const struct midr_node_update *node);
int midr_topology_node_withdraw(struct midr_context *ctx, uint32_t node_id,
				uint64_t version);
int midr_topology_link_upsert(struct midr_context *ctx,
			      const struct midr_link_update *link);
int midr_topology_link_withdraw(struct midr_context *ctx,
				const struct midr_link_key *key,
				uint64_t version);

/* A provider registers its existing snapshot callbacks once.  Resync pulls
 * one complete snapshot and commits it atomically before returning success.
 * A failed get transfers no snapshot ownership; release follows a successful
 * get even when validation or commit subsequently fails. */
int midr_topology_provider_register(
	struct midr_context *ctx, midr_topology_snapshot_get_cb snapshot_get,
	midr_topology_snapshot_release_cb snapshot_release);
void midr_topology_provider_unregister(struct midr_context *ctx);
int midr_topology_resync_begin(struct midr_context *ctx,
			       enum midr_topology_resync_reason reason);
int midr_topology_snapshot_apply(struct midr_context *ctx,
				 const struct midr_topology_snapshot *snapshot);

#endif /* MIDRD_TOPOLOGY_H */
