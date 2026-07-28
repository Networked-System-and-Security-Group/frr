// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR liveness confirmation and failure gossip.
 *
 * Direct ALIVE evidence is still the periodically refreshed BGP-LS Node
 * NLRI.  A local timeout or ordinary MP_UNREACH only moves a node to
 * SUSPECT.  This module then asks a stable sample of Established MIDR
 * neighbors; one ALIVE response restores the node, while a configured
 * majority of STALE responses commits removal and gossips DEAD.
 */

#include <zebra.h>

#include "memory.h"
#include "frrevent.h"
#include "linklist.h"
#include "log.h"
#include "monotime.h"
#include "network.h"
#include "prefix.h"
#include "vty.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr.h"
#include "bgpd/bgp_midr_ctrl.h"
#include "bgpd/bgp_midr_liveness.h"
#include "bgpd/bgp_debug.h"

#define MIDR_LIVENESS_TICK_INTERVAL 1
#define MIDR_LIVENESS_AGE_UNKNOWN UINT32_MAX
#define MIDR_LIVENESS_AGE_MAX (UINT32_MAX - 1)
#define MIDR_LIVENESS_DEFAULT_MAX_INFLIGHT_ROUNDS 32
#define MIDR_LIVENESS_LEAVE_RETX_COUNT 2
#define MIDR_LIVENESS_LEAVE_RETX_INTERVAL_MS 500
#define MIDR_LIVENESS_LEAVE_RETX_PENDING_MAX 64

DEFINE_MTYPE_STATIC(BGPD, BGP_MIDR_LIVENESS, "MIDR liveness context");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LIVENESS_ROUND, "MIDR liveness round");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LIVENESS_SEEN, "MIDR liveness seen rumor");
DEFINE_MTYPE_STATIC(BGPD, MIDR_LIVENESS_RETX, "MIDR liveness retransmit");

struct midr_liveness_wire_hdr {
	uint8_t version;
	uint8_t type;
	uint16_t length;
	uint32_t message_id;
	struct in_addr origin;
};

struct midr_liveness_probe_wire {
	struct midr_liveness_wire_hdr hdr;
	struct in_addr subject;
	uint32_t requester_age;
};

struct midr_liveness_probe_resp_wire {
	struct midr_liveness_wire_hdr hdr;
	struct in_addr subject;
	uint8_t result;
	uint8_t reserved[3];
	uint32_t age;
};

struct midr_liveness_dead_wire {
	struct midr_liveness_wire_hdr hdr;
	struct in_addr subject;
	struct in_addr subject_transport;
	uint32_t round_id;
	uint8_t hop_limit;
	uint8_t voter_count;
	uint16_t declared_quorum;
};

struct midr_liveness_leave_wire {
	struct midr_liveness_wire_hdr hdr;
	struct in_addr subject;
	struct in_addr subject_transport;
	uint8_t hop_limit;
	uint8_t reserved[3];
};

_Static_assert(sizeof(struct midr_liveness_wire_hdr) == 12,
	       "MIDR liveness header padding changed");
_Static_assert(sizeof(struct midr_liveness_probe_wire) == 20,
	       "MIDR liveness probe padding changed");
_Static_assert(sizeof(struct midr_liveness_probe_resp_wire) == 24,
	       "MIDR liveness response padding changed");
_Static_assert(sizeof(struct midr_liveness_dead_wire) == 28,
	       "MIDR liveness dead padding changed");
_Static_assert(sizeof(struct midr_liveness_leave_wire) == 24,
	       "MIDR liveness leave padding changed");

struct midr_liveness_voter {
	struct in_addr node_id;
	struct in_addr transport;
	bool responded;
	enum midr_liveness_probe_result result;
};

enum midr_liveness_round_state {
	MIDR_LIVENESS_ROUND_DELAYED,
	MIDR_LIVENESS_ROUND_WAITING,
	MIDR_LIVENESS_ROUND_BACKOFF,
};

enum midr_liveness_detector_tier {
	MIDR_LIVENESS_DETECTOR_DIRECT,
	MIDR_LIVENESS_DETECTOR_SAME_GROUP,
	MIDR_LIVENESS_DETECTOR_REMOTE,
};

struct midr_liveness_round {
	struct prefix subject;
	uint32_t round_id;
	uint32_t required_quorum;
	uint32_t requester_age;
	struct midr_liveness_voter voters[MIDR_LIVENESS_MAX_VOTERS];
	size_t voter_count;
	time_t started_at;
	time_t deadline;
	enum midr_liveness_round_state state;
	enum midr_liveness_detector_tier tier;
	bool counted_inflight;
};

struct midr_liveness_seen {
	uint8_t type;
	struct in_addr origin;
	uint32_t message_id;
	struct in_addr subject;
	struct in_addr transport;
	uint8_t max_hop_limit;
	bool has_transport;
	bool blocks_indirect_discovery;
	time_t expires_at;
};

enum midr_liveness_seen_result {
	MIDR_LIVENESS_SEEN_DUPLICATE,
	MIDR_LIVENESS_SEEN_FIRST,
	MIDR_LIVENESS_SEEN_FORWARD_ONLY,
	MIDR_LIVENESS_SEEN_SUBJECT_SUPPRESSED,
};

struct midr_liveness_retx {
	struct bgp *bgp;
	struct event *timer;
	uint8_t payload[MIDR_LIVENESS_MAX_WIRE_SIZE];
	size_t length;
	struct sockaddr_in source;
	struct in_addr subject;
	bool has_source;
	uint8_t retries_left;
};

struct midr_liveness {
	struct midr_liveness_config config;
	struct list *rounds;
	struct list *seen;
	struct list *leave_retx;
	struct event *t_self_advertise;
	uint32_t tx_counter;
	uint32_t inflight_rounds;
	uint32_t max_inflight_rounds;
	time_t next_node_scan;
};

static struct midr_liveness *midr_liveness_ctx(const struct bgp *bgp);
static void midr_liveness_keepalive_timer(struct event *event);
static void midr_liveness_self_advertise(struct event *event);
static void midr_liveness_round_begin(struct bgp *bgp,
				      struct midr_liveness_round *round,
				      time_t now);
static bool midr_liveness_recent_transport_blocked(
	struct midr_liveness *ctx, struct in_addr transport);
static void midr_liveness_recover(struct bgp *bgp,
				  struct midr_node_entry *entry, bool direct);
static void midr_liveness_leave_retx_cancel_subject(
	struct midr_liveness *ctx, struct in_addr subject);
static uint32_t midr_liveness_initial_delay(
	const struct bgp *bgp, const struct midr_liveness *ctx,
	const struct midr_liveness_round *round);
static void midr_liveness_round_schedule(
	struct midr_liveness *ctx, struct midr_liveness_round *round,
	enum midr_liveness_round_state state, time_t deadline);

static void midr_liveness_rearm_keepalive(struct bgp *bgp)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct bgp_midr *mi;

	if (!ctx)
		return;
	mi = bgp->midr_info;
	event_cancel(&mi->t_keepalive);
	event_add_timer(bm->master, midr_liveness_keepalive_timer, bgp,
			ctx->config.keepalive_interval, &mi->t_keepalive);
}

static struct midr_liveness *midr_liveness_ctx(const struct bgp *bgp)
{
	if (!bgp || !bgp->midr_info)
		return NULL;
	return bgp->midr_info->liveness;
}

static void midr_liveness_prefix_from_addr(struct prefix *prefix,
					   struct in_addr address)
{
	memset(prefix, 0, sizeof(*prefix));
	prefix->family = AF_INET;
	prefix->prefixlen = IPV4_MAX_BITLEN;
	prefix->u.prefix4 = address;
}

static struct midr_node_entry *
midr_liveness_find_node(struct bgp *bgp, const struct prefix *node_id)
{
	struct midr_node_entry key = {};

	if (!bgp || !bgp->midr_info || !node_id)
		return NULL;
	prefix_copy(&key.node_id, node_id);
	return midr_node_hash_find(&bgp->midr_info->global_view->nodes, &key);
}

static struct midr_node_entry *
midr_liveness_find_node_addr(struct bgp *bgp, struct in_addr node_id)
{
	struct prefix prefix;

	midr_liveness_prefix_from_addr(&prefix, node_id);
	return midr_liveness_find_node(bgp, &prefix);
}

static uint32_t midr_liveness_age(time_t now, time_t last_seen)
{
	time_t age;

	if (last_seen <= 0)
		return MIDR_LIVENESS_AGE_UNKNOWN;
	if (now <= last_seen)
		return 0;
	age = now - last_seen;
	if ((uint64_t)age > MIDR_LIVENESS_AGE_MAX)
		return MIDR_LIVENESS_AGE_MAX;
	return (uint32_t)age;
}

