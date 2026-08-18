// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 第二组 topology 接口的 shim（垫片 / 假实现）—— 轮 0 备料
 *
 * 存在的三个理由（对接计划轮 0）：
 *   ① 我方调用侧（轮 1 node 上报 / 轮 2 link 上报）立刻能编能跑，不用等第二组
 *      分支合进来；
 *   ② 行为与现状完全一致 —— 轮 0 无调用方；轮 1/2 起由本文件把 upsert 转调原有
 *      的 midr_propagate_self / E-1，NLRI 照发，回归照跑；
 *   ③ 轮 4 联调时**整份删除**本文件、换成第二组的实现文件（他们的 bgp_midr.c /
 *      bgp_midr_input.c / bgp_midr_lsdb.c …），我方调用侧一行不用改。
 *
 * ⚠ 本文件是**临时件**。不要往这里加我方的业务逻辑 —— 事实表、单位换算、
 * link_id 分配都在 bgp_midr_nds_facts.{c,h}（长期件），删本文件不会带走它们。
 *
 * 原型来源：bgp_midr.h（原名拷自第二组 @c344e40ba1，以他们为准）。
 *
 * **不在本文件里的两个函数**：midr_topology_snapshot_get / _release 是 provider
 * 方向、由我方实现（轮 3）。他们树里那两个带 __attribute__((weak))
 * （bgp_midr_input.c:111/122，返回 -ENOSYS），我方强定义会覆盖，合树无重复符号。
 */

#include <zebra.h>

#include <errno.h>

#include "log.h"
#include "sockunion.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_nds.h"

/*
 * midr_context 在接口头文件里只有前向声明（opaque handle），真定义在第二组的
 * bgp_midr_private.h。我方只负责透传、绝不解引用，所以 shim 期给一个非 NULL
 * 的哨兵即可（计划 Q7：init 时取一次存进实例状态，之后每次调接口透传）。
 */
static int shim_ctx_sentinel;

struct midr_context *midr_context_get_default(void)
{
	return (struct midr_context *)&shim_ctx_sentinel;
}

/* ===========================================================================
 * 校验 —— 逐条镜像第二组 bgp_midr_input.c:213-283 @c344e40ba1
 *
 * 为什么要在假实现里也校验：轮 1/2 填字段填错时当场就报 -EINVAL，而不是拖到
 * 轮 4 合栈联调才炸。**必须与他们保持一致**，改动前先比对他们的源文件；轮 4
 * 删本文件时这段一起消失。
 *
 * 注意两处比文档更严的地方（读代码才看到的）：
 *   - loss_ppm 是 **>= 1000000 即 -EINVAL**（严格小于），不是文档写的「≤1e6」；
 *   - 三项指标（rtt / loss / bw）的 has_* 必须**全为真**，且 rtt_us != 0、
 *     available_bandwidth_kbps != 0 —— bw 不是「可以先不填」。
 *
 * ⚠ **本段镜像不到的一道闸门（轮 1 实读发现）**：真实现在 validate 之后还有
 * apply 阶段，那里对 node/link 的 upsert 与 withdraw 一律
 * `version <= 已存版本` 即丢弃（他们 bgp_midr_input.c:293 / :316 / :344 / :367
 * @c344e40ba1），而且**不报错** —— 事件计进 ignored_old，API 照样返回 0。
 * shim 没有事实表、镜像不了这道检查，所以「版本没递增导致上报静默失效」这类
 * 错误在 shim 期查不出来，只能靠上报层自己守住：version 的语义必须是
 * 「上报序号」（每次发出即递增），说明见 bgp_midr_nds_facts.h 文件头。
 * =========================================================================*/

static bool shim_policy_valid(enum midr_policy_state state)
{
	return state == MIDR_POLICY_ALLOWED || state == MIDR_POLICY_BLOCKED;
}

static bool shim_ipaddr_present(const struct ipaddr *address)
{
	return address->ipa_type == IPADDR_V4 || address->ipa_type == IPADDR_V6;
}

/* 本机 router-id 的 s_addr 原值；他们用 ctx->bgp->router_id.s_addr。 */
static uint32_t shim_local_node_id(void)
{
	struct bgp *bgp = bgp_get_default();

	return bgp ? bgp->router_id.s_addr : 0;
}

static int shim_validate_node_update(uint32_t local_node_id,
				     const struct midr_node_update *node)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node || !node->node_id || node->node_id != local_node_id)
		return -EINVAL;
	if (!shim_policy_valid(node->policy_state))
		return -EINVAL;
	if (node->has_transport_address) {
		if (!shim_ipaddr_present(&node->transport_address))
			return -EINVAL;
	} else if (node->transport_address.ipa_type != IPADDR_NONE) {
		return -EINVAL;
	}

	return 0;
}

static int shim_validate_node_withdraw(uint32_t local_node_id, uint32_t node_id)
{
	if (!local_node_id)
		return -ENOENT;
	if (!node_id || node_id != local_node_id)
		return -EINVAL;

	return 0;
}

