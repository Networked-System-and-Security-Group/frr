// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR peer End-of-RIB synchronization barrier.
 */

#include <zebra.h>

#include <errno.h>
#include <pthread.h>

#include "linklist.h"
#include "memory.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted.h"
#include "bgpd/bgp_packet.h"
#include "bgpd/bgp_route.h"
#include "bgpd/bgp_updgrp.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_STORE, "MIDR sync store");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_PEER, "MIDR sync peer");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_SESSION, "MIDR sync session");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_PACKET, "MIDR sync packet");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_TOKEN, "MIDR sync session token");
DEFINE_MTYPE_STATIC(BGPD, MIDR_SYNC_SHUTDOWN_TARGET,
			   "MIDR shutdown sync target");

struct midr_sync_peer {
	struct peer *peer;
	uint64_t generation;
	bool waiting;
	bool timed_out;
};

struct midr_sync_token {
	struct midr_sync_session *session;
	size_t references;
};

struct midr_sync_session_packet {
	struct stream *stream;
	struct peer_connection *connection;
	struct midr_sync_token *token;
	struct event *completion;
	struct midr_sync_session_packet *next;
	bool eor;
	bool written;
};

struct midr_sync_shutdown_target {
	struct peer_connection *connection;
	uint64_t generation;
	bool failed;
};

/* The writer only accesses this registry under the mutex, never the LS store. */
static pthread_mutex_t midr_sync_packets_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct midr_sync_session_packet *midr_sync_packets;

struct midr_sync_session {
	struct midr_sync_store *store;
	struct peer *peer;
	struct peer_connection *connection;
	uint64_t generation;
	enum midr_sync_session_state state;
	bool remote_eor;
	bool local_eor_queued;
	bool local_eor_written;
	bool needs_resync;
	bool snapshot_requested;
	bool snapshot_active;
	bool snapshot_completed;
	struct event *resync_event;
	uint64_t snapshot_generation;
	size_t pending_updates;
	size_t pending_input;
	struct list *packets;
	struct midr_sync_token *token;
};

struct midr_sync_store {
	struct midr_context *ctx;
	struct list *peers;
	struct list *sessions;
	struct event *t_timeout;
	enum midr_sync_state state;
	uint32_t timeout_seconds;
	bool local_ready;
	uint64_t barrier_count;
	uint64_t timeout_count;
	uint64_t next_session_generation;
	uint64_t reconnect_count;
	uint64_t stale_event_count;
	bool shutdown_active;
	bool shutdown_announce_complete;
	enum midr_sync_shutdown_state shutdown_state;
	struct list *shutdown_targets;
};

static struct midr_sync_session *midr_sync_session_lookup_generation(
	struct midr_sync_store *store, struct peer_connection *connection,
	uint64_t generation);

static struct midr_sync_shutdown_target *midr_sync_shutdown_target_lookup(
	struct midr_sync_store *store, struct peer_connection *connection,
	uint64_t generation)
{
	struct listnode *node;
	struct midr_sync_shutdown_target *target;

	for (ALL_LIST_ELEMENTS_RO(store->shutdown_targets, node, target))
		if (target->connection == connection &&
		    target->generation == generation)
			return target;
	return NULL;
}

static void midr_sync_peer_free(void *arg)
{
	XFREE(MTYPE_MIDR_SYNC_PEER, arg);
}

static void midr_sync_shutdown_target_free(void *arg)
{
	XFREE(MTYPE_MIDR_SYNC_SHUTDOWN_TARGET, arg);
}

static void midr_sync_token_release(struct midr_sync_token *token)
{
	if (!token || !token->references)
		return;
	if (!--token->references)
		XFREE(MTYPE_MIDR_SYNC_TOKEN, token);
}

