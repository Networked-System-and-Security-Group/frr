// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR End-of-RIB barrier tests.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"
#include "stream.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;

static void process_until_no_pending_update(void)
{
	struct midr_sync_status status;

	midr_sync_test_drain_completions(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.pending_update_count == 0);
}

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct peer *established_peer(const char *id)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer && peer->connection);
	peer->remote_id.s_addr = router_id(id);
	peer->connection->status = Established;
	peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] = 1;
	return peer;
}

static void test_no_peer_and_configuration(void)
{
	struct midr_sync_status status;
	uint64_t reasons = 0;

	assert(!midr_sync_view_ready(ctx, false, &reasons));
	assert(midr_sync_view_ready(ctx, true, &reasons));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_READY);
	assert(status.initial_peer_count == 0);
	assert(midr_sync_timeout_set(ctx, 0) == -EINVAL);
	assert(midr_sync_timeout_set(ctx, 3601) == -EINVAL);
	assert(midr_sync_timeout_set(ctx, 5) == 0);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.timeout_seconds == 5);
	assert(!midr_sync_view_ready(ctx, false, &reasons));
}

static void test_eor_peer_down_timeout_and_late_eor(void)
{
	struct peer *peer = established_peer("10.0.0.2");
	struct midr_sync_status status;
	uint64_t reasons = 0;

	/* Peer establishment can precede local input readiness. */
	midr_sync_peer_status_changed(ctx, peer);
	assert(!midr_sync_view_ready(ctx, true, &reasons));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_REMOTE_WAIT);
	assert(status.initial_peer_count == 1);
	assert(status.waiting_peer_count == 1);
	midr_sync_peer_eor(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_READY);

	assert(!midr_sync_view_ready(ctx, false, &reasons));
	UNSET_FLAG(peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS], PEER_STATUS_EOR_RECEIVED);
	midr_sync_test_session_down(ctx, peer->connection);
	assert(!midr_sync_view_ready(ctx, true, &reasons));
	midr_sync_test_timeout(ctx);
	reasons = 0;
	assert(midr_sync_view_ready(ctx, true, &reasons));
	assert(reasons & MIDR_TED_SYNC_REASON_EOR_TIMEOUT);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.timed_out_peer_count == 1);
	midr_sync_peer_eor(ctx, peer);
	reasons = 0;
	assert(midr_sync_view_ready(ctx, true, &reasons));
	assert(!(reasons & MIDR_TED_SYNC_REASON_EOR_TIMEOUT));

	assert(!midr_sync_view_ready(ctx, false, &reasons));
	peer->connection->status = Established;
	UNSET_FLAG(peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS], PEER_STATUS_EOR_RECEIVED);
	midr_sync_test_session_down(ctx, peer->connection);
	assert(!midr_sync_view_ready(ctx, true, &reasons));
	midr_sync_test_timeout(ctx);
	reasons = 0;
	assert(midr_sync_view_ready(ctx, true, &reasons));
	assert(reasons & MIDR_TED_SYNC_REASON_EOR_TIMEOUT);
	peer->connection->status = Idle;
	midr_sync_peer_status_changed(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_READY);
	assert(status.timed_out_peer_count == 0);
	reasons = 0;
	assert(midr_sync_view_ready(ctx, true, &reasons));
	assert(!(reasons & MIDR_TED_SYNC_REASON_EOR_TIMEOUT));
}

static void test_session_generation_and_packet_lifecycle(void)
{
	struct peer *peer = established_peer("10.0.0.3");
	struct peer_connection *old_connection = peer->connection;
	struct peer_connection *new_connection;
	struct midr_sync_status status;
	struct stream *stream;
	uint64_t reasons = 0;
	uint64_t first_generation;
	uint64_t reconnects;

	assert(midr_sync_view_ready(ctx, false, &reasons) == false);
	assert(midr_sync_view_ready(ctx, true, &reasons) == false);
	midr_sync_peer_status_changed(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.active_session_count == 1);
	first_generation = status.next_session_generation;
	reconnects = status.reconnect_count;
	assert(first_generation != 0);

	stream = stream_new(64);
	midr_sync_snapshot_begin(old_connection);
	midr_sync_snapshot_end(old_connection);
	midr_sync_packet_queued(old_connection, stream, false);
	assert(!midr_sync_can_send_eor(ctx, old_connection));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.pending_update_count == 1);
	midr_sync_packet_written(old_connection, stream);
	process_until_no_pending_update();
	stream_free(stream);
	assert(midr_sync_can_send_eor(ctx, old_connection));

	old_connection->status = Idle;
	midr_sync_peer_status_changed(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.active_session_count == 0);

	new_connection = bgp_peer_connection_new(peer, NULL, CONNECTION_OUTGOING);
	assert(new_connection);
	new_connection->status = Established;
	peer->afc_nego[AFI_BGP_LS][SAFI_MIDR_LS] = 1;
	midr_sync_test_session_start(ctx, peer, new_connection);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.active_session_count == 1);
	assert(status.next_session_generation == first_generation + 1);
	assert(status.reconnect_count == reconnects + 1);

	midr_sync_snapshot_begin(new_connection);
	midr_sync_snapshot_end(new_connection);
	midr_sync_peer_eor_connection(ctx, new_connection);
	assert(midr_sync_can_send_eor(ctx, new_connection));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.eor_received_count == 1);
	assert(status.syncing_session_count == 1);

	stream = stream_new(64);
	midr_sync_packet_queued(new_connection, stream, true);
	assert(!midr_sync_can_send_eor(ctx, new_connection));
	midr_sync_packet_written(new_connection, stream);
	midr_sync_test_drain_completions(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.syncing_session_count == 0);
	assert(status.eor_written_count == 1);
	stream_free(stream);
	midr_sync_test_session_down(ctx, new_connection);
	new_connection->status = Idle;
	event_cancel(&new_connection->t_generate_updgrp_packets);
	bgp_peer_connection_free(&new_connection);
	event_cancel(&old_connection->t_generate_updgrp_packets);

}

