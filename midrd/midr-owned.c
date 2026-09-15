/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "midr-owned.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

struct midr_owned_entry {
	struct midr_core_object object;
	bool present;
};

struct midr_owned {
	struct midr_owned_config config;
	char *sequence_file;
	midr_owned_publish_cb publish;
	void *publish_arg;
	struct midr_owned_entry *entries;
	size_t count;
	uint64_t last_sequence;
};

static ssize_t find_entry(const struct midr_owned *owned,
				  const struct midr_core_identity *identity)
{
	for (size_t i = 0; i < owned->count; i++)
		if (midr_core_identity_equal(&owned->entries[i].object.identity,
					     identity))
			return (ssize_t)i;
	return -1;
}

static int load_sequence(const char *path, uint64_t *sequence)
{
	FILE *stream;
	unsigned long long value;

	if (!path || !sequence)
		return -EINVAL;
	stream = fopen(path, "r");
	if (!stream) {
		if (errno == ENOENT)
			return 0;
		return -errno;
	}
	if (fscanf(stream, "%llu", &value) != 1) {
		(void)fclose(stream);
		return -EINVAL;
	}
	if (fclose(stream) != 0)
		return -EIO;
	*sequence = (uint64_t)value;
	return 0;
}

static int save_sequence(const char *path, uint64_t sequence)
{
	char temporary[4096];
	FILE *stream;

	if (!path)
		return 0;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) < 1 ||
	    strlen(path) + 4U >= sizeof(temporary))
		return -ENAMETOOLONG;
	stream = fopen(temporary, "w");
	if (!stream)
		return -errno;
	if (fprintf(stream, "%llu\n", (unsigned long long)sequence) < 0 ||
	    fflush(stream) != 0) {
		(void)fclose(stream);
		(void)remove(temporary);
		return -EIO;
	}
	if (fclose(stream) != 0) {
		(void)remove(temporary);
		return -EIO;
	}
	if (rename(temporary, path) != 0) {
		int error = errno;

		(void)remove(temporary);
		return -error;
	}
	return 0;
}

static int allocate_sequence(struct midr_owned *owned, uint64_t *sequence)
{
	uint64_t next;
	int ret;

	if (owned->last_sequence == UINT64_MAX)
		return -ERANGE;
	next = owned->last_sequence + 1U;
	ret = save_sequence(owned->sequence_file, next);
	if (ret)
		return ret;
	owned->last_sequence = next;
	*sequence = next;
	return 0;
}

static int normalize_identity(const struct midr_owned *owned,
				      const struct midr_core_identity *input,
				      struct midr_core_identity *output)
{
	if (!input || !output || midr_core_identity_normalize(input, output) ||
	    midr_core_identity_validate(output) ||
	    output->originator != owned->config.originator)
		return -EINVAL;
	return 0;
}

int midr_owned_create(const struct midr_owned_config *config,
			      midr_owned_publish_cb publish, void *arg,
			      struct midr_owned **out)
{
	struct midr_owned *owned;
	int ret;

	if (!config || !publish || !out || *out || !config->originator ||
	    !config->max_objects || !config->lifetime_ms ||
	    (config->sequence_file && !*config->sequence_file))
		return -EINVAL;
	owned = calloc(1, sizeof(*owned));
	if (!owned)
		return -ENOMEM;
	owned->entries = calloc(config->max_objects, sizeof(*owned->entries));
	if (!owned->entries) {
		free(owned);
		return -ENOMEM;
	}
	owned->config = *config;
	owned->publish = publish;
	owned->publish_arg = arg;
	if (config->sequence_file) {
		owned->sequence_file = strdup(config->sequence_file);
		if (!owned->sequence_file) {
			midr_owned_destroy(&owned);
			return -ENOMEM;
		}
		ret = load_sequence(owned->sequence_file, &owned->last_sequence);
		if (ret) {
			midr_owned_destroy(&owned);
			return ret;
		}
	}
	*out = owned;
	return 0;
}

int midr_owned_clone(const struct midr_owned *source,
			     midr_owned_publish_cb publish, void *arg,
			     struct midr_owned **out)
{
	struct midr_owned_config config;
	struct midr_owned *clone = NULL;
	int ret;

	if (!source || !publish || !out || *out)
		return -EINVAL;
	config = source->config;
	config.sequence_file = source->sequence_file;
	ret = midr_owned_create(&config, publish, arg, &clone);
	if (ret)
		return ret;
	clone->count = source->count;
	memcpy(clone->entries, source->entries,
	       source->config.max_objects * sizeof(*source->entries));
	if (clone->last_sequence < source->last_sequence)
		clone->last_sequence = source->last_sequence;
	*out = clone;
	return 0;
}

void midr_owned_destroy(struct midr_owned **ownedp)
{
	if (!ownedp || !*ownedp)
		return;
	free((*ownedp)->sequence_file);
	free((*ownedp)->entries);
	free(*ownedp);
	*ownedp = NULL;
}