static bool midr_sync_receive_drained(struct midr_sync_session *session)
{
	struct midr_lsdb_summary summary;

	if (!session->store->ctx->lsdb_store)
		return session->remote_eor && !session->pending_input &&
		       !session->needs_resync;
	if (midr_lsdb_summary_get(session->store->ctx, &summary) != 0)
		return false;

	return session->remote_eor && !session->pending_input &&
	       !session->needs_resync &&
	       summary.ready && !summary.dirty_count &&
	       !summary.derivation_pending && !summary.last_error;
}

static void midr_sync_start(struct midr_sync_store *store);

static void midr_sync_session_packet_free(void *arg)
{
	struct midr_sync_session_packet *packet = arg;
	struct midr_sync_session_packet **link;
	struct midr_sync_token *token = packet->token;

	pthread_mutex_lock(&midr_sync_packets_mutex);
	for (link = &midr_sync_packets; *link; link = &(*link)->next)
		if (*link == packet) {
			*link = packet->next;
			break;
		}
	pthread_mutex_unlock(&midr_sync_packets_mutex);
	event_cancel(&packet->completion);
	midr_sync_token_release(token);
	XFREE(MTYPE_MIDR_SYNC_PACKET, packet);
}

static void midr_sync_packet_event(struct event *event)
{
	struct midr_sync_session_packet *packet = EVENT_ARG(event);
	struct midr_sync_session *session = packet->token->session;
	bool in_session = session && listnode_lookup(session->packets, packet);

	packet->completion = NULL;
	if (session && in_session) {
		if (packet->eor)
			session->local_eor_written = packet->written;
		else if (session->pending_updates)
			session->pending_updates--;
		if (!packet->written) {
			session->needs_resync = true;
			session->state = MIDR_SYNC_SESSION_RESYNC;
			if (session->store->shutdown_active) {
				struct midr_sync_shutdown_target *target =
					midr_sync_shutdown_target_lookup(
						session->store, session->connection,
						session->generation);

				if (target) {
					target->failed = true;
					session->store->shutdown_state =
						MIDR_SYNC_SHUTDOWN_DEGRADED;
				}
			}
			midr_sync_request_resync(session->connection);
		}
		if (session->local_eor_written && midr_sync_receive_drained(session))
			session->state = MIDR_SYNC_SESSION_READY;
		if (peer_established(session->connection))
			event_add_event(bm->master, bgp_generate_updgrp_packets,
					session->connection, 0,
					&session->connection->t_generate_updgrp_packets);
		listnode_delete(session->packets, packet);
		midr_sync_session_packet_free(packet);
		return;
	}
	midr_sync_session_packet_free(packet);
}

static void midr_sync_session_free(void *arg)
{
	struct midr_sync_session *session = arg;
	struct midr_sync_token *token = session->token;
	bool resync_pending = session->resync_event != NULL;

	token->session = NULL;
	event_cancel(&session->resync_event);
	list_delete(&session->packets);
	if (resync_pending)
		midr_sync_token_release(token);
	midr_sync_token_release(token);
	XFREE(MTYPE_MIDR_SYNC_SESSION, session);
}

static void midr_sync_resync_event(struct event *event)
{
	struct midr_sync_token *token = EVENT_ARG(event);
	struct midr_sync_session *session = token ? token->session : NULL;

	if (session) {
		session->resync_event = NULL;
		if (peer_established(session->connection) && !session->snapshot_active)
			bgp_announce_route(session->peer, AFI_BGP_LS,
					   SAFI_MIDR_LS, true);
	}
	midr_sync_token_release(token);
}

static void midr_sync_schedule_resync(struct midr_sync_session *session)
{
	if (!session || session->resync_event || !bm || !bm->master ||
	    !peer_established(session->connection))
		return;
	session->snapshot_requested = true;
	session->snapshot_completed = false;
	session->local_eor_queued = false;
	session->local_eor_written = false;
	UNSET_FLAG(session->peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS],
		   PEER_STATUS_EOR_SEND);
	session->token->references++;
	event_add_event(bm->master, midr_sync_resync_event, session->token, 0,
			&session->resync_event);
}

static struct midr_sync_session *midr_sync_session_lookup(
	struct midr_sync_store *store, const struct peer_connection *connection)
{
	struct listnode *node;
	struct midr_sync_session *session;