static void test_delayed_completion_reuse_and_destroy(void)
{
	struct peer *peer = established_peer("10.0.0.4");
	struct peer_connection *connection = peer->connection;
	struct midr_sync_status status;
	struct stream *stream = stream_new(64);
	uint64_t reasons = 0;
	uint64_t generation;

	assert(!midr_sync_view_ready(ctx, false, &reasons));
	assert(!midr_sync_view_ready(ctx, true, &reasons));
	assert(midr_sync_status_get(ctx, &status) == 0);
	generation = status.next_session_generation;
	midr_sync_snapshot_begin(connection);
	midr_sync_snapshot_end(connection);
	midr_sync_packet_queued(connection, stream, false);
	midr_sync_packet_written(connection, stream);
	/* Reuse both addresses before the old completion reaches the main loop. */
	midr_sync_test_session_down(ctx, connection);
	SET_FLAG(peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS], PEER_STATUS_EOR_RECEIVED);
	midr_sync_test_session_start(ctx, peer, connection);
	midr_sync_snapshot_begin(connection);
	midr_sync_snapshot_end(connection);
	midr_sync_packet_queued(connection, stream, false);
	midr_sync_test_drain_completions(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.next_session_generation == generation + 1);
	assert(status.pending_update_count == 1);
	assert(status.eor_received_count == 0);
	midr_sync_packet_dropped(connection, stream);
	process_until_no_pending_update();
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.resync_required_count == 1);
	assert(!midr_sync_can_send_eor(ctx, connection));

	midr_sync_test_session_down(ctx, connection);
	midr_sync_test_session_start(ctx, peer, connection);
	midr_sync_snapshot_begin(connection);
	midr_sync_snapshot_end(connection);
	midr_sync_packet_queued(connection, stream, false);
	midr_sync_packet_written(connection, stream);
	midr_sync_finish(ctx);
	/* Completion and cancellation must not dereference the destroyed store. */
	midr_sync_packet_dropped(connection, stream);
	assert(midr_sync_init(ctx) == 0);
	midr_sync_test_drain_completions(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.active_session_count == 0);
	assert(status.pending_update_count == 0);
	event_cancel(&connection->t_generate_updgrp_packets);
	stream_free(stream);

}

static void test_eor_waits_for_admission_and_derivation(void)
{
	struct peer *peer;
	struct listnode *node;
	struct midr_sync_status status;
	struct midr_lsdb_summary lsdb;
	struct midr_instance instance = {
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key.type = MIDR_NLRI_TYPE_MEMBERSHIP,
			.ls_sequence = 1,
			.payload.membership.group_id = 1,
		},
	};
	uint64_t reasons = 0;

	for (ALL_LIST_ELEMENTS_RO(bgp->peer, node, peer)) {
		midr_sync_test_session_down(ctx, peer->connection);
		peer->connection->status = Idle;
	}
	peer = established_peer("10.0.0.5");
	instance.object.key.originator_node_id = peer->remote_id.s_addr;
	assert(!midr_sync_view_ready(ctx, false, &reasons));
	assert(!midr_sync_view_ready(ctx, true, &reasons));
	midr_sync_input_begin(ctx, peer->connection);
	midr_sync_peer_eor(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.waiting_peer_count == 1);
	assert(midr_rib_instance_upsert(ctx, peer, &instance, 0) == 0);
	midr_sync_input_end(ctx, peer->connection, true);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(lsdb.dirty_count > 0);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.waiting_peer_count == 1);
	midr_lsdb_test_fail_next_prepare(ctx);
	assert(midr_lsdb_test_process(ctx) == -ENOMEM);
	midr_sync_peer_eor(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.waiting_peer_count == 1);
	assert(midr_lsdb_test_process(ctx) == 0);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(midr_lsdb_summary_get(ctx, &lsdb) == 0);
	assert(status.waiting_peer_count == 1);
	assert(status.receive_drained_count == 0);
	assert(!lsdb.ready);
	assert(!lsdb.derivation_pending);

	midr_sync_test_session_down(ctx, peer->connection);
	midr_sync_test_session_start(ctx, peer, peer->connection);
	midr_sync_input_begin(ctx, peer->connection);
	midr_sync_input_end(ctx, peer->connection, false);
	midr_sync_peer_eor(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.resync_required_count == 1);
	assert(status.receive_drained_count == 0);
	assert(!midr_sync_can_send_eor(ctx, peer->connection));
}

