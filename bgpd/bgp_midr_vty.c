// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR VTY commands. */

#include <zebra.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "command.h"
#include "sockunion.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_vty.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_owned.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted_private.h"
#include "bgpd/bgp_midr_vty.h"

static int midr_parse_router_id(const char *str, uint32_t *out)
{
	struct in_addr addr;

	if (!str || !out || inet_pton(AF_INET, str, &addr) != 1)
		return -EINVAL;

	*out = addr.s_addr;
	return 0;
}

static int midr_parse_u64(const char *str, uint64_t *out)
{
	const unsigned char *cursor;
	unsigned long long value;
	char *end = NULL;

	if (!str || !str[0] || !out)
		return -EINVAL;
	for (cursor = (const unsigned char *)str; *cursor; cursor++)
		if (!isdigit(*cursor))
			return -EINVAL;

	errno = 0;
	value = strtoull(str, &end, 10);
	if (errno == ERANGE || !end || *end || value > UINT64_MAX)
		return -EINVAL;

	*out = (uint64_t)value;
	return 0;
}

static int midr_parse_u32(const char *str, uint32_t *out)
{
	uint64_t value;

	if (midr_parse_u64(str, &value) != 0 || value > UINT32_MAX)
		return -EINVAL;

	*out = (uint32_t)value;
	return 0;
}

static int midr_parse_ifindex(const char *str, ifindex_t *out)
{
	uint64_t value;

	if (midr_parse_u64(str, &value) != 0 || value > INT_MAX)
		return -EINVAL;

	*out = (ifindex_t)value;
	return 0;
}

static struct midr_context *midr_vty_context(struct vty *vty)
{
	struct midr_context *ctx = midr_context_get_default();

	if (!ctx)
		vty_out(vty, "%% MIDR is not initialized\n");

	return ctx;
}

