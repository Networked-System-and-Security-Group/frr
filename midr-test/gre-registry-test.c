/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MIDR GRE registry error-path tests.
 *
 * Covers the review items for midr_gre_registry_add():
 *   - calloc() failure is reported, leaks nothing and leaves the tunnel
 *     unregistered;
 *   - rebinding an existing device name to new endpoints updates the entry
 *     in place (no stale endpoint binding left behind);
 *   - the same endpoint pair requested under a different explicit name does
 *     not create a second, ambiguous entry (the explicit name wins);
 *   - re-adding the identical name and endpoints is a no-op.
 *
 * The GRE ZAPI send is wrapped so this runs without a real zebra; the shared
 * zclient only needs a non-negative socket.  Real netdevice coverage lives in
 * midr-gre-connectivity-test.sh.
 */

#define main midrd_program_main
#include "midrd.c"
#undef main

#include <arpa/inet.h>
#include <assert.h>

#include "zclient.h"

#include "midr-dp-backend.h"
#include "midr-gre.h"

/* ------------------------------------------------------------------ *
 *  Wrapped ZAPI traffic and allocation fault injection
 * ------------------------------------------------------------------ */
static size_t gre_add_calls;
static size_t gre_del_calls;
static bool fail_next_calloc;

enum zclient_send_status __wrap_zclient_send_gre_add(
	struct zclient *client, vrf_id_t vrf_id,
	const struct zclient_gre_if *gre)
{
	(void)client;
	(void)vrf_id;
	(void)gre;
	gre_add_calls++;
	return ZCLIENT_SEND_SUCCESS;
}

enum zclient_send_status __wrap_zclient_send_gre_delete(
	struct zclient *client, vrf_id_t vrf_id, const char *ifname)
{
	(void)client;
	(void)vrf_id;
	(void)ifname;
	gre_del_calls++;
	return ZCLIENT_SEND_SUCCESS;
}

void *__real_calloc(size_t nmemb, size_t size);

/* Fail exactly the next calloc() issued from the linked test objects, so we
 * can drive the registry's allocation-failure path deterministically. */
void *__wrap_calloc(size_t nmemb, size_t size)
{
	if (fail_next_calloc) {
		fail_next_calloc = false;
		return NULL;
	}
	return __real_calloc(nmemb, size);
}

/* ------------------------------------------------------------------ *
 *  Helpers
 * ------------------------------------------------------------------ */
static void ted_create(struct midr_context *ctx)
{
	struct midr_ted_config config = {
		.max_events = 16,
	};

	memset(ctx, 0, sizeof(*ctx));
	ctx->node_id = 1;
	assert(midr_ted_create(&config, &ctx->ted) == 0);
}

static struct ipaddr ipv4(const char *text)
{
	struct ipaddr ia = {};

	SET_IPADDR_V4(&ia);
	assert(inet_pton(AF_INET, text, &ia.ipaddr_v4) == 1);
	return ia;
}

static const char *name_for(vrf_id_t vrf, const struct ipaddr *local,
			    const struct ipaddr *remote)
{
	return midr_gre_interface_name(vrf, local, remote);
}

static void add_expect_ok(struct midr_context *ctx,
			  const struct midr_gre_tunnel *tun)
{
	struct midr_gre_status status = {};

	assert(midr_gre_interface_add(ctx, tun, &status) == 0);
}

static void test_alloc_failure_is_reported(void)
{
	struct midr_context ctx;
	struct midr_gre_tunnel tun = {};
	struct midr_gre_status status = {};
	struct ipaddr local = ipv4("10.9.9.1");
	struct ipaddr remote = ipv4("10.9.9.2");

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-gre-registry-test");
	assert(ctx.master);
	midr_gre_init(ctx.master);
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	midr_dp_backend_zclient()->sock = 0;

	strlcpy(tun.ifname, "midr9", sizeof(tun.ifname));
	tun.vrf_id = VRF_DEFAULT;
	tun.local = local;
	tun.remote = remote;

	/* Force the registry's calloc() to fail. */
	fail_next_calloc = true;
	assert(midr_gre_interface_add(&ctx, &tun, &status) == -1);
	assert(status.state == MIDR_GRE_STATE_FAILED);
	assert(status.err == ENOMEM);
	assert(!fail_next_calloc); /* the injected failure was consumed */

	/* Nothing was registered, so an endpoint lookup must miss. */
	assert(name_for(VRF_DEFAULT, &local, &remote) == NULL);

	/* And a retry after the failure succeeds cleanly. */
	add_expect_ok(&ctx, &tun);
	assert(name_for(VRF_DEFAULT, &local, &remote) != NULL);

	midr_dp_backend_stop();
	midr_gre_fini();
	midr_ted_destroy(&ctx.ted);
}

