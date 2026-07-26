// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Persistent sequence allocation for locally originated MIDR objects.
 */

#include <zebra.h>

#include <errno.h>
#include <json-c/json.h>

#include "libfrr.h"

#include "bgpd/bgp_midr_sequence.h"

#define MIDR_STATE_ROOT "midr"
#define MIDR_STATE_SEQUENCE_ALLOCATORS "sequence_allocators"
#define MIDR_STATE_BOOT_EPOCH "boot_epoch"

static int midr_sequence_node_key(uint32_t node_id, char *buffer, size_t buffer_size)
{
	struct in_addr address = {
		.s_addr = node_id,
	};

	if (!node_id || !inet_ntop(AF_INET, &address, buffer, buffer_size))
		return -EINVAL;
	return 0;
}

static int midr_sequence_json_child(struct json_object *parent, const char *name, bool create,
				    struct json_object **child)
{
	if (json_object_object_get_ex(parent, name, child))
		return json_object_is_type(*child, json_type_object) ? 0 : -EINVAL;
	if (!create) {
		*child = NULL;
		return 0;
	}

	*child = json_object_new_object();
	if (!*child)
		return -ENOMEM;
	json_object_object_add(parent, name, *child);
	return 0;
}

static int midr_sequence_frr_load_epoch(void *arg, uint32_t node_id, uint32_t *epoch, bool *found)
{
	struct json_object *state;
	struct json_object *midr;
	struct json_object *allocators;
	struct json_object *entry;
	struct json_object *value;
	int64_t stored_epoch;
	char key[INET_ADDRSTRLEN];
	int result;

	(void)arg;
	if (!epoch || !found)
		return -EINVAL;
	*epoch = 0;
	*found = false;

	result = midr_sequence_node_key(node_id, key, sizeof(key));
	if (result)
		return result;

	result = frr_daemon_state_load_status(&state);
	if (result)
		return result;
	if (!json_object_is_type(state, json_type_object)) {
		result = -EINVAL;
		goto out;
	}

	result = midr_sequence_json_child(state, MIDR_STATE_ROOT, false, &midr);
	if (result || !midr)
		goto out;
	result = midr_sequence_json_child(midr, MIDR_STATE_SEQUENCE_ALLOCATORS, false, &allocators);
	if (result || !allocators)
		goto out;
	if (!json_object_object_get_ex(allocators, key, &entry)) {
		result = 0;
		goto out;
	}
	if (!json_object_is_type(entry, json_type_object) ||
	    !json_object_object_get_ex(entry, MIDR_STATE_BOOT_EPOCH, &value) ||
	    !json_object_is_type(value, json_type_int)) {
		result = -EINVAL;
		goto out;
	}

	stored_epoch = json_object_get_int64(value);
	if (stored_epoch < 0 || (uint64_t)stored_epoch > UINT32_MAX) {
		result = -ERANGE;
		goto out;
	}

	*epoch = (uint32_t)stored_epoch;
	*found = true;
	result = 0;

out:
	json_object_put(state);
	return result;
}

static int midr_sequence_frr_save_epoch(void *arg, uint32_t node_id, uint32_t epoch)
{
	struct json_object *state;
	struct json_object *midr;
	struct json_object *allocators;
	struct json_object *entry;
	struct json_object *value;
	char key[INET_ADDRSTRLEN];
	int result;

	(void)arg;
	result = midr_sequence_node_key(node_id, key, sizeof(key));
	if (result)
		return result;

	result = frr_daemon_state_load_status(&state);
	if (result)
		return result;
	if (!json_object_is_type(state, json_type_object)) {
		result = -EINVAL;
		goto out;
	}

	result = midr_sequence_json_child(state, MIDR_STATE_ROOT, true, &midr);
	if (result)
		goto out;
	result = midr_sequence_json_child(midr, MIDR_STATE_SEQUENCE_ALLOCATORS, true, &allocators);
	if (result)
		goto out;

	entry = json_object_new_object();
	value = json_object_new_int64(epoch);
	if (!entry || !value) {
		json_object_put(entry);
		json_object_put(value);
		result = -ENOMEM;
		goto out;
	}
	json_object_object_add(entry, MIDR_STATE_BOOT_EPOCH, value);
	json_object_object_add(allocators, key, entry);
	return frr_daemon_state_save_status(&state);

out:
	json_object_put(state);
	return result;
}

