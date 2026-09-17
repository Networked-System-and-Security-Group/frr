// SPDX-License-Identifier: GPL-2.0-or-later
/* MIDR scale evidence harness: objects x peers matrix with virtual-time
 * churn, emitting one TSV row per phase for FOLLOW-05 capacity data.
 *
 * Env knobs: MIDR_SCALE_OBJECTS, MIDR_SCALE_PEERS, MIDR_SCALE_CYCLES.
 * Every object is advertised by every peer (worst-case fan-in), so the
 * per-identity state is single-instance while per-peer advertisement
 * state grows with objects x peers.
 */

#include <zebra.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "command.h"
#include "linklist.h"
#include "memory.h"
#include "privs.h"
#include "qobj.h"

#include "bgpd/bgpd.h"
#include "bgpd/bgp_attr.h"
#include "bgpd/bgp_midr_canonical.h"
#include "bgpd/bgp_midr_lsdb.h"
#include "bgpd/bgp_midr_private.h"
#include "bgpd/bgp_midr_rib.h"
#include "bgpd/bgp_network.h"
#include "bgpd/bgp_vty.h"

struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct scale_ctx {
	struct bgp *bgp;
	struct midr_context *ctx;
	struct peer **peers;
	size_t objects;
	size_t peers_count;
	uint64_t base_ns;
	uint64_t life_ns;
};

struct mtype_sample {
	size_t rib_identity_alloc;
	size_t rib_identity_bytes;
	size_t rib_advert_alloc;
	size_t rib_advert_bytes;
};

static uint32_t router_id_of(unsigned int n)
{
	return htonl(0x0b000000 + n);
}

static struct peer *scale_peer(struct bgp *bgp, unsigned int n)
{
	struct peer *peer = peer_create_accept(bgp, NULL);

	assert(peer);
	peer->remote_id.s_addr = htonl(0x0a000000 + 100 + n);
	return peer;
}

static struct midr_instance object_instance(unsigned int object,
					    uint64_t sequence)
{
	return (struct midr_instance){
		.state = MIDR_INSTANCE_ACTIVE,
		.object = {
			.key = {
				.type = MIDR_NLRI_TYPE_MEMBERSHIP,
				.originator_node_id = router_id_of(object),
			},
			.ls_sequence = sequence,
			.payload.membership = {
				.group_id = object % 64 + 1,
				.cap_flags = 0,
			},
		},
	};
}

/* Worst-case fan-in: every peer supplies every object. */
static void phase_fill(struct scale_ctx *s, uint64_t sequence)
{
	for (size_t object = 0; object < s->objects; object++) {
		struct midr_instance instance =
			object_instance(object, sequence);

		for (size_t p = 0; p < s->peers_count; p++)
			assert(midr_rib_instance_upsert(
				       s->ctx, s->peers[p], &instance,
				       0) == 0);
	}
}

static void phase_expire_and_reclaim(struct scale_ctx *s, uint64_t now_ns,
					     bool expect_empty)
{
	/* Lifetime sweeping is deliberately bounded in production.  Drive enough
	 * virtual passes for a large scale point to drain every bucket, including
	 * the extra pass needed after LSDB staging commits. */
	size_t sweep_rounds = (s->objects + 255U) / 256U;
	size_t passes = expect_empty ? 2U * sweep_rounds + 4U : 1U;

	assert(midr_rib_test_set_now_ns(s->ctx, now_ns) == 0);
	for (size_t pass = 0; pass < passes; pass++) {
		assert(midr_rib_test_run_lifetime(s->ctx) == 0);
		/* A pass removes paths and queues LSDB staging; reclamation may
		 * proceed only after that committed view has drained. */
		assert(midr_lsdb_test_process(s->ctx) == 0);
		if (expect_empty) {
			struct midr_rib_summary summary = {};

			assert(midr_rib_summary_get(s->ctx, &summary) == 0);
			if (!summary.identity_count)
				break;
		}
	}
	if (expect_empty) {
		struct midr_rib_summary summary = {};

		assert(midr_rib_summary_get(s->ctx, &summary) == 0);
		assert(summary.identity_count == 0);
	}
}

static int mtype_sampler(void *arg, struct memgroup *mg, struct memtype *mt)
{
	struct mtype_sample *sample = arg;

	(void)mg;
	if (!mt)
		return 0;
	if (strcmp(mt->name, "MIDR RIB identity") == 0) {
		sample->rib_identity_alloc = atomic_load_explicit(
			&mt->n_alloc, memory_order_relaxed);
		sample->rib_identity_bytes = atomic_load_explicit(
			&mt->total, memory_order_relaxed);
	} else if (strcmp(mt->name, "MIDR peer advertisement") == 0) {
		sample->rib_advert_alloc = atomic_load_explicit(
			&mt->n_alloc, memory_order_relaxed);
		sample->rib_advert_bytes = atomic_load_explicit(
			&mt->total, memory_order_relaxed);
	}
	return 0;
}

