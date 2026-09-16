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
	struct midr_scope *scope;
	struct midr_scope *batch_scope;
	struct midr_lsdb *lsdb;
	struct midr_consumer *consumer;
	struct midr_ted *ted;
	uint64_t generation;
	uint64_t batch_generation;
	unsigned int batch_depth;
	bool batch_dirty;
	bool batch_view_dirty;
	bool batch_failed;
	int batch_error;
	bool publication_pending;
	bool publication_local_metadata;
	int publication_error;
	bool scope_rebuild_pending;
};

static struct midr_core *active_core(struct midr_engine *engine)
{
	return engine->batch_core ? engine->batch_core : engine->core;
}

static struct midr_scope *active_scope(struct midr_engine *engine)
{
	return engine->batch_scope ? engine->batch_scope : engine->scope;
}

static int rebuild_scope(struct midr_engine *engine, struct midr_core *core,
				 uint64_t now_ms, struct midr_scope **out)
{
	struct midr_scope_config config = {
		.local_node_id = engine->config.node_id,
		.max_memberships = engine->config.max_objects,
	};
	struct midr_scope *scope = NULL;
	struct midr_core_object *objects = NULL;
	size_t count = 0;
	int ret;

	if (!engine || !core || !out || *out)
		return -EINVAL;
	ret = midr_scope_create(&config, &scope);
	if (ret)
		return ret;
	objects = calloc(engine->config.max_objects, sizeof(*objects));
	if (!objects) {
		midr_scope_destroy(&scope);
		return -ENOMEM;
	}
	ret = midr_core_snapshot(core, now_ms, objects,
				 engine->config.max_objects, &count);
	if (!ret)
		for (size_t i = 0; i < count; i++)
			if (objects[i].identity.type == MIDR_CORE_MEMBERSHIP &&
			    objects[i].state == MIDR_CORE_ACTIVE) {
				ret = midr_scope_apply(scope, &objects[i]);
				if (ret)
					break;
			}
	free(objects);
	if (ret) {
		midr_scope_destroy(&scope);
		return ret;
	}
	*out = scope;
	return 0;
}

/* midr_core_snapshot() expires entries as part of its read contract.  Keep
 * the engine's scope in lockstep when a caller reaches a snapshot between
 * periodic expiry passes; otherwise a just-expired Membership could still
 * make a dependent object appear usable for one publication cycle. */
static int synchronize_expiry(struct midr_engine *engine,
				      struct midr_core *core,
				      uint64_t now_ms,
				      struct midr_scope **scope)
{
	struct midr_scope *rebuilt = NULL;
	struct midr_scope *old_scope;
	size_t expired = 0;
	int ret;

	ret = midr_core_expire(core, now_ms, &expired);
	if (ret)
		return ret;
	if (!expired && !engine->scope_rebuild_pending)
		return 0;
	if (expired)
		engine->generation++;
	ret = rebuild_scope(engine, core, now_ms, &rebuilt);
	if (ret) {
		if (core == engine->core)
			engine->scope_rebuild_pending = true;
		return ret;
	}
	if (core == engine->batch_core) {
		old_scope = engine->batch_scope;
		engine->batch_scope = rebuilt;
		engine->batch_view_dirty = true;
		engine->batch_dirty = true;
	} else if (core == engine->core) {
		old_scope = engine->scope;
		engine->scope = rebuilt;
		engine->scope_rebuild_pending = false;
	} else {
		midr_scope_destroy(&rebuilt);
		return -EINVAL;
	}
	midr_scope_destroy(&old_scope);
	if (scope)
		*scope = core == engine->batch_core ? engine->batch_scope :
			engine->scope;
	return 0;
}

