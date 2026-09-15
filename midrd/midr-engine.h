#ifndef MIDRD_ENGINE_H
#define MIDRD_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "midr-consumer.h"
#include "midr-core.h"
#include "midr-prefix-provider.h"
#include "midr-scope.h"

struct midr_engine;

struct midr_engine_config {
	uint32_t node_id;
	size_t max_objects;
	uint32_t lifetime_ms;
};

int midr_engine_create(const struct midr_engine_config *config,
			       struct midr_engine **out);
void midr_engine_destroy(struct midr_engine **engine);
int midr_engine_attach_consumer(struct midr_engine *engine,
				struct midr_consumer *consumer);
int midr_engine_begin_batch(struct midr_engine *engine);
int midr_engine_end_batch(struct midr_engine *engine, uint64_t now_ms);
int midr_engine_abort_batch(struct midr_engine *engine);
int midr_engine_apply(struct midr_engine *engine,
			      const struct midr_core_object *object, uint64_t now_ms,
			      enum midr_core_result *result);
int midr_engine_refresh(struct midr_engine *engine,
			       const struct midr_core_identity *identity,
			       uint64_t now_ms);
int midr_engine_withdraw(struct midr_engine *engine,
				const struct midr_core_identity *identity,
				uint64_t now_ms);
int midr_engine_expire(struct midr_engine *engine, uint64_t now_ms,
			      size_t *expired);
int midr_engine_snapshot(struct midr_engine *engine, uint64_t now_ms,
				struct midr_core_object *objects, size_t capacity,
				size_t *count);
int midr_engine_event_next(struct midr_engine *engine,
				  struct midr_core_object *object);
uint64_t midr_engine_generation(const struct midr_engine *engine);
size_t midr_engine_count(const struct midr_engine *engine);
bool midr_engine_export(const struct midr_engine *engine,
			const struct midr_core_object *object,
			uint32_t peer_node_id);
bool midr_engine_usable(const struct midr_engine *engine,
			const struct midr_core_object *object);
int midr_engine_membership(const struct midr_engine *engine,
			   uint32_t node_id, uint32_t *group);
int midr_engine_representative(const struct midr_engine *engine,
			       uint32_t group, uint32_t *node_id);

int midr_engine_apply_prefix_event(struct midr_engine *engine,
				   const struct midr_prefix_event *event,
				   uint64_t now_ms);

#endif
