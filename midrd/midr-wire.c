#include "midr-wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

static uint64_t htonll_u64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return ((uint64_t)htonl((uint32_t)value) << 32) |
	       htonl((uint32_t)(value >> 32));
#else
	return value;
#endif
}

static uint64_t ntohll_u64(uint64_t value)
{
	return htonll_u64(value);
}

static void put_u16(uint8_t *p, uint16_t value)
{
	uint16_t n = htons(value);
	memcpy(p, &n, sizeof(n));
}

static void put_u32(uint8_t *p, uint32_t value)
{
	uint32_t n = htonl(value);
	memcpy(p, &n, sizeof(n));
}

static void put_u64(uint8_t *p, uint64_t value)
{
	uint64_t n = htonll_u64(value);
	memcpy(p, &n, sizeof(n));
}

static uint16_t get_u16(const uint8_t *p)
{
	uint16_t n;
	memcpy(&n, p, sizeof(n));
	return ntohs(n);
}

static uint32_t get_u32(const uint8_t *p)
{
	uint32_t n;
	memcpy(&n, p, sizeof(n));
	return ntohl(n);
}

static uint64_t get_u64(const uint8_t *p)
{
	uint64_t n;
	memcpy(&n, p, sizeof(n));
	return ntohll_u64(n);
}

int midr_wire_encode_object(const struct midr_core_object *object,
				   uint8_t *payload, size_t capacity,
				   size_t *length)
{
	const struct midr_core_identity *id;

	if (!object || !payload || !length || capacity < MIDR_WIRE_OBJECT_LEN ||
	    !object->sequence ||
	    midr_core_identity_validate(&object->identity) ||
	    (object->state != MIDR_CORE_ACTIVE &&
	     object->state != MIDR_CORE_WITHDRAWN))
		return -EINVAL;
	if (object->state == MIDR_CORE_ACTIVE &&
	    object->identity.type == MIDR_CORE_LINK &&
	    (!object->metric || object->metric == UINT32_MAX))
		return -EINVAL;
	id = &object->identity;
	payload[0] = id->type;
	payload[1] = id->family;
	payload[2] = id->prefix_len;
	payload[3] = 0;
	put_u32(payload + 4, id->originator);
	put_u32(payload + 8, id->remote);
	put_u32(payload + 12, id->type == MIDR_CORE_MEMBERSHIP ?
			object->group : id->group);
	put_u64(payload + 16, id->link_id);
	memcpy(payload + 24, id->prefix, sizeof(id->prefix));
	payload[40] = object->state;
	memset(payload + 41, 0, 3);
	put_u64(payload + 44, object->sequence);
	put_u32(payload + 52, object->lifetime_ms);
	put_u32(payload + 56, object->metric);
	memcpy(payload + 60, object->local_address, 16);
	memcpy(payload + 76, object->remote_address, 16);
	*length = MIDR_WIRE_OBJECT_LEN;
	return 0;
}

int midr_wire_decode_object(const uint8_t *payload, size_t length,
				   struct midr_core_object *object)
{
	if (!payload || !object || length != MIDR_WIRE_OBJECT_LEN)
		return -EINVAL;
	memset(object, 0, sizeof(*object));
	object->identity.type = payload[0];
	object->identity.family = payload[1];
	object->identity.prefix_len = payload[2];
	object->identity.originator = get_u32(payload + 4);
	object->identity.remote = get_u32(payload + 8);
	object->identity.group = get_u32(payload + 12);
	object->identity.link_id = get_u64(payload + 16);
	memcpy(object->identity.prefix, payload + 24,
	       sizeof(object->identity.prefix));
	object->state = payload[40];
	object->sequence = get_u64(payload + 44);
	object->lifetime_ms = get_u32(payload + 52);
	object->metric = get_u32(payload + 56);
	memcpy(object->local_address, payload + 60, 16);
	memcpy(object->remote_address, payload + 76, 16);
	if (object->identity.type == MIDR_CORE_MEMBERSHIP) {
		object->group = object->identity.group;
		object->identity.group = 0;
	}
	if (object->state == MIDR_CORE_ACTIVE &&
	    object->identity.type == MIDR_CORE_LINK &&
	    (!object->metric || object->metric == UINT32_MAX))
		return -EINVAL;
	return midr_core_identity_validate(&object->identity) ? -EINVAL :
	       ((object->state == MIDR_CORE_ACTIVE ||
		 object->state == MIDR_CORE_WITHDRAWN) && object->sequence
		? 0 : -EINVAL);
}

int midr_wire_encode_frame(const struct midr_wire_frame *frame,
				   uint8_t *buffer, size_t capacity, size_t *length)
{
	uint32_t payload_len;

	if (!frame || !buffer || !length || frame->version != MIDR_WIRE_VERSION ||
	    frame->type < MIDR_WIRE_HELLO || frame->type > MIDR_WIRE_WITHDRAW ||
	    frame->payload_len > UINT32_MAX ||
	    capacity < MIDR_WIRE_HEADER_LEN + frame->payload_len)
		return -EINVAL;
	payload_len = (uint32_t)frame->payload_len;
	put_u32(buffer, MIDR_WIRE_MAGIC);
	buffer[4] = frame->version;
	buffer[5] = frame->type;
	put_u16(buffer + 6, frame->flags);
	put_u64(buffer + 8, frame->sequence);
	put_u32(buffer + 16, payload_len);
	if (payload_len && !frame->payload)
		return -EINVAL;
	if (payload_len)
		memcpy(buffer + MIDR_WIRE_HEADER_LEN, frame->payload, payload_len);
	*length = MIDR_WIRE_HEADER_LEN + payload_len;
	return 0;
}

int midr_wire_decode_frame(const uint8_t *buffer, size_t length,
				   struct midr_wire_frame *frame)
{
	uint32_t payload_len;

	if (!buffer || !frame || length < MIDR_WIRE_HEADER_LEN ||
	    get_u32(buffer) != MIDR_WIRE_MAGIC ||
	    buffer[4] != MIDR_WIRE_VERSION)
		return -EINVAL;
	payload_len = get_u32(buffer + 16);
	if (payload_len > length - MIDR_WIRE_HEADER_LEN ||
	    length != MIDR_WIRE_HEADER_LEN + payload_len ||
	    buffer[5] < MIDR_WIRE_HELLO || buffer[5] > MIDR_WIRE_WITHDRAW)
		return -EINVAL;
	frame->version = buffer[4];
	frame->type = buffer[5];
	frame->flags = get_u16(buffer + 6);
	frame->sequence = get_u64(buffer + 8);
	frame->payload = buffer + MIDR_WIRE_HEADER_LEN;
	frame->payload_len = payload_len;
	return 0;
}
