/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_ZEBRA_H
#define MIDRD_ZEBRA_H

#include <zebra.h>

#include <stdbool.h>
#include <stdint.h>

#include "ipaddr.h"
#include "prefix.h"

struct midr_context;

#define MIDR_INSTANCE_SPF 0
#define MIDR_INSTANCE_TE 1
#define MIDR_SRV6_MAX_SEGS 8

/* Keep the former bgpd control-plane to data-plane carrier and field names.
 * The nexthop is upgraded from an untagged union g_addr to FRR's tagged
 * struct ipaddr so address-family validation no longer depends on the prefix. */
struct midr_path {
	struct ipaddr nexthop;
	uint32_t ifindex;
	uint32_t metric;
	float path_avail_bw;
	uint8_t weight;
};

struct midr_path_result {
	struct midr_path *paths;
	uint8_t path_count;
	struct {
		struct in6_addr sid_list[MIDR_SRV6_MAX_SEGS];
		uint8_t sid_count;
	} explicit;
	uint8_t instance;
};

/* Third-group implementations deep-copy route_add input before returning.
 * abort_pending discards every operation staged since the previous successful
 * deferred/flush submission.  This gives the SPF adapter all-or-nothing
 * staging even when a backend reports an error midway through a diff.
 * update_deferred/flush return success only after the batch is accepted for
 * delivery.  A backend must not advance its installed hash after a failed ZAPI
 * send; it retains or reconstructs that batch for resync instead. */
struct midr_zebra_backend_ops {
	int (*route_add)(void *arg, const struct prefix *prefix,
			 const struct midr_path_result *result);
	int (*route_del)(void *arg, const struct prefix *prefix,
			 uint8_t instance);
	int (*update_deferred)(void *arg);
	int (*flush)(void *arg);
	void (*abort_pending)(void *arg);
};

/*
 * Register the data-plane backend implementing struct midr_zebra_backend_ops.
 *
 * Ownership: @ops is deep-copied into midrd-owned storage.  The caller may
 * pass a stack-scoped or otherwise short-lived table and is free to discard
 * or reuse it as soon as this call returns; midrd never dereferences the
 * caller's table afterwards.  midrd owns the copy until
 * midr_zebra_backend_unregister() (or a failed registration) frees it.
 *
 * @arg is NOT copied: it is an opaque backend handle that is only stored and
 * forwarded to the ops callbacks.  The caller keeps ownership and must keep
 * it valid until midr_zebra_backend_unregister(); unlike @ops it cannot be
 * deep-copied generically.  (The single in-tree caller passes the long-lived
 * data-plane backend instance, so this is satisfied.)
 *
 * Returns 0 on success; -EINVAL when a required callback is missing;
 * -EALREADY when a backend is already registered; -ENOMEM when the ops copy
 * cannot be allocated; otherwise the midr_spf_install_start() error.  On any
 * non-zero return no backend stays registered and no memory is leaked.
 */
int midr_zebra_backend_register(
	struct midr_context *ctx, const struct midr_zebra_backend_ops *ops,
	void *arg);

/*
 * Unregister the backend, stopping the SPF installation adapter first (which
 * may still call back through the ops) and then releasing the midrd-owned
 * ops copy.  Idempotent: returns 0 when nothing is registered.
 */
int midr_zebra_backend_unregister(struct midr_context *ctx);
bool midr_zebra_backend_ready(const struct midr_context *ctx);

/* Compatibility facade corresponding to the former bgp_midr_zebra.h API.
 * Only the daemon host parameter changes to struct midr_context *; return
 * values now make staging and submission failures observable. */
int midr_zebra_route_add(struct midr_context *ctx,
			 const struct prefix *prefix,
			 const struct midr_path_result *result);
int midr_zebra_route_del(struct midr_context *ctx,
			 const struct prefix *prefix, uint8_t instance);
int midr_zebra_route_update_deferred(struct midr_context *ctx);
int midr_zebra_route_flush(struct midr_context *ctx);
void midr_zebra_route_abort(struct midr_context *ctx);

#endif /* MIDRD_ZEBRA_H */