DEFUN(show_midr_owned, show_midr_owned_cmd,
      "show midr owned",
      SHOW_STR
      "MIDR information\n"
      "Locally originated objects\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;
	midr_show_owned(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_lsdb_summary, show_midr_lsdb_summary_cmd,
      "show midr lsdb summary",
      SHOW_STR
      "MIDR information\n"
      "Selected-object database\n"
      "LSDB readiness and object counts\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;
	midr_show_lsdb(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_rib_summary, show_midr_rib_summary_cmd,
      "show midr rib summary",
      SHOW_STR
      "MIDR information\n"
      "MIDR SAFI RIB\n"
      "RIB identity and path counts\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_rib_summary summary;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	ret = midr_rib_summary_get(ctx, &summary);
	if (ret) {
		vty_out(vty, "%% MIDR RIB summary failed: %d\n", ret);
		return CMD_WARNING;
	}
	vty_out(vty, "MIDR RIB summary:\n");
	vty_out(vty, "  identities:        %zu/%zu\n",
		summary.identity_count, summary.identity_limit);
	vty_out(vty, "  paths:             %zu\n", summary.path_count);
	vty_out(vty, "  selected:          %zu\n",
		summary.selected_count);
	vty_out(vty, "  conflicts:         %zu\n",
		summary.conflict_count);
	vty_out(vty, "  rejected limit:    %" PRIu64 "\n",
		summary.rejected_limit);
	vty_out(vty, "  payload conflicts: %" PRIu64 "\n",
		summary.rejected_payload_conflict);
	return CMD_SUCCESS;
}

static void midr_vty_show_sync_reasons(struct vty *vty, uint64_t reasons)
{
	bool separator = false;

	if (!reasons) {
		vty_out(vty, "none");
		return;
	}
	if (reasons & MIDR_TED_SYNC_REASON_EOR_TIMEOUT) {
		vty_out(vty, "EOR_TIMEOUT");
		separator = true;
	}
	if (reasons & MIDR_TED_SYNC_REASON_RESYNC_FAILED)
		vty_out(vty, "%sRESYNC_FAILED", separator ? "," : "");
}

DEFUN(show_midr_ted_summary, show_midr_ted_summary_cmd,
      "show midr ted summary",
      SHOW_STR
      "MIDR information\n"
      "Path-computation TED\n"
      "TED readiness and object counts\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	const struct midr_ted_snapshot *snapshot = NULL;
	struct midr_ted_status status;
	struct in_addr local_node;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	ret = midr_ted_status_get(ctx, &status);
	if (ret) {
		vty_out(vty, "%% MIDR TED status failed: %d\n", ret);
		return CMD_WARNING;
	}

	vty_out(vty, "MIDR TED summary:\n");
	vty_out(vty, "  state:                 %s\n", status.ready ? "READY" : "NOT_READY");
	vty_out(vty, "  generation:            %" PRIu64 "\n", status.generation);
	vty_out(vty, "  sync reasons:          ");
	midr_vty_show_sync_reasons(vty, status.sync_reason_flags);
	vty_out(vty, "\n");
	vty_out(vty, "  consumers:             %zu\n", status.consumer_count);
	vty_out(vty, "  pending links:         %zu\n", status.pending_link_count);
	vty_out(vty, "  pending node-prefixes: %zu\n", status.pending_node_prefix_count);
	vty_out(vty, "  pending prefix-groups: %zu\n", status.pending_prefix_group_count);

	if (!status.ready)
		return CMD_SUCCESS;
	ret = midr_ted_snapshot_get(ctx, &snapshot);
	if (ret) {
		vty_out(vty, "%% MIDR TED snapshot failed: %d\n", ret);
		return CMD_WARNING;
	}

	local_node.s_addr = snapshot->local_node_id;
	vty_out(vty, "  local node:            %pI4\n", &local_node);
	vty_out(vty, "  local group:           %u\n", snapshot->local_group_id);
	vty_out(vty, "  nodes:                 %zu\n", snapshot->node_count);
	vty_out(vty, "  intra links:           %zu\n", snapshot->intra_link_count);
	vty_out(vty, "  egress links:          %zu\n", snapshot->egress_link_count);
	vty_out(vty, "  node-prefixes:         %zu\n", snapshot->node_prefix_count);
	vty_out(vty, "  group edges:           %zu\n", snapshot->group_edge_count);
	vty_out(vty, "  prefix-groups:         %zu\n", snapshot->prefix_group_count);
	midr_ted_snapshot_release(&snapshot);
	return CMD_SUCCESS;
}

DEFUN(show_midr_ted_generation, show_midr_ted_generation_cmd,
      "show midr ted generation",
      SHOW_STR
      "MIDR information\n"
      "Path-computation TED\n"
      "Current immutable snapshot generation\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_ted_status status;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	ret = midr_ted_status_get(ctx, &status);
	if (ret) {
		vty_out(vty, "%% MIDR TED status failed: %d\n", ret);
		return CMD_WARNING;
	}

	vty_out(vty, "MIDR TED generation: %" PRIu64 " (%s)\n", status.generation,
		status.ready ? "READY" : "NOT_READY");
	return CMD_SUCCESS;
}

DEFUN(show_midr_sync, show_midr_sync_cmd,
      "show midr sync",
      SHOW_STR
      "MIDR information\n"
      "Peer End-of-RIB synchronization\n")
{
	static const char *const state_names[] = {
		[MIDR_SYNC_LOCAL_WAIT] = "LOCAL_WAIT",
		[MIDR_SYNC_REMOTE_WAIT] = "REMOTE_WAIT",
		[MIDR_SYNC_READY] = "READY",
	};
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_sync_status status;

	if (!ctx || midr_sync_status_get(ctx, &status) != 0)
		return CMD_WARNING;
	vty_out(vty, "MIDR synchronization:\n");
	vty_out(vty, "  state:              %s\n", state_names[status.state]);
	vty_out(vty, "  EoR timeout:        %u seconds\n", status.timeout_seconds);
	vty_out(vty, "  initial peers:      %zu\n", status.initial_peer_count);
	vty_out(vty, "  waiting peers:      %zu\n", status.waiting_peer_count);
	vty_out(vty, "  timed-out peers:    %zu\n", status.timed_out_peer_count);
	vty_out(vty, "  barriers/timeouts:  %" PRIu64 "/%" PRIu64 "\n",
		status.barrier_count, status.timeout_count);
	return CMD_SUCCESS;
}

DEFUN(midr_eor_timeout, midr_eor_timeout_cmd,
      "midr eor-timeout (1-3600)",
      "MIDR configuration\n"
      "Initial peer End-of-RIB timeout\n"
      "Timeout in seconds\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	uint32_t seconds;

	if (!bgp || !bgp->midr_info ||
	    midr_parse_u32(argv[2]->arg, &seconds) != 0 ||
	    midr_sync_timeout_set(&bgp->midr_info->ctx, seconds) != 0)
		return CMD_WARNING;
	return CMD_SUCCESS;
}

DEFUN(no_midr_eor_timeout, no_midr_eor_timeout_cmd,
      "no midr eor-timeout [(1-3600)]",
      NO_STR
      "MIDR configuration\n"
      "Initial peer End-of-RIB timeout\n"
      "Timeout in seconds\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp || !bgp->midr_info ||
	    midr_sync_timeout_set(&bgp->midr_info->ctx,
				  MIDR_EOR_TIMEOUT_DEFAULT) != 0)
		return CMD_WARNING;
	return CMD_SUCCESS;
}

static int midr_config_write(struct bgp *bgp, struct vty *vty)
{
	struct midr_sync_status status;

	if (!bgp || !bgp->midr_info ||
	    midr_sync_status_get(&bgp->midr_info->ctx, &status) != 0)
		return 0;
	if (status.timeout_seconds != MIDR_EOR_TIMEOUT_DEFAULT)
		vty_out(vty, " midr eor-timeout %u\n", status.timeout_seconds);
	return 0;
}

DEFUN(show_midr_topology_nodes, show_midr_topology_nodes_cmd,
      "show midr topology nodes",
      SHOW_STR
      "MIDR information\n"
      "Topology state\n"
      "Active local Node facts\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;

	midr_show_topology_nodes(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_topology_links, show_midr_topology_links_cmd,
      "show midr topology links",
      SHOW_STR
      "MIDR information\n"
      "Topology state\n"
      "Active local Link facts\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;

	midr_show_topology_links(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_topology_tombstones, show_midr_topology_tombstones_cmd,
      "show midr topology tombstones",
      SHOW_STR
      "MIDR information\n"
      "Topology state\n"
      "Withdrawn local fact versions\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;

	midr_show_topology_tombstones(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_topology_sync, show_midr_topology_sync_cmd,
      "show midr topology sync",
      SHOW_STR
      "MIDR information\n"
      "Topology state\n"
      "Provider synchronization state\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;

	midr_show_topology_sync(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(show_midr_events, show_midr_events_cmd,
      "show midr events",
      SHOW_STR
      "MIDR information\n"
      "Event counters\n")
{
	struct midr_context *ctx = midr_vty_context(vty);

	if (!ctx)
		return CMD_WARNING;

	midr_show_events(vty, ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_topology_node_upsert, midr_topology_node_upsert_cmd,
      "midr topology node upsert A.B.C.D group (0-4294967295) version WORD",
      "MIDR commands\n"
      "Topology test input\n"
      "Node object\n"
      "Insert or update object\n"
      "Node router-id\n"
      "Group membership\n"
      "Group id\n"
      "Object version\n"
      "Unsigned 64-bit version\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_node_update node = {};
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (midr_parse_router_id(argv[4]->arg, &node.node_id) != 0 ||
	    midr_parse_u32(argv[6]->arg, &node.group_id) != 0 ||
	    midr_parse_u64(argv[8]->arg, &node.version) != 0) {
		vty_out(vty, "%% Malformed MIDR node input\n");
		return CMD_WARNING;
	}

	node.policy_state = MIDR_POLICY_ALLOWED;
	midr_input_test_resync_now(ctx);
	ret = midr_topology_node_upsert(ctx, &node);
	if (ret) {
		vty_out(vty, "%% MIDR node upsert failed: %d\n", ret);
		return CMD_WARNING;
	}

	midr_topology_process_pending(ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_topology_node_upsert_transport,
      midr_topology_node_upsert_transport_cmd,
      "midr topology node upsert A.B.C.D group (0-4294967295) transport <A.B.C.D|X:X::X:X> version WORD",
      "MIDR commands\n"
      "Topology test input\n"
      "Node object\n"
      "Insert or update object\n"
      "Node router-id\n"
      "Group membership\n"
      "Group id\n"
      "Transport address\n"
      "IPv4 transport address\n"
      "IPv6 transport address\n"
      "Object version\n"
      "Unsigned 64-bit version\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_node_update node = {};
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (midr_parse_router_id(argv[4]->arg, &node.node_id) != 0 ||
	    midr_parse_u32(argv[6]->arg, &node.group_id) != 0 ||
	    str2ipaddr(argv[8]->arg, &node.transport_address) != 0 ||
	    midr_parse_u64(argv[10]->arg, &node.version) != 0) {
		vty_out(vty, "%% Malformed MIDR node input\n");
		return CMD_WARNING;
	}

	node.has_transport_address = true;
	node.policy_state = MIDR_POLICY_ALLOWED;
	midr_input_test_resync_now(ctx);
	ret = midr_topology_node_upsert(ctx, &node);
	if (ret) {
		vty_out(vty, "%% MIDR node upsert failed: %d\n", ret);
		return CMD_WARNING;
	}

	midr_topology_process_pending(ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_topology_node_withdraw, midr_topology_node_withdraw_cmd,
      "midr topology node withdraw A.B.C.D version WORD",
      "MIDR commands\n"
      "Topology test input\n"
      "Node object\n"
      "Withdraw object\n"
      "Node router-id\n"
      "Object version\n"
      "Unsigned 64-bit version\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	uint32_t node_id;
	uint64_t version;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (midr_parse_router_id(argv[4]->arg, &node_id) != 0 ||
	    midr_parse_u64(argv[6]->arg, &version) != 0) {
		vty_out(vty, "%% Malformed MIDR node withdraw input\n");
		return CMD_WARNING;
	}

	midr_input_test_resync_now(ctx);
	ret = midr_topology_node_withdraw(ctx, node_id, version);
	if (ret) {
		vty_out(vty, "%% MIDR node withdraw failed: %d\n", ret);
		return CMD_WARNING;
	}

	midr_topology_process_pending(ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_topology_link_upsert, midr_topology_link_upsert_cmd,
      "midr topology link upsert A.B.C.D A.B.C.D id WORD local-address <A.B.C.D|X:X::X:X> remote-address <A.B.C.D|X:X::X:X> rtt-us (0-4294967295) loss-ppm (0-4294967295) available-bandwidth-kbps (0-4294967295) version WORD [ifindex WORD] [seqno WORD] [timestamp-ms WORD]",
      "MIDR commands\n"
      "Topology test input\n"
      "Link object\n"
      "Insert or update object\n"
      "Local node router-id\n"
      "Remote node router-id\n"
      "Link identifier\n"
      "Unsigned 64-bit link identifier\n"
      "Local Link endpoint address\n"
      "IPv4 Link endpoint\n"
      "IPv6 Link endpoint\n"
      "Remote Link endpoint address\n"
      "IPv4 Link endpoint\n"
      "IPv6 Link endpoint\n"
      "RTT in microseconds\n"
      "RTT value\n"
      "Loss in parts per million\n"
      "Loss value\n"
      "Available bandwidth in kilobits per second\n"
      "Available bandwidth value\n"
      "Object version\n"
      "Unsigned 64-bit version\n"
      "Local interface index\n"
      "Non-negative interface index\n"
      "Measurement sequence number\n"
      "Unsigned 64-bit sequence number\n"
      "Measurement timestamp in milliseconds\n"
      "Unsigned 64-bit timestamp\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_link_update link = {};
	int idx;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (midr_parse_router_id(argv[4]->arg, &link.key.local_node_id) != 0 ||
	    midr_parse_router_id(argv[5]->arg, &link.key.remote_node_id) != 0 ||
	    midr_parse_u64(argv[7]->arg, &link.key.link_id) != 0 ||
	    str2ipaddr(argv[9]->arg, &link.link_local_address) != 0 ||
	    str2ipaddr(argv[11]->arg, &link.link_remote_address) != 0 ||
	    midr_parse_u32(argv[13]->arg, &link.metrics.rtt_us) != 0 ||
	    midr_parse_u32(argv[15]->arg, &link.metrics.loss_ppm) != 0 ||
	    midr_parse_u32(argv[17]->arg, &link.metrics.available_bandwidth_kbps) != 0 ||
	    midr_parse_u64(argv[19]->arg, &link.version) != 0) {
		vty_out(vty, "%% Malformed MIDR link input\n");
		return CMD_WARNING;
	}

	if (argv_find(argv, argc, "ifindex", &idx) &&
	    midr_parse_ifindex(argv[idx + 1]->arg, &link.local_ifindex) != 0) {
		vty_out(vty, "%% Malformed MIDR link ifindex\n");
		return CMD_WARNING;
	}
	if (argv_find(argv, argc, "seqno", &idx) &&
	    midr_parse_u64(argv[idx + 1]->arg, &link.metrics.measurement_seqno) != 0) {
		vty_out(vty, "%% Malformed MIDR measurement seqno\n");
		return CMD_WARNING;
	}
	if (argv_find(argv, argc, "timestamp-ms", &idx) &&
	    midr_parse_u64(argv[idx + 1]->arg, &link.metrics.measurement_timestamp_ms) != 0) {
		vty_out(vty, "%% Malformed MIDR measurement timestamp\n");
		return CMD_WARNING;
	}

	link.metrics.has_rtt_us = true;
	link.metrics.has_loss_ppm = true;
	link.metrics.has_available_bandwidth_kbps = true;
	link.policy_state = MIDR_POLICY_ALLOWED;

	midr_input_test_resync_now(ctx);
	ret = midr_topology_link_upsert(ctx, &link);
	if (ret) {
		vty_out(vty, "%% MIDR link upsert failed: %d\n", ret);
		return CMD_WARNING;
	}

	midr_topology_process_pending(ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_topology_link_withdraw, midr_topology_link_withdraw_cmd,
      "midr topology link withdraw A.B.C.D A.B.C.D id WORD version WORD",
      "MIDR commands\n"
      "Topology test input\n"
      "Link object\n"
      "Withdraw object\n"
      "Local node router-id\n"
      "Remote node router-id\n"
      "Link identifier\n"
      "Unsigned 64-bit link identifier\n"
      "Object version\n"
      "Unsigned 64-bit version\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_link_key key = {};
	uint64_t version;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (midr_parse_router_id(argv[4]->arg, &key.local_node_id) != 0 ||
	    midr_parse_router_id(argv[5]->arg, &key.remote_node_id) != 0 ||
	    midr_parse_u64(argv[7]->arg, &key.link_id) != 0 ||
	    midr_parse_u64(argv[9]->arg, &version) != 0) {
		vty_out(vty, "%% Malformed MIDR link withdraw input\n");
		return CMD_WARNING;
	}

	midr_input_test_resync_now(ctx);
	ret = midr_topology_link_withdraw(ctx, &key, version);
	if (ret) {
		vty_out(vty, "%% MIDR link withdraw failed: %d\n", ret);
		return CMD_WARNING;
	}

	midr_topology_process_pending(ctx);
	return CMD_SUCCESS;
}

DEFUN(midr_cmd_peer_session, midr_peer_session_cmd,
      "midr peer session A.B.C.D remote-as (1-4294967295) <ipv4-unicast|midr-link-state>",
      "MIDR commands\n"
      "Peer helper\n"
      "Request BGP session\n"
      "Remote peer address\n"
      "Remote AS\n"
      "Remote AS number\n"
      "Activate IPv4 unicast\n"
      "Activate MIDR link-state\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	struct midr_peer_session_request_info req = {};
	int idx = 0;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (str2sockunion(argv[3]->arg, &req.remote_address) != 0) {
		vty_out(vty, "%% Malformed peer address\n");
		return CMD_WARNING;
	}

	req.remote_as = strtoul(argv[5]->arg, NULL, 10);
	if (argv_find(argv, argc, "midr-link-state", &idx)) {
		req.afi = AFI_BGP_LS;
		req.safi = SAFI_MIDR_LS;
	} else {
		req.afi = AFI_IP;
		req.safi = SAFI_UNICAST;
	}

	ret = midr_peer_session_request(ctx, &req);
	if (ret) {
		vty_out(vty, "%% MIDR peer session request failed: %d\n", ret);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

DEFUN(midr_cmd_peer_session_release, midr_peer_session_release_cmd,
      "midr peer session A.B.C.D release <ipv4-unicast|midr-link-state>",
      "MIDR commands\n"
      "Peer helper\n"
      "Request BGP session\n"
      "Remote peer address\n"
      "Release peer AFI/SAFI\n"
      "Deactivate IPv4 unicast\n"
      "Deactivate MIDR link-state\n")
{
	struct midr_context *ctx = midr_vty_context(vty);
	union sockunion remote_address;
	afi_t afi;
	safi_t safi;
	int idx = 0;
	int ret;

	if (!ctx)
		return CMD_WARNING;
	if (str2sockunion(argv[3]->arg, &remote_address) != 0) {
		vty_out(vty, "%% Malformed peer address\n");
		return CMD_WARNING;
	}

	if (argv_find(argv, argc, "midr-link-state", &idx)) {
		afi = AFI_BGP_LS;
		safi = SAFI_MIDR_LS;
	} else {
		afi = AFI_IP;
		safi = SAFI_UNICAST;
	}
	ret = midr_peer_session_release(ctx, &remote_address, afi, safi,
					MIDR_PEER_RELEASE_ADMIN);
	if (ret) {
		vty_out(vty, "%% MIDR peer session release failed: %d\n", ret);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

void bgp_midr_vty_init(void)
{
	install_element(VIEW_NODE, &show_midr_ted_summary_cmd);
	install_element(VIEW_NODE, &show_midr_ted_generation_cmd);
	install_element(VIEW_NODE, &show_midr_sync_cmd);
	install_element(VIEW_NODE, &show_midr_owned_cmd);
	install_element(VIEW_NODE, &show_midr_lsdb_summary_cmd);
	install_element(VIEW_NODE, &show_midr_rib_summary_cmd);
	install_element(VIEW_NODE, &show_midr_topology_nodes_cmd);
	install_element(VIEW_NODE, &show_midr_topology_links_cmd);
	install_element(VIEW_NODE, &show_midr_topology_tombstones_cmd);
	install_element(VIEW_NODE, &show_midr_topology_sync_cmd);
	install_element(VIEW_NODE, &show_midr_events_cmd);
	install_element(ENABLE_NODE, &midr_topology_node_upsert_cmd);
	install_element(ENABLE_NODE, &midr_topology_node_upsert_transport_cmd);
	install_element(ENABLE_NODE, &midr_topology_node_withdraw_cmd);
	install_element(ENABLE_NODE, &midr_topology_link_upsert_cmd);
	install_element(ENABLE_NODE, &midr_topology_link_withdraw_cmd);
	install_element(ENABLE_NODE, &midr_peer_session_cmd);
	install_element(ENABLE_NODE, &midr_peer_session_release_cmd);
	install_element(BGP_NODE, &midr_eor_timeout_cmd);
	install_element(BGP_NODE, &no_midr_eor_timeout_cmd);
	hook_register(bgp_inst_config_write, midr_config_write);
}