static void emit_row(const char *phase, struct scale_ctx *s,
		     unsigned int cycle, double seconds)
{
	struct midr_rib_summary summary = {};
	struct mtype_sample sample = {};

	assert(midr_rib_summary_get(s->ctx, &summary) == 0);
	qmem_walk(mtype_sampler, &sample);
	printf("SCALE\t%s\t%zu\t%zu\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%.3f\n",
	       phase, s->objects, s->peers_count, cycle,
	       summary.identity_count, summary.canonical_identity_count,
	       summary.floor_count, summary.pending_event_count,
	       summary.retired_ref_count, summary.retired_ref_bytes,
	       summary.advertisement_count, summary.path_count,
	       sample.rib_identity_alloc, sample.rib_identity_bytes,
	       sample.rib_advert_alloc, sample.rib_advert_bytes, seconds);
}

static double monotonic_seconds(void)
{
	struct timespec ts;

	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static size_t env_size(const char *name, size_t fallback)
{
	const char *text = getenv(name);
	char *end = NULL;
	unsigned long long value;

	if (!text || !*text)
		return fallback;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno == ERANGE || !end || *end || !value ||
	    value > SIZE_MAX)
		return fallback;
	return (size_t)value;
}

int main(void)
{
	as_t asn = 65000;
	struct scale_ctx s = {};
	struct timespec ts;
	size_t cycles;
	double t0, t1;

	s.objects = env_size("MIDR_SCALE_OBJECTS", 100);
	s.peers_count = env_size("MIDR_SCALE_PEERS", 2);
	cycles = env_size("MIDR_SCALE_CYCLES", 1);
	assert(s.objects > 0 && s.objects <= MIDR_RIB_MAX_IDENTITIES);
	assert(s.peers_count > 0 && s.peers_count <= 1024);
	assert(cycles > 0 && cycles <= 1000);
	assert(s.objects <= UINT32_MAX - 0x0b000000U);

	qobj_init();
	cmd_init(0);
	bgp_vty_init();
	master = event_master_create("test MIDR scale");
	bgp_master_init(master, BGP_SOCKET_SNDBUF_SIZE, list_new());
	vrf_init(NULL, NULL, NULL, NULL);
	bgp_option_set(BGP_OPT_NO_LISTEN);
	bgp_attr_init();
	assert(bgp_get(&s.bgp, &asn, NULL, BGP_INSTANCE_TYPE_DEFAULT, NULL,
		       ASNOTATION_PLAIN) >= 0);
	s.bgp->router_id.s_addr = htonl(0x0a000001);
	s.ctx = &s.bgp->midr_info->ctx;
	s.life_ns = (uint64_t)MIDR_CANONICAL_MAX_AGE_MS * 1000000ULL;

	/* Anchor the virtual clock past real monotonic time. */
	assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	s.base_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec +
		    1000000000ULL;
	assert(midr_rib_test_set_now_ns(s.ctx, s.base_ns) == 0);

	s.peers = calloc(s.peers_count, sizeof(*s.peers));
	assert(s.peers);
	for (size_t p = 0; p < s.peers_count; p++)
		s.peers[p] = scale_peer(s.bgp, p);

	/* Floor GC must be enabled for reclaim evidence; production default
	 * (disabled) keeps floors, which is covered by test_midr_rib. */
	assert(midr_rib_test_set_gc_enabled(s.ctx, true) == 0);
	printf("SCALE\tphase\tobjects\tpeers\tcycle\tidentities\tcanonical\tfloors"
	       "\tpending_events\tretired_refs\tretired_ref_bytes\tadvertisements\tpaths"
	       "\trib_identity_alloc\trib_identity_bytes\trib_advert_alloc\trib_advert_bytes\tseconds\n");

	for (unsigned int cycle = 0; cycle < cycles; cycle++) {
		uint64_t round_ns = s.base_ns + (uint64_t)(cycle + 2) *
						       4 * s.life_ns;

		assert(midr_rib_test_set_now_ns(s.ctx, round_ns) == 0);
		t0 = monotonic_seconds();
		phase_fill(&s, cycle * 100 + 1);
		t1 = monotonic_seconds();
		emit_row("fill", &s, cycle, t1 - t0);

		/* Mid-life: everything still live and single-instance. */
		phase_expire_and_reclaim(&s, round_ns + s.life_ns / 2, false);
		emit_row("midlife", &s, cycle, 0);

		/* At L: expiry and reclamation drop the whole round. */
		t0 = monotonic_seconds();
		phase_expire_and_reclaim(&s, round_ns + s.life_ns, true);
		t1 = monotonic_seconds();
		emit_row("reclaimed", &s, cycle, t1 - t0);

		{
			struct midr_rib_summary summary = {};

			assert(midr_rib_summary_get(s.ctx, &summary) == 0);
			assert(summary.identity_count == 0);
			assert(summary.canonical_identity_count == 0);
			assert(summary.path_count == 0);
		}
	}

	/* Slot budget survives the whole run. */
	{
		struct midr_rib_summary summary = {};

		assert(midr_rib_summary_get(s.ctx, &summary) == 0);
		assert(summary.identity_count == 0);
		assert(summary.identities_reclaimed ==
		       cycles * s.objects);
	}
	free(s.peers);
	puts("MIDR scale evidence collected");
	return 0;
}