static bool midr_liveness_direct_evidence_fresh(
	const struct midr_liveness *ctx, const struct midr_node_entry *entry,
	time_t now)
{
	uint32_t age;

	if (!ctx || !entry)
		return false;
	age = midr_liveness_age(now, entry->last_seen);
	return age != MIDR_LIVENESS_AGE_UNKNOWN &&
	       age <= ctx->config.suspect_timeout;
}

static bool midr_liveness_any_evidence_fresh(
	const struct midr_liveness *ctx, const struct midr_node_entry *entry,
	time_t now)
{
	return midr_liveness_direct_evidence_fresh(ctx, entry, now) ||
	       entry->nontransitive_alive_until > now;
}

void
midr_liveness_config_defaults(struct midr_liveness_config *config)
{
	if (!config)
		return;
	memset(config, 0, sizeof(*config));
	config->keepalive_interval = MIDR_KEEPALIVE_INTERVAL;
	config->suspect_timeout = MIDR_NODE_EXPIRE_TIME;
	config->scan_interval = MIDR_EXPIRE_CHECK_INTERVAL;
	config->voter_sample_size =
		MIDR_LIVENESS_DEFAULT_VOTER_SAMPLE_SIZE;
	config->quorum = MIDR_LIVENESS_DEFAULT_QUORUM;
	config->confirm_timeout = MIDR_LIVENESS_DEFAULT_CONFIRM_TIMEOUT;
	config->retry_backoff = MIDR_LIVENESS_DEFAULT_RETRY_BACKOFF;
	config->hop_limit = MIDR_LIVENESS_DEFAULT_HOP_LIMIT;
	config->cache_ttl = MIDR_LIVENESS_DEFAULT_CACHE_TTL;
}

static bool
midr_liveness_config_valid(const struct midr_liveness_config *config)
{
	if (!config || config->keepalive_interval == 0 ||
	    config->keepalive_interval > 3600 ||
	    config->suspect_timeout <= config->keepalive_interval ||
	    config->suspect_timeout > 7200 || config->scan_interval == 0 ||
	    config->scan_interval > 3600 ||
	    config->scan_interval > config->suspect_timeout ||
	    config->confirm_timeout == 0 || config->confirm_timeout > 300 ||
	    config->retry_backoff == 0 || config->retry_backoff > 300 ||
	    config->cache_ttl == 0 || config->cache_ttl > 3600)
		return false;
	if (config->voter_sample_size < 2 ||
	    config->voter_sample_size > MIDR_LIVENESS_MAX_VOTERS ||
	    config->quorum < 2 || config->quorum > config->voter_sample_size ||
	    config->quorum <= config->voter_sample_size / 2)
		return false;
	if (config->hop_limit == 0 || config->hop_limit > UINT8_MAX)
		return false;
	return true;
}

bool midr_liveness_get_config(const struct bgp *bgp,
			      struct midr_liveness_config *config)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);

	if (!config)
		return false;
	if (!ctx) {
		midr_liveness_config_defaults(config);
		return false;
	}
	*config = ctx->config;
	return true;
}

bool midr_liveness_set_config(struct bgp *bgp,
			      const struct midr_liveness_config *config)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct listnode *node;
	struct midr_liveness_round *round;
	struct midr_liveness_seen *seen;
	struct midr_node_entry *entry;
	bool keepalive_changed;
	bool round_changed;
	bool scan_changed;
	bool cache_changed;
	uint32_t old_cache_ttl;
	uint32_t old_suspect_timeout;
	time_t now;

	if (!ctx || !midr_liveness_config_valid(config))
		return false;
	keepalive_changed =
		ctx->config.keepalive_interval != config->keepalive_interval;
	round_changed =
		ctx->config.voter_sample_size != config->voter_sample_size ||
		ctx->config.quorum != config->quorum ||
		ctx->config.confirm_timeout != config->confirm_timeout ||
		ctx->config.retry_backoff != config->retry_backoff;
	scan_changed = ctx->config.suspect_timeout != config->suspect_timeout ||
		       ctx->config.scan_interval != config->scan_interval;
	cache_changed = ctx->config.cache_ttl != config->cache_ttl;
	old_cache_ttl = ctx->config.cache_ttl;
	old_suspect_timeout = ctx->config.suspect_timeout;
	ctx->config = *config;
	now = monotime(NULL);
	if (scan_changed)
		ctx->next_node_scan = now + ctx->config.scan_interval;
	if (keepalive_changed)
		midr_liveness_rearm_keepalive(bgp);
	/*
	 * A weak-evidence lease is never extended by a configuration change.
	 * When the timeout is reduced, cap outstanding leases to the new upper
	 * bound so old policy cannot keep a node ACTIVE for longer than the new
	 * policy allows.
	 */
	if (ctx->config.suspect_timeout < old_suspect_timeout)
		frr_each (midr_node_hash,
			  &bgp->midr_info->global_view->nodes, entry) {
			time_t cap = now + ctx->config.suspect_timeout;

			if (entry->nontransitive_alive_until > cap)
				entry->nontransitive_alive_until = cap;
		}
	/* Only a timeout-only suspicion can become invalid merely because the
	 * operator raised the timeout.  A WITHDRAW remains independent evidence.
	 */
	if (ctx->config.suspect_timeout > old_suspect_timeout)
		frr_each_safe (midr_node_hash,
			       &bgp->midr_info->global_view->nodes, entry) {
			if (entry->liveness_state == MIDR_NODE_SUSPECT &&
			    entry->liveness_suspect_causes ==
				    MIDR_NODE_SUSPECT_TIMEOUT) {
				if (midr_liveness_direct_evidence_fresh(
					    ctx, entry, now))
					midr_liveness_recover(bgp, entry, true);
				else if (entry->nontransitive_alive_until >
					 now)
					midr_liveness_recover(bgp, entry, false);
			}
		}
	/* Re-snapshot voters and issue a new round-id so responses to the old
	 * runtime configuration cannot commit under a newly displayed policy.
	 */
	if (round_changed)
		for (ALL_LIST_ELEMENTS_RO(ctx->rounds, node, round)) {
			if (round->state == MIDR_LIVENESS_ROUND_WAITING)
				midr_liveness_round_begin(bgp, round, now);
			else if (round->state ==
				 MIDR_LIVENESS_ROUND_DELAYED)
				round->deadline =
					now + midr_liveness_initial_delay(
						      bgp, ctx, round);
			else
				midr_liveness_round_schedule(
					ctx, round,
					MIDR_LIVENESS_ROUND_BACKOFF,
					now + ctx->config.retry_backoff);
		}
	if (cache_changed)
		for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen))
			seen->expires_at += (time_t)ctx->config.cache_ttl -
					    (time_t)old_cache_ttl;
	return true;
}

bool midr_liveness_node_usable(const struct midr_node_entry *entry)
{
	return entry && entry->liveness_state == MIDR_NODE_ACTIVE;
}

const char *midr_liveness_state_name(const struct midr_node_entry *entry)
{
	if (!entry)
		return "unknown";
	if (entry->liveness_state == MIDR_NODE_SUSPECT)
		return "suspect";
	if (entry->liveness_state == MIDR_NODE_REMOVING)
		return "removing";
	return "active";
}

bool midr_liveness_transport_usable(struct bgp *bgp,
				    struct in_addr transport)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return true;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		struct in_addr locator = entry->has_transport_addr
						? entry->transport_addr
						: entry->node_id.u.prefix4;

		if (IPV4_ADDR_SAME(&locator, &transport) &&
		    !midr_liveness_node_usable(entry))
			return false;
	}
	return !midr_liveness_recent_transport_blocked(
		midr_liveness_ctx(bgp), transport);
}

bool midr_liveness_endpoint_usable(struct bgp *bgp,
				   const struct prefix *endpoint)
{
	struct midr_node_entry *entry;

	if (!bgp || !endpoint || endpoint->family != AF_INET)
		return false;
	if (!midr_liveness_indirect_discovery_allowed(bgp, endpoint))
		return false;
	entry = midr_liveness_find_node(bgp, endpoint);
	if (entry && !midr_liveness_node_usable(entry))
		return false;
	return midr_liveness_transport_usable(bgp, endpoint->u.prefix4);
}

bool midr_liveness_indirect_endpoint_usable(
	struct bgp *bgp, const struct prefix *node_id,
	const struct in_addr *transport)
{
	struct midr_node_entry *entry;

	if (!bgp)
		return false;
	if (node_id) {
		if (node_id->family != AF_INET ||
		    !midr_liveness_indirect_discovery_allowed(bgp, node_id))
			return false;
		entry = midr_liveness_find_node(bgp, node_id);
		if (entry && !midr_liveness_node_usable(entry))
			return false;
	}
	if (transport)
		return midr_liveness_transport_usable(bgp, *transport);
	if (node_id)
		return midr_liveness_transport_usable(
			bgp, node_id->u.prefix4);
	return true;
}

