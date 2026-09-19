/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-spf-install.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "midr-context-private.h"
#include "midr-spf.h"
#include "midr-zebra.h"

enum install_op_kind {
	INSTALL_ADD,
	INSTALL_DELETE,
};

struct install_op {
	enum install_op_kind kind;
	struct prefix prefix;
	struct midr_path_result result;
	uint8_t instance;
};

struct route_ref {
	const struct midr_spf_route *route;
};

struct midr_spf_install_runtime {
	struct midr_context *ctx;
	struct midr_spf_consumer *consumer;
	const struct midr_spf_results *desired;
	uint64_t reconcile_count;
	uint64_t withdraw_count;
	int last_error;
};

static int route_key_compare(const struct midr_spf_route *left,
			     const struct midr_spf_route *right)
{
	int ret;

	ret = (left->family > right->family) -
	      (left->family < right->family);
	if (ret)
		return ret;
	ret = (left->prefix_len > right->prefix_len) -
	      (left->prefix_len < right->prefix_len);
	if (ret)
		return ret;
	return memcmp(left->prefix, right->prefix, sizeof(left->prefix));
}

static int route_ref_compare(const void *leftp, const void *rightp)
{
	const struct route_ref *left = leftp;
	const struct route_ref *right = rightp;

	return route_key_compare(left->route, right->route);
}

static bool route_supported(const struct midr_spf_route *route)
{
	size_t count;

	if (!route || !route->reachable || route->local_destination ||
	    (route->scope != MIDR_SPF_ROUTE_INTRA_GROUP &&
	     route->scope != MIDR_SPF_ROUTE_INTER_GROUP) ||
	    !route->nexthop_count)
		return false;
	if ((route->family == MIDR_CORE_AF_IPV4 && route->prefix_len > 32) ||
	    (route->family == MIDR_CORE_AF_IPV6 && route->prefix_len > 128) ||
	    (route->family != MIDR_CORE_AF_IPV4 &&
	     route->family != MIDR_CORE_AF_IPV6))
		return false;
	count = route->nexthop_count < UINT8_MAX ? route->nexthop_count
						 : UINT8_MAX;
	for (size_t i = 0; i < count; i++)
		if (route->nexthops[i].family != route->family)
			return false;
	return true;
}

static bool route_nexthops_same(const struct midr_spf_route *left,
				const struct midr_spf_route *right)
{
	size_t count;

	if (!route_supported(left) || !route_supported(right))
		return false;
	count = left->nexthop_count < UINT8_MAX ? left->nexthop_count
						 : UINT8_MAX;
	if (count != (right->nexthop_count < UINT8_MAX
			      ? right->nexthop_count
			      : UINT8_MAX))
		return false;
	for (size_t i = 0; i < count; i++)
		if (left->nexthops[i].family != right->nexthops[i].family ||
		    left->nexthops[i].ifindex != right->nexthops[i].ifindex ||
		    memcmp(left->nexthops[i].address,
			   right->nexthops[i].address,
			   sizeof(left->nexthops[i].address)))
			return false;
	return true;
}

static int prefix_from_route(const struct midr_spf_route *route,
			     struct prefix *prefix)
{
	memset(prefix, 0, sizeof(*prefix));
	if (route->family == MIDR_CORE_AF_IPV4) {
		prefix->family = AF_INET;
		prefix->prefixlen = route->prefix_len;
		memcpy(&prefix->u.prefix4, route->prefix,
		       sizeof(prefix->u.prefix4));
	} else if (route->family == MIDR_CORE_AF_IPV6) {
		prefix->family = AF_INET6;
		prefix->prefixlen = route->prefix_len;
		memcpy(&prefix->u.prefix6, route->prefix,
		       sizeof(prefix->u.prefix6));
	} else {
		return -EAFNOSUPPORT;
	}
	apply_mask(prefix);
	return 0;
}

static void install_op_clear(struct install_op *op)
{
	if (!op)
		return;
	free(op->result.paths);
	op->result.paths = NULL;
	op->result.path_count = 0;
}

