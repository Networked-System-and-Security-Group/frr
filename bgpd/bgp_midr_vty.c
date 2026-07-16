// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR VTY commands
 *
 * Provides CLI for MIDR configuration and diagnostics:
 *   midr group-id <N>
 *   midr neighbor <IP> remote-as <ASN>
 *   no midr neighbor <IP>
 *   midr help [plain]
 *   show midr nodes
 */

#include "zebra.h"

#include "command.h"
#include "hook.h"
#include "vty.h"
#include "log.h"
#include "sockunion.h"
#include "prefix.h"
#include "linklist.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_ls.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_liveness.h"
#include "bgpd/bgp_midr_vty.h"
#include "bgpd/bgp_debug.h"
#include "bgpd/bgp_nexthop.h"
#include "bgpd/bgp_table.h"

/* ------------------------------------------------------------------ */
/* midr group-id <N>                                                   */
/* ------------------------------------------------------------------ */

DEFUN(midr_group_id,
      midr_group_id_cmd,
      "midr group-id (0-4294967295)",
      "MIDR configuration\n"
      "Set local MIDR group ID\n"
      "Group ID value\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	uint32_t gid = (uint32_t)atol(argv[2]->arg);
	midr_nds_set_group_id(bgp, gid);

	vty_out(vty, "MIDR group-id set to %u\n", gid);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr neighbor <IP> remote-as <ASN>                                  */
/* ------------------------------------------------------------------ */

DEFUN(midr_neighbor,
      midr_neighbor_cmd,
      "midr neighbor A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Add a MIDR neighbor\n"
      "Neighbor IP address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	as_t asn = (as_t)atol(argv[4]->arg);
	int ret;

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	ret = peer_remote_as(bgp, &su, NULL, &asn, AS_SPECIFIED, NULL);
	if (ret != 0) {
		vty_out(vty, "%% Failed to add neighbor %s (err %d)\n",
			argv[2]->arg, ret);
		return CMD_WARNING_CONFIG_FAILED;
	}

	struct peer *peer = peer_lookup(bgp, &su);
	if (peer) {
		if (asn != bgp->as)
			peer_ebgp_multihop_set(peer, MAXTTL);
		peer_activate(peer, AFI_BGP_LS, SAFI_BGP_LS);
	}

	vty_out(vty, "MIDR neighbor %s AS %u added\n", argv[2]->arg, asn);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* no midr neighbor <IP>                                               */
/* ------------------------------------------------------------------ */

DEFUN(no_midr_neighbor,
      no_midr_neighbor_cmd,
      "no midr neighbor A.B.C.D",
      NO_STR
      "MIDR configuration\n"
      "Remove a MIDR neighbor\n"
      "Neighbor IP address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	struct peer *peer;

	if (str2sockunion(argv[3]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[3]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	peer = peer_lookup(bgp, &su);
	if (!peer) {
		vty_out(vty, "%% Neighbor %s not found\n", argv[3]->arg);
		return CMD_WARNING;
	}

	peer_delete(peer);
	vty_out(vty, "MIDR neighbor %s removed\n", argv[3]->arg);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr role bootstrap / no midr role bootstrap                        */
/* ------------------------------------------------------------------ */

DEFUN(midr_role_bootstrap,
      midr_role_bootstrap_cmd,
      "midr role bootstrap",
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a bootstrap node\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities |
					     MIDR_CAP_BOOTSTRAP);
	vty_out(vty, "MIDR role bootstrap set\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_role_bootstrap,
      no_midr_role_bootstrap_cmd,
      "no midr role bootstrap",
      NO_STR
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a bootstrap node\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities &
					     ~MIDR_CAP_BOOTSTRAP);
	vty_out(vty, "MIDR role bootstrap cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr role group-rep / no midr role group-rep                        */
/* ------------------------------------------------------------------ */

DEFUN(midr_role_group_rep,
      midr_role_group_rep_cmd,
      "midr role group-rep",
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a group representative\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities |
					     MIDR_CAP_GROUP_REP);
	vty_out(vty, "MIDR role group-rep set\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_role_group_rep,
      no_midr_role_group_rep_cmd,
      "no midr role group-rep",
      NO_STR
      "MIDR configuration\n"
      "Set the local MIDR role\n"
      "Act as a group representative\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_nds_set_capability(bgp, bgp->midr_info->local_capabilities &
					     ~MIDR_CAP_GROUP_REP);
	vty_out(vty, "MIDR role group-rep cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr rep group <GID> transport <IP> remote-as <ASN>                 */
/* (bootstrap-side static representative directory)                     */
/* ------------------------------------------------------------------ */

DEFUN(midr_rep,
      midr_rep_cmd,
      "midr rep group (1-4294967295) transport A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Register a group representative (bootstrap directory)\n"
      "Group\n"
      "Group ID value\n"
      "Representative transport address\n"
      "Representative IPv4 address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	uint32_t gid = (uint32_t)atol(argv[3]->arg);
	struct in_addr rep_transport;
	as_t asn = (as_t)atol(argv[7]->arg);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	if (inet_pton(AF_INET, argv[5]->arg, &rep_transport) != 1) {
		vty_out(vty, "%% Invalid transport address: %s\n", argv[5]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	midr_rep_dir_add(bgp, gid, rep_transport, asn);
	vty_out(vty, "MIDR rep for group %u at %s AS %u added\n", gid,
		argv[5]->arg, asn);
	return CMD_SUCCESS;
}

DEFUN(no_midr_rep,
      no_midr_rep_cmd,
      "no midr rep group (1-4294967295) transport A.B.C.D",
      NO_STR
      "MIDR configuration\n"
      "Register a group representative (bootstrap directory)\n"
      "Group\n"
      "Group ID value\n"
      "Representative transport address\n"
      "Representative IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	uint32_t gid = (uint32_t)atol(argv[4]->arg);
	struct in_addr rep_transport;

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	if (inet_pton(AF_INET, argv[6]->arg, &rep_transport) != 1) {
		vty_out(vty, "%% Invalid transport address: %s\n", argv[6]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (!midr_rep_dir_del(bgp, gid, rep_transport)) {
		vty_out(vty, "%% No such rep (group %u, %s)\n", gid,
			argv[6]->arg);
		return CMD_WARNING;
	}
	vty_out(vty, "MIDR rep for group %u at %s removed\n", gid, argv[6]->arg);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr bootstrap <IP> remote-as <ASN>  (command O, run on a new node) */
/* ------------------------------------------------------------------ */

DEFUN(midr_bootstrap,
      midr_bootstrap_cmd,
      "midr bootstrap A.B.C.D remote-as (1-4294967295)",
      "MIDR configuration\n"
      "Join the network via a bootstrap node\n"
      "Bootstrap node IP address\n"
      "Remote AS\n"
      "AS number\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	union sockunion su;
	as_t asn = (as_t)atol(argv[4]->arg);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (str2sockunion(argv[2]->arg, &su) < 0) {
		vty_out(vty, "%% Invalid IP address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	vty_out(vty, "MIDR: joining via bootstrap %s AS %u ...\n", argv[2]->arg,
		asn);
	midr_join_via_bootstrap(bgp, &su, asn);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr transport-address A.B.C.D   (node's real reachable locator)    */
/* ------------------------------------------------------------------ */

DEFUN(midr_transport_address,
      midr_transport_address_cmd,
      "midr transport-address A.B.C.D",
      "MIDR configuration\n"
      "Set the local reachable address others use to peer with / probe us\n"
      "IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct in_addr addr;

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	if (inet_pton(AF_INET, argv[2]->arg, &addr) != 1) {
		vty_out(vty, "%% Invalid IPv4 address: %s\n", argv[2]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	bgp->midr_info->local_transport_addr = addr;
	bgp->midr_info->transport_addr_set = true;
	midr_propagate_self(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* +TLV 1188 */

	vty_out(vty, "MIDR transport-address set to %s\n", argv[2]->arg);
	return CMD_SUCCESS;
}

DEFUN(no_midr_transport_address,
      no_midr_transport_address_cmd,
      "no midr transport-address [A.B.C.D]",
      NO_STR
      "MIDR configuration\n"
      "Clear the local reachable address\n"
      "IPv4 address\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->transport_addr_set = false;
	bgp->midr_info->local_transport_addr.s_addr = INADDR_ANY;
	midr_propagate_self(bgp, MIDR_ORIGIN_TRANSPORT_UPDATE); /* -TLV 1188 */

	vty_out(vty, "MIDR transport-address cleared\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr shutdown   (graceful departure: withdraw self + stop keepalive) */
/* ------------------------------------------------------------------ */

DEFUN(midr_shutdown,
      midr_shutdown_cmd,
      "midr shutdown",
      "MIDR configuration\n"
      "Gracefully leave MIDR: withdraw our Node NLRI and stop advertising\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->shutdown = true; /* keepalive will stop re-originating */
	midr_propagate_self(bgp, MIDR_ORIGIN_LEAVE); /* withdraw: we are leaving */

	vty_out(vty, "MIDR: gracefully shut down (Node NLRI withdrawn)\n");
	return CMD_SUCCESS;
}

DEFUN(no_midr_shutdown,
      no_midr_shutdown_cmd,
      "no midr shutdown",
      NO_STR
      "MIDR configuration\n"
      "Rejoin MIDR: resume advertising our Node NLRI\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	bgp->midr_info->shutdown = false;
	midr_propagate_self(bgp, MIDR_ORIGIN_REJOIN); /* re-announce ourselves */

	vty_out(vty, "MIDR: resumed (Node NLRI re-originated)\n");
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* MIDR liveness configuration                                        */
/* ------------------------------------------------------------------ */

static int midr_liveness_apply_vty(struct vty *vty, struct bgp *bgp,
				   const struct midr_liveness_config *config)
{
	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	if (!midr_liveness_set_config(bgp, config)) {
		vty_out(vty,
			"%% Invalid MIDR liveness configuration: suspect must exceed keepalive, scan must not exceed suspect, and quorum must be a strict majority\n");
		return CMD_WARNING_CONFIG_FAILED;
	}
	return CMD_SUCCESS;
}

DEFUN(midr_liveness_timers,
      midr_liveness_timers_cmd,
      "midr liveness timers keepalive (1-3600) suspect (2-7200) scan (1-3600) confirm (1-300) retry (1-300)",
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Configure liveness timers atomically\n"
      "BGP-LS Node keepalive interval\n"
      "Seconds\n"
      "Age that moves a node to SUSPECT\n"
      "Seconds\n"
      "Node-table scan interval\n"
      "Seconds\n"
      "Indirect confirmation timeout\n"
      "Seconds\n"
      "Retry backoff after an inconclusive round\n"
      "Seconds\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config;

	midr_liveness_get_config(bgp, &config);
	config.keepalive_interval = strtoul(argv[4]->arg, NULL, 10);
	config.suspect_timeout = strtoul(argv[6]->arg, NULL, 10);
	config.scan_interval = strtoul(argv[8]->arg, NULL, 10);
	config.confirm_timeout = strtoul(argv[10]->arg, NULL, 10);
	config.retry_backoff = strtoul(argv[12]->arg, NULL, 10);
	return midr_liveness_apply_vty(vty, bgp, &config);
}

DEFUN(no_midr_liveness_timers,
      no_midr_liveness_timers_cmd,
      "no midr liveness timers",
      NO_STR
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Restore default liveness timers\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config, defaults;

	midr_liveness_get_config(bgp, &config);
	midr_liveness_config_defaults(&defaults);
	config.keepalive_interval = defaults.keepalive_interval;
	config.suspect_timeout = defaults.suspect_timeout;
	config.scan_interval = defaults.scan_interval;
	config.confirm_timeout = defaults.confirm_timeout;
	config.retry_backoff = defaults.retry_backoff;
	return midr_liveness_apply_vty(vty, bgp, &config);
}

DEFUN(midr_liveness_voting,
      midr_liveness_voting_cmd,
      "midr liveness voting sample-size (2-16) quorum (2-16)",
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Configure indirect voting\n"
      "Maximum stable voter sample\n"
      "Number of voters\n"
      "Required STALE votes\n"
      "Number of votes\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config;

	midr_liveness_get_config(bgp, &config);
	config.voter_sample_size = strtoul(argv[4]->arg, NULL, 10);
	config.quorum = strtoul(argv[6]->arg, NULL, 10);
	return midr_liveness_apply_vty(vty, bgp, &config);
}

DEFUN(no_midr_liveness_voting,
      no_midr_liveness_voting_cmd,
      "no midr liveness voting",
      NO_STR
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Restore default indirect voting\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config, defaults;

	midr_liveness_get_config(bgp, &config);
	midr_liveness_config_defaults(&defaults);
	config.voter_sample_size = defaults.voter_sample_size;
	config.quorum = defaults.quorum;
	return midr_liveness_apply_vty(vty, bgp, &config);
}

DEFUN(midr_liveness_gossip,
      midr_liveness_gossip_cmd,
      "midr liveness gossip hop-limit (1-255) cache-ttl (1-3600)",
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Configure DEAD/LEAVE gossip\n"
      "Maximum gossip hops\n"
      "Hop count\n"
      "Seen-message cache lifetime\n"
      "Seconds\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config;

	midr_liveness_get_config(bgp, &config);
	config.hop_limit = strtoul(argv[4]->arg, NULL, 10);
	config.cache_ttl = strtoul(argv[6]->arg, NULL, 10);
	return midr_liveness_apply_vty(vty, bgp, &config);
}

DEFUN(no_midr_liveness_gossip,
      no_midr_liveness_gossip_cmd,
      "no midr liveness gossip",
      NO_STR
      "MIDR configuration\n"
      "Node liveness confirmation\n"
      "Restore default gossip settings\n")
{
	VTY_DECLVAR_CONTEXT(bgp, bgp);
	struct midr_liveness_config config, defaults;

	midr_liveness_get_config(bgp, &config);
	midr_liveness_config_defaults(&defaults);
	config.hop_limit = defaults.hop_limit;
	config.cache_ttl = defaults.cache_ttl;
	return midr_liveness_apply_vty(vty, bgp, &config);
}

/* ------------------------------------------------------------------ */
/* show midr nodes                                                     */
/* ------------------------------------------------------------------ */

static const char *midr_caps_str(uint32_t caps, char *buf, size_t len)
{
	buf[0] = '\0';
	if (!caps) {
		strlcpy(buf, "none", len);
		return buf;
	}
	if (caps & MIDR_CAP_SRV6)
		strlcat(buf, "SRv6 ", len);
	if (caps & MIDR_CAP_ROUTING)
		strlcat(buf, "Routing ", len);
	if (caps & MIDR_CAP_BOOTSTRAP)
		strlcat(buf, "Bootstrap ", len);
	if (caps & MIDR_CAP_GROUP_REP)
		strlcat(buf, "GroupRep", len);
	return buf;
}

/*
 * 三个节点表视图命令 (nodes/reps/bootstraps) 共用的表格体: required_caps=0
 * 列全部, 非 0 只列 capabilities 含全部所要位的节点。reps/bootstraps 是同一
 * 张表的过滤视图, 含 SUSPECT 条目 (诊断命令要能看见异常; 协议侧 REP_LIST 组
 * 装另用严过滤, 见 midr_rep_candidates)。
 */
static void midr_show_node_table(struct vty *vty, struct bgp *bgp,
				 uint32_t required_caps)
{
	struct midr_node_entry *entry;
	char caps_buf[64];

	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"Router-ID", "Transport-Addr", "ASN", "Group-ID", "Status",
		"Capabilities");
	vty_out(vty, "%-18s %-18s %-8s %-10s %-8s %s\n",
		"------------------", "------------------", "--------",
		"----------", "--------", "------------");

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		char taddr[INET_ADDRSTRLEN];

		if ((entry->capabilities & required_caps) != required_caps)
			continue;

		if (entry->has_transport_addr)
			inet_ntop(AF_INET, &entry->transport_addr, taddr,
				  sizeof(taddr));
		else
			snprintf(taddr, sizeof(taddr), "-");

		vty_out(vty, "%-18pI4 %-18s %-8u %-10u %-8s %s\n",
			&entry->node_id.u.prefix4,
			taddr,
			entry->asn,
			entry->group_id,
			midr_liveness_state_name(entry),
			midr_caps_str(entry->capabilities, caps_buf,
				      sizeof(caps_buf)));
	}
}

DEFUN(show_midr_nodes,
      show_midr_nodes_cmd,
      "show midr nodes",
      SHOW_STR
      "MIDR information\n"
      "Show MIDR node table\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, 0);

	return CMD_SUCCESS;
}

DEFUN(show_midr_liveness,
      show_midr_liveness_cmd,
      "show midr liveness",
      SHOW_STR
      "MIDR information\n"
      "Show liveness configuration and confirmation rounds\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp || !bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	midr_liveness_show(vty, bgp);
	return CMD_SUCCESS;
}

DEFUN(show_midr_reps,
      show_midr_reps_cmd,
      "show midr reps",
      SHOW_STR
      "MIDR information\n"
      "Show group representatives (nodes with the GROUP_REP capability)\n")
{
	struct bgp *bgp = bgp_get_default();
	struct listnode *node;
	struct midr_rep_entry *r;

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, MIDR_CAP_GROUP_REP);

	/* 本地 rep_dir (手配/学来, REP_LIST 应答排前的来源) 恒显示——一眼区
	 * 分"全网视图"(上表, 按位推导)与"本地目录"(本段); 手配/学来的区分标
	 * 记是未来项 F (from_config)。 */
	vty_out(vty, "\nLocal rep directory (rep_dir, served first in REP_LIST):\n");
	if (list_isempty(bgp->midr_info->rep_dir))
		vty_out(vty, "  (none)\n");
	else
		for (ALL_LIST_ELEMENTS_RO(bgp->midr_info->rep_dir, node, r))
			vty_out(vty, "  group %u -> %pI4 (AS %u)\n",
				r->group_id, &r->rep_transport,
				(unsigned int)r->rep_asn);

	return CMD_SUCCESS;
}

DEFUN(show_midr_bootstraps,
      show_midr_bootstraps_cmd,
      "show midr bootstraps",
      SHOW_STR
      "MIDR information\n"
      "Show bootstrap nodes (nodes with the BOOTSTRAP capability)\n")
{
	struct bgp *bgp = bgp_get_default();

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	midr_show_node_table(vty, bgp, MIDR_CAP_BOOTSTRAP);

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr neighbors                                                 */
/*                                                                     */
/* Unlike `show midr nodes` (which lists every node learned from a     */
/* BGP-LS Node NLRI — possibly relayed, not directly connected), this  */
/* lists the BGP peers we have an actual MIDR session with, i.e. peers */
/* with the BGP-LS address-family activated.  Group-id / capabilities  */
/* are cross-referenced from the node table when available.            */
/* ------------------------------------------------------------------ */

/*
 * BGP RIB（ipv4 unicast）里有没有覆盖该前缀的路由。注意这只是"BGP 视角
 * 有无覆盖路由"，不是严格的转发可达性（zebra/内核 FIB 才是权威）；用途是
 * 给 show 命令里"卡住"的目标一个可行动的提示（underlay 路由缺失时 UDP 黑洞
 * / 会话卡 Active 都是静默的，见 transport 互通部署契约）。
 */
static bool midr_bgp_rib_covers(struct bgp *bgp, const struct prefix *p)
{
	struct bgp_dest *dest;

	if (!bgp->rib[AFI_IP][SAFI_UNICAST])
		return true; /* 判不了当可达，不误报 */

	dest = bgp_node_match(bgp->rib[AFI_IP][SAFI_UNICAST], p);
	if (!dest)
		return false;
	bgp_dest_unlock_node(dest); /* bgp_node_match 返回已加锁节点 */
	return true;
}

/*
 * 对端 transport 地址在本端是否可达：优先查 BGP nexthop tracking 缓存
 * （multihop peer 建连时经 FSM 注册，键 = connection->su 的 /32 host 前缀，
 * 见 bgp_find_or_add_nexthop 的 peer 分支）；无缓存条目（peer 未注册 NHT）
 * 再退化查 BGP RIB 覆盖路由。只读，不注册、不改状态。
 */
static bool midr_underlay_reachable(struct bgp *bgp, union sockunion *su)
{
	struct prefix p;
	struct bgp_nexthop_cache *bnc;

	if (sockunion_family(su) != AF_INET || !sockunion2hostprefix(su, &p))
		return true; /* 判不了当可达，不误报 */

	bnc = bnc_find(&bgp->nexthop_cache_table[AFI_IP], &p, 0, 0);
	if (bnc)
		return CHECK_FLAG(bnc->flags, BGP_NEXTHOP_VALID);

	return midr_bgp_rib_covers(bgp, &p);
}

DEFUN(show_midr_neighbors,
      show_midr_neighbors_cmd,
      "show midr neighbors",
      SHOW_STR
      "MIDR information\n"
      "Show established MIDR (BGP-LS) sessions\n")
{
	struct bgp *bgp = bgp_get_default();
	struct midr_node_hash_head *nodes;
	struct peer *peer;
	struct listnode *node;
	char caps_buf[64];
	bool any = false;

	if (!bgp) {
		vty_out(vty, "%% No BGP instance found\n");
		return CMD_WARNING;
	}

	if (!bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}

	nodes = &bgp->midr_info->global_view->nodes;

	vty_out(vty, "%-18s %-8s %-14s %-10s %s\n",
		"Neighbor", "ASN", "State", "Group-ID", "Capabilities");
	vty_out(vty, "%-18s %-8s %-14s %-10s %s\n",
		"------------------", "--------", "--------------",
		"----------", "------------");

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		struct midr_node_entry *ne = NULL;
		const char *nbr_disp;
		char taddr[INET_ADDRSTRLEN];
		char state_buf[32];

		/* Only peers carrying the BGP-LS AF are MIDR sessions. */
		if (!peer->afc[AFI_BGP_LS][SAFI_BGP_LS])
			continue;

		any = true;

		/*
		 * 按对端 router-id 反查节点表：节点表以 node_id(router-id) 为键，
		 * peer->remote_id 是对端 OPEN 报文里的 router-id，与会话用哪个地址
		 * 建立无关——故直连静态链路会话也能命中。旧版按 peer 连接地址匹配
		 * 节点 locator，静态会话的链路地址对不上 transport，会错误显示 n/a。
		 */
		if (peer->remote_id.s_addr != INADDR_ANY) {
			struct midr_node_entry key = {};

			key.node_id.family = AF_INET;
			key.node_id.prefixlen = IPV4_MAX_BITLEN;
			key.node_id.u.prefix4 = peer->remote_id;
			ne = midr_node_hash_find(nodes, &key);
		}

		/*
		 * 命中节点则统一显示其 transport（稳定 loopback，比链路地址直观）；
		 * 否则回退显示 peer 的连接地址。
		 */
		nbr_disp = peer->host;
		if (ne && ne->has_transport_addr) {
			inet_ntop(AF_INET, &ne->transport_addr, taddr,
				  sizeof(taddr));
			nbr_disp = taddr;
		}

		/*
		 * 卡在非 Established 时区分两种情形：对端尚未拨入（正常等待）
		 * vs 到对端 transport 无路由（underlay 断，坏事）——后者追加
		 * " (no route)" 标注。
		 */
		snprintf(state_buf, sizeof(state_buf), "%s",
			 lookup_msg(bgp_status_msg, peer->connection->status,
				    NULL));
		if (peer->connection->status != Established &&
		    !midr_underlay_reachable(bgp, &peer->connection->su))
			strlcat(state_buf, " (no route)", sizeof(state_buf));
		if (ne && !midr_liveness_node_usable(ne))
			strlcat(state_buf, " (suspect)", sizeof(state_buf));

		vty_out(vty, "%-18s %-8u %-14s ", nbr_disp, peer->as,
			state_buf);

		if (ne)
			vty_out(vty, "%-10u %s\n", ne->group_id,
				midr_caps_str(ne->capabilities, caps_buf,
					      sizeof(caps_buf)));
		else
			vty_out(vty, "%-10s %s\n", "n/a", "n/a");
	}

	if (!any)
		vty_out(vty, "%% No MIDR (BGP-LS) sessions\n");

	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* show midr join                                                      */
/* ------------------------------------------------------------------ */

static const char *midr_join_phase_str(enum midr_join_phase phase)
{
	switch (phase) {
	case MIDR_JOIN_IDLE:
		return "idle";
	case MIDR_JOIN_PROBING_REPS:
		return "probing-reps";
	case MIDR_JOIN_PROBING_MEMBERS:
		return "probing-members";
	}
	return "unknown";
}

DEFUN(show_midr_join,
      show_midr_join_cmd,
      "show midr join",
      SHOW_STR
      "MIDR information\n"
      "Show MIDR new-node join state\n")
{
	struct bgp *bgp = bgp_get_default();
	struct bgp_midr *mi;
	struct listnode *node;
	struct midr_ctrl_pending *pend;

	if (!bgp || !bgp->midr_info) {
		vty_out(vty, "%% MIDR not initialized\n");
		return CMD_WARNING;
	}
	mi = bgp->midr_info;

	/* 未配 bootstrap 也继续输出：稳态的 PEER_REQUEST pending 与
	 * bootstrap 无关，卡住的请求同样要在这里可见。 */
	if (mi->bootstrap_set)
		vty_out(vty, "Bootstrap node : %pSU AS %u\n",
			&mi->bootstrap_su, mi->bootstrap_asn);
	else
		vty_out(vty, "Bootstrap node : (not configured)\n");
	vty_out(vty, "Join state     : %s\n",
		mi->join_in_progress ? "in-progress" : "idle/done");
	vty_out(vty, "Join phase     : %s\n",
		midr_join_phase_str(mi->join_phase));
	vty_out(vty, "Local group-id : %u\n", mi->local_group_id);
	vty_out(vty, "Joined group   : %u\n", mi->join_group_id);
	vty_out(vty, "Members linked : %u\n", mi->join_members);

	vty_out(vty, "Pending ctrl requests:\n");
	if (!mi->ctrl_pending || list_isempty(mi->ctrl_pending)) {
		vty_out(vty, "  (none)\n");
		return CMD_SUCCESS;
	}
	for (ALL_LIST_ELEMENTS_RO(mi->ctrl_pending, node, pend)) {
		struct prefix p = {};

		p.family = AF_INET;
		p.prefixlen = IPV4_MAX_BITLEN;
		p.u.prefix4 = pend->target_transport;
		vty_out(vty, "  %-16s -> %-15pI4  retries_left=%d%s\n",
			midr_ctrl_msg_type_str(pend->type),
			&pend->target_transport, pend->retries_left,
			midr_bgp_rib_covers(bgp, &p) ? ""
						     : "  [no BGP route]");
	}
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* midr help [plain]                                                   */
/*                                                                     */
/* 集中列出所有 MIDR 命令及其功能,仿 SYNLS -h 的分组样式。            */
/* 默认 ANSI 彩色;带 plain -> 纯文本(开发期两套并存)。             */
/* ------------------------------------------------------------------ */

static void bgp_midr_print_help(struct vty *vty, bool color)
{
	/* ANSI 转义码:彩色时生效,纯文本时全部为空串。 */
	const char *C_TITLE = color ? "\033[1;36m" : ""; /* 标题:青色加粗   */
	const char *C_SEC = color ? "\033[1;33m" : "";	 /* 分组:黄色加粗   */
	const char *C_CMD = color ? "\033[32m" : "";	 /* 命令名:绿色     */
	const char *C_RST = color ? "\033[0m" : "";	 /* 复位            */

	/* 标题 */
	vty_out(vty, "%s📖 MIDR 命令一览:%s\n\n", C_TITLE, C_RST);

	/*
	 * 配置命令组:都需先进入配置态——`configure terminal` → `router bgp <ASN>`
	 * (命令缩进 4 空格,说明另起一行缩进 8 空格)
	 */
	vty_out(vty, "%s配置命令(需先进入 `router bgp <ASN>` 配置态):%s\n", C_SEC,
		C_RST);
	vty_out(vty, "    %smidr group-id <0-4294967295>%s\n", C_CMD, C_RST);
	vty_out(vty, "        设置本节点 MIDR 群组 ID\n");
	vty_out(vty, "    %smidr transport-address <IP>%s\n", C_CMD, C_RST);
	vty_out(vty, "        设置本节点可达地址(TLV 1188,实际填 loopback)\n");
	vty_out(vty, "    %sno midr transport-address [<IP>]%s\n", C_CMD, C_RST);
	vty_out(vty, "        清除本节点的 transport-address\n");
	vty_out(vty, "    %smidr neighbor <IP> remote-as <ASN>%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        手动添加 MIDR 邻居(eBGP multihop + 激活 BGP-LS 地址族)\n");
	vty_out(vty, "    %sno midr neighbor <IP>%s\n", C_CMD, C_RST);
	vty_out(vty, "        删除指定的 MIDR 邻居\n");
	vty_out(vty, "    %smidr role bootstrap%s\n", C_CMD, C_RST);
	vty_out(vty, "        将本节点设为 bootstrap(引导)节点\n");
	vty_out(vty, "    %sno midr role bootstrap%s\n", C_CMD, C_RST);
	vty_out(vty, "        取消本节点的 bootstrap 角色\n");
	vty_out(vty, "    %smidr role group-rep%s\n", C_CMD, C_RST);
	vty_out(vty, "        将本节点设为群代表(group-rep)\n");
	vty_out(vty, "    %sno midr role group-rep%s\n", C_CMD, C_RST);
	vty_out(vty, "        取消本节点的群代表角色\n");
	vty_out(vty,
		"    %smidr rep group <GID> transport <IP> remote-as <ASN>%s\n",
		C_CMD, C_RST);
	vty_out(vty,
		"        引导目录:登记某群的群代表(可达地址 + ASN),供新节点发现\n");
	vty_out(vty, "    %sno midr rep group <GID> transport <IP>%s\n", C_CMD,
		C_RST);
	vty_out(vty, "        从引导目录删除指定群代表\n");
	vty_out(vty, "    %smidr bootstrap <IP> remote-as <ASN>%s\n", C_CMD,
		C_RST);
	vty_out(vty, "        作为新节点,经 bootstrap 节点入网(命令 O)\n");
	vty_out(vty, "    %smidr shutdown%s\n", C_CMD, C_RST);
	vty_out(vty, "        优雅下线本节点(撤销自通告,抑制 keepalive)\n");
	vty_out(vty, "    %sno midr shutdown%s\n", C_CMD, C_RST);
	vty_out(vty, "        重新上线(恢复自通告)\n");
	vty_out(vty,
		"    %smidr liveness timers keepalive <S> suspect <S> scan <S> confirm <S> retry <S>%s\n",
		C_CMD, C_RST);
	vty_out(vty, "        原子配置保活、怀疑扫描和确认重试定时器\n");
	vty_out(vty,
		"    %smidr liveness voting sample-size <K> quorum <Q>%s\n",
		C_CMD, C_RST);
	vty_out(vty, "        配置稳定邻居样本与严格多数门限\n");
	vty_out(vty,
		"    %smidr liveness gossip hop-limit <N> cache-ttl <S>%s\n",
		C_CMD, C_RST);
	vty_out(vty, "        配置 DEAD/LEAVE Gossip 扩散范围与去重缓存\n");

	/* 查看 / 帮助命令组:vtysh 顶层(enable/view)直接可用 */
	vty_out(vty, "\n%s查看 / 帮助命令(vtysh 顶层直接可用,无需进配置态):%s\n",
		C_SEC, C_RST);
	vty_out(vty, "    %sshow midr nodes%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        显示 MIDR 节点表(由 BGP-LS Node NLRI 学到的所有节点)\n");
	vty_out(vty, "    %sshow midr liveness%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示保活参数、SUSPECT 数量与进行中的确认轮次\n");
	vty_out(vty, "    %sshow midr reps%s\n", C_CMD, C_RST);
	vty_out(vty,
		"        列全网群代表(按 GROUP_REP 能力位过滤节点表) + 本地 rep 目录\n");
	vty_out(vty, "    %sshow midr bootstraps%s\n", C_CMD, C_RST);
	vty_out(vty, "        列全网引导节点(按 BOOTSTRAP 能力位过滤节点表)\n");
	vty_out(vty, "    %sshow midr neighbors%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示已建立的 MIDR(BGP-LS)直连会话\n");
	vty_out(vty, "    %sshow midr join%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示新节点入网状态\n");
	vty_out(vty, "    %smidr help [plain]%s\n", C_CMD, C_RST);
	vty_out(vty, "        显示本帮助(默认彩色,附加 plain 显示纯文本版)\n");
}

DEFUN(midr_help,
      midr_help_cmd,
      "midr help [plain]",
      "MIDR configuration\n"
      "Show all MIDR commands and their functions\n"
      "Use plain (no ANSI color) output\n")
{
	/* 默认彩色;附加 plain token 时输出纯文本。 */
	bool color = !(argc > 2 && strmatch(argv[argc - 1]->text, "plain"));

	bgp_midr_print_help(vty, color);
	return CMD_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void bgp_midr_vty_init(void)
{
	install_element(BGP_NODE, &midr_group_id_cmd);
	install_element(BGP_NODE, &midr_neighbor_cmd);
	install_element(BGP_NODE, &no_midr_neighbor_cmd);
	install_element(BGP_NODE, &midr_role_bootstrap_cmd);
	install_element(BGP_NODE, &no_midr_role_bootstrap_cmd);
	install_element(BGP_NODE, &midr_role_group_rep_cmd);
	install_element(BGP_NODE, &no_midr_role_group_rep_cmd);
	install_element(BGP_NODE, &midr_rep_cmd);
	install_element(BGP_NODE, &no_midr_rep_cmd);
	install_element(BGP_NODE, &midr_bootstrap_cmd);
	install_element(BGP_NODE, &midr_transport_address_cmd);
	install_element(BGP_NODE, &no_midr_transport_address_cmd);
	install_element(BGP_NODE, &midr_shutdown_cmd);
	install_element(BGP_NODE, &no_midr_shutdown_cmd);
	install_element(BGP_NODE, &midr_liveness_timers_cmd);
	install_element(BGP_NODE, &no_midr_liveness_timers_cmd);
	install_element(BGP_NODE, &midr_liveness_voting_cmd);
	install_element(BGP_NODE, &no_midr_liveness_voting_cmd);
	install_element(BGP_NODE, &midr_liveness_gossip_cmd);
	install_element(BGP_NODE, &no_midr_liveness_gossip_cmd);
	install_element(BGP_NODE, &midr_help_cmd);
	/* `midr help` 也在 vtysh 顶层可用(enable/view),无需进配置态 */
	install_element(VIEW_NODE, &midr_help_cmd);
	install_element(ENABLE_NODE, &midr_help_cmd);
	install_element(VIEW_NODE, &show_midr_nodes_cmd);
	install_element(VIEW_NODE, &show_midr_liveness_cmd);
	install_element(VIEW_NODE, &show_midr_reps_cmd);
	install_element(VIEW_NODE, &show_midr_bootstraps_cmd);
	install_element(VIEW_NODE, &show_midr_neighbors_cmd);
	install_element(VIEW_NODE, &show_midr_join_cmd);

	hook_register(bgp_inst_config_write, midr_liveness_config_write);
}
