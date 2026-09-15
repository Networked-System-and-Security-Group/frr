/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "midr-engine.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct midr_engine {
	struct midr_engine_config config;
	struct midr_core *core;
	struct midr_core *batch_core;
	struct midr_consumer *consumer;
	uint64_t generation;
	uint64_t batch_generation;
	unsigned int batch_depth;
	bool batch_dirty;
	bool batch_failed;
	int batch_error;
};

static struct midr_core *active_core(struct midr_engine *engine)
{
	return engine->batch_core ? engine->batch_core : engine->core;
}

static int object_to_consumer(const struct midr_core_object *object,
				      uint64_t generation,
				      struct midr_consumer_event *event)
{
	memset(event, 0, sizeof(*event));
	event->generation = generation;
	event->originator = object->identity.originator;
	event->remote = object->identity.remote;
	event->group = object->identity.group;
	event->link_id = object->identity.link_id;
	event->family = object->identity.family;
	event->prefix_len = object->identity.prefix_len;
	memcpy(event->prefix, object->identity.prefix,
	       sizeof(event->prefix));
	event->metric = object->metric;
	switch (object->identity.type) {
	case MIDR_CORE_LINK:
		event->kind = MIDR_CONSUMER_LINK;
		break;
	case MIDR_CORE_NODE_PREFIX:
		event->kind = MIDR_CONSUMER_NODE_PREFIX;
		break;
	case MIDR_CORE_GROUP_PREFIX:
		event->kind = MIDR_CONSUMER_GROUP_PREFIX;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int publish_snapshot(struct midr_engine *engine, struct midr_core *core,
				    uint64_t now_ms)
{
	struct midr_core_object *objects;
	struct midr_consumer_event *events;
	size_t count = 0;
	size_t event_count = 0;
	int ret;

	if (!engine->consumer)
		return 0;
	objects = calloc(engine->config.max_objects, sizeof(*objects));
	if (!objects)
		return -ENOMEM;
	events = calloc(engine->config.max_objects, sizeof(*events));
	if (!events) {
		free(objects);
		return -ENOMEM;
	}
	ret = midr_core_snapshot(core, now_ms, objects,
				 engine->config.max_objects, &count);
	if (ret)
		goto done;
	for (size_t i = 0; i < count; i++) {
		if (objects[i].state != MIDR_CORE_ACTIVE)
			continue;
		ret = object_to_consumer(&objects[i], engine->generation,
					 &events[event_count]);
		if (ret == -ENOENT)
			continue;
		if (ret)
			goto done;
		event_count++;
	}
	ret = midr_consumer_commit_snapshot(engine->consumer, engine->generation,
					    engine->config.node_id, events, event_count);
done:
	free(events);
	free(objects);
	return ret;
}

int midr_engine_create(const struct midr_engine_config *config,
			       struct midr_engine **out)
{
	struct midr_engine *engine;
	struct midr_core_config core_config;

	if (!config || !out || *out || !config->node_id ||
	    !config->max_objects || !config->lifetime_ms)
		return -EINVAL;
	engine = calloc(1, sizeof(*engine));
	if (!engine)
		return -ENOMEM;
	core_config.max_objects = config->max_objects;
	core_config.lifetime_ms = config->lifetime_ms;
	if (midr_core_create(&core_config, &engine->core)) {
		free(engine);
		return -ENOMEM;
	}
	engine->config = *config;
	engine->generation = 1;
	*out = engine;
	return 0;
}

void midr_engine_destroy(struct midr_engine **enginep)
{
	if (!enginep || !*enginep)
		return;
	midr_core_destroy(&(*enginep)->batch_core);
	midr_core_destroy(&(*enginep)->core);
	free(*enginep);
	*enginep = NULL;
}

int midr_engine_attach_consumer(struct midr_engine *engine,
				struct midr_consumer *consumer)
{
	if (!engine)
		return -EINVAL;
	engine->consumer = consumer;
	return publish_snapshot(engine, engine->core, 0);
}

int midr_engine_apply(struct midr_engine *engine,
			      const struct midr_core_object *object, uint64_t now_ms,
			      enum midr_core_result *result)
{
	struct midr_core_object old;
	bool had_old;
	int ret;

	if (!engine || !object || !result)
		return -EINVAL;
	if (engine->batch_failed)
		return engine->batch_error;
	had_old = midr_core_lookup(active_core(engine), &object->identity, now_ms,
					&old, NULL) == 0;
	ret = midr_core_upsert(active_core(engine), object, now_ms, result);
	if (ret || *result != MIDR_CORE_ACCEPTED) {
		if (ret && engine->batch_core) {
			engine->batch_failed = true;
			engine->batch_error = ret;
		}
		return ret;
	}
	if (!had_old || !midr_core_object_semantic_equal(&old, object))
		engine->generation++;
	if (engine->batch_depth) {
		engine->batch_dirty = true;
		return 0;
	}
	return publish_snapshot(engine, engine->core, now_ms);
}

int midr_engine_begin_batch(struct midr_engine *engine)
{
	if (!engine)
		return -EINVAL;
	if (engine->batch_depth == UINT_MAX)
		return -ERANGE;
	if (!engine->batch_depth) {
		if (midr_core_clone(engine->core, &engine->batch_core))
			return -ENOMEM;
		engine->batch_generation = engine->generation;
		engine->batch_failed = false;
		engine->batch_error = 0;
	}
	engine->batch_depth++;
	return 0;
}

int midr_engine_end_batch(struct midr_engine *engine, uint64_t now_ms)
{
	if (!engine || !engine->batch_depth)
		return -EINVAL;
	engine->batch_depth--;
	if (!engine->batch_depth) {
		int ret;

		if (engine->batch_failed) {
			int error = engine->batch_error;

			(void)midr_engine_abort_batch(engine);
			return error;
		}
		if (!engine->batch_dirty) {
			midr_core_destroy(&engine->batch_core);
			return 0;
		}
		ret = publish_snapshot(engine, engine->batch_core, now_ms);
		if (ret) {
			(void)midr_engine_abort_batch(engine);
			return ret;
		}
		{
			struct midr_core *old_core = engine->core;

			engine->core = engine->batch_core;
			engine->batch_core = NULL;
			midr_core_destroy(&old_core);
		}
		engine->batch_dirty = false;
		engine->batch_failed = false;
		engine->batch_error = 0;
	}
	return 0;
}

int midr_engine_abort_batch(struct midr_engine *engine)
{
	if (!engine)
		return -EINVAL;
	if (!engine->batch_core && !engine->batch_depth)
		return 0;
	if (engine->batch_core)
		midr_core_destroy(&engine->batch_core);
	engine->batch_depth = 0;
	engine->batch_dirty = false;
	engine->batch_failed = false;
	engine->batch_error = 0;
	engine->generation = engine->batch_generation;
	return 0;
}

int midr_engine_refresh(struct midr_engine *engine,
			       const struct midr_core_identity *identity,
			       uint64_t now_ms)
{
	struct midr_core_object old;
	int ret;

	if (!engine || !identity)
		return -EINVAL;
	if (engine->batch_failed)
		return engine->batch_error;
	ret = midr_core_lookup(active_core(engine), identity, now_ms, &old, NULL);
	if (ret)
		return ret;
	ret = midr_core_refresh(active_core(engine), identity, now_ms);
	if (ret) {
		if (engine->batch_core) {
			engine->batch_failed = true;
			engine->batch_error = ret;
		}
		return ret;
	}
	if (engine->batch_depth) {
		engine->batch_dirty = true;
		return 0;
	}
	ret = publish_snapshot(engine, engine->core, now_ms);
	return ret;
}

int midr_engine_withdraw(struct midr_engine *engine,
				const struct midr_core_identity *identity,
				uint64_t now_ms)
{
	int ret;

	if (!engine || !identity)
		return -EINVAL;
	if (engine->batch_failed)
		return engine->batch_error;
	ret = midr_core_withdraw(active_core(engine), identity, now_ms);
	if (ret && engine->batch_core) {
		engine->batch_failed = true;
		engine->batch_error = ret;
	}
	if (!ret) {
		engine->generation++;
		if (engine->batch_depth) {
			engine->batch_dirty = true;
			return 0;
		}
		ret = publish_snapshot(engine, engine->core, now_ms);
	}
	return ret;
}

int midr_engine_expire(struct midr_engine *engine, uint64_t now_ms,
			      size_t *expired)
{
	int ret;

	if (!engine)
		return -EINVAL;
	if (engine->batch_failed)
		return engine->batch_error;
	ret = midr_core_expire(active_core(engine), now_ms, expired);
	if (ret && engine->batch_core) {
		engine->batch_failed = true;
		engine->batch_error = ret;
	}
	if (!ret && expired && *expired) {
		engine->generation++;
		if (engine->batch_depth) {
			engine->batch_dirty = true;
			return 0;
		}
		ret = publish_snapshot(engine, engine->core, now_ms);
	}
	return ret;
}

int midr_engine_snapshot(struct midr_engine *engine, uint64_t now_ms,
				struct midr_core_object *objects, size_t capacity,
				size_t *count)
{
	return engine ? midr_core_snapshot(engine->core, now_ms, objects, capacity,
							 count) : -EINVAL;
}

int midr_engine_event_next(struct midr_engine *engine,
				  struct midr_core_object *object)
{
	return engine ? midr_core_event_next(engine->core, object) : -EINVAL;
}

uint64_t midr_engine_generation(const struct midr_engine *engine)
{
	return engine ? engine->generation : 0;
}

size_t midr_engine_count(const struct midr_engine *engine)
{
	return engine ? midr_core_count(engine->core) : 0;
}

int midr_engine_apply_prefix_event(struct midr_engine *engine,
				   const struct midr_prefix_event *event,
				   uint64_t now_ms)
{
	struct midr_core_object object = {0};
	enum midr_core_result result;

	if (!engine || midr_prefix_event_validate(event))
		return -EINVAL;
	if (event->kind == MIDR_PREFIX_SNAPSHOT_BEGIN ||
	    event->kind == MIDR_PREFIX_SNAPSHOT_END ||
	    event->kind == MIDR_PREFIX_EOR)
		return 0;
	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = event->prefix.family;
	object.identity.prefix_len = event->prefix.prefix_len;
	object.identity.originator = event->originator;
	memcpy(object.identity.prefix, event->prefix.address,
	       sizeof(object.identity.prefix));
	object.state = event->kind == MIDR_PREFIX_WITHDRAW
			     ? MIDR_CORE_WITHDRAWN : MIDR_CORE_ACTIVE;
	object.sequence = event->generation;
	object.lifetime_ms = engine->config.lifetime_ms;
	object.metric = event->prefix.metric;
	return midr_engine_apply(engine, &object, now_ms, &result);
}
