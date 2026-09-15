/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-ted.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct midr_ted {
	struct midr_ted_config config;
	struct midr_consumer_event *events;
	size_t count;
	uint64_t generation;
	enum midr_ted_state state;
	int last_error;
	int fail_next;
};

static int compare_u32(uint32_t left, uint32_t right)
{
	return (left > right) - (left < right);
}

static int compare_u64(uint64_t left, uint64_t right)
{
	return (left > right) - (left < right);
}

static int event_key_compare(const void *leftp, const void *rightp)
{
	const struct midr_consumer_event *left = leftp;
	const struct midr_consumer_event *right = rightp;
	int ret;

	ret = compare_u32((uint32_t)left->kind, (uint32_t)right->kind);
	if (ret)
		return ret;
	ret = compare_u32(left->originator, right->originator);
	if (ret)
		return ret;
	switch (left->kind) {
	case MIDR_CONSUMER_LINK:
		ret = compare_u32(left->remote, right->remote);
		return ret ? ret : compare_u64(left->link_id, right->link_id);
	case MIDR_CONSUMER_NODE_PREFIX:
		ret = compare_u32(left->family, right->family);
		if (ret)
			return ret;
		ret = compare_u32(left->prefix_len, right->prefix_len);
		return ret ? ret : memcmp(left->prefix, right->prefix,
					  sizeof(left->prefix));
	case MIDR_CONSUMER_GROUP_PREFIX:
		ret = compare_u32(left->group, right->group);
		if (ret)
			return ret;
		ret = compare_u32(left->family, right->family);
		if (ret)
			return ret;
		ret = compare_u32(left->prefix_len, right->prefix_len);
		return ret ? ret : memcmp(left->prefix, right->prefix,
					  sizeof(left->prefix));
	default:
		return 0;
	}
}

static bool event_semantic_equal(const struct midr_consumer_event *left,
				 const struct midr_consumer_event *right)
{
	if (event_key_compare(left, right))
		return false;
	if (left->metric != right->metric || left->family != right->family ||
	    memcmp(left->local_address, right->local_address,
		   sizeof(left->local_address)) ||
	    memcmp(left->remote_address, right->remote_address,
		   sizeof(left->remote_address)))
		return false;
	return true;
}

static bool snapshot_semantic_equal(
	const struct midr_ted *ted, const struct midr_consumer_event *events,
	size_t count)
{
	if (ted->count != count)
		return false;
	for (size_t i = 0; i < count; i++)
		if (!event_semantic_equal(&ted->events[i], &events[i]))
			return false;
	return true;
}

static int prepare_candidate(const struct midr_ted *ted,
			     const struct midr_consumer_snapshot *snapshot,
			     struct midr_consumer_event **out)
{
	struct midr_consumer_event *candidate = NULL;

	if (!ted || !snapshot || !out || *out || !snapshot->generation ||
	    snapshot->count > ted->config.max_events ||
	    (snapshot->count && !snapshot->events))
		return -EINVAL;
	if (snapshot->count > SIZE_MAX / sizeof(*candidate))
		return -EOVERFLOW;
	if (snapshot->count) {
		candidate = calloc(snapshot->count, sizeof(*candidate));
		if (!candidate)
			return -ENOMEM;
		memcpy(candidate, snapshot->events,
		       snapshot->count * sizeof(*candidate));
	}
	for (size_t i = 0; i < snapshot->count; i++) {
		const struct midr_consumer_event *event = &candidate[i];

		if (event->generation != snapshot->generation ||
		    midr_consumer_event_validate(event) ||
		    event->kind == MIDR_CONSUMER_SNAPSHOT_BEGIN ||
		    event->kind == MIDR_CONSUMER_SNAPSHOT_END) {
			free(candidate);
			return -EINVAL;
		}
	}
	if (snapshot->count > 1U)
		qsort(candidate, snapshot->count, sizeof(*candidate),
		      event_key_compare);
	for (size_t i = 1; i < snapshot->count; i++)
		if (!event_key_compare(&candidate[i - 1], &candidate[i])) {
			free(candidate);
			return -EEXIST;
		}
	*out = candidate;
	return 0;
}

int midr_ted_create(const struct midr_ted_config *config,
		    struct midr_ted **out)
{
	struct midr_ted *ted;

	if (!config || !out || *out || !config->max_events)
		return -EINVAL;
	ted = calloc(1, sizeof(*ted));
	if (!ted)
		return -ENOMEM;
	ted->config = *config;
	ted->state = MIDR_TED_NOT_READY;
	*out = ted;
	return 0;
}

void midr_ted_destroy(struct midr_ted **tedp)
{
	if (!tedp || !*tedp)
		return;
	free((*tedp)->events);
	free(*tedp);
	*tedp = NULL;
}

int midr_ted_apply_snapshot(struct midr_ted *ted,
			    const struct midr_consumer_snapshot *snapshot)
{
	struct midr_consumer_event *candidate = NULL;
	int ret;

	if (!ted || !snapshot)
		return -EINVAL;
	if (snapshot->generation < ted->generation)
		return -ESTALE;
	ret = prepare_candidate(ted, snapshot, &candidate);
	if (ret)
		goto failed;
	if (ted->fail_next) {
		ret = ted->fail_next;
		ted->fail_next = 0;
		goto failed;
	}
	if (snapshot->generation == ted->generation &&
	    ted->state == MIDR_TED_READY) {
		if (!snapshot_semantic_equal(ted, candidate, snapshot->count)) {
			ret = -EPROTO;
			goto failed;
		}
		free(candidate);
		ted->last_error = 0;
		return 0;
	}
	free(ted->events);
	ted->events = candidate;
	ted->count = snapshot->count;
	ted->generation = snapshot->generation;
	ted->state = MIDR_TED_READY;
	ted->last_error = 0;
	return 0;

failed:
	free(candidate);
	if (!ret)
		ret = -EINVAL;
	ted->state = MIDR_TED_NOT_READY;
	ted->last_error = ret;
	return ret;
}

int midr_ted_invalidate(struct midr_ted *ted, int error)
{
	if (!ted || error >= 0)
		return -EINVAL;
	ted->state = MIDR_TED_NOT_READY;
	ted->last_error = error;
	return 0;
}

int midr_ted_snapshot_acquire(const struct midr_ted *ted,
			      struct midr_consumer_snapshot *snapshot)
{
	if (!ted || !snapshot)
		return -EINVAL;
	if (ted->state != MIDR_TED_READY)
		return -EAGAIN;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->generation = ted->generation;
	snapshot->count = ted->count;
	if (!snapshot->count)
		return 0;
	snapshot->events = calloc(snapshot->count, sizeof(*snapshot->events));
	if (!snapshot->events)
		return -ENOMEM;
	memcpy(snapshot->events, ted->events,
	       snapshot->count * sizeof(*snapshot->events));
	return 0;
}

enum midr_ted_state midr_ted_state(const struct midr_ted *ted)
{
	return ted ? ted->state : MIDR_TED_NOT_READY;
}

uint64_t midr_ted_generation(const struct midr_ted *ted)
{
	return ted ? ted->generation : 0;
}

int midr_ted_last_error(const struct midr_ted *ted)
{
	return ted ? ted->last_error : -EINVAL;
}

int midr_ted_test_fail_next(struct midr_ted *ted, int error)
{
	if (!ted || error >= 0)
		return -EINVAL;
	ted->fail_next = error;
	return 0;
}
