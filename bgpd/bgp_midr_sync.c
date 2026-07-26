// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR peer End-of-RIB synchronization barrier.
 */

#include <zebra.h>

#include <errno.h>

#include "linklist.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_STORE, "MIDR sync store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_PEER, "MIDR sync peer");

struct midr_sync_peer {
	struct peer *peer;
	bool waiting;
	bool timed_out;
};

struct midr_sync_store {
	struct midr_context *ctx;
	struct list *peers;
	struct event *t_timeout;
	enum midr_sync_state state;
	uint32_t timeout_seconds;
	bool local_ready;
	uint64_t barrier_count;
	uint64_t timeout_count;
};

static void midr_sync_peer_free(void *arg)
{
	XFREE(MTYPE_MIDR_SYNC_PEER, arg);
}

static struct midr_sync_peer *midr_sync_peer_lookup(struct midr_sync_store *store,
						    const struct peer *peer)
{
	struct listnode *node;
	struct midr_sync_peer *entry;

	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry))
		if (entry->peer == peer)
			return entry;
	return NULL;
}

static size_t midr_sync_waiting_count(const struct midr_sync_store *store)
{
	struct listnode *node;
	struct midr_sync_peer *entry;
	size_t count = 0;

	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry))
		if (entry->waiting)
			count++;
	return count;
}

static void midr_sync_complete_if_ready(struct midr_sync_store *store)
{
	if (store->state != MIDR_SYNC_REMOTE_WAIT || midr_sync_waiting_count(store))
		return;
	store->state = MIDR_SYNC_READY;
	event_cancel(&store->t_timeout);
	midr_lsdb_sync_changed(store->ctx);
}

static void midr_sync_timeout_event(struct event *event)
{
	struct midr_sync_store *store = EVENT_ARG(event);
	struct listnode *node;
	struct midr_sync_peer *entry;

	store->t_timeout = NULL;
	if (store->state != MIDR_SYNC_REMOTE_WAIT)
		return;
	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry)) {
		if (!entry->waiting)
			continue;
		entry->waiting = false;
		entry->timed_out = true;
	}
	store->timeout_count++;
	midr_sync_complete_if_ready(store);
}

static void midr_sync_reset(struct midr_sync_store *store)
{
	event_cancel(&store->t_timeout);
	list_delete_all_node(store->peers);
	store->state = MIDR_SYNC_LOCAL_WAIT;
	store->local_ready = false;
}

static void midr_sync_start(struct midr_sync_store *store)
{
	struct listnode *node;
	struct peer *peer;

	store->state = MIDR_SYNC_REMOTE_WAIT;
	store->barrier_count++;
	for (ALL_LIST_ELEMENTS_RO(store->ctx->bgp->peer, node, peer)) {
		struct midr_sync_peer *entry;

		if (!peer->connection || !peer_established(peer->connection) ||
		    !peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS])
			continue;
		entry = XCALLOC(MTYPE_MIDR_SYNC_PEER, sizeof(*entry));
		entry->peer = peer;
		entry->waiting = !CHECK_FLAG(peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS],
					     PEER_STATUS_EOR_RECEIVED);
		listnode_add(store->peers, entry);
	}
	if (!midr_sync_waiting_count(store)) {
		store->state = MIDR_SYNC_READY;
		return;
	}
	if (bm && bm->master)
		event_add_timer(bm->master, midr_sync_timeout_event, store, store->timeout_seconds,
				&store->t_timeout);
}

int midr_sync_init(struct midr_context *ctx)
{
	struct midr_sync_store *store;

	if (!ctx || !ctx->bgp)
		return -EINVAL;
	if (ctx->sync_store)
		return -EALREADY;
	store = XCALLOC(MTYPE_MIDR_SYNC_STORE, sizeof(*store));
	store->ctx = ctx;
	store->state = MIDR_SYNC_LOCAL_WAIT;
	store->timeout_seconds = MIDR_EOR_TIMEOUT_DEFAULT;
	store->peers = list_new();
	store->peers->del = midr_sync_peer_free;
	ctx->sync_store = store;
	return 0;
}