	if (!connection)
		return NULL;
	for (ALL_LIST_ELEMENTS_RO(store->sessions, node, session))
		if (session->connection == connection &&
		    session->state != MIDR_SYNC_SESSION_DOWN)
			return session;
	return NULL;
}

static struct midr_sync_peer *midr_sync_peer_lookup(struct midr_sync_store *store,
						    const struct peer *peer);

static struct midr_sync_peer *midr_sync_peer_ensure(
	struct midr_sync_store *store, struct peer *peer, uint64_t generation)
{
	struct midr_sync_peer *entry;

	entry = midr_sync_peer_lookup(store, peer);
	if (entry)
		return entry;

	entry = XCALLOC(MTYPE_MIDR_SYNC_PEER, sizeof(*entry));
	entry->peer = peer;
	entry->generation = generation;
	entry->waiting = true;
	listnode_add(store->peers, entry);
	return entry;
}

static struct midr_sync_session *midr_sync_session_create(
	struct midr_sync_store *store, struct peer *peer,
	struct peer_connection *connection)
{
	struct midr_sync_session *session;
	struct listnode *node;
	struct listnode *next;
	struct midr_sync_session *old;

	if (!peer || !connection || !peer_established(connection) ||
	    !peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS])
		return NULL;
	if (midr_sync_session_lookup(store, connection))
		return midr_sync_session_lookup(store, connection);

	session = XCALLOC(MTYPE_MIDR_SYNC_SESSION, sizeof(*session));
	session->store = store;
	session->peer = peer;
	session->connection = connection;
	session->generation = ++store->next_session_generation;
	session->state = MIDR_SYNC_SESSION_SYNCING;
	session->snapshot_requested = true;
	session->packets = list_new();
	session->packets->del = midr_sync_session_packet_free;
	session->token = XCALLOC(MTYPE_MIDR_SYNC_TOKEN, sizeof(*session->token));
	session->token->session = session;
	session->token->references = 1;
	for (ALL_LIST_ELEMENTS(store->sessions, node, next, old))
		if (old->peer == peer && old->state == MIDR_SYNC_SESSION_DOWN) {
			store->reconnect_count++;
			list_delete_node(store->sessions, node);
			midr_sync_session_free(old);
		}
	listnode_add(store->sessions, session);
	return session;
}

static void midr_sync_session_mark_down(struct midr_sync_store *store,
						struct peer_connection *connection)
{
	struct midr_sync_session *session;

	if (!connection)
		return;
	for (struct listnode *node = listhead(store->sessions); node;
	     node = listnextnode(node)) {
		session = listgetdata(node);
		if (session->connection != connection ||
		    session->state == MIDR_SYNC_SESSION_DOWN)
			continue;
		session->state = MIDR_SYNC_SESSION_DOWN;
		list_delete_all_node(session->packets);
		session->pending_updates = 0;
		session->pending_input = 0;
	}
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

bool midr_sync_view_can_derive(struct midr_context *ctx,
				       uint64_t *reason_flags)
{
	struct midr_sync_store *store;
	struct listnode *node;
	struct midr_sync_peer *entry;
	struct midr_sync_session *session;

	if (!ctx || !(store = ctx->sync_store))
		return false;
	if (!store->local_ready) {
		store->local_ready = true;
		midr_sync_start(store);
	}
	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry)) {
		if (entry->timed_out) {
			if (reason_flags)
				*reason_flags |= MIDR_TED_SYNC_REASON_EOR_TIMEOUT;
			continue;
		}
		if (!entry->waiting)
			continue;
		session = NULL;
		for (struct listnode *session_node = listhead(store->sessions);
		     session_node; session_node = listnextnode(session_node)) {
			struct midr_sync_session *candidate = listgetdata(session_node);

			if (candidate->peer == entry->peer &&
			    candidate->generation == entry->generation &&
			    candidate->state != MIDR_SYNC_SESSION_DOWN) {
				session = candidate;
				break;
			}
		}
		if (!session || !session->remote_eor || session->pending_input ||
		    session->needs_resync)
			return false;
	}
	return true;
}