static int publish_snapshot(struct midr_engine *engine, struct midr_core *core,
			    struct midr_scope *scope, uint64_t now_ms,
			    bool local_metadata_update)
{
	struct midr_core_object *objects = NULL;
	struct midr_lsdb_stage *lsdb_stage = NULL;
	struct midr_ted_stage *ted_stage = NULL;
	struct midr_consumer_stage *consumer_stage = NULL;
	struct midr_consumer_snapshot snapshot = {0};
	size_t count = 0;
	int ret;

	if (!engine->consumer)
		return 0;
	{
		struct midr_scope *current_scope = scope;

		ret = synchronize_expiry(engine, core, now_ms, &current_scope);
		if (ret)
			return ret;
		scope = current_scope;
	}
	objects = calloc(engine->config.max_objects, sizeof(*objects));
	if (!objects)
		return -ENOMEM;
	ret = midr_core_snapshot(core, now_ms, objects,
				 engine->config.max_objects, &count);
	if (ret)
		goto done;
	ret = midr_lsdb_prepare(engine->lsdb, engine->generation, objects, count,
				scope, &lsdb_stage);
	if (ret)
		goto done;
	ret = midr_lsdb_stage_consumer_snapshot(lsdb_stage, &snapshot);
	if (ret)
		goto done;
	ret = midr_consumer_prepare_snapshot(
		engine->consumer, snapshot.generation, engine->config.node_id,
		snapshot.events, snapshot.count, &consumer_stage);
	if (ret)
		goto done;
	if (engine->ted) {
		ret = midr_ted_prepare_lsdb(engine->ted, engine->config.node_id,
					    lsdb_stage, &snapshot,
					    local_metadata_update, &ted_stage);
		if (ret)
			goto done;
	}
	/* Every allocation and validation has completed.  These commits only swap
	 * prepared ownership; Consumer callbacks run last and observe a matching
	 * LSDB/TED generation. */
	midr_lsdb_commit_prepared(engine->lsdb, &lsdb_stage);
	if (engine->ted)
		midr_ted_commit_prepared(engine->ted, &ted_stage);
	midr_consumer_commit_prepared(engine->consumer, &consumer_stage);
	ret = 0;
done:
	midr_consumer_abort_prepared(&consumer_stage);
	midr_ted_abort_prepared(&ted_stage);
	midr_lsdb_abort_prepared(&lsdb_stage);
	free(objects);
	return ret;
}

static int publish_current_snapshot(struct midr_engine *engine,
				    uint64_t now_ms,
				    bool local_metadata_update)
{
	int ret;

	/* Clear the marker before invoking the Consumer callback.  A successful
	 * commit callback may synchronously inspect or retry the engine and must
	 * observe that the latest view is already published. */
	engine->publication_pending = false;
	engine->publication_local_metadata = false;
	engine->publication_error = 0;
	ret = publish_snapshot(engine, engine->core, engine->scope, now_ms,
			       local_metadata_update);
	if (ret) {
		engine->publication_pending = true;
		engine->publication_local_metadata = local_metadata_update;
		engine->publication_error = ret;
		if (engine->ted)
			(void)midr_ted_invalidate(engine->ted, ret);
	}
	return ret;
}

int midr_engine_create(const struct midr_engine_config *config,
			       struct midr_engine **out)
{
	struct midr_engine *engine;
	struct midr_core_config core_config = {0};
	struct midr_lsdb_config lsdb_config;
	struct midr_scope_config scope_config;
	int ret;

	if (!config || !out || *out || !config->node_id ||
	    !config->max_objects || !config->lifetime_ms)
		return -EINVAL;
	engine = calloc(1, sizeof(*engine));
	if (!engine)
		return -ENOMEM;
	core_config.max_objects = config->max_objects;
	core_config.lifetime_ms = config->lifetime_ms;
	ret = midr_core_create(&core_config, &engine->core);
	if (ret) {
		free(engine);
		return ret;
	}
	engine->config = *config;
	scope_config.local_node_id = config->node_id;
	scope_config.max_memberships = config->max_objects;
	ret = midr_scope_create(&scope_config, &engine->scope);
	if (ret) {
		midr_core_destroy(&engine->core);
		free(engine);
		return ret;
	}
	lsdb_config.max_objects = config->max_objects;
	ret = midr_lsdb_create(&lsdb_config, &engine->lsdb);
	if (ret) {
		midr_scope_destroy(&engine->scope);
		midr_core_destroy(&engine->core);
		free(engine);
		return ret;
	}
	engine->generation = 1;
	*out = engine;
	return 0;
}

void midr_engine_destroy(struct midr_engine **enginep)
{
	if (!enginep || !*enginep)
		return;
	midr_scope_destroy(&(*enginep)->batch_scope);
	midr_scope_destroy(&(*enginep)->scope);
	midr_lsdb_destroy(&(*enginep)->lsdb);
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
	return publish_current_snapshot(engine, 0, false);
}

int midr_engine_attach_ted(struct midr_engine *engine, struct midr_ted *ted)
{
	if (!engine)
		return -EINVAL;
	engine->ted = ted;
	return engine->consumer ? publish_current_snapshot(engine, 0, false) : 0;
}

int midr_engine_apply(struct midr_engine *engine,
			      const struct midr_core_object *object, uint64_t now_ms,
			      enum midr_core_result *result)
{
	struct midr_core_object old;
	bool had_old;
	bool semantic_changed;
	bool implicit_batch;
	int ret;