static uint32_t midr_liveness_next_id(struct midr_liveness *ctx)
{
	ctx->tx_counter++;
	if (ctx->tx_counter == 0)
		ctx->tx_counter++;
	return ctx->tx_counter;
}

static void midr_liveness_fill_hdr(struct midr_liveness_wire_hdr *hdr,
				   uint8_t type, size_t length,
				   uint32_t message_id,
				   struct in_addr origin)
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->version = MIDR_CTRL_MSG_VERSION;
	hdr->type = type;
	hdr->length = htons((uint16_t)length);
	hdr->message_id = htonl(message_id);
	hdr->origin = origin;
}

static bool midr_liveness_udp_send(struct bgp *bgp, struct in_addr address,
				   const void *payload, size_t length)
{
	struct sockaddr_in destination = {};

	destination.sin_family = AF_INET;
	destination.sin_port = htons(MIDR_CTRL_UDP_PORT);
	destination.sin_addr = address;
	return midr_ctrl_udp_send(bgp, &destination, payload, length) == 0;
}

static bool
midr_liveness_voter_eligible(struct bgp *bgp,
			     const struct midr_node_entry *entry,
			     const struct prefix *subject)
{
	if (!entry || entry->is_self || !entry->is_adjacent ||
	    !entry->has_transport_addr || !midr_liveness_node_usable(entry))
		return false;
	if (subject && prefix_same(&entry->node_id, subject))
		return false;
	return midr_node_established_peer(bgp, &entry->node_id) != NULL;
}

static bool
midr_liveness_gossip_peer_eligible(struct bgp *bgp,
				   const struct midr_node_entry *entry)
{
	if (!entry || entry->is_self || !entry->has_transport_addr ||
	    !midr_liveness_node_usable(entry))
		return false;
	/*
	 * Gossip may use every active Established MIDR session, including a
	 * statically configured or cross-group peer.  Voting remains limited
	 * to semantic adjacencies by midr_liveness_voter_eligible().
	 */
	return midr_node_established_peer(bgp, &entry->node_id) != NULL;
}

static void midr_liveness_gossip_raw(struct bgp *bgp, const uint8_t *payload,
				     size_t length,
				     const struct sockaddr_in *source)
{
	struct midr_node_entry *entry;

	if (!bgp || !bgp->midr_info)
		return;
	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (!midr_liveness_gossip_peer_eligible(bgp, entry))
			continue;
		if (source && IPV4_ADDR_SAME(&source->sin_addr,
					  &entry->transport_addr))
			continue;
		midr_liveness_udp_send(bgp, entry->transport_addr, payload,
					   length);
	}
}

static void
midr_liveness_leave_retx_delete(struct midr_liveness *ctx,
				struct midr_liveness_retx *retry)
{
	struct listnode *node, *next;
	struct midr_liveness_retx *candidate;

	if (!ctx || !retry)
		return;
	for (ALL_LIST_ELEMENTS(ctx->leave_retx, node, next, candidate)) {
		if (candidate != retry)
			continue;
		event_cancel(&candidate->timer);
		list_delete_node(ctx->leave_retx, node);
		XFREE(MTYPE_MIDR_LIVENESS_RETX, candidate);
		return;
	}
}

static void midr_liveness_leave_retx_timer(struct event *event)
{
	struct midr_liveness_retx *retry = EVENT_ARG(event);
	struct midr_liveness *ctx;

	if (!retry)
		return;
	retry->timer = NULL;
	ctx = midr_liveness_ctx(retry->bgp);
	if (!ctx) {
		XFREE(MTYPE_MIDR_LIVENESS_RETX, retry);
		return;
	}
	midr_liveness_gossip_raw(
		retry->bgp, retry->payload, retry->length,
		retry->has_source ? &retry->source : NULL);
	if (--retry->retries_left == 0) {
		midr_liveness_leave_retx_delete(ctx, retry);
		return;
	}
	event_add_timer_msec(bm->master, midr_liveness_leave_retx_timer,
			     retry, MIDR_LIVENESS_LEAVE_RETX_INTERVAL_MS,
			     &retry->timer);
}

static void midr_liveness_leave_retx_schedule(
	struct bgp *bgp, const uint8_t *payload, size_t length,
	const struct sockaddr_in *source)
{
	struct midr_liveness_leave_wire leave;
	struct midr_liveness_leave_wire pending;
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_retx *retry;
	struct listnode *node;

	if (!ctx || !payload || length != sizeof(leave))
		return;
	memcpy(&leave, payload, sizeof(leave));
	for (ALL_LIST_ELEMENTS_RO(ctx->leave_retx, node, retry)) {
		memcpy(&pending, retry->payload, sizeof(pending));
		if (pending.hdr.message_id != leave.hdr.message_id ||
		    !IPV4_ADDR_SAME(&pending.hdr.origin,
				    &leave.hdr.origin))
			continue;
		/*
		 * A later copy with more remaining reach replaces the pending
		 * payload; it does not create a second retransmission burst.
		 */
		if (leave.hop_limit > pending.hop_limit) {
			memcpy(retry->payload, payload, length);
			retry->length = length;
			if (source) {
				retry->source = *source;
				retry->has_source = true;
			} else
				retry->has_source = false;
		}
		return;
	}
	if (listcount(ctx->leave_retx) >=
	    MIDR_LIVENESS_LEAVE_RETX_PENDING_MAX)
		return;
	retry = XCALLOC(MTYPE_MIDR_LIVENESS_RETX, sizeof(*retry));
	retry->bgp = bgp;
	memcpy(retry->payload, payload, length);
	retry->length = length;
	retry->subject = leave.subject;
	retry->retries_left = MIDR_LIVENESS_LEAVE_RETX_COUNT;
	if (source) {
		retry->source = *source;
		retry->has_source = true;
	}
	listnode_add(ctx->leave_retx, retry);
	event_add_timer_msec(bm->master, midr_liveness_leave_retx_timer,
			     retry, MIDR_LIVENESS_LEAVE_RETX_INTERVAL_MS,
			     &retry->timer);
}

static void midr_liveness_leave_retx_cancel_subject(
	struct midr_liveness *ctx, struct in_addr subject)
{
	struct listnode *node, *next;
	struct midr_liveness_retx *retry;

	if (!ctx)
		return;
	for (ALL_LIST_ELEMENTS(ctx->leave_retx, node, next, retry)) {
		if (!IPV4_ADDR_SAME(&retry->subject, &subject))
			continue;
		event_cancel(&retry->timer);
		list_delete_node(ctx->leave_retx, node);
		XFREE(MTYPE_MIDR_LIVENESS_RETX, retry);
	}
}

static void midr_liveness_seen_gc(struct midr_liveness *ctx, time_t now)
{
	struct listnode *node, *next;
	struct midr_liveness_seen *seen;

	for (ALL_LIST_ELEMENTS(ctx->seen, node, next, seen)) {
		if (seen->expires_at > now)
			continue;
		list_delete_node(ctx->seen, node);
		XFREE(MTYPE_MIDR_LIVENESS_SEEN, seen);
	}
}

static bool midr_liveness_recent_transport_blocked(
	struct midr_liveness *ctx, struct in_addr transport)
{
	struct listnode *node;
	struct midr_liveness_seen *seen;
	time_t now;

	if (!ctx)
		return false;
	now = monotime(NULL);
	midr_liveness_seen_gc(ctx, now);
	for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen))
		if (seen->blocks_indirect_discovery && seen->has_transport &&
		    IPV4_ADDR_SAME(&seen->transport, &transport))
			return true;
	return false;
}

bool midr_liveness_indirect_discovery_allowed(
	struct bgp *bgp, const struct prefix *node_id)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct listnode *node;
	struct midr_liveness_seen *seen;
	time_t now;

	if (!ctx || !node_id || node_id->family != AF_INET)
		return true;
	now = monotime(NULL);
	midr_liveness_seen_gc(ctx, now);
	for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen))
		if (seen->blocks_indirect_discovery &&
		    IPV4_ADDR_SAME(&seen->subject, &node_id->u.prefix4))
			return false;
	return true;
}

static enum midr_liveness_seen_result midr_liveness_seen_accept(
	struct bgp *bgp, struct midr_liveness *ctx, uint8_t type,
	struct in_addr origin, uint32_t message_id, struct in_addr subject,
	struct in_addr subject_transport, uint8_t hop_limit, time_t now)
{
	struct listnode *node;
	struct midr_liveness_seen *seen;
	struct midr_liveness_seen *exact = NULL;
	struct midr_liveness_seen *subject_seen = NULL;
	struct midr_node_entry *entry;
	struct prefix locator;
	uint8_t subject_max_hop = 0;

