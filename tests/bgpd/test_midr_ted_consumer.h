// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimal path-computation consumer used to verify the public TED contract.
 */

#ifndef _FRR_TEST_MIDR_TED_CONSUMER_H
#define _FRR_TEST_MIDR_TED_CONSUMER_H

#include "bgpd/bgp_midr_ted.h"

struct midr_ted_path_consumer_stub {
	struct midr_ted_consumer *registration;
	uint64_t last_generation;
	uint32_t last_change_flags;
	unsigned int notification_count;
};

extern int midr_ted_path_consumer_stub_start(struct midr_context *ctx,
					     struct midr_ted_path_consumer_stub *consumer);
extern void midr_ted_path_consumer_stub_stop(struct midr_context *ctx,
					     struct midr_ted_path_consumer_stub *consumer);
extern int
midr_ted_path_consumer_stub_snapshot_get(struct midr_context *ctx,
					 const struct midr_ted_path_consumer_stub *consumer,
					 const struct midr_ted_snapshot **snapshot);
extern bool
midr_ted_path_consumer_stub_result_is_current(struct midr_context *ctx,
					      const struct midr_ted_path_consumer_stub *consumer,
					      uint64_t generation);

#endif /* _FRR_TEST_MIDR_TED_CONSUMER_H */