static void midr_sync_complete_if_ready(struct midr_sync_store *store)
{
	if (store->state != MIDR_SYNC_REMOTE_WAIT || midr_sync_waiting_count(store))
		return;
	if (store->ctx->lsdb_store) {
		struct midr_lsdb_summary summary;

		if (midr_lsdb_summary_get(store->ctx, &summary) != 0 ||
		    !summary.ready ||
		    summary.dirty_count || summary.derivation_pending ||
		    summary.last_error)
			return;
	}
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
	/* A timeout is a usable initial-sync outcome.  Wake derivation even
	 * when no prior READY TED exists; otherwise the first candidate can
	 * remain NOT_READY indefinitely. */
	midr_lsdb_sync_changed(store->ctx);
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
		struct midr_sync_session *session;

		if (!peer->connection || !peer_established(peer->connection) ||
		    !peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS])
			continue;
		session = midr_sync_session_create(store, peer, peer->connection);
		entry = midr_sync_peer_ensure(store, peer, session->generation);
		if (!entry)
			continue;
		entry->generation = session->generation;
		entry->waiting = !midr_sync_receive_drained(session);
		entry->timed_out = false;
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
	store->sessions = list_new();
	store->sessions->del = midr_sync_session_free;
	store->shutdown_targets = list_new();
	store->shutdown_targets->del = midr_sync_shutdown_target_free;
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
	list_delete(&store->shutdown_targets);
	list_delete(&store->sessions);
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
	struct peer_connection *connection;
	struct midr_sync_peer *entry;
	bool clear_timeout = false;

	if (!ctx || !ctx->sync_store || !peer)
		return;
	store = ctx->sync_store;
	connection = peer->connection;
	if (connection && peer_established(connection) &&
	    peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS]) {
		struct midr_sync_session *session =
			midr_sync_session_create(store, peer, connection);

		if (!session)
			return;
		entry = midr_sync_peer_ensure(store, peer, session->generation);
		if (entry && session && entry->generation != session->generation) {
			entry->generation = session->generation;
			entry->waiting = true;
			entry->timed_out = false;
		}
		return;
	}
	if (connection)
		midr_sync_session_mark_down(store, connection);

	entry = midr_sync_peer_lookup(store, peer);
	if (!entry)
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
	if (peer)
		midr_sync_peer_eor_connection(ctx, peer->connection);
}

void midr_sync_peer_eor_connection(struct midr_context *ctx,
					   struct peer_connection *connection)
{
	struct midr_sync_store *store;
	struct midr_sync_peer *entry;
	struct midr_sync_session *session;
	bool first_eor;

	if (!ctx || !ctx->sync_store || !connection)
		return;
	store = ctx->sync_store;
	session = midr_sync_session_lookup(store, connection);
	if (!session) {
		if (!peer_established(connection) ||
		    !connection->peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS]) {
			store->stale_event_count++;
			return;
		}
		session = midr_sync_session_create(store, connection->peer,
						   connection);
		if (!session) {
			store->stale_event_count++;
			return;
		}
	}
	if (session) {
		first_eor = !session->remote_eor;
		if (first_eor)
			session->remote_eor = true;
		if (first_eor)
			/* An empty snapshot has no route hook to wake LSDB. */
			midr_lsdb_sync_changed(ctx);
		if (session->local_eor_written && midr_sync_receive_drained(session))
			session->state = MIDR_SYNC_SESSION_READY;
	}
	entry = midr_sync_peer_ensure(store, connection->peer,
					      session->generation);
	if (entry && entry->generation == session->generation &&
	    midr_sync_receive_drained(session)) {
		entry->waiting = false;
		if (entry->timed_out) {
			entry->timed_out = false;
			midr_lsdb_sync_changed(ctx);
		}
	}
	midr_sync_complete_if_ready(store);
}