	midr_liveness_seen_gc(ctx, now);
	for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen))
		if (seen->type == type && seen->message_id == message_id &&
		    IPV4_ADDR_SAME(&seen->origin, &origin)) {
			/*
			 * A direct rejoin clears the quarantine bit but retains
			 * the dedup record.  Never let a delayed higher-hop copy
			 * of that resolved event restart propagation.
			 */
			if (!seen->blocks_indirect_discovery ||
			    !IPV4_ADDR_SAME(&seen->subject, &subject) ||
			    hop_limit <= seen->max_hop_limit)
				return MIDR_LIVENESS_SEEN_DUPLICATE;
			exact = seen;
			break;
		}

	/*
	 * Independent detectors may create different message IDs for the same
	 * DEAD subject.  Merge their waves by subject while still allowing a
	 * later copy with a larger remaining radius to progress.
	 */
	if (type == MIDR_LIVENESS_DEAD)
		for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen)) {
			if (seen->type != MIDR_LIVENESS_DEAD ||
			    !seen->blocks_indirect_discovery ||
			    !IPV4_ADDR_SAME(&seen->subject, &subject))
				continue;
			if (!subject_seen ||
			    seen->max_hop_limit > subject_max_hop) {
				subject_seen = seen;
				subject_max_hop = seen->max_hop_limit;
			}
		}
	if (subject_seen) {
		if (hop_limit <= subject_max_hop)
			return MIDR_LIVENESS_SEEN_SUBJECT_SUPPRESSED;
		subject_seen->max_hop_limit = hop_limit;
		if (exact)
			exact->max_hop_limit = hop_limit;
		return MIDR_LIVENESS_SEEN_FORWARD_ONLY;
	}
	if (exact) {
		exact->max_hop_limit = hop_limit;
		return MIDR_LIVENESS_SEEN_FORWARD_ONLY;
	}

	seen = XCALLOC(MTYPE_MIDR_LIVENESS_SEEN, sizeof(*seen));
	seen->type = type;
	seen->origin = origin;
	seen->message_id = message_id;
	seen->subject = subject;
	seen->max_hop_limit = hop_limit;
	seen->blocks_indirect_discovery = true;
	if (subject_transport.s_addr != INADDR_ANY) {
		seen->transport = subject_transport;
		seen->has_transport = true;
	}
	entry = midr_liveness_find_node_addr(bgp, subject);
	if (!seen->has_transport && entry) {
		midr_node_get_locator(entry, &locator);
		if (locator.family == AF_INET) {
			seen->transport = locator.u.prefix4;
			seen->has_transport = true;
		}
	}
	seen->expires_at = now + ctx->config.cache_ttl;
	listnode_add(ctx->seen, seen);
	return MIDR_LIVENESS_SEEN_FIRST;
}

static uint32_t midr_liveness_mix32(uint32_t value)
{
	value ^= value >> 16;
	value *= 0x7feb352dU;
	value ^= value >> 15;
	value *= 0x846ca68bU;
	value ^= value >> 16;
	return value;
}

static uint32_t midr_liveness_round_jitter(
	const struct bgp *bgp, const struct midr_liveness_round *round,
	uint32_t span)
{
	uint32_t seed;

	if (span == 0)
		return 0;
	seed = ntohl(bgp->router_id.s_addr) ^
	       (ntohl(round->subject.u.prefix4.s_addr) * 0x9e3779b9U);
	return midr_liveness_mix32(seed) % span;
}

static enum midr_liveness_detector_tier
midr_liveness_detector_tier(const struct bgp *bgp,
			    const struct midr_node_entry *entry)
{
	if (entry->is_adjacent)
		return MIDR_LIVENESS_DETECTOR_DIRECT;
	if (entry->group_id != 0 &&
	    entry->group_id == bgp->midr_info->local_group_id)
		return MIDR_LIVENESS_DETECTOR_SAME_GROUP;
	return MIDR_LIVENESS_DETECTOR_REMOTE;
}

static uint32_t midr_liveness_initial_delay(
	const struct bgp *bgp, const struct midr_liveness *ctx,
	const struct midr_liveness_round *round)
{
	switch (round->tier) {
	case MIDR_LIVENESS_DETECTOR_DIRECT:
		return midr_liveness_round_jitter(bgp, round, 2);
	case MIDR_LIVENESS_DETECTOR_SAME_GROUP:
		return ctx->config.confirm_timeout +
		       midr_liveness_round_jitter(
			       bgp, round, ctx->config.scan_interval + 1);
	case MIDR_LIVENESS_DETECTOR_REMOTE:
		return 2 * ctx->config.confirm_timeout +
		       midr_liveness_round_jitter(
			       bgp, round, ctx->config.retry_backoff + 1);
	}
	return 0;
}

static void midr_liveness_round_leave_waiting(
	struct midr_liveness *ctx, struct midr_liveness_round *round)
{
	if (!round->counted_inflight)
		return;
	if (ctx->inflight_rounds > 0)
		ctx->inflight_rounds--;
	round->counted_inflight = false;
}

static void midr_liveness_round_schedule(
	struct midr_liveness *ctx, struct midr_liveness_round *round,
	enum midr_liveness_round_state state, time_t deadline)
{
	midr_liveness_round_leave_waiting(ctx, round);
	round->state = state;
	round->deadline = deadline;
}

static const char *midr_liveness_round_state_name(
	const struct midr_liveness_round *round)
{
	switch (round->state) {
	case MIDR_LIVENESS_ROUND_DELAYED:
		return "delayed";
	case MIDR_LIVENESS_ROUND_WAITING:
		return "waiting";
	case MIDR_LIVENESS_ROUND_BACKOFF:
		return "backoff";
	}
	return "unknown";
}

static struct midr_liveness_round *
midr_liveness_round_find(struct midr_liveness *ctx,
			 const struct prefix *subject)
{
	struct listnode *node;
	struct midr_liveness_round *round;

	for (ALL_LIST_ELEMENTS_RO(ctx->rounds, node, round))
		if (prefix_same(&round->subject, subject))
			return round;
	return NULL;
}

static void midr_liveness_round_delete(struct midr_liveness *ctx,
				       struct midr_liveness_round *round)
{
	struct listnode *node, *next;
	struct midr_liveness_round *candidate;

	for (ALL_LIST_ELEMENTS(ctx->rounds, node, next, candidate)) {
		if (candidate != round)
			continue;
		midr_liveness_round_leave_waiting(ctx, candidate);
		list_delete_node(ctx->rounds, node);
		XFREE(MTYPE_MIDR_LIVENESS_ROUND, candidate);
		return;
	}
}

static void midr_liveness_round_counts(
	const struct midr_liveness_round *round, size_t *responses,
	size_t *stale_votes)
{
	size_t i;

	*responses = 0;
	*stale_votes = 0;
	for (i = 0; i < round->voter_count; i++) {
		if (!round->voters[i].responded)
			continue;
		(*responses)++;
		if (round->voters[i].result == MIDR_LIVENESS_RESULT_STALE)
			(*stale_votes)++;
	}
}

static size_t
midr_liveness_select_voters(struct bgp *bgp,
			    const struct prefix *subject,
			    struct midr_liveness_voter *voters,
			    size_t limit)
{
	struct midr_node_entry *entry;
	size_t count = 0;

	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		struct midr_liveness_voter candidate = {};
		uint32_t candidate_id;
		size_t position = 0;

		if (!midr_liveness_voter_eligible(bgp, entry, subject))
			continue;
		candidate.node_id = entry->node_id.u.prefix4;
		candidate.transport = entry->transport_addr;
		candidate_id = ntohl(candidate.node_id.s_addr);
		while (position < count &&
		       ntohl(voters[position].node_id.s_addr) < candidate_id)
			position++;

		if (count < limit) {
			memmove(&voters[position + 1], &voters[position],
				(count - position) * sizeof(*voters));
			voters[position] = candidate;
			count++;
		} else if (position < limit) {
			memmove(&voters[position + 1], &voters[position],
				(limit - position - 1) * sizeof(*voters));
			voters[position] = candidate;
		}
	}
	return count;
}