struct writer_completion {
	struct peer_connection *connection;
	struct stream *stream;
};

static void *writer_complete(void *arg)
{
	struct writer_completion *completion = arg;

	midr_sync_packet_written(completion->connection, completion->stream);
	return NULL;
}

static void test_writer_teardown_race(void)
{
	struct peer *peer = established_peer("10.0.0.6");
	struct writer_completion completion = {.connection = peer->connection};
	struct midr_sync_status status;
	pthread_t writer;

	for (unsigned int i = 0; i < 32; i++) {
		midr_sync_test_session_start(ctx, peer, peer->connection);
		midr_sync_snapshot_begin(peer->connection);
		midr_sync_snapshot_end(peer->connection);
		completion.stream = stream_new(64);
		midr_sync_packet_queued(peer->connection, completion.stream, false);
		assert(pthread_create(&writer, NULL, writer_complete, &completion) == 0);
		midr_sync_connection_down(ctx, peer->connection);
		assert(pthread_join(writer, NULL) == 0);
		midr_sync_test_drain_completions(ctx);
		assert(midr_sync_status_get(ctx, &status) == 0);
		assert(status.pending_update_count == 0);
		stream_free(completion.stream);
	}
}

static void test_shutdown_result_is_bounded_and_retained(void)
{
	struct peer *peer = established_peer("10.0.0.7");
	struct midr_sync_status status;
	struct stream *stream;

	midr_sync_test_session_start(ctx, peer, peer->connection);
	midr_sync_snapshot_begin(peer->connection);
	midr_sync_snapshot_end(peer->connection);
	stream = stream_new(64);
	midr_sync_packet_queued(peer->connection, stream, false);

	midr_sync_shutdown_begin(ctx);
	midr_sync_shutdown_announce_complete(ctx);
	assert(!midr_sync_shutdown_ready(ctx));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.shutdown_state == MIDR_SYNC_SHUTDOWN_WAITING);
	midr_sync_shutdown_expired(ctx);
	assert(midr_sync_shutdown_ready(ctx));
	midr_sync_packet_dropped(peer->connection, stream);
	midr_sync_test_drain_completions(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.shutdown_state == MIDR_SYNC_SHUTDOWN_DEGRADED);
	midr_sync_shutdown_finish(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(!status.shutdown_active);
	assert(status.shutdown_state == MIDR_SYNC_SHUTDOWN_DEGRADED);

	midr_sync_shutdown_begin(ctx);
	midr_sync_shutdown_announce_complete(ctx);
	assert(midr_sync_shutdown_ready(ctx));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.shutdown_state == MIDR_SYNC_SHUTDOWN_COMPLETE);
	midr_sync_shutdown_finish(ctx);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.shutdown_state == MIDR_SYNC_SHUTDOWN_COMPLETE);
	midr_sync_shutdown_cancel(ctx);
	stream_free(stream);
}

int main(void)
{
	as_t asn = 65000;

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR sync");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL, ASNOTATION_PLAIN) >= 0);
	bgp->router_id.s_addr = router_id("10.0.0.1");
	ctx = &bgp->midr_info->ctx;

	test_no_peer_and_configuration();
	/* The following cases exercise the sync state machine without the
	 * LSDB/TED readiness gate. Full integration coverage follows below. */
	midr_lsdb_finish(ctx);
	test_eor_peer_down_timeout_and_late_eor();
	test_session_generation_and_packet_lifecycle();
	test_delayed_completion_reuse_and_destroy();
	assert(midr_lsdb_init(ctx) == 0);
	test_eor_waits_for_admission_and_derivation();
	test_writer_teardown_race();
	test_shutdown_result_is_bounded_and_retained();
	assert(midr_sync_status_get(NULL, &(struct midr_sync_status){}) == -ENOENT);
	assert(midr_sync_status_get(ctx, NULL) == -EINVAL);
	puts("MIDR sync tests passed");
	return 0;
}
