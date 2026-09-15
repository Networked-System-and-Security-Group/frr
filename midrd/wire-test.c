#include "midr-wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct midr_core_object object = {0}, decoded = {0};
	struct midr_wire_frame frame, parsed;
	uint8_t payload[MIDR_WIRE_OBJECT_LEN], packet[256];
	uint8_t identity_prefix[16] = {0x20, 0x01, 0x0d, 0xb8};
	size_t payload_len, packet_len;

	object.identity.type = MIDR_CORE_NODE_PREFIX;
	object.identity.family = MIDR_CORE_AF_IPV6;
	object.identity.prefix_len = 64;
	object.identity.originator = 42;
	memcpy(object.identity.prefix, identity_prefix, sizeof(identity_prefix));
	object.state = MIDR_CORE_ACTIVE;
	object.sequence = 9;
	object.lifetime_ms = 1000;
	object.metric = 10;
	assert(midr_wire_encode_object(&object, payload, sizeof(payload),
				       &payload_len) == 0);
	assert(midr_wire_decode_object(payload, payload_len, &decoded) == 0);
	assert(midr_core_object_semantic_equal(&object, &decoded));
	frame.version = MIDR_WIRE_VERSION;
	frame.type = MIDR_WIRE_UPDATE;
	frame.flags = 3;
	frame.sequence = 17;
	frame.payload = payload;
	frame.payload_len = payload_len;
	assert(midr_wire_encode_frame(&frame, packet, sizeof(packet),
				      &packet_len) == 0);
	assert(midr_wire_decode_frame(packet, packet_len, &parsed) == 0);
	assert(parsed.type == MIDR_WIRE_UPDATE && parsed.sequence == 17);
	assert(parsed.payload_len == payload_len);
	assert(midr_wire_decode_frame(packet, packet_len - 1, &parsed) == -EINVAL);
	puts("midrd-wire-test: PASS");
	return 0;
}