int midr_owned_upsert(struct midr_owned *owned,
				     const struct midr_core_object *fact)
{
	struct midr_core_identity identity;
	struct midr_core_object object;
	ssize_t index, reusable = -1;
	uint64_t sequence;
	int ret;

	if (!owned || !fact || fact->state != MIDR_CORE_ACTIVE ||
	    normalize_identity(owned, &fact->identity, &identity))
		return -EINVAL;
	index = find_entry(owned, &identity);
	if (index < 0) {
		for (size_t i = 0; i < owned->count; i++)
			if (!owned->entries[i].present) {
				reusable = (ssize_t)i;
				break;
			}
		if (reusable < 0 && owned->count == owned->config.max_objects)
			return -ENOSPC;
	}
	ret = allocate_sequence(owned, &sequence);
	if (ret)
		return ret;
	object = *fact;
	object.identity = identity;
	object.sequence = sequence;
	object.lifetime_ms = owned->config.lifetime_ms;
	ret = owned->publish(owned->publish_arg, &object);
	if (ret)
		return ret;
	if (index < 0) {
		index = reusable >= 0 ? reusable : (ssize_t)owned->count++;
		memset(&owned->entries[index], 0, sizeof(owned->entries[index]));
	}
	owned->entries[index].object = object;
	owned->entries[index].present = true;
	return 0;
}

int midr_owned_refresh(struct midr_owned *owned,
			      const struct midr_core_identity *identity)
{
	struct midr_core_identity normalized;
	struct midr_core_object object;
	ssize_t index;
	uint64_t sequence;
	int ret;

	if (!owned || normalize_identity(owned, identity, &normalized))
		return -EINVAL;
	index = find_entry(owned, &normalized);
	if (index < 0 || !owned->entries[index].present)
		return -ENOENT;
	ret = allocate_sequence(owned, &sequence);
	if (ret)
		return ret;
	object = owned->entries[index].object;
	object.sequence = sequence;
	object.lifetime_ms = owned->config.lifetime_ms;
	ret = owned->publish(owned->publish_arg, &object);
	if (ret)
		return ret;
	owned->entries[index].object = object;
	return 0;
}

int midr_owned_withdraw(struct midr_owned *owned,
			       const struct midr_core_identity *identity)
{
	struct midr_core_identity normalized;
	struct midr_core_object object;
	ssize_t index;
	uint64_t sequence;
	int ret;

	if (!owned || normalize_identity(owned, identity, &normalized))
		return -EINVAL;
	index = find_entry(owned, &normalized);
	if (index < 0 || !owned->entries[index].present)
		return -ENOENT;
	ret = allocate_sequence(owned, &sequence);
	if (ret)
		return ret;
	object = owned->entries[index].object;
	object.state = MIDR_CORE_WITHDRAWN;
	object.sequence = sequence;
	object.lifetime_ms = owned->config.lifetime_ms;
	object.group = 0;
	object.metric = 0;
	memset(object.local_address, 0, sizeof(object.local_address));
	memset(object.remote_address, 0, sizeof(object.remote_address));
	ret = owned->publish(owned->publish_arg, &object);
	if (ret)
		return ret;
	owned->entries[index].object = object;
	owned->entries[index].present = false;
	return 0;
}

int midr_owned_withdraw_all(struct midr_owned *owned, size_t *withdrawn)
{
	size_t count = 0;
	int result = 0;

	if (!owned)
		return -EINVAL;
	for (size_t i = 0; i < owned->count; i++) {
		int ret;

		if (!owned->entries[i].present)
			continue;
		ret = midr_owned_withdraw(owned, &owned->entries[i].object.identity);
		if (ret) {
			if (!result)
				result = ret;
			continue;
		}
		count++;
	}
	if (withdrawn)
		*withdrawn = count;
	return result;
}

int midr_owned_lookup(const struct midr_owned *owned,
			     const struct midr_core_identity *identity,
			     struct midr_core_object *object)
{
	struct midr_core_identity normalized;
	ssize_t index;

	if (!owned || !object || midr_core_identity_normalize(identity, &normalized) ||
	    midr_core_identity_validate(&normalized) ||
	    normalized.originator != owned->config.originator)
		return -EINVAL;
	index = find_entry(owned, &normalized);
	if (index < 0 || !owned->entries[index].present)
		return -ENOENT;
	*object = owned->entries[index].object;
	return 0;
}

size_t midr_owned_count(const struct midr_owned *owned)
{
	size_t count = 0;

	if (!owned)
		return 0;
	for (size_t i = 0; i < owned->count; i++)
		if (owned->entries[i].present)
			count++;
	return count;
}

uint64_t midr_owned_last_sequence(const struct midr_owned *owned)
{
	return owned ? owned->last_sequence : 0;
}

int midr_owned_sequence_floor(struct midr_owned *owned, uint64_t sequence)
{
	int ret;

	if (!owned)
		return -EINVAL;
	if (sequence <= owned->last_sequence)
		return 0;
	ret = save_sequence(owned->sequence_file, sequence);
	if (ret)
		return ret;
	owned->last_sequence = sequence;
	return 0;
}
