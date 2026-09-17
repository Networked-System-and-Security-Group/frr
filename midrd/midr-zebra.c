/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-zebra.h"

#include <errno.h>

#include "midr-context-private.h"
#include "midr-spf-install.h"

static int path_result_validate(const struct midr_path_result *result)
{
	if (!result || !result->paths || !result->path_count)
		return -EINVAL;
	if (result->instance != MIDR_INSTANCE_SPF &&
	    result->instance != MIDR_INSTANCE_TE)
		return -EINVAL;
	if (result->explicit.sid_count > MIDR_SRV6_MAX_SEGS)
		return -E2BIG;
	for (size_t i = 0; i < result->path_count; i++)
		if (result->paths[i].nexthop.ipa_type != IPADDR_V4 &&
		    result->paths[i].nexthop.ipa_type != IPADDR_V6)
			return -EAFNOSUPPORT;
	return 0;
}

bool midr_zebra_backend_ready(const struct midr_context *ctx)
{
	return ctx && ctx->zebra_ops;
}

int midr_zebra_backend_register(
	struct midr_context *ctx, const struct midr_zebra_backend_ops *ops,
	void *arg)
{
	int ret;

	if (!ctx || !ops || !ops->route_add || !ops->route_del ||
	    !ops->update_deferred || !ops->flush || !ops->abort_pending)
		return -EINVAL;
	if (ctx->zebra_ops)
		return -EALREADY;
	ctx->zebra_ops = ops;
	ctx->zebra_arg = arg;
	ret = midr_spf_install_start(ctx);
	if (ret) {
		ctx->zebra_ops = NULL;
		ctx->zebra_arg = NULL;
	}
	return ret;
}

int midr_zebra_backend_unregister(struct midr_context *ctx)
{
	int ret;

	if (!ctx)
		return -EINVAL;
	if (!ctx->zebra_ops)
		return 0;
	ret = midr_spf_install_stop(ctx);
	ctx->zebra_ops = NULL;
	ctx->zebra_arg = NULL;
	return ret;
}

int midr_zebra_route_add(struct midr_context *ctx,
			 const struct prefix *prefix,
			 const struct midr_path_result *result)
{
	int ret;

	if (!midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	if (!prefix)
		return -EINVAL;
	ret = path_result_validate(result);
	if (ret)
		return ret;
	return ctx->zebra_ops->route_add(ctx->zebra_arg, prefix, result);
}

int midr_zebra_route_del(struct midr_context *ctx,
			 const struct prefix *prefix, uint8_t instance)
{
	if (!midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	if (!prefix || (instance != MIDR_INSTANCE_SPF &&
			instance != MIDR_INSTANCE_TE))
		return -EINVAL;
	return ctx->zebra_ops->route_del(ctx->zebra_arg, prefix, instance);
}

int midr_zebra_route_update_deferred(struct midr_context *ctx)
{
	if (!midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	return ctx->zebra_ops->update_deferred(ctx->zebra_arg);
}

int midr_zebra_route_flush(struct midr_context *ctx)
{
	if (!midr_zebra_backend_ready(ctx))
		return -ENOSYS;
	return ctx->zebra_ops->flush(ctx->zebra_arg);
}

void midr_zebra_route_abort(struct midr_context *ctx)
{
	if (midr_zebra_backend_ready(ctx))
		ctx->zebra_ops->abort_pending(ctx->zebra_arg);
}