	if (!engine || !object || !result)
		return -EINVAL;
	implicit_batch = object->identity.type == MIDR_CORE_MEMBERSHIP &&
			 !engine->batch_depth;
	if (implicit_batch) {
		ret = midr_engine_begin_batch(engine);
		if (ret)
			return ret;
		ret = midr_engine_apply(engine, object, now_ms, result);
		if (ret) {
			(void)midr_engine_abort_batch(engine);
			return ret;
		}
		return midr_engine_end_batch(engine, now_ms);
	}
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
	semantic_changed = !had_old ||
			   !midr_core_object_semantic_equal(&old, object);
	if (semantic_changed && object->identity.type == MIDR_CORE_MEMBERSHIP) {
		ret = midr_scope_apply(active_scope(engine), object);
		if (ret) {
			if (engine->batch_core) {
				engine->batch_failed = true;
				engine->batch_error = ret;
				return ret;
			}
			/* Canonical already accepted the object.  Gate scope-dependent
			 * export and retry a complete scope/publication rebuild. */
			engine->generation++;
			engine->scope_rebuild_pending = true;
			engine->publication_pending = true;
			engine->publication_error = ret;
			return 0;
		}
	}
	if (semantic_changed)
		engine->generation++;
	if (engine->batch_depth) {
		engine->batch_dirty = true;
		engine->batch_view_dirty |= semantic_changed;
		return 0;
	}
	if (semantic_changed)
		(void)publish_current_snapshot(engine, now_ms, false);
	/* Canonical acceptance is independent from downstream publication.  The
	 * caller observes publication_pending and keeps the derived view gated
	 * until retry_publication() succeeds. */
	return 0;
}