static void midr_liveness_round_begin(struct bgp *bgp,
				      struct midr_liveness_round *round,
				      time_t now)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_probe_wire request = {};
	struct midr_node_entry *entry;
	size_t i;

	if (!ctx || !round)
		return;
	midr_liveness_round_leave_waiting(ctx, round);
	if (ctx->inflight_rounds >= ctx->max_inflight_rounds) {
		midr_liveness_round_schedule(
			ctx, round, MIDR_LIVENESS_ROUND_BACKOFF,
			now + 1 + midr_liveness_round_jitter(bgp, round, 2));
		MIDR_LOG("MIDR liveness: defer %pFX; %u/%u rounds in flight",
			 &round->subject, ctx->inflight_rounds,
			 ctx->max_inflight_rounds);
		return;
	}

	memset(round->voters, 0, sizeof(round->voters));
	round->voter_count = midr_liveness_select_voters(
		bgp, &round->subject, round->voters,
		ctx->config.voter_sample_size);
	round->round_id = midr_liveness_next_id(ctx);
	round->required_quorum = round->voter_count < ctx->config.quorum
					 ? round->voter_count
					 : ctx->config.quorum;

	/* One voter can never constitute a majority confirmation. */
	if (round->voter_count < 2 || round->required_quorum < 2) {
		midr_liveness_round_schedule(
			ctx, round, MIDR_LIVENESS_ROUND_BACKOFF,
			now + ctx->config.retry_backoff);
		MIDR_LOG("MIDR liveness: %pFX remains SUSPECT; only %zu eligible voter(s)",
			 &round->subject, round->voter_count);
		return;
	}

	midr_liveness_fill_hdr(&request.hdr, MIDR_LIVENESS_PROBE_REQ,
				 sizeof(request), round->round_id,
				 bgp->router_id);
	request.subject = round->subject.u.prefix4;
	entry = midr_liveness_find_node(bgp, &round->subject);
	round->requester_age =
		entry ? midr_liveness_age(now, entry->last_seen)
		      : MIDR_LIVENESS_AGE_UNKNOWN;
	round->started_at = now;
	request.requester_age = htonl(round->requester_age);
	round->state = MIDR_LIVENESS_ROUND_WAITING;
	round->deadline = now + ctx->config.confirm_timeout;
	round->counted_inflight = true;
	ctx->inflight_rounds++;
	for (i = 0; i < round->voter_count; i++)
		midr_liveness_udp_send(bgp, round->voters[i].transport,
					   &request, sizeof(request));

	MIDR_LOG("MIDR liveness: probing %pFX round %u via %zu voters (quorum %u)",
		 &round->subject, round->round_id, round->voter_count,
		 round->required_quorum);
}

static void midr_liveness_mark_suspect(struct bgp *bgp,
				       struct midr_node_entry *entry,
				       enum midr_node_suspect_cause cause,
				       const char *reason)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_round *round;
	time_t now = monotime(NULL);

	if (!ctx || !entry || entry->is_self ||
	    entry->liveness_state == MIDR_NODE_REMOVING)
		return;
	if (entry->liveness_state != MIDR_NODE_SUSPECT) {
		entry->liveness_state = MIDR_NODE_SUSPECT;
		entry->liveness_suspect_causes = cause;
		entry->suspect_since = now;
		MIDR_LOG("MIDR liveness: node %pFX entered SUSPECT (%s)",
			 &entry->node_id, reason);
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
	} else
		entry->liveness_suspect_causes |= cause;

	round = midr_liveness_round_find(ctx, &entry->node_id);
	if (round)
		return;
	round = XCALLOC(MTYPE_MIDR_LIVENESS_ROUND, sizeof(*round));
	prefix_copy(&round->subject, &entry->node_id);
	round->tier = midr_liveness_detector_tier(bgp, entry);
	round->state = MIDR_LIVENESS_ROUND_DELAYED;
	round->deadline =
		now + midr_liveness_initial_delay(bgp, ctx, round);
	listnode_add(ctx->rounds, round);
}

static void midr_liveness_recover(struct bgp *bgp,
				  struct midr_node_entry *entry, bool direct)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_round *round;
	struct midr_liveness_seen *seen;
	struct listnode *node;
	bool recovered;

	if (!ctx || !entry || entry->liveness_state == MIDR_NODE_REMOVING)
		return;
	recovered = entry->liveness_state == MIDR_NODE_SUSPECT;
	entry->liveness_state = MIDR_NODE_ACTIVE;
	entry->liveness_suspect_causes = MIDR_NODE_SUSPECT_NONE;
	entry->suspect_since = 0;
	/*
	 * Only a real Node NLRI (or the local self source) supersedes a DEAD or
	 * LEAVE quarantine.  An indirect ALIVE lease is deliberately local and
	 * non-transitive: it cannot erase the rumor or be relayed as evidence.
	 */
	if (direct) {
		entry->nontransitive_alive_until = 0;
		for (ALL_LIST_ELEMENTS_RO(ctx->seen, node, seen))
			if (IPV4_ADDR_SAME(&seen->subject,
					   &entry->node_id.u.prefix4))
				seen->blocks_indirect_discovery = false;
		midr_liveness_leave_retx_cancel_subject(
			ctx, entry->node_id.u.prefix4);
	}
	round = midr_liveness_round_find(ctx, &entry->node_id);
	if (round)
		midr_liveness_round_delete(ctx, round);
	if (recovered) {
		MIDR_LOG("MIDR liveness: node %pFX recovered to ACTIVE (%s evidence)",
			 &entry->node_id, direct ? "direct" : "indirect");
		midr_nds_notify_cl(bgp, MIDR_TRIGGER_NODE_CHANGE);
	}
}

void midr_liveness_on_alive(struct bgp *bgp, struct midr_node_entry *entry)
{
	midr_liveness_recover(bgp, entry, true);
}

void midr_liveness_on_withdraw(struct bgp *bgp, const struct prefix *node_id)
{
	struct midr_node_entry *entry = midr_liveness_find_node(bgp, node_id);

	if (entry)
		midr_liveness_mark_suspect(bgp, entry,
					   MIDR_NODE_SUSPECT_WITHDRAW,
					   "BGP-LS withdraw");
}

void midr_liveness_on_node_removed(struct bgp *bgp,
				   const struct prefix *node_id)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_round *round;

	if (!ctx || !node_id)
		return;
	round = midr_liveness_round_find(ctx, node_id);
	if (round)
		midr_liveness_round_delete(ctx, round);
}

static void
midr_liveness_commit_dead(struct bgp *bgp,
			  struct midr_liveness_round *round)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_dead_wire dead = {};
	struct in_addr voters[MIDR_LIVENESS_MAX_VOTERS];
	uint8_t payload[MIDR_LIVENESS_MAX_WIRE_SIZE];
	struct prefix subject;
	struct midr_node_entry *entry;
	struct prefix locator;
	uint32_t message_id;
	enum midr_liveness_seen_result seen_result;
	size_t voter_count = 0;
	size_t length;
	size_t i;
	time_t now = monotime(NULL);

	if (!ctx || !round)
		return;
	for (i = 0; i < round->voter_count; i++)
		if (round->voters[i].responded &&
		    round->voters[i].result == MIDR_LIVENESS_RESULT_STALE)
			voters[voter_count++] = round->voters[i].node_id;
	if (voter_count < round->required_quorum)
		return;

	prefix_copy(&subject, &round->subject);
	message_id = midr_liveness_next_id(ctx);
	length = sizeof(dead) + voter_count * sizeof(voters[0]);
	midr_liveness_fill_hdr(&dead.hdr, MIDR_LIVENESS_DEAD, length,
				 message_id, bgp->router_id);
	dead.subject = subject.u.prefix4;
	dead.subject_transport = dead.subject;
	entry = midr_liveness_find_node(bgp, &subject);
	if (entry) {
		midr_node_get_locator(entry, &locator);
		if (locator.family == AF_INET &&
		    locator.u.prefix4.s_addr != INADDR_ANY)
			dead.subject_transport = locator.u.prefix4;
	}
	dead.round_id = htonl(round->round_id);
	dead.hop_limit = (uint8_t)ctx->config.hop_limit;
	dead.voter_count = (uint8_t)voter_count;
	dead.declared_quorum = htons((uint16_t)round->required_quorum);
	memcpy(payload, &dead, sizeof(dead));
	memcpy(payload + sizeof(dead), voters,
	       voter_count * sizeof(voters[0]));

	/* Mark before publication so a reflected rumor is idempotent. */
	seen_result = midr_liveness_seen_accept(
		bgp, ctx, MIDR_LIVENESS_DEAD, bgp->router_id, message_id,
		subject.u.prefix4, dead.subject_transport, dead.hop_limit, now);
	if (seen_result == MIDR_LIVENESS_SEEN_DUPLICATE ||
	    seen_result == MIDR_LIVENESS_SEEN_SUBJECT_SUPPRESSED)
		return;
	if (seen_result == MIDR_LIVENESS_SEEN_FIRST) {
		if (!midr_nds_commit_node_remove(
			    bgp, &subject, MIDR_NODE_REMOVE_QUORUM_DEAD))
			return;
		MIDR_LOG("MIDR liveness: committed DEAD for %pFX with %zu STALE votes",
			 &subject, voter_count);
	}
	midr_liveness_gossip_raw(bgp, payload, length, NULL);
}