static void test_rebind_updates_in_place(void)
{
	struct midr_context ctx;
	struct midr_gre_tunnel tun = {};
	struct midr_gre_status status = {};
	struct ipaddr l1 = ipv4("10.0.0.1");
	struct ipaddr r1 = ipv4("10.0.0.2");
	struct ipaddr l2 = ipv4("10.0.0.3");
	struct ipaddr r2 = ipv4("10.0.0.4");

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-gre-registry-test");
	assert(ctx.master);
	midr_gre_init(ctx.master);
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	midr_dp_backend_zclient()->sock = 0;

	strlcpy(tun.ifname, "midr0", sizeof(tun.ifname));
	tun.vrf_id = VRF_DEFAULT;
	tun.local = l1;
	tun.remote = r1;
	add_expect_ok(&ctx, &tun);
	assert(strcmp(name_for(VRF_DEFAULT, &l1, &r1), "midr0") == 0);

	/* Rebind the same name to a new endpoint pair. */
	tun.local = l2;
	tun.remote = r2;
	add_expect_ok(&ctx, &tun);

	/* Name lookup still resolves, but now to the new endpoints only. */
	assert(midr_gre_interface_get_state(VRF_DEFAULT, "midr0", &status) == 0);
	assert(strcmp(name_for(VRF_DEFAULT, &l2, &r2), "midr0") == 0);
	/* The stale endpoint binding is gone. */
	assert(name_for(VRF_DEFAULT, &l1, &r1) == NULL);

	/* Idempotent re-add of the identical mapping changes nothing. */
	add_expect_ok(&ctx, &tun);
	assert(strcmp(name_for(VRF_DEFAULT, &l2, &r2), "midr0") == 0);
	assert(name_for(VRF_DEFAULT, &l1, &r1) == NULL);

	midr_dp_backend_stop();
	midr_gre_fini();
	midr_ted_destroy(&ctx.ted);
}

static void test_same_endpoints_different_name(void)
{
	struct midr_context ctx;
	struct midr_gre_tunnel tun = {};
	struct midr_gre_status status = {};
	struct ipaddr local = ipv4("10.0.0.3");
	struct ipaddr remote = ipv4("10.0.0.4");

	ted_create(&ctx);
	ctx.master = event_master_create("midrd-gre-registry-test");
	assert(ctx.master);
	midr_gre_init(ctx.master);
	assert(midr_dp_backend_start(&ctx, ctx.master, VRF_DEFAULT) == 0);
	midr_dp_backend_zclient()->sock = 0;

	strlcpy(tun.ifname, "midr0", sizeof(tun.ifname));
	tun.vrf_id = VRF_DEFAULT;
	tun.local = local;
	tun.remote = remote;
	add_expect_ok(&ctx, &tun);

	/* Same endpoints under a different explicit name: the explicit name
	 * wins and no duplicate endpoint entry is left behind. */
	strlcpy(tun.ifname, "midr1", sizeof(tun.ifname));
	add_expect_ok(&ctx, &tun);

	assert(midr_gre_interface_get_state(VRF_DEFAULT, "midr1", &status) == 0);
	assert(midr_gre_interface_get_state(VRF_DEFAULT, "midr0", &status) == -1);
	assert(strcmp(name_for(VRF_DEFAULT, &local, &remote), "midr1") == 0);

	/* Deleting by endpoints removes the single entry: a leftover duplicate
	 * would still be found here. */
	assert(midr_gre_interface_del_by_endpoints(&ctx, VRF_DEFAULT, &local,
						   &remote, &status) == 0);
	assert(name_for(VRF_DEFAULT, &local, &remote) == NULL);

	midr_dp_backend_stop();
	midr_gre_fini();
	midr_ted_destroy(&ctx.ted);
}

int main(void)
{
	test_alloc_failure_is_reported();
	test_rebind_updates_in_place();
	test_same_endpoints_different_name();
	/* Every create/delete above really went through the GRE ZAPI path. */
	assert(gre_add_calls > 0 && gre_del_calls > 0);
	puts("midrd-dp-gre-registry-test: PASS");
	return 0;
}