static int shim_validate_link_update(uint32_t local_node_id,
				     const struct midr_link_update *link)
{
	if (!local_node_id)
		return -ENOENT;
	if (!link || !link->key.local_node_id ||
	    link->key.local_node_id != local_node_id ||
	    !link->key.remote_node_id ||
	    link->key.remote_node_id == link->key.local_node_id)
		return -EINVAL;
	if (link->local_ifindex < 0 ||
	    !shim_ipaddr_present(&link->link_local_address) ||
	    !shim_ipaddr_present(&link->link_remote_address) ||
	    link->link_local_address.ipa_type !=
		    link->link_remote_address.ipa_type)
		return -EINVAL;
	if (!link->metrics.has_rtt_us || !link->metrics.has_loss_ppm ||
	    !link->metrics.has_available_bandwidth_kbps)
		return -EINVAL;
	if (!link->metrics.rtt_us || !link->metrics.available_bandwidth_kbps ||
	    link->metrics.loss_ppm >= 1000000)
		return -EINVAL;
	if (!shim_policy_valid(link->policy_state))
		return -EINVAL;

	return 0;
}

static int shim_validate_link_withdraw(uint32_t local_node_id,
				       const struct midr_link_key *key)
{
	if (!local_node_id)
		return -ENOENT;
	if (!key || !key->local_node_id || key->local_node_id != local_node_id ||
	    !key->remote_node_id ||
	    key->remote_node_id == key->local_node_id)
		return -EINVAL;

	return 0;
}

/* ===========================================================================
 * topology 上报（我方调用、第二组实现）
 *
 * 轮 0：校验 + 记日志 + 返回 0，**不产生任何副作用** —— 此刻没有调用方，行为零变化。
 * 轮 1（node，已做）/ 轮 2（link，待做）：把原有的 midr_propagate_self / E-1
 *         转调补上，使「调用形式换成 upsert，NLRI 仍照旧发出」（计划答疑 26：
 *         壳先换、芯不换）。node 半边的调用方是 midr_nds_report_node()。
 * =========================================================================*/

int midr_topology_node_upsert(struct midr_context *ctx,
			      const struct midr_node_update *node)
{
	int ret;

	if (!ctx)
		return -ENOENT;

	ret = shim_validate_node_update(shim_local_node_id(), node);
	if (ret) {
		MIDR_LOG("MIDR shim: node_upsert 校验失败 ret=%d", ret);
		return ret;
	}

	MIDR_LOG("MIDR shim: node_upsert node_id=%u group=%u caps=0x%" PRIx64
		 " version=%" PRIu64,
		 node->node_id, node->group_id, node->cap_flags, node->version);

	/* 轮 1：转调我方原有自通告出口，NLRI 照旧发出（壳换芯不换）。reason 用
	 * 专设的 TOPOLOGY_UPSERT，日志里一眼分得出这条 origination 走的是新上报
	 * 路径、而非 keepalive 的 5s 重发。 */
	midr_propagate_self(bgp_get_default(), MIDR_ORIGIN_TOPOLOGY_UPSERT);
	return 0;
}

int midr_topology_node_withdraw(struct midr_context *ctx, uint32_t node_id,
				uint64_t version)
{
	int ret;

	if (!ctx)
		return -ENOENT;

	ret = shim_validate_node_withdraw(shim_local_node_id(), node_id);
	if (ret) {
		MIDR_LOG("MIDR shim: node_withdraw 校验失败 ret=%d", ret);
		return ret;
	}

	MIDR_LOG("MIDR shim: node_withdraw node_id=%u version=%" PRIu64, node_id,
		 version);

	/* 轮 1：转调我方原有的 withdraw 路径（midr_propagate_self 的 LEAVE 分支
	 * = bgp_ls_withdraw_bgp_node）。 */
	midr_propagate_self(bgp_get_default(), MIDR_ORIGIN_LEAVE);
	return 0;
}

int midr_topology_link_upsert(struct midr_context *ctx,
			      const struct midr_link_update *link)
{
	int ret;

	if (!ctx)
		return -ENOENT;

	ret = shim_validate_link_update(shim_local_node_id(), link);
	if (ret) {
		MIDR_LOG("MIDR shim: link_upsert 校验失败 ret=%d", ret);
		return ret;
	}

	MIDR_LOG("MIDR shim: link_upsert %u->%u link_id=%" PRIu64
		 " rtt=%uus loss=%uppm bw=%ukbps version=%" PRIu64,
		 link->key.local_node_id, link->key.remote_node_id,
		 link->key.link_id, link->metrics.rtt_us,
		 link->metrics.loss_ppm,
		 link->metrics.available_bandwidth_kbps, link->version);

	/* TODO(轮 2)：此处转调 E-1（midr_e1_write_to_bgpls）重发 Link NLRI。 */
	return 0;
}