void midr_sync_finish(struct midr_context *ctx)
{
	struct midr_sync_store *store;

	if (!ctx || !ctx->sync_store)
		return;
	store = ctx->sync_store;
	ctx->sync_store = NULL;
	event_cancel(&store->t_timeout);
	list_delete(&store->peers);
	XFREE(MTYPE_MIDR_SYNC_STORE, store);
}

bool midr_sync_view_ready(struct midr_context *ctx, bool local_ready, uint64_t *reason_flags)
{
	struct midr_sync_store *store;
	struct listnode *node;
	struct midr_sync_peer *entry;

	if (!ctx || !ctx->sync_store)
		return false;
	store = ctx->sync_store;
	if (!local_ready) {
		if (store->local_ready)
			midr_sync_reset(store);
		return false;
	}
	if (!store->local_ready) {
		store->local_ready = true;
		midr_sync_start(store);
	}
	if (reason_flags)
		for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry))
			if (entry->timed_out) {
				*reason_flags |= MIDR_TED_SYNC_REASON_EOR_TIMEOUT;
				break;
			}
	return store->state == MIDR_SYNC_READY;
}

bool midr_sync_local_ready(const struct midr_context *ctx)
{
	return ctx && ctx->sync_store && ctx->sync_store->local_ready;
}

void midr_sync_peer_status_changed(struct midr_context *ctx, struct peer *peer)
{
	struct midr_sync_store *store;
	struct midr_sync_peer *entry;
	bool clear_timeout = false;

	if (!ctx || !ctx->sync_store || !peer)
		return;
	store = ctx->sync_store;
	entry = midr_sync_peer_lookup(store, peer);
	if (!entry || (peer->connection && peer_established(peer->connection)))
		return;
	if (entry->waiting)
		entry->waiting = false;
	if (entry->timed_out) {
		entry->timed_out = false;
		clear_timeout = true;
	}
	if (clear_timeout)
		midr_lsdb_sync_changed(ctx);
	midr_sync_complete_if_ready(store);
}

void midr_sync_peer_eor(struct midr_context *ctx, struct peer *peer)
{
	struct midr_sync_store *store;
	struct midr_sync_peer *entry;

	if (!ctx || !ctx->sync_store || !peer)
		return;
	store = ctx->sync_store;
	entry = midr_sync_peer_lookup(store, peer);
	if (!entry)
		return;
	entry->waiting = false;
	if (entry->timed_out) {
		entry->timed_out = false;
		midr_lsdb_sync_changed(ctx);
	}
	midr_sync_complete_if_ready(store);
}

int midr_sync_timeout_set(struct midr_context *ctx, uint32_t seconds)
{
	if (!ctx || !ctx->sync_store)
		return -ENOENT;
	if (seconds < MIDR_EOR_TIMEOUT_MIN || seconds > MIDR_EOR_TIMEOUT_MAX)
		return -EINVAL;
	ctx->sync_store->timeout_seconds = seconds;
	return 0;
}

int midr_sync_status_get(struct midr_context *ctx, struct midr_sync_status *status)
{
	struct midr_sync_store *store;
	struct listnode *node;
	struct midr_sync_peer *entry;

	if (!ctx || !ctx->sync_store)
		return -ENOENT;
	if (!status)
		return -EINVAL;
	store = ctx->sync_store;
	memset(status, 0, sizeof(*status));
	status->state = store->state;
	status->timeout_seconds = store->timeout_seconds;
	status->initial_peer_count = listcount(store->peers);
	status->barrier_count = store->barrier_count;
	status->timeout_count = store->timeout_count;
	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry)) {
		if (entry->waiting)
			status->waiting_peer_count++;
		if (entry->timed_out)
			status->timed_out_peer_count++;
	}
	return 0;
}

void midr_sync_test_timeout(struct midr_context *ctx)
{
	struct event event = {};

	if (!ctx || !ctx->sync_store)
		return;
	event_cancel(&ctx->sync_store->t_timeout);
	event.arg = ctx->sync_store;
	midr_sync_timeout_event(&event);
}