void midr_liveness_publish_leave(struct bgp *bgp)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_leave_wire leave = {};
	uint32_t message_id;
	time_t now;

	if (!ctx || bgp->router_id.s_addr == INADDR_ANY)
		return;
	message_id = midr_liveness_next_id(ctx);
	now = monotime(NULL);
	midr_liveness_fill_hdr(&leave.hdr, MIDR_LIVENESS_GRACEFUL_LEAVE,
				 sizeof(leave), message_id, bgp->router_id);
	leave.subject = bgp->router_id;
	leave.subject_transport = bgp->router_id;
	if (bgp->midr_info->transport_addr_set &&
	    bgp->midr_info->local_transport_addr.s_addr != INADDR_ANY)
		leave.subject_transport = bgp->midr_info->local_transport_addr;
	leave.hop_limit = (uint8_t)ctx->config.hop_limit;
	midr_liveness_seen_accept(bgp, ctx,
				  MIDR_LIVENESS_GRACEFUL_LEAVE,
				  bgp->router_id, message_id, leave.subject,
				  leave.subject_transport, leave.hop_limit, now);
	midr_liveness_gossip_raw(bgp, (const uint8_t *)&leave, sizeof(leave),
				 NULL);
	midr_liveness_leave_retx_schedule(
		bgp, (const uint8_t *)&leave, sizeof(leave), NULL);
	MIDR_LOG("MIDR liveness: published graceful leave for %pI4",
		 &bgp->router_id);
}

static bool midr_liveness_type(uint8_t type)
{
	return type >= MIDR_LIVENESS_PROBE_REQ &&
	       type <= MIDR_LIVENESS_GRACEFUL_LEAVE;
}

static bool midr_liveness_parse_hdr(const uint8_t *buf, size_t len,
				    struct midr_liveness_wire_hdr *hdr)
{
	if (len < sizeof(*hdr))
		return false;
	memcpy(hdr, buf, sizeof(*hdr));
	return hdr->version == MIDR_CTRL_MSG_VERSION &&
	       ntohs(hdr->length) == len && ntohl(hdr->message_id) != 0;
}

static enum midr_liveness_probe_result
midr_liveness_local_result(struct bgp *bgp, struct in_addr subject,
			   uint32_t requester_age, uint32_t *age_out)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_node_entry *entry;
	time_t now = monotime(NULL);
	uint32_t age;

	entry = midr_liveness_find_node_addr(bgp, subject);
	if (!entry) {
		*age_out = MIDR_LIVENESS_AGE_UNKNOWN;
		return MIDR_LIVENESS_RESULT_UNKNOWN;
	}
	age = midr_liveness_age(now, entry->last_seen);
	*age_out = age;
	/*
	 * MEMBER_LIST discovery and an indirect ALIVE may keep our local entry
	 * ACTIVE, but neither is transferable evidence for another requester.
	 */
	if (age == MIDR_LIVENESS_AGE_UNKNOWN)
		return MIDR_LIVENESS_RESULT_UNKNOWN;
	if ((entry->is_self && bgp->midr_info->shutdown) ||
	    !midr_liveness_node_usable(entry) ||
	    age > ctx->config.suspect_timeout)
		return MIDR_LIVENESS_RESULT_STALE;
	if (requester_age != MIDR_LIVENESS_AGE_UNKNOWN &&
	    age >= requester_age)
		return MIDR_LIVENESS_RESULT_UNKNOWN;
	return MIDR_LIVENESS_RESULT_ALIVE;
}

static void midr_liveness_handle_probe_req(
	struct bgp *bgp, const uint8_t *buf, size_t len,
	const struct sockaddr_in *source,
	const struct midr_liveness_wire_hdr *parsed_hdr)
{
	struct midr_liveness_probe_wire request;
	struct midr_liveness_probe_resp_wire response = {};
	uint32_t age;

	if (len != sizeof(request) || !source)
		return;
	memcpy(&request, buf, sizeof(request));
	if (request.hdr.origin.s_addr == INADDR_ANY ||
	    request.subject.s_addr == INADDR_ANY)
		return;

	midr_liveness_fill_hdr(&response.hdr, MIDR_LIVENESS_PROBE_RESP,
				 sizeof(response),
				 ntohl(parsed_hdr->message_id), bgp->router_id);
	response.subject = request.subject;
	response.result = midr_liveness_local_result(
		bgp, request.subject, ntohl(request.requester_age), &age);
	response.age = htonl(age);
	midr_ctrl_udp_send(bgp, source, &response, sizeof(response));
}

static void midr_liveness_handle_probe_resp(
	struct bgp *bgp, const uint8_t *buf, size_t len,
	const struct midr_liveness_wire_hdr *parsed_hdr)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_probe_resp_wire response;
	struct midr_liveness_round *round;
	struct prefix subject;
	struct midr_node_entry *entry;
	struct midr_node_entry *voter_entry;
	size_t i;
	size_t responses = 0;
	size_t stale_votes = 0;
	uint32_t response_age;
	uint32_t effective_age;
	uint64_t elapsed;
	enum midr_liveness_probe_result result;
	uint32_t round_id = ntohl(parsed_hdr->message_id);
	time_t now = monotime(NULL);

	if (len != sizeof(response))
		return;
	memcpy(&response, buf, sizeof(response));
	if (response.result > MIDR_LIVENESS_RESULT_STALE ||
	    response.hdr.origin.s_addr == INADDR_ANY)
		return;
	midr_liveness_prefix_from_addr(&subject, response.subject);
	round = midr_liveness_round_find(ctx, &subject);
	if (!round || round->state != MIDR_LIVENESS_ROUND_WAITING ||
	    round->round_id != round_id || now >= round->deadline)
		return;

	for (i = 0; i < round->voter_count; i++)
		if (IPV4_ADDR_SAME(&round->voters[i].node_id,
				   &response.hdr.origin))
			break;
	if (i == round->voter_count || round->voters[i].responded)
		return;
	/*
	 * The round snapshot proves this Router-ID was selected, but a delayed
	 * datagram must not retain voting authority after the voter becomes
	 * SUSPECT, loses adjacency/session, or is removed.
	 */
	voter_entry = midr_liveness_find_node_addr(bgp,
						   response.hdr.origin);
	result = midr_liveness_voter_eligible(bgp, voter_entry,
					      &round->subject)
			 ? response.result
			 : MIDR_LIVENESS_RESULT_UNKNOWN;
	response_age = ntohl(response.age);
	entry = midr_liveness_find_node(bgp, &round->subject);

	/*
	 * ALIVE is accepted only when the voter has fresher direct Node-NLRI
	 * evidence.  Account conservatively for the whole round elapsed time;
	 * the resulting local lease can therefore never outlive that evidence.
	 */
	if (result == MIDR_LIVENESS_RESULT_ALIVE) {
		elapsed = now > round->started_at
				  ? (uint64_t)(now - round->started_at)
				  : 0;
		if (response_age == MIDR_LIVENESS_AGE_UNKNOWN ||
		    (round->requester_age != MIDR_LIVENESS_AGE_UNKNOWN &&
		     response_age >= round->requester_age) ||
		    elapsed > MIDR_LIVENESS_AGE_MAX ||
		    response_age >
			    MIDR_LIVENESS_AGE_MAX - (uint32_t)elapsed)
			result = MIDR_LIVENESS_RESULT_UNKNOWN;
		else {
			effective_age = response_age + (uint32_t)elapsed;
			if (effective_age >= ctx->config.suspect_timeout ||
			    !entry)
				result = MIDR_LIVENESS_RESULT_UNKNOWN;
		}
	}
	round->voters[i].responded = true;
	round->voters[i].result = result;

	/* In the trusted prototype, one indirect ALIVE is sufficient proof. */
	if (result == MIDR_LIVENESS_RESULT_ALIVE) {
		time_t lease_until =
			now + ctx->config.suspect_timeout - effective_age;

		if (entry->nontransitive_alive_until < lease_until)
			entry->nontransitive_alive_until = lease_until;
		midr_liveness_recover(bgp, entry, false);
		return;
	}

	midr_liveness_round_counts(round, &responses, &stale_votes);
	/* A selected voter that has not replied may still return ALIVE.  Since
	 * one trusted ALIVE vetoes the round, do not commit merely because the
	 * STALE quorum happened to arrive first.
	 */
	if (stale_votes >= round->required_quorum &&
	    responses == round->voter_count) {
		midr_liveness_round_schedule(
			ctx, round, MIDR_LIVENESS_ROUND_BACKOFF,
			now + ctx->config.retry_backoff);
		midr_liveness_commit_dead(bgp, round);
	} else if (responses == round->voter_count) {
		midr_liveness_round_schedule(
			ctx, round, MIDR_LIVENESS_ROUND_BACKOFF,
			now + ctx->config.retry_backoff);
	}
}