bool midr_sync_can_send_eor(struct midr_context *ctx,
					struct peer_connection *connection)
{
	struct midr_sync_store *store;
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store || !connection ||
	    !peer_established(connection) ||
	    !connection->peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] ||
	    !midr_sync_local_ready(ctx))
		return false;
	store = ctx->sync_store;
	session = midr_sync_session_lookup(store, connection);
	if (!session)
		session = midr_sync_session_create(store, connection->peer, connection);
	return session && session->snapshot_completed && !session->snapshot_active &&
	       !session->needs_resync && !session->pending_updates &&
	       !session->local_eor_queued;
}

void midr_sync_packet_queued(struct peer_connection *connection,
					     struct stream *stream, bool eor)
{
	struct midr_context *ctx;
	struct midr_sync_store *store;
	struct midr_sync_session *session;
	struct midr_sync_session_packet *packet;

	if (!connection || !stream || !connection->peer || !connection->peer->bgp)
		return;
	if (!connection->peer->bgp->midr_info)
		return;
	ctx = &connection->peer->bgp->midr_info->ctx;
	if (!ctx || !ctx->sync_store)
		return;
	store = ctx->sync_store;
	session = midr_sync_session_lookup(store, connection);
	if (!session)
		return;
	packet = XCALLOC(MTYPE_MIDR_SYNC_PACKET, sizeof(*packet));
	packet->stream = stream;
	packet->connection = connection;
	packet->token = session->token;
	packet->token->references++;
	packet->eor = eor;
	listnode_add(session->packets, packet);
	if (eor)
		session->local_eor_queued = true;
	else
		session->pending_updates++;
	pthread_mutex_lock(&midr_sync_packets_mutex);
	packet->next = midr_sync_packets;
	midr_sync_packets = packet;
	pthread_mutex_unlock(&midr_sync_packets_mutex);
}

static void midr_sync_packet_complete(struct peer_connection *connection,
				     struct stream *stream, bool written)
{
	struct midr_sync_session_packet **link;
	struct midr_sync_session_packet *packet;

	if (!connection || !stream || !bm || !bm->master)
		return;
	pthread_mutex_lock(&midr_sync_packets_mutex);
	for (link = &midr_sync_packets; *link; link = &(*link)->next) {
		packet = *link;
		if (packet->connection != connection || packet->stream != stream)
			continue;
		*link = packet->next;
		packet->stream = NULL;
		packet->written = written;
		event_add_event(bm->master, midr_sync_packet_event, packet, 0,
				&packet->completion);
		break;
	}
	pthread_mutex_unlock(&midr_sync_packets_mutex);
}

void midr_sync_packet_written(struct peer_connection *connection,
			      struct stream *stream)
{
	midr_sync_packet_complete(connection, stream, true);
}

void midr_sync_packet_dropped(struct peer_connection *connection,
			      struct stream *stream)
{
	midr_sync_packet_complete(connection, stream, false);
}

void midr_sync_snapshot_begin(struct peer_connection *connection)
{
	struct midr_context *ctx;
	struct midr_sync_session *session;

	if (!connection || !connection->peer || !connection->peer->bgp ||
	    !connection->peer->bgp->midr_info)
		return;
	ctx = &connection->peer->bgp->midr_info->ctx;
	if (!ctx->sync_store)
		return;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (!session || session->snapshot_active ||
	    !session->snapshot_requested)
		return;
	session->snapshot_active = true;
	session->snapshot_completed = false;
	session->snapshot_generation++;
	session->local_eor_queued = false;
	session->local_eor_written = false;
	session->needs_resync = false;
	session->state = MIDR_SYNC_SESSION_SYNCING;
}

void midr_sync_snapshot_end(struct peer_connection *connection)
{
	struct midr_context *ctx;
	struct midr_sync_session *session;

	if (!connection || !connection->peer || !connection->peer->bgp ||
	    !connection->peer->bgp->midr_info)
		return;
	ctx = &connection->peer->bgp->midr_info->ctx;
	if (!ctx->sync_store)
		return;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (!session || !session->snapshot_active)
		return;
	session->snapshot_active = false;
	session->snapshot_completed = true;
	session->snapshot_requested = false;
}