int midr_engine_begin_batch(struct midr_engine *engine)
{
	if (!engine)
		return -EINVAL;
	if (engine->batch_depth == UINT_MAX)
		return -ERANGE;
	if (!engine->batch_depth) {
		int ret = midr_core_clone(engine->core, &engine->batch_core);

		if (ret)
			return ret;
		ret = midr_scope_clone(engine->scope, &engine->batch_scope);
		if (ret) {
			midr_core_destroy(&engine->batch_core);
			return ret;
		}
		engine->batch_generation = engine->generation;
		engine->batch_failed = false;
		engine->batch_error = 0;
		engine->batch_view_dirty = false;
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
		bool publish;

		if (engine->batch_failed) {
			int error = engine->batch_error;

			(void)midr_engine_abort_batch(engine);
			return error;
		}
		if (!engine->batch_dirty) {
			midr_core_destroy(&engine->batch_core);
			midr_scope_destroy(&engine->batch_scope);
			return 0;
		}
		publish = engine->batch_view_dirty;
		{
			struct midr_core *old_core = engine->core;
			struct midr_scope *old_scope = engine->scope;

			engine->core = engine->batch_core;
			engine->batch_core = NULL;
			midr_core_destroy(&old_core);
			if (engine->batch_view_dirty) {
				engine->scope = engine->batch_scope;
				engine->batch_scope = NULL;
				midr_scope_destroy(&old_scope);
			} else {
				midr_scope_destroy(&engine->batch_scope);
			}
		}
		engine->batch_dirty = false;
		engine->batch_view_dirty = false;
		engine->batch_failed = false;
		engine->batch_error = 0;
		if (publish)
			(void)publish_current_snapshot(engine, now_ms, false);
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
	if (engine->batch_scope)
		midr_scope_destroy(&engine->batch_scope);
	engine->batch_depth = 0;
	engine->batch_dirty = false;
	engine->batch_view_dirty = false;
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
	if (ret) {
		if (engine->batch_core) {
			engine->batch_failed = true;
			engine->batch_error = ret;
		}
		return ret;
	}
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
	(void)publish_current_snapshot(engine, now_ms, false);
	return 0;
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
		if (identity->type == MIDR_CORE_MEMBERSHIP) {
			struct midr_scope *scope = NULL;

			ret = rebuild_scope(engine, active_core(engine), now_ms, &scope);
			if (ret) {
				if (engine->batch_core) {
					engine->batch_failed = true;
					engine->batch_error = ret;
					return ret;
				}
				engine->scope_rebuild_pending = true;
				engine->publication_pending = true;
				engine->publication_error = ret;
				return 0;
			}
			if (engine->batch_core) {
				midr_scope_destroy(&engine->batch_scope);
				engine->batch_scope = scope;
				engine->batch_view_dirty = true;
			} else {
				struct midr_scope *old_scope = engine->scope;

				engine->scope = scope;
				midr_scope_destroy(&old_scope);
			}
		}
		if (engine->batch_depth) {
			engine->batch_dirty = true;
			return 0;
		}
		(void)publish_current_snapshot(engine, now_ms, false);
		ret = 0;
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
		struct midr_scope *scope = NULL;

		engine->generation++;
		ret = rebuild_scope(engine, active_core(engine), now_ms, &scope);
		if (ret) {
			if (engine->batch_core) {
				engine->batch_failed = true;
				engine->batch_error = ret;
				return ret;
			}
			engine->scope_rebuild_pending = true;
			engine->publication_pending = true;
			engine->publication_error = ret;
			return 0;
		}
		if (engine->batch_core) {
			midr_scope_destroy(&engine->batch_scope);
			engine->batch_scope = scope;
			engine->batch_view_dirty = true;
		} else {
			struct midr_scope *old_scope = engine->scope;

			engine->scope = scope;
			midr_scope_destroy(&old_scope);
		}
		if (engine->batch_depth) {
			engine->batch_dirty = true;
			engine->batch_view_dirty = true;
			return 0;
		}
		(void)publish_current_snapshot(engine, now_ms, false);
		ret = 0;
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

int midr_engine_lookup(const struct midr_engine *engine,
		       const struct midr_core_identity *identity,
		       uint64_t now_ms, struct midr_core_object *object,
		       uint32_t *remaining_ms)
{
	return engine ? midr_core_lookup(engine->core, identity, now_ms, object,
					 remaining_ms) : -EINVAL;
}

int midr_engine_batch_snapshot(struct midr_engine *engine, uint64_t now_ms,
				      struct midr_core_object *objects,
				      size_t capacity, size_t *count)
{
	if (!engine || !engine->batch_depth || !engine->batch_core)
		return -EINVAL;
	return midr_core_snapshot(engine->batch_core, now_ms, objects, capacity,
				  count);
}

int midr_engine_event_next(struct midr_engine *engine,
				  struct midr_core_object *object)
{
	return engine ? midr_core_event_next(engine->core, object) : -EINVAL;
}

int midr_engine_retry_publication(struct midr_engine *engine,
					  uint64_t now_ms)
{
	if (!engine)
		return -EINVAL;
	if (!engine->publication_pending)
		return 0;
	return publish_current_snapshot(engine, now_ms,
					engine->publication_local_metadata);
}

int midr_engine_republish(struct midr_engine *engine, uint64_t now_ms)
{
	return engine ? publish_current_snapshot(engine, now_ms, true) : -EINVAL;
}

bool midr_engine_publication_pending(const struct midr_engine *engine)
{
	return engine && engine->publication_pending;
}

int midr_engine_publication_error(const struct midr_engine *engine)
{
	return engine && engine->publication_pending
		       ? engine->publication_error : 0;
}

int midr_engine_lsdb_snapshot_acquire(const struct midr_engine *engine,
				      struct midr_lsdb_snapshot *snapshot)
{
	return engine ? midr_lsdb_snapshot_acquire(engine->lsdb, snapshot) :
		-EINVAL;
}

int midr_engine_test_fail_next_lsdb(struct midr_engine *engine, int error)
{
	return engine ? midr_lsdb_test_fail_next(engine->lsdb, error) :
		-EINVAL;
}

uint64_t midr_engine_generation(const struct midr_engine *engine)
{
	return engine ? engine->generation : 0;
}

size_t midr_engine_count(const struct midr_engine *engine)
{
	return engine ? midr_core_count(engine->core) : 0;
}

bool midr_engine_export(const struct midr_engine *engine,
			const struct midr_core_object *object,
			uint32_t peer_node_id)
{
	if (!engine || !object)
		return false;
	if (engine->scope_rebuild_pending &&
	    object->state != MIDR_CORE_WITHDRAWN &&
	    object->identity.type != MIDR_CORE_MEMBERSHIP)
		return false;
	return midr_scope_export(engine->scope, object, peer_node_id);
}

bool midr_engine_usable(const struct midr_engine *engine,
			const struct midr_core_object *object)
{
	return engine && !engine->scope_rebuild_pending &&
	       midr_scope_usable(engine->scope, object);
}

int midr_engine_membership(const struct midr_engine *engine,
			   uint32_t node_id, uint32_t *group)
{
	if (!engine)
		return -EINVAL;
	if (engine->scope_rebuild_pending)
		return -EAGAIN;
	return midr_scope_membership(engine->scope, node_id, group);
}

int midr_engine_representative(const struct midr_engine *engine,
			       uint32_t group, uint32_t *node_id)
{
	if (!engine)
		return -EINVAL;
	if (engine->scope_rebuild_pending)
		return -EAGAIN;
	return midr_scope_representative(engine->scope, group, node_id);
}

int midr_engine_batch_membership(const struct midr_engine *engine,
				 uint32_t node_id, uint32_t *group)
{
	return engine && engine->batch_scope
		       ? midr_scope_membership(engine->batch_scope, node_id, group)
		       : -EINVAL;
}

int midr_engine_batch_representative(const struct midr_engine *engine,
				     uint32_t group, uint32_t *node_id)
{
	return engine && engine->batch_scope
		       ? midr_scope_representative(engine->batch_scope, group,
						   node_id)
		       : -EINVAL;
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
	object.metric = object.state == MIDR_CORE_WITHDRAWN ? 0 :
			event->prefix.metric;
	return midr_engine_apply(engine, &object, now_ms, &result);
}