const struct midr_sequence_store_ops midr_sequence_frr_store_ops = {
	.load_epoch = midr_sequence_frr_load_epoch,
	.save_epoch = midr_sequence_frr_save_epoch,
};

static int midr_sequence_allocator_persist(struct midr_sequence_allocator *allocator,
					   uint32_t epoch, uint32_t counter)
{
	int result;

	result = allocator->store_ops->save_epoch(allocator->store_arg, allocator->node_id, epoch);
	if (result) {
		allocator->ready = false;
		return result;
	}

	allocator->boot_epoch = epoch;
	allocator->origin_counter = counter;
	return 0;
}

int midr_sequence_allocator_init(struct midr_sequence_allocator *allocator, uint32_t node_id,
				 const struct midr_sequence_store_ops *store_ops, void *store_arg)
{
	uint32_t persisted_epoch = 0;
	bool found = false;
	int result;

	if (!allocator || !node_id || !store_ops || !store_ops->load_epoch ||
	    !store_ops->save_epoch)
		return -EINVAL;

	memset(allocator, 0, sizeof(*allocator));
	allocator->node_id = node_id;
	allocator->store_ops = store_ops;
	allocator->store_arg = store_arg;

	result = store_ops->load_epoch(store_arg, node_id, &persisted_epoch, &found);
	if (result)
		return result;
	if (!found)
		persisted_epoch = 0;
	if (persisted_epoch == UINT32_MAX)
		return -EOVERFLOW;

	result = midr_sequence_allocator_persist(allocator, persisted_epoch + 1, 0);
	if (result)
		return result;

	allocator->ready = true;
	return 0;
}

int midr_sequence_allocator_next(struct midr_sequence_allocator *allocator, uint64_t *sequence)
{
	int result;

	if (!allocator || !sequence)
		return -EINVAL;
	if (!allocator->ready)
		return -EAGAIN;

	if (allocator->origin_counter == UINT32_MAX) {
		if (allocator->boot_epoch == UINT32_MAX) {
			allocator->ready = false;
			return -EOVERFLOW;
		}
		result = midr_sequence_allocator_persist(allocator, allocator->boot_epoch + 1, 0);
		if (result)
			return result;
	}

	allocator->origin_counter++;
	*sequence = ((uint64_t)allocator->boot_epoch << 32) | allocator->origin_counter;
	return 0;
}

int midr_sequence_allocator_advance_past(struct midr_sequence_allocator *allocator,
					 uint64_t observed_sequence)
{
	uint32_t observed_epoch;
	uint32_t observed_counter;
	uint64_t current_sequence;

	if (!allocator)
		return -EINVAL;
	if (!allocator->ready)
		return -EAGAIN;

	current_sequence = ((uint64_t)allocator->boot_epoch << 32) | allocator->origin_counter;
	if (observed_sequence <= current_sequence)
		return 0;

	observed_epoch = observed_sequence >> 32;
	observed_counter = observed_sequence;
	if (observed_counter != UINT32_MAX) {
		if (observed_epoch == allocator->boot_epoch) {
			allocator->origin_counter = observed_counter;
			return 0;
		}
		return midr_sequence_allocator_persist(allocator, observed_epoch, observed_counter);
	}

	if (observed_epoch == UINT32_MAX) {
		allocator->ready = false;
		return -EOVERFLOW;
	}
	return midr_sequence_allocator_persist(allocator, observed_epoch + 1, 0);
}

bool midr_sequence_allocator_is_ready(const struct midr_sequence_allocator *allocator)
{
	return allocator && allocator->ready;
}
