#ifndef MIDR_WIRE_H
#define MIDR_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "midr-core.h"

#define MIDR_WIRE_MAGIC 0x4d494452U
#define MIDR_WIRE_VERSION 1U
#define MIDR_WIRE_HEADER_LEN 20U
#define MIDR_WIRE_OBJECT_LEN 92U

enum midr_wire_frame_type {
	MIDR_WIRE_HELLO = 1,
	MIDR_WIRE_KEEPALIVE = 2,
	MIDR_WIRE_SNAPSHOT_BEGIN = 3,
	MIDR_WIRE_SNAPSHOT_OBJECT = 4,
	MIDR_WIRE_SNAPSHOT_END = 5,
	MIDR_WIRE_EOR = 6,
	MIDR_WIRE_UPDATE = 7,
	MIDR_WIRE_WITHDRAW = 8,
};

struct midr_wire_frame {
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint64_t sequence;
	const uint8_t *payload;
	size_t payload_len;
};

int midr_wire_encode_object(const struct midr_core_object *object,
				   uint8_t *payload, size_t capacity,
				   size_t *length);
int midr_wire_decode_object(const uint8_t *payload, size_t length,
				   struct midr_core_object *object);
int midr_wire_encode_frame(const struct midr_wire_frame *frame,
				   uint8_t *buffer, size_t capacity, size_t *length);
int midr_wire_decode_frame(const uint8_t *buffer, size_t length,
				   struct midr_wire_frame *frame);

#endif