static bool midr_liveness_dead_valid(
	const struct midr_liveness_dead_wire *dead,
	const struct in_addr *voters, size_t len)
{
	uint16_t quorum = ntohs(dead->declared_quorum);
	size_t expected = sizeof(*dead) +
			  (size_t)dead->voter_count * sizeof(voters[0]);
	size_t i, j;

	if (len != expected || dead->voter_count < 2 ||
	    dead->voter_count > MIDR_LIVENESS_MAX_VOTERS || quorum < 2 ||
	    quorum > dead->voter_count || quorum <= dead->voter_count / 2 ||
	    dead->hop_limit == 0 || dead->subject.s_addr == INADDR_ANY ||
	    dead->subject_transport.s_addr == INADDR_ANY ||
	    dead->hdr.origin.s_addr == INADDR_ANY ||
	    IPV4_ADDR_SAME(&dead->subject, &dead->hdr.origin) ||
	    ntohl(dead->round_id) == 0)
		return false;
	for (i = 0; i < dead->voter_count; i++) {
		if (voters[i].s_addr == INADDR_ANY ||
		    IPV4_ADDR_SAME(&voters[i], &dead->subject) ||
		    IPV4_ADDR_SAME(&voters[i], &dead->hdr.origin))
			return false;
		for (j = i + 1; j < dead->voter_count; j++)
			if (IPV4_ADDR_SAME(&voters[i], &voters[j]))
				return false;
	}
	return true;
}

static void midr_liveness_handle_dead(
	struct bgp *bgp, const uint8_t *buf, size_t len,
	const struct sockaddr_in *source,
	const struct midr_liveness_wire_hdr *parsed_hdr)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_dead_wire dead;
	struct in_addr voters[MIDR_LIVENESS_MAX_VOTERS];
	uint8_t forward[MIDR_LIVENESS_MAX_WIRE_SIZE];
	struct prefix subject;
	uint32_t message_id = ntohl(parsed_hdr->message_id);
	enum midr_liveness_seen_result seen_result;
	time_t now = monotime(NULL);

	if (len < sizeof(dead) || len > sizeof(forward))
		return;
	memcpy(&dead, buf, sizeof(dead));
	if (dead.voter_count > MIDR_LIVENESS_MAX_VOTERS ||
	    len != sizeof(dead) +
			   (size_t)dead.voter_count * sizeof(voters[0]))
		return;
	memcpy(voters, buf + sizeof(dead),
	       (size_t)dead.voter_count * sizeof(voters[0]));
	if (!midr_liveness_dead_valid(&dead, voters, len) ||
	    IPV4_ADDR_SAME(&dead.subject, &bgp->router_id))
		return;
	seen_result = midr_liveness_seen_accept(
		bgp, ctx, MIDR_LIVENESS_DEAD, dead.hdr.origin, message_id,
		dead.subject, dead.subject_transport, dead.hop_limit, now);
	if (seen_result == MIDR_LIVENESS_SEEN_DUPLICATE ||
	    seen_result == MIDR_LIVENESS_SEEN_SUBJECT_SUPPRESSED)
		return;

	if (seen_result == MIDR_LIVENESS_SEEN_FIRST) {
		midr_liveness_prefix_from_addr(&subject, dead.subject);
		MIDR_LOG("MIDR liveness: accepted DEAD for %pFX from detector %pI4",
			 &subject, &dead.hdr.origin);
		midr_nds_commit_node_remove(bgp, &subject,
					    MIDR_NODE_REMOVE_QUORUM_DEAD);
	}
	if (dead.hop_limit <= 1)
		return;
	dead.hop_limit--;
	memcpy(forward, &dead, sizeof(dead));
	memcpy(forward + sizeof(dead), voters,
	       (size_t)dead.voter_count * sizeof(voters[0]));
	midr_liveness_gossip_raw(bgp, forward, len, source);
}

static void midr_liveness_handle_leave(
	struct bgp *bgp, const uint8_t *buf, size_t len,
	const struct sockaddr_in *source,
	const struct midr_liveness_wire_hdr *parsed_hdr)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_leave_wire leave;
	struct prefix subject;
	uint32_t message_id = ntohl(parsed_hdr->message_id);
	enum midr_liveness_seen_result seen_result;
	time_t now = monotime(NULL);

	if (len != sizeof(leave))
		return;
	memcpy(&leave, buf, sizeof(leave));
	if (leave.subject.s_addr == INADDR_ANY ||
	    leave.subject_transport.s_addr == INADDR_ANY ||
	    !IPV4_ADDR_SAME(&leave.subject, &leave.hdr.origin) ||
	    leave.hop_limit == 0 ||
	    IPV4_ADDR_SAME(&leave.subject, &bgp->router_id))
		return;
	seen_result = midr_liveness_seen_accept(
		bgp, ctx, MIDR_LIVENESS_GRACEFUL_LEAVE, leave.hdr.origin,
		message_id, leave.subject, leave.subject_transport,
		leave.hop_limit, now);
	if (seen_result == MIDR_LIVENESS_SEEN_DUPLICATE)
		return;

	if (seen_result == MIDR_LIVENESS_SEEN_FIRST) {
		midr_liveness_prefix_from_addr(&subject, leave.subject);
		MIDR_LOG("MIDR liveness: accepted graceful leave for %pFX",
			 &subject);
		midr_nds_commit_node_remove(bgp, &subject,
					    MIDR_NODE_REMOVE_GRACEFUL_LEAVE);
	}
	if (leave.hop_limit <= 1)
		return;
	leave.hop_limit--;
	midr_liveness_gossip_raw(bgp, (const uint8_t *)&leave, sizeof(leave),
				 source);
	midr_liveness_leave_retx_schedule(
		bgp, (const uint8_t *)&leave, sizeof(leave), source);
}

bool midr_liveness_handle_ctrl(struct bgp *bgp, const uint8_t *buf,
			       size_t len, const struct sockaddr_in *source)
{
	struct midr_liveness_wire_hdr hdr;
	uint8_t type;

	if (!buf || len < 2)
		return false;
	type = buf[1];
	if (!midr_liveness_type(type))
		return false;
	/* Recognized liveness types are consumed even when malformed. */
	if (!midr_liveness_ctx(bgp) ||
	    !midr_liveness_parse_hdr(buf, len, &hdr) || hdr.type != type)
		return true;

	switch (type) {
	case MIDR_LIVENESS_PROBE_REQ:
		midr_liveness_handle_probe_req(bgp, buf, len, source, &hdr);
		break;
	case MIDR_LIVENESS_PROBE_RESP:
		midr_liveness_handle_probe_resp(bgp, buf, len, &hdr);
		break;
	case MIDR_LIVENESS_DEAD:
		midr_liveness_handle_dead(bgp, buf, len, source, &hdr);
		break;
	case MIDR_LIVENESS_GRACEFUL_LEAVE:
		midr_liveness_handle_leave(bgp, buf, len, source, &hdr);
		break;
	}
	return true;
}

static void midr_liveness_tick(struct event *event)
{
	struct bgp *bgp = EVENT_ARG(event);
	struct bgp_midr *mi = bgp->midr_info;
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_node_entry *entry;
	struct listnode *node, *next;
	struct midr_liveness_round *round;
	time_t now = monotime(NULL);

	if (!ctx)
		return;

	if (now >= ctx->next_node_scan) {
		frr_each_safe (midr_node_hash, &mi->global_view->nodes, entry) {
			uint32_t age;

			if (entry->is_self)
				continue;
			age = midr_liveness_age(now, entry->last_seen);
			if (entry->liveness_state == MIDR_NODE_ACTIVE &&
			    !midr_liveness_any_evidence_fresh(
				    ctx, entry, now))
				midr_liveness_mark_suspect(bgp, entry,
							   MIDR_NODE_SUSPECT_TIMEOUT,
							   "keepalive timeout");
			else if (entry->liveness_state == MIDR_NODE_SUSPECT) {
				if (age == MIDR_LIVENESS_AGE_UNKNOWN ||
				    age > ctx->config.suspect_timeout)
					entry->liveness_suspect_causes |=
						MIDR_NODE_SUSPECT_TIMEOUT;
				if (!midr_liveness_round_find(ctx,
							     &entry->node_id))
					midr_liveness_mark_suspect(
						bgp, entry, MIDR_NODE_SUSPECT_NONE,
						"confirmation retry");
			}
		}
		ctx->next_node_scan = now + ctx->config.scan_interval;
	}

	for (ALL_LIST_ELEMENTS(ctx->rounds, node, next, round)) {
		size_t responses, stale_votes;

		entry = midr_liveness_find_node(bgp, &round->subject);
		if (!entry || entry->liveness_state != MIDR_NODE_SUSPECT) {
			midr_liveness_round_delete(ctx, round);
			continue;
		}
		if (now < round->deadline)
			continue;
		if (round->state == MIDR_LIVENESS_ROUND_WAITING) {
			midr_liveness_round_counts(round, &responses,
						   &stale_votes);
			if (stale_votes >= round->required_quorum) {
				midr_liveness_round_schedule(
					ctx, round,
					MIDR_LIVENESS_ROUND_BACKOFF,
					now + ctx->config.retry_backoff);
				midr_liveness_commit_dead(bgp, round);
				continue;
			}
			midr_liveness_round_schedule(
				ctx, round, MIDR_LIVENESS_ROUND_BACKOFF,
				now + ctx->config.retry_backoff);
			MIDR_LOG("MIDR liveness: round %u for %pFX timed out; retry in %us",
				 round->round_id, &round->subject,
				 ctx->config.retry_backoff);
		} else {
			midr_liveness_round_begin(bgp, round, now);
		}
	}

	midr_liveness_seen_gc(ctx, now);
	event_add_timer(bm->master, midr_liveness_tick, bgp,
			MIDR_LIVENESS_TICK_INTERVAL, &mi->t_expire_check);
}

