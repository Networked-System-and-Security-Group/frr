// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public MIDR interface shared with the topology provider.
 */

/*
 * ⚠ 拷自第二组分支 feat/pc-ls-propagation @c344e40ba1（2026-07-26 commit
 * "docs: publish MIDR configuration and TED handoff guides"），除本注释块外
 * 与他们的 bgpd/bgp_midr.h 逐字节一致。**以他们为准**：不要在此手改任何原型或
 * 字段。需要同步他们的更新时，重新整份拷贝再把本注释块补回来即可。
 *
 * ⊕ 2026-08-22（对接轮 4 步 0）**已合栈**：他们的实现文件（bgp_midr.c /
 * bgp_midr_input.c / bgp_midr_lsdb.c 等 31 个）已进树，本头文件的原型从此有真
 * 实现顶着。我方假实现 bgp_midr_group2_shim.c **已从 subdir.am 摘除**（同名同
 * 签名，不摘则重复符号链接失败）；文件本身保留在树里，把那行翻回来即可重编回
 * shim 做 A/B 对照，轮 5 评估删除。
 *
 * 一个例外：midr_topology_snapshot_get/release 是 **provider 方向**，由我方实现
 * （轮 3，在 bgp_midr_nds_facts.c）。他们树中那两个带 __attribute__((weak))
 * （bgp_midr_input.c，返回 -ENOSYS），我方的强定义覆盖它们，合树不出现重复符号。
 */

#ifndef _FRR_BGP_MIDR_H
#define _FRR_BGP_MIDR_H

#include <zebra.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asn.h"
#include "if.h"
#include "ipaddr.h"
#include "sockunion.h"

struct midr_context;

enum midr_policy_state {
	MIDR_POLICY_ALLOWED = 0,
	MIDR_POLICY_BLOCKED,
};

enum midr_peer_release_reason {
	MIDR_PEER_RELEASE_ADMIN = 0,
	MIDR_PEER_RELEASE_NODE_DOWN,
	MIDR_PEER_RELEASE_POLICY,
};

enum midr_topology_resync_reason {
	MIDR_TOPOLOGY_RESYNC_VERSION_LOST = 0,
	MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART,
	MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT,
};

struct midr_peer_session_request_info {
	union sockunion remote_address;
	as_t remote_as;
	afi_t afi;
	safi_t safi;
	bool has_update_source;
	union sockunion update_source;
	uint8_t ebgp_multihop;
	const char *password;
	uint64_t policy_tags;
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

struct midr_remote_node_info {
	uint32_t node_id;
	uint32_t group_id;
	bool has_transport_address;
	struct ipaddr transport_address;
	uint64_t cap_flags;
	uint64_t policy_tags;
	uint64_t ls_sequence;
};

struct midr_remote_link_info {
	struct midr_link_key key;
	struct ipaddr link_local_address;
	struct ipaddr link_remote_address;
	struct midr_link_metrics metrics;
	uint64_t policy_tags;
	uint64_t ls_sequence;
};

struct midr_remote_view_snapshot {
	const struct midr_remote_node_info *nodes;
	size_t node_count;
	const struct midr_remote_link_info *links;
	size_t link_count;
	uint64_t snapshot_version;
};

struct midr_remote_view_callbacks {
	void (*remote_node_update)(const struct midr_remote_node_info *node);
	void (*remote_node_withdraw)(uint32_t node_id, uint64_t ls_sequence);
	void (*remote_link_update)(const struct midr_remote_link_info *link);
	void (*remote_link_withdraw)(const struct midr_link_key *key, uint64_t ls_sequence);
};

extern struct midr_context *midr_context_get_default(void);

extern int midr_peer_session_request(struct midr_context *ctx,
				     const struct midr_peer_session_request_info *req);
extern int midr_peer_session_release(struct midr_context *ctx,
				     const union sockunion *remote_address, afi_t afi, safi_t safi,
				     enum midr_peer_release_reason reason);

extern int midr_topology_node_upsert(struct midr_context *ctx, const struct midr_node_update *node);
extern int midr_topology_node_withdraw(struct midr_context *ctx, uint32_t node_id,
				       uint64_t version);
extern int midr_topology_link_upsert(struct midr_context *ctx, const struct midr_link_update *link);
extern int midr_topology_link_withdraw(struct midr_context *ctx, const struct midr_link_key *key,
				       uint64_t version);
extern int midr_topology_resync_begin(struct midr_context *ctx,
				      enum midr_topology_resync_reason reason);

/*
 * The topology provider implements this snapshot API.  MIDR consumes it
 * during startup and resynchronization beginning in M2.
 */
extern int midr_topology_snapshot_get(struct midr_context *ctx,
				      struct midr_topology_snapshot *snapshot);
extern void midr_topology_snapshot_release(struct midr_context *ctx,
					   struct midr_topology_snapshot *snapshot);

extern int midr_remote_view_snapshot_get(struct midr_context *ctx,
					 struct midr_remote_view_snapshot *snapshot);
extern void midr_remote_view_snapshot_release(struct midr_context *ctx,
					      struct midr_remote_view_snapshot *snapshot);
extern int midr_remote_view_callbacks_register(struct midr_context *ctx,
					       const struct midr_remote_view_callbacks *callbacks);

#endif /* _FRR_BGP_MIDR_H */
