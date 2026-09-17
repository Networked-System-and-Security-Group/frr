// SPDX-License-Identifier: GPL-2.0-or-later
#include <zebra.h>
#include <errno.h>

#include "bgpd/bgp_midr_instance.h"

int midr_instance_validate(const struct midr_instance *instance)
{
	if (!instance || !instance->object.ls_sequence ||
	    midr_ls_object_key_validate(&instance->object.key))
		return -EINVAL;
	if (instance->state == MIDR_INSTANCE_ACTIVE)
		return midr_ls_object_validate(&instance->object);
	if (instance->state != MIDR_INSTANCE_WITHDRAWN || instance->object.policy_tags)
		return -EINVAL;
	/* WITHDRAWN has no active payload, including unused union bytes. */
	for (size_t i = 0; i < sizeof(instance->object.payload); i++)
		if (((const unsigned char *)&instance->object.payload)[i])
			return -EINVAL;
	return 0;
}

bool midr_instance_same(const struct midr_instance *a, const struct midr_instance *b)
{
	if (!a || !b || a->state != b->state)
		return false;
	if (a->state == MIDR_INSTANCE_WITHDRAWN)
		return a->object.ls_sequence == b->object.ls_sequence &&
		       midr_ls_object_key_same(&a->object.key, &b->object.key);
	return a->state == MIDR_INSTANCE_ACTIVE && midr_ls_object_same(&a->object, &b->object);
}

int midr_instance_age(uint32_t received_ms, uint64_t received_ns,
		      uint64_t now_ns, uint32_t budget_ms,
		      uint32_t max_age_ms, uint32_t *age_ms)
{
	uint64_t elapsed_ns, elapsed_ms;

	if (!age_ms || !max_age_ms)
		return -EINVAL;
	if (now_ns < received_ns)
		return -ERANGE;
	elapsed_ns = now_ns - received_ns;
	elapsed_ms = elapsed_ns / 1000000U + (elapsed_ns % 1000000U != 0);
	/* Compare before adding: a long pause must never wrap to a fresh age. */
	if (received_ms >= max_age_ms || budget_ms >= max_age_ms - received_ms ||
	    elapsed_ms >= (uint64_t)max_age_ms - received_ms - budget_ms)
		*age_ms = max_age_ms;
	else
		*age_ms = received_ms + budget_ms + (uint32_t)elapsed_ms;
	return 0;
}