bool midr_sync_snapshot_requested(struct peer_connection *connection)
{
	struct midr_context *ctx;
	struct midr_sync_session *session;

	if (!connection || !connection->peer || !connection->peer->bgp ||
	    !connection->peer->bgp->midr_info)
		return false;
	ctx = &connection->peer->bgp->midr_info->ctx;
	if (!ctx->sync_store)
		return false;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	return session && session->snapshot_requested;
}

void midr_sync_request_resync(struct peer_connection *connection)
{
	struct midr_context *ctx;
	struct midr_sync_session *session;

	if (!connection || !connection->peer || !connection->peer->bgp ||
	    !connection->peer->bgp->midr_info)
		return;
	ctx = &connection->peer->bgp->midr_info->ctx;
	if (!ctx->sync_store)
		return;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (!session)
		return;
	session->needs_resync = true;
	session->state = MIDR_SYNC_SESSION_RESYNC;
	midr_sync_schedule_resync(session);
}

static bool midr_sync_peer_output_idle(struct midr_sync_session *session)
{
	struct peer_af *paf;
	struct listnode *node;
	struct midr_sync_session_packet *packet;

	if (!session || session->snapshot_active || session->pending_updates)
		return false;
	for (ALL_LIST_ELEMENTS_RO(session->packets, node, packet))
		if (packet->eor)
			return false;
	paf = peer_af_find(session->peer, AFI_BGP_LS, SAFI_MIDR_LS);
	if (!paf)
		return true;
	if (paf->t_announce_route ||
	    (paf->next_pkt_to_send && paf->next_pkt_to_send->buffer) ||
	    !advertise_list_is_empty(PAF_SUBGRP(paf)))
		return false;
	return true;
}

static struct midr_sync_session *midr_sync_session_lookup_generation(
	struct midr_sync_store *store, struct peer_connection *connection,
	uint64_t generation)
{
	struct listnode *node;
	struct midr_sync_session *session;

	for (ALL_LIST_ELEMENTS_RO(store->sessions, node, session))
		if (session->connection == connection &&
		    session->generation == generation)
			return session;
	return NULL;
}

void midr_sync_shutdown_begin(struct midr_context *ctx)
{
	struct listnode *node;
	struct midr_sync_session *session;
	struct midr_sync_shutdown_target *target;

	if (!ctx || !ctx->sync_store)
		return;
	list_delete_all_node(ctx->sync_store->shutdown_targets);
	ctx->sync_store->shutdown_active = true;
	ctx->sync_store->shutdown_announce_complete = false;
	ctx->sync_store->shutdown_state = MIDR_SYNC_SHUTDOWN_WAITING;
	for (ALL_LIST_ELEMENTS_RO(ctx->sync_store->sessions, node, session)) {
		if (session->state == MIDR_SYNC_SESSION_DOWN ||
		    !peer_established(session->connection))
			continue;
		target = XCALLOC(MTYPE_MIDR_SYNC_SHUTDOWN_TARGET,
					 sizeof(*target));
		target->connection = session->connection;
		target->generation = session->generation;
		listnode_add(ctx->sync_store->shutdown_targets, target);
	}
}

void midr_sync_shutdown_announce_complete(struct midr_context *ctx)
{
	if (ctx && ctx->sync_store && ctx->sync_store->shutdown_active) {
		ctx->sync_store->shutdown_announce_complete = true;
		if (ctx->sync_store->shutdown_state ==
		    MIDR_SYNC_SHUTDOWN_GENERATION_FAILED)
			ctx->sync_store->shutdown_state =
				MIDR_SYNC_SHUTDOWN_WAITING;
	}
}