int midr_topology_link_withdraw(struct midr_context *ctx,
				const struct midr_link_key *key,
				uint64_t version)
{
	int ret;

	if (!ctx)
		return -ENOENT;

	ret = shim_validate_link_withdraw(shim_local_node_id(), key);
	if (ret) {
		MIDR_LOG("MIDR shim: link_withdraw 校验失败 ret=%d", ret);
		return ret;
	}

	MIDR_LOG("MIDR shim: link_withdraw %u->%u link_id=%" PRIu64
		 " version=%" PRIu64,
		 key->local_node_id, key->remote_node_id, key->link_id, version);

	/* TODO(轮 2)：此处转调链路撤销路径。 */
	return 0;
}

int midr_topology_resync_begin(struct midr_context *ctx,
			       enum midr_topology_resync_reason reason)
{
	if (!ctx)
		return -ENOENT;
	if (reason != MIDR_TOPOLOGY_RESYNC_VERSION_LOST &&
	    reason != MIDR_TOPOLOGY_RESYNC_PROVIDER_RESTART &&
	    reason != MIDR_TOPOLOGY_RESYNC_STATE_INCONSISTENT)
		return -EINVAL;

	/* shim 期没有 LSDB 可重同步。轮 5 收尾时才真正用到这条路。 */
	MIDR_LOG("MIDR shim: resync_begin reason=%d（shim 无实际动作）", reason);
	return 0;
}

/* ===========================================================================
 * 远端视图（第二组实现、我方消费）—— 轮 4 收包侧切换才用
 * =========================================================================*/

int midr_remote_view_snapshot_get(struct midr_context *ctx,
				  struct midr_remote_view_snapshot *snapshot)
{
	(void)ctx;

	if (!snapshot)
		return -EINVAL;

	memset(snapshot, 0, sizeof(*snapshot));

	/* shim 期没有 LSDB。沿用他们 weak 桩的约定：provider 不在就 -ENOSYS。 */
	return -ENOSYS;
}

void midr_remote_view_snapshot_release(struct midr_context *ctx,
				       struct midr_remote_view_snapshot *snapshot)
{
	(void)ctx;

	if (snapshot)
		memset(snapshot, 0, sizeof(*snapshot));
}

int midr_remote_view_callbacks_register(
	struct midr_context *ctx,
	const struct midr_remote_view_callbacks *callbacks)
{
	static struct midr_remote_view_callbacks shim_callbacks;

	if (!ctx)
		return -ENOENT;
	if (!callbacks)
		return -EINVAL;

	/* 收下即可，shim 期永不回调（没有 LSDB 事件源）。轮 4 换真实现后，
	 * 他们从 LSDB 新旧状态 diff 里调这些函数（他们
	 * bgp_midr_lsdb.c:902 midr_lsdb_remote_update_iter / :865 _withdraw_iter）。
	 * ⚠ 那条路径**没有任何去抖或缓冲**：entry 一变 unusable 就立刻
	 * remote_*_withdraw —— 闪断/GR 会不会虚报「消失+再现」取决于他们 LSDB
	 * 的 usable 语义，正是《致第二组-失联语义与远端视图接口对接》的核心确认问。 */
	shim_callbacks = *callbacks;
	MIDR_LOG("MIDR shim: remote_view_callbacks 已登记（shim 期不会触发）node_up=%d node_wd=%d link_up=%d link_wd=%d",
		 !!shim_callbacks.remote_node_update,
		 !!shim_callbacks.remote_node_withdraw,
		 !!shim_callbacks.remote_link_update,
		 !!shim_callbacks.remote_link_withdraw);
	return 0;
}

/* ===========================================================================
 * 本端 peer/session 封装（第二组实现、我方调用）
 *
 * 我方现阶段**不用**它：已核对他们 bgp_midr.c 的 request()，对
 * update_source / ebgp_multihop / password / policy_tags 一律 -ENOTSUP，也不撤
 * FRR 自动附送的 IPv4 单播，release() 只 deactivate 不删 peer —— 我方 overlay
 * 会话是 loopback 多跳 eBGP，multihop + update-source 是硬需求，建不出来
 * （问题清单 #10）。轮 4 继续用我方 midr_nds_ctrl_setup_overlay_peer。
 *
 * 所以这两个 shim 只保证可链接，直接返回 -ENOTSUP。
 * =========================================================================*/

int midr_peer_session_request(struct midr_context *ctx,
			      const struct midr_peer_session_request_info *req)
{
	(void)req;

	if (!ctx)
		return -ENOENT;

	MIDR_LOG("MIDR shim: peer_session_request 未实现（我方用自己的建连 helper）");
	return -ENOTSUP;
}

int midr_peer_session_release(struct midr_context *ctx,
			      const union sockunion *remote_address, afi_t afi,
			      safi_t safi, enum midr_peer_release_reason reason)
{
	(void)remote_address;
	(void)afi;
	(void)safi;
	(void)reason;

	if (!ctx)
		return -ENOENT;

	MIDR_LOG("MIDR shim: peer_session_release 未实现（我方用自己的拆连路径）");
	return -ENOTSUP;
}