static int install_op_add(struct install_op *op,
			  const struct midr_spf_route *route)
{
	size_t count;
	uint32_t metric;
	int ret;

	ret = prefix_from_route(route, &op->prefix);
	if (ret)
		return ret;
	count = route->nexthop_count < UINT8_MAX ? route->nexthop_count
						 : UINT8_MAX;
	op->result.paths = calloc(count, sizeof(*op->result.paths));
	if (!op->result.paths)
		return -ENOMEM;
	metric = route->metric > UINT32_MAX ? UINT32_MAX : (uint32_t)route->metric;
	for (size_t i = 0; i < count; i++) {
		const struct midr_spf_nexthop *source = &route->nexthops[i];
		struct midr_path *target = &op->result.paths[i];

		if (source->family == MIDR_CORE_AF_IPV4) {
			SET_IPADDR_V4(&target->nexthop);
			memcpy(&target->nexthop.ipaddr_v4, source->address,
			       sizeof(target->nexthop.ipaddr_v4));
		} else {
			SET_IPADDR_V6(&target->nexthop);
			memcpy(&target->nexthop.ipaddr_v6, source->address,
			       sizeof(target->nexthop.ipaddr_v6));
		}
		target->ifindex = source->ifindex;
		target->metric = metric;
		/* Base SPF is ECMP.  UCMP and SRv6 producers use the same
		 * compatibility carrier and set weight/SID fields explicitly. */
		target->path_avail_bw = 0.0f;
		target->weight = 0;
	}
	op->kind = INSTALL_ADD;
	op->result.path_count = (uint8_t)count;
	op->result.instance = MIDR_INSTANCE_SPF;
	return 0;
}

static int install_op_delete(struct install_op *op,
			     const struct midr_spf_route *route)
{
	int ret = prefix_from_route(route, &op->prefix);

	if (ret)
		return ret;
	op->kind = INSTALL_DELETE;
	op->instance = MIDR_INSTANCE_SPF;
	return 0;
}

static int refs_build(const struct midr_spf_results *results,
		      struct route_ref **refsp, size_t *countp)
{
	struct route_ref *refs;
	size_t count = midr_spf_results_count(results);

	*refsp = NULL;
	*countp = 0;
	if (!count)
		return 0;
	if (count > SIZE_MAX / sizeof(*refs))
		return -EOVERFLOW;
	refs = calloc(count, sizeof(*refs));
	if (!refs)
		return -ENOMEM;
	for (size_t i = 0; i < count; i++) {
		refs[i].route = midr_spf_results_at(results, i);
		if (!refs[i].route) {
			free(refs);
			return -EINVAL;
		}
	}
	qsort(refs, count, sizeof(*refs), route_ref_compare);
	for (size_t i = 1; i < count; i++)
		if (!route_key_compare(refs[i - 1].route, refs[i].route)) {
			free(refs);
			return -EEXIST;
		}
	*refsp = refs;
	*countp = count;
	return 0;
}

static int operation_append(struct install_op *ops, size_t capacity,
			    size_t *count, enum install_op_kind kind,
			    const struct midr_spf_route *route)
{
	int ret;

	if (*count == capacity)
		return -ENOSPC;
	if (kind == INSTALL_ADD)
		ret = install_op_add(&ops[*count], route);
	else
		ret = install_op_delete(&ops[*count], route);
	if (!ret)
		(*count)++;
	return ret;
}