void midr_sync_shutdown_generation_failed(struct midr_context *ctx)
{
	if (!ctx || !ctx->sync_store || !ctx->sync_store->shutdown_active)
		return;
	ctx->sync_store->shutdown_announce_complete = false;
	ctx->sync_store->shutdown_state =
		MIDR_SYNC_SHUTDOWN_GENERATION_FAILED;
}

void midr_sync_shutdown_expired(struct midr_context *ctx)
{
	struct midr_sync_store *store;

	if (!ctx || !(store = ctx->sync_store) || !store->shutdown_active)
		return;
	if (store->shutdown_state == MIDR_SYNC_SHUTDOWN_WAITING)
		store->shutdown_state = MIDR_SYNC_SHUTDOWN_DEGRADED;
}

bool midr_sync_shutdown_ready(struct midr_context *ctx)
{
	struct listnode *node;
	struct midr_sync_shutdown_target *target;
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store || !ctx->sync_store->shutdown_active ||
	    !ctx->sync_store->shutdown_announce_complete)
		return false;
	if (ctx->sync_store->shutdown_state == MIDR_SYNC_SHUTDOWN_DEGRADED)
		return true;
	for (ALL_LIST_ELEMENTS_RO(ctx->sync_store->shutdown_targets, node, target)) {
		if (target->failed)
			return false;
		session = midr_sync_session_lookup_generation(
			ctx->sync_store, target->connection, target->generation);
		if (session && !midr_sync_peer_output_idle(session))
			return false;
	}
	if (ctx->sync_store->shutdown_state == MIDR_SYNC_SHUTDOWN_WAITING)
		ctx->sync_store->shutdown_state = MIDR_SYNC_SHUTDOWN_COMPLETE;
	return true;
}

bool midr_sync_shutdown_degraded(struct midr_context *ctx)
{
	return ctx && ctx->sync_store && ctx->sync_store->shutdown_active &&
	       ctx->sync_store->shutdown_state == MIDR_SYNC_SHUTDOWN_DEGRADED;
}

void midr_sync_shutdown_finish(struct midr_context *ctx)
{
	struct midr_sync_store *store;

	if (!ctx || !(store = ctx->sync_store))
		return;
	store->shutdown_active = false;
	store->shutdown_announce_complete = false;
	list_delete_all_node(store->shutdown_targets);
}

void midr_sync_shutdown_cancel(struct midr_context *ctx)
{
	if (!ctx || !ctx->sync_store)
		return;
	ctx->sync_store->shutdown_active = false;
	ctx->sync_store->shutdown_announce_complete = false;
	ctx->sync_store->shutdown_state = MIDR_SYNC_SHUTDOWN_IDLE;
	list_delete_all_node(ctx->sync_store->shutdown_targets);
}

int midr_sync_connection_generation(struct midr_context *ctx,
				    struct peer_connection *connection,
				    uint64_t *generation)
{
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store)
		return -ENOENT;
	if (!connection || !generation)
		return -EINVAL;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (!session)
		return -ENOENT;
	*generation = session->generation;
	return 0;
}

void midr_sync_input_begin(struct midr_context *ctx,
					   struct peer_connection *connection)
{
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store || !connection)
		return;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (session)
		session->pending_input++;
}

void midr_sync_input_end(struct midr_context *ctx,
			struct peer_connection *connection, bool accepted)
{
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store || !connection)
		return;
	session = midr_sync_session_lookup(ctx->sync_store, connection);
	if (!session)
		return;
	if (session->pending_input)
		session->pending_input--;
	if (!accepted)
		midr_sync_request_resync(connection);
	if (session->remote_eor)
		midr_sync_peer_eor_connection(ctx, connection);
}

void midr_sync_derivation_complete(struct midr_context *ctx)
{
	struct listnode *node;
	struct midr_sync_session *session;

	if (!ctx || !ctx->sync_store)
		return;
	for (ALL_LIST_ELEMENTS_RO(ctx->sync_store->sessions, node, session))
		if (session->state != MIDR_SYNC_SESSION_DOWN && session->remote_eor)
			midr_sync_peer_eor_connection(ctx, session->connection);
}

