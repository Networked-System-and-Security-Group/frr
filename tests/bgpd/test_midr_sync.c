// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR End-of-RIB barrier tests.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "command.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_sync.h"
#include "bgpd/bgp_midr_ted.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

static struct bgp *bgp;
static struct midr_context *ctx;

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

	assert(!midr_sync_view_ready(ctx, true, &reasons));
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_REMOTE_WAIT);
	assert(status.waiting_peer_count == 1);
	midr_sync_peer_eor(ctx, peer);
	assert(midr_sync_status_get(ctx, &status) == 0);
	assert(status.state == MIDR_SYNC_READY);

	assert(!midr_sync_view_ready(ctx, false, &reasons));
	UNSET_FLAG(peer->af_sflags[AFI_BGP_LS][SAFI_MIDR_LS], PEER_STATUS_EOR_RECEIVED);
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
	test_eor_peer_down_timeout_and_late_eor();
	assert(midr_sync_status_get(NULL, &(struct midr_sync_status){}) == -ENOENT);
	assert(midr_sync_status_get(ctx, NULL) == -EINVAL);
	puts("MIDR sync tests passed");
	return 0;
}