static void midr_liveness_keepalive_timer(struct event *event)
{
	struct bgp *bgp = EVENT_ARG(event);
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);

	if (!ctx)
		return;
	midr_propagate_self(bgp, MIDR_ORIGIN_KEEPALIVE);
	event_add_timer(bm->master, midr_liveness_keepalive_timer, bgp,
			ctx->config.keepalive_interval,
			&bgp->midr_info->t_keepalive);
}

static void midr_liveness_self_advertise(struct event *event)
{
	struct bgp *bgp = EVENT_ARG(event);

	if (midr_liveness_ctx(bgp))
		midr_propagate_self(bgp, MIDR_ORIGIN_INIT);
}

void midr_liveness_schedule_self_advertisement(struct bgp *bgp)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);

	if (!ctx || ctx->t_self_advertise)
		return;
	event_add_event(bm->master, midr_liveness_self_advertise, bgp, 0,
			&ctx->t_self_advertise);
}

void midr_liveness_init(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct midr_liveness *ctx;
	time_t now;

	if (!bgp || !bgp->midr_info || bgp->midr_info->liveness)
		return;
	mi = bgp->midr_info;
	ctx = XCALLOC(MTYPE_BGP_MIDR_LIVENESS, sizeof(*ctx));
	midr_liveness_config_defaults(&ctx->config);
	ctx->rounds = list_new();
	ctx->seen = list_new();
	ctx->leave_retx = list_new();
	ctx->max_inflight_rounds =
		MIDR_LIVENESS_DEFAULT_MAX_INFLIGHT_ROUNDS;
	now = monotime(NULL);
	ctx->tx_counter = (uint32_t)frr_weak_random() ^
			  ntohl(bgp->router_id.s_addr) ^ (uint32_t)now;
	if (ctx->tx_counter == 0)
		ctx->tx_counter = 1;
	ctx->next_node_scan = now + ctx->config.scan_interval;
	mi->liveness = ctx;
	midr_liveness_schedule_self_advertisement(bgp);
	midr_liveness_rearm_keepalive(bgp);
	event_add_timer(bm->master, midr_liveness_tick, bgp,
			MIDR_LIVENESS_TICK_INTERVAL, &mi->t_expire_check);
}

void midr_liveness_finish(struct bgp *bgp)
{
	struct bgp_midr *mi;
	struct midr_liveness *ctx;
	struct listnode *node, *next;
	struct midr_liveness_round *round;
	struct midr_liveness_seen *seen;
	struct midr_liveness_retx *retry;

	if (!bgp || !bgp->midr_info || !bgp->midr_info->liveness)
		return;
	mi = bgp->midr_info;
	ctx = mi->liveness;
	event_cancel(&mi->t_keepalive);
	event_cancel(&mi->t_expire_check);
	event_cancel(&ctx->t_self_advertise);
	for (ALL_LIST_ELEMENTS(ctx->rounds, node, next, round)) {
		list_delete_node(ctx->rounds, node);
		XFREE(MTYPE_MIDR_LIVENESS_ROUND, round);
	}
	list_delete(&ctx->rounds);
	for (ALL_LIST_ELEMENTS(ctx->seen, node, next, seen)) {
		list_delete_node(ctx->seen, node);
		XFREE(MTYPE_MIDR_LIVENESS_SEEN, seen);
	}
	list_delete(&ctx->seen);
	for (ALL_LIST_ELEMENTS(ctx->leave_retx, node, next, retry)) {
		event_cancel(&retry->timer);
		list_delete_node(ctx->leave_retx, node);
		XFREE(MTYPE_MIDR_LIVENESS_RETX, retry);
	}
	list_delete(&ctx->leave_retx);
	XFREE(MTYPE_BGP_MIDR_LIVENESS, ctx);
	mi->liveness = NULL;
}

void midr_liveness_show(struct vty *vty, struct bgp *bgp)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_node_entry *entry;
	struct listnode *node;
	struct midr_liveness_round *round;
	size_t active = 0, suspect = 0;
	time_t now = monotime(NULL);

	if (!ctx) {
		vty_out(vty, "%% MIDR liveness not initialized\n");
		return;
	}
	frr_each (midr_node_hash, &bgp->midr_info->global_view->nodes, entry) {
		if (entry->liveness_state == MIDR_NODE_SUSPECT)
			suspect++;
		else
			active++;
	}
	vty_out(vty,
		"MIDR liveness: keepalive %us, suspect %us, scan %us, confirm %us, retry %us\n",
		ctx->config.keepalive_interval, ctx->config.suspect_timeout,
		ctx->config.scan_interval, ctx->config.confirm_timeout,
		ctx->config.retry_backoff);
	vty_out(vty,
		"Voting: sample %u, quorum %u; gossip hop-limit %u, seen-cache %us\n",
		ctx->config.voter_sample_size, ctx->config.quorum,
		ctx->config.hop_limit, ctx->config.cache_ttl);
	vty_out(vty,
		"Nodes: %zu active, %zu suspect; rounds %u (%u/%u in flight); seen rumors %u; leave retries %u\n",
		active, suspect, listcount(ctx->rounds),
		ctx->inflight_rounds, ctx->max_inflight_rounds,
		listcount(ctx->seen), listcount(ctx->leave_retx));
	if (list_isempty(ctx->rounds))
		return;

	vty_out(vty, "%-18s %-10s %-8s %-8s %-8s %s\n", "Subject",
		"Round", "Phase", "Voters", "Stale/Q", "Remaining");
	for (ALL_LIST_ELEMENTS_RO(ctx->rounds, node, round)) {
		size_t i, stale_votes = 0;
		time_t remaining = round->deadline > now ? round->deadline - now : 0;

		for (i = 0; i < round->voter_count; i++)
			if (round->voters[i].responded &&
			    round->voters[i].result == MIDR_LIVENESS_RESULT_STALE)
				stale_votes++;
		vty_out(vty, "%-18pI4 %-10u %-8s %-8zu %zu/%-5u %lds\n",
			&round->subject.u.prefix4, round->round_id,
			midr_liveness_round_state_name(round),
			round->voter_count, stale_votes, round->required_quorum,
			(long)remaining);
	}
}

int midr_liveness_config_write(struct bgp *bgp, struct vty *vty)
{
	struct midr_liveness *ctx = midr_liveness_ctx(bgp);
	struct midr_liveness_config defaults;
	int written = 0;

	if (!ctx)
		return 0;
	midr_liveness_config_defaults(&defaults);
	if (ctx->config.keepalive_interval != defaults.keepalive_interval ||
	    ctx->config.suspect_timeout != defaults.suspect_timeout ||
	    ctx->config.scan_interval != defaults.scan_interval ||
	    ctx->config.confirm_timeout != defaults.confirm_timeout ||
	    ctx->config.retry_backoff != defaults.retry_backoff) {
		vty_out(vty,
			" midr liveness timers keepalive %u suspect %u scan %u confirm %u retry %u\n",
			ctx->config.keepalive_interval,
			ctx->config.suspect_timeout, ctx->config.scan_interval,
			ctx->config.confirm_timeout, ctx->config.retry_backoff);
		written++;
	}
	if (ctx->config.voter_sample_size != defaults.voter_sample_size ||
	    ctx->config.quorum != defaults.quorum) {
		vty_out(vty,
			" midr liveness voting sample-size %u quorum %u\n",
			ctx->config.voter_sample_size, ctx->config.quorum);
		written++;
	}
	if (ctx->config.hop_limit != defaults.hop_limit ||
	    ctx->config.cache_ttl != defaults.cache_ttl) {
		vty_out(vty,
			" midr liveness gossip hop-limit %u cache-ttl %u\n",
			ctx->config.hop_limit, ctx->config.cache_ttl);
		written++;
	}
	return written;
}