void midr_sync_connection_down(struct midr_context *ctx,
					       struct peer_connection *connection)
{
	struct midr_sync_store *store;
	struct midr_sync_session *session;
	struct midr_sync_peer *entry;
	struct listnode *node, *next;

	if (!ctx || !ctx->sync_store || !connection)
		return;
	store = ctx->sync_store;
	for (ALL_LIST_ELEMENTS(store->sessions, node, next, session)) {
		if (session->connection != connection)
			continue;
		entry = midr_sync_peer_lookup(store, session->peer);
		if (entry && entry->generation == session->generation) {
			listnode_delete(store->peers, entry);
			midr_sync_peer_free(entry);
		}
		if (store->shutdown_active) {
			struct midr_sync_shutdown_target *target =
				midr_sync_shutdown_target_lookup(
					store, connection, session->generation);

			if (target) {
				target->failed = true;
				store->shutdown_state =
					MIDR_SYNC_SHUTDOWN_DEGRADED;
			}
		}
		list_delete_node(store->sessions, node);
		midr_sync_session_free(session);
	}
	midr_sync_complete_if_ready(store);
}

void midr_sync_test_session_start(struct midr_context *ctx, struct peer *peer,
					  struct peer_connection *connection)
{
	if (!ctx || !ctx->sync_store)
		return;
	(void)midr_sync_session_create(ctx->sync_store, peer, connection);
}

void midr_sync_test_session_down(struct midr_context *ctx,
					 struct peer_connection *connection)
{
	if (!ctx || !ctx->sync_store)
		return;
	midr_sync_session_mark_down(ctx->sync_store, connection);
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
	struct midr_sync_session *session;

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
	status->next_session_generation = store->next_session_generation;
	status->reconnect_count = store->reconnect_count;
	status->stale_event_count = store->stale_event_count;
	status->shutdown_state = store->shutdown_state;
	status->shutdown_active = store->shutdown_active;
	for (ALL_LIST_ELEMENTS_RO(store->peers, node, entry)) {
		if (entry->waiting)
			status->waiting_peer_count++;
		if (entry->timed_out)
			status->timed_out_peer_count++;
	}
	for (ALL_LIST_ELEMENTS_RO(store->sessions, node, session)) {
		if (session->state == MIDR_SYNC_SESSION_DOWN)
			continue;
		status->active_session_count++;
		if (session->state == MIDR_SYNC_SESSION_SYNCING)
			status->syncing_session_count++;
		if (session->remote_eor)
			status->eor_received_count++;
		if (session->local_eor_written)
			status->eor_written_count++;
		if (midr_sync_receive_drained(session))
			status->receive_drained_count++;
		if (session->needs_resync)
			status->resync_required_count++;
		if (session->snapshot_active)
			status->snapshot_active_count++;
		if (session->snapshot_completed)
			status->snapshot_completed_count++;
		status->pending_update_count += session->pending_updates;
		status->pending_input_count += session->pending_input;
	}
	if (store->shutdown_active) {
		struct midr_sync_shutdown_target *target;

		for (ALL_LIST_ELEMENTS_RO(store->shutdown_targets, node, target)) {
			session = midr_sync_session_lookup_generation(
				store, target->connection, target->generation);
			if (!target->failed && session &&
			    !midr_sync_peer_output_idle(session))
				status->shutdown_pending_count++;
		}
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

void midr_sync_test_drain_completions(struct midr_context *ctx)
{
	struct listnode *node, *pnode, *next;
	struct midr_sync_session *session;
	struct midr_sync_session_packet *packet;
	struct event event = {};

	if (!ctx || !ctx->sync_store)
		return;
	for (ALL_LIST_ELEMENTS_RO(ctx->sync_store->sessions, node, session))
		for (ALL_LIST_ELEMENTS(session->packets, pnode, next, packet)) {
			if (!packet->completion)
				continue;
			event_cancel(&packet->completion);
			event.arg = packet;
			midr_sync_packet_event(&event);
		}
}
