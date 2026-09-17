/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_CONTEXT_PRIVATE_H
#define MIDRD_CONTEXT_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frrevent.h"
#include "midr-context.h"
#include "midr-consumer.h"
#include "midr-core.h"
#include "midr-cost.h"
#include "midr-engine.h"
#include "midr-local-ipc.h"
#include "midr-local-provider.h"
#include "midr-owned.h"
#include "midr-prefix-ipc.h"
#include "midr-prefix-provider.h"
#include "midr-session-private.h"
#include "midr-spf.h"
#include "midr-ted.h"
#include "midr-topology.h"
#include "midr-transport.h"

struct midr_spf_install_runtime;
struct midr_zebra_backend_ops;

#define MIDRD_MAX_PEERS 32U
#define MIDRD_MAX_LINKS 64U
#define MIDRD_MAX_SNAPSHOT 4096U
#define MIDRD_MAX_FRAME 4096U
#define MIDRD_DEFAULT_LIFETIME 6000U
#define MIDRD_DEFAULT_HELLO 1000U
#define MIDRD_DEFAULT_TAKEOVER_DELAY 3000U
#define MIDRD_FORWARD_BUDGET_MS 1000U
#define MIDRD_TED_RETRY_MS 1000U
#define MIDRD_SHUTDOWN_WAIT_MS 1000U
#define MIDRD_POLL_INTERVAL_MS 10U

struct midrd_peer_config {
	struct midr_transport_endpoint endpoint;
};

/* Development-time static Link input. */
struct midrd_link_config {
	uint32_t remote;
	uint32_t local_ifindex;
	uint32_t metric;
	uint32_t candidate_metric;
	uint64_t link_id;
	uint8_t address_family;
	uint8_t local_address[MIDR_CORE_ADDR_BYTES];
	uint8_t remote_address[MIDR_CORE_ADDR_BYTES];
	struct midr_cost_metrics latest_metrics;
	uint64_t input_version;
	uint64_t measurement_sequence;
	uint64_t measurement_timestamp_ms;
	uint64_t last_cost_advertised_ms;
	bool cost_pending;
};

struct midrd_snapshot_stage {
	struct midr_transport_endpoint peer;
	struct midr_core_object *objects;
	uint64_t *received_ns;
	struct midr_core_object *updates;
	uint64_t *update_received_ns;
	size_t count;
	size_t update_count;
	size_t capacity;
	bool active;
	bool ended;
	uint64_t generation;
};

struct midrd_prefix_stage {
	struct midr_prefix prefixes[MIDRD_MAX_SNAPSHOT];
	size_t count;
	uint64_t generation;
	uint32_t originator;
	bool active;
	bool ended;
	bool discard;
};

struct midrd_local_link_version {
	uint32_t remote;
	uint64_t link_id;
	uint64_t version;
};

struct midrd_local_stage {
	struct midr_local_membership membership;
	struct midr_local_link links[MIDRD_MAX_LINKS];
	struct midrd_local_link_version link_versions[MIDRD_MAX_SNAPSHOT];
	size_t link_count;
	size_t link_version_count;
	uint64_t membership_floor;
	uint64_t generation;
	uint32_t originator;
	bool membership_present;
	bool active;
	bool ended;
	bool discard;
};

struct midr_context {
	struct event_loop *master;
	struct event *poll_event;
	uint32_t node_id;
	uint32_t group_id;
	uint32_t lifetime_ms;
	uint32_t hello_ms;
	uint32_t hold_time_ms;
	uint32_t takeover_delay_ms;
	struct midr_engine *engine;
	struct midr_owned *owned;
	struct midr_consumer *consumer;
	struct midr_ted *ted;
	struct midr_prefix_provider *prefix_provider;
	struct midr_prefix_ipc *prefix_ipc;
	struct midr_local_ipc *local_ipc;
	struct midr_session_manager *sessions;
	struct midr_spf_consumer *spf_consumers;
	struct midr_spf_install_runtime *spf_install;
	const struct midr_zebra_backend_ops *zebra_ops;
	void *zebra_arg;
	struct midrd_peer_config static_peers[MIDRD_MAX_PEERS];
	struct midrd_link_config links[MIDRD_MAX_LINKS];
	struct midr_core_identity group_prefixes[MIDRD_MAX_SNAPSHOT];
	struct midr_prefix ipc_prefixes[MIDRD_MAX_SNAPSHOT];
	struct midrd_prefix_stage prefix_stage;
	struct midrd_local_stage local_stage;
	struct midrd_local_link_version local_link_versions[MIDRD_MAX_SNAPSHOT];
	struct midrd_snapshot_stage stages[MIDRD_MAX_PEERS];
	size_t static_peer_count;
	size_t link_count;
	size_t group_prefix_count;
	size_t ipc_prefix_count;
	size_t local_link_version_count;
	uint64_t prefix_generation;
	uint64_t local_generation;
	midr_topology_snapshot_get_cb topology_snapshot_get;
	midr_topology_snapshot_release_cb topology_snapshot_release;
	uint64_t membership_version;
	uint32_t representative_group;
	uint32_t representative_node;
	uint64_t takeover_ready_at;
	bool representative_committed;
	bool group_reconcile_pending;
	bool topology_resync_required;
	bool topology_write_active;
	bool owned_commit_active;
	uint64_t owned_commit_now_ms;
	struct midr_core_identity local_identity;
	bool have_local_identity;
	const char *sequence_file;
	uint64_t next_refresh;
	uint64_t next_expire;
	uint64_t next_ted_retry;
	uint64_t last_spf_generation;
	uint64_t stop_at;
	bool ted_rebuild_pending;
	/* Preserve the previous TED view until an inbound batch commits. */
	bool sync_derivation_wait;
	uint64_t shutdown_generation_failures;
	bool shutdown_active;
	uint64_t shutdown_write_failures;
	bool terminating;
};

#endif /* MIDRD_CONTEXT_PRIVATE_H */