int midr_spf_install_results(struct midr_context *ctx,
			     const struct midr_spf_results *old_results,
			     const struct midr_spf_results *new_results)
{
	struct route_ref *old_refs = NULL;
	struct route_ref *new_refs = NULL;
	struct install_op *ops = NULL;
	size_t old_count = 0;
	size_t new_count = 0;
	size_t old_index = 0;
	size_t new_index = 0;
	size_t op_count = 0;
	size_t capacity;
	int ret;

	if (!ctx || !midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	ret = refs_build(old_results, &old_refs, &old_count);
	if (ret)
		goto done;
	ret = refs_build(new_results, &new_refs, &new_count);
	if (ret)
		goto done;
	if (old_count > SIZE_MAX - new_count) {
		ret = -EOVERFLOW;
		goto done;
	}
	capacity = old_count + new_count;
	if (capacity > SIZE_MAX / sizeof(*ops)) {
		ret = -EOVERFLOW;
		goto done;
	}
	if (capacity) {
		ops = calloc(capacity, sizeof(*ops));
		if (!ops) {
			ret = -ENOMEM;
			goto done;
		}
	}

	while (old_index < old_count || new_index < new_count) {
		const struct midr_spf_route *old_route =
			old_index < old_count ? old_refs[old_index].route : NULL;
		const struct midr_spf_route *new_route =
			new_index < new_count ? new_refs[new_index].route : NULL;
		int key_cmp;

		if (!old_route)
			key_cmp = 1;
		else if (!new_route)
			key_cmp = -1;
		else
			key_cmp = route_key_compare(old_route, new_route);
		if (key_cmp < 0) {
			if (route_supported(old_route)) {
				ret = operation_append(ops, capacity, &op_count,
						       INSTALL_DELETE, old_route);
				if (ret)
					goto done;
			}
			old_index++;
			continue;
		}
		if (key_cmp > 0) {
			if (route_supported(new_route)) {
				ret = operation_append(ops, capacity, &op_count,
						       INSTALL_ADD, new_route);
				if (ret)
					goto done;
			}
			new_index++;
			continue;
		}

		if (route_supported(old_route) && !route_supported(new_route))
			ret = operation_append(ops, capacity, &op_count,
					       INSTALL_DELETE, old_route);
		else if (!route_supported(old_route) && route_supported(new_route))
			ret = operation_append(ops, capacity, &op_count,
					       INSTALL_ADD, new_route);
		else if (route_supported(old_route) && route_supported(new_route)) {
			if (old_route->metric != new_route->metric &&
			    route_nexthops_same(old_route, new_route)) {
				/* The legacy installed-hash intentionally ignores metric,
				 * so force replacement for a metric-only change. */
				ret = operation_append(ops, capacity, &op_count,
						       INSTALL_DELETE, old_route);
				if (!ret)
					ret = operation_append(ops, capacity, &op_count,
							       INSTALL_ADD, new_route);
			} else if (old_route->metric != new_route->metric ||
				   !route_nexthops_same(old_route, new_route)) {
				ret = operation_append(ops, capacity, &op_count,
						       INSTALL_ADD, new_route);
			} else {
				ret = 0;
			}
		} else {
			ret = 0;
		}
		if (ret)
			goto done;
		old_index++;
		new_index++;
	}

	for (size_t i = 0; i < op_count; i++) {
		if (ops[i].kind == INSTALL_ADD)
			ret = midr_zebra_route_add(ctx, &ops[i].prefix,
						   &ops[i].result);
		else
			ret = midr_zebra_route_del(ctx, &ops[i].prefix,
						   ops[i].instance);
		if (ret) {
			midr_zebra_route_abort(ctx);
			goto done;
		}
	}
	if (op_count) {
		ret = midr_zebra_route_update_deferred(ctx);
		if (ret)
			midr_zebra_route_abort(ctx);
	} else {
		ret = 0;
	}

done:
	for (size_t i = 0; i < op_count; i++)
		install_op_clear(&ops[i]);
	free(ops);
	free(old_refs);
	free(new_refs);
	return ret;
}

static int install_reconcile(struct midr_context *ctx, bool replay)
{
	struct midr_spf_install_runtime *runtime;
	const struct midr_spf_results *candidate = NULL;
	const struct midr_spf_results *old;
	int ret;

	if (!ctx || !ctx->spf_install)
		return -ENOENT;
	runtime = ctx->spf_install;
	ret = midr_spf_results_get(ctx, &candidate);
	if (ret) {
		runtime->last_error = ret;
		return ret;
	}
	if (!replay && midr_spf_results_generation(candidate) ==
	    midr_spf_results_generation(runtime->desired)) {
		midr_spf_results_release(&candidate);
		runtime->last_error = 0;
		return 0;
	}
	ret = midr_spf_install_results(ctx, replay ? NULL : runtime->desired,
				       candidate);
	if (ret) {
		midr_spf_results_release(&candidate);
		runtime->last_error = ret;
		return ret;
	}
	old = runtime->desired;
	runtime->desired = candidate;
	runtime->reconcile_count++;
	runtime->last_error = 0;
	midr_spf_results_release(&old);
	return 0;
}

int midr_spf_install_resync(struct midr_context *ctx)
{
	return install_reconcile(ctx, false);
}

int midr_spf_install_replay(struct midr_context *ctx)
{
	return install_reconcile(ctx, true);
}

static void install_results_changed(struct midr_context *ctx,
				    uint64_t generation,
				    uint32_t change_flags,
				    enum midr_ted_state state, int error,
				    void *arg)
{
	struct midr_spf_install_runtime *runtime = arg;
	const struct midr_spf_results *old;
	int ret;

	(void)generation;
	(void)change_flags;
	if (!runtime || runtime->ctx != ctx)
		return;
	if (state == MIDR_TED_READY) {
		(void)install_reconcile(ctx, false);
		return;
	}
	if (!runtime->desired) {
		runtime->last_error = error ? error : -EAGAIN;
		return;
	}
	ret = midr_spf_install_results(ctx, runtime->desired, NULL);
	if (ret) {
		runtime->last_error = ret;
		return;
	}
	old = runtime->desired;
	runtime->desired = NULL;
	runtime->withdraw_count++;
	runtime->last_error = error ? error : -EAGAIN;
	midr_spf_results_release(&old);
}

int midr_spf_install_start(struct midr_context *ctx)
{
	static const struct midr_spf_consumer_ops ops = {
		.results_changed = install_results_changed,
	};
	struct midr_spf_install_runtime *runtime;
	int ret;

	if (!ctx || !midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	if (ctx->spf_install)
		return -EALREADY;
	runtime = calloc(1, sizeof(*runtime));
	if (!runtime)
		return -ENOMEM;
	runtime->ctx = ctx;
	ret = midr_spf_consumer_register(ctx, &ops, runtime,
					 &runtime->consumer);
	if (ret) {
		free(runtime);
		return ret;
	}
	ctx->spf_install = runtime;
	ret = install_reconcile(ctx, false);
	if (ret && ret != -EAGAIN) {
		midr_spf_consumer_unregister(ctx, &runtime->consumer);
		ctx->spf_install = NULL;
		free(runtime);
		return ret;
	}
	return 0;
}

int midr_spf_install_stop(struct midr_context *ctx)
{
	struct midr_spf_install_runtime *runtime;
	const struct midr_spf_results *old;
	int ret = 0;
	int flush_ret;

	if (!ctx)
		return -EINVAL;
	if (!ctx->spf_install)
		return 0;
	runtime = ctx->spf_install;
	ctx->spf_install = NULL;
	midr_spf_consumer_unregister(ctx, &runtime->consumer);
	if (runtime->desired) {
		ret = midr_spf_install_results(ctx, runtime->desired, NULL);
		old = runtime->desired;
		runtime->desired = NULL;
		midr_spf_results_release(&old);
	}
	if (!ret) {
		flush_ret = midr_zebra_route_flush(ctx);
		if (flush_ret)
			ret = flush_ret;
	}
	free(runtime);
	return ret;
}

int midr_spf_install_status_get(struct midr_context *ctx,
				struct midr_spf_install_status *status)
{
	struct midr_spf_install_runtime *runtime;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	if (!ctx || !ctx->spf_install)
		return -ENOENT;
	runtime = ctx->spf_install;
	status->desired_generation =
		midr_spf_results_generation(runtime->desired);
	status->reconcile_count = runtime->reconcile_count;
	status->withdraw_count = runtime->withdraw_count;
	status->last_error = runtime->last_error;
	status->active = true;
	return 0;
}
