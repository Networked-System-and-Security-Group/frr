#include "midr-wire.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct midr_core_object object = {0}, decoded = {0};
	struct midr_core_object membership = {0}, link = {0};
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
	membership.identity.type = MIDR_CORE_MEMBERSHIP;
	membership.identity.originator = 42;
	membership.group = 7;
	membership.state = MIDR_CORE_ACTIVE;
	membership.sequence = 10;
	membership.lifetime_ms = 1000;
	assert(midr_wire_encode_object(&membership, payload, sizeof(payload),
				       &payload_len) == 0);
	assert(midr_wire_decode_object(payload, payload_len, &decoded) == 0);
	assert(decoded.identity.group == 0 && decoded.group == 7);
	assert(midr_core_object_semantic_equal(&membership, &decoded));
	link.identity.type = MIDR_CORE_LINK;
	link.identity.originator = 42;
	link.identity.remote = 43;
	link.identity.link_id = 9;
	link.state = MIDR_CORE_ACTIVE;
	link.address_family = MIDR_CORE_AF_IPV6;
	link.sequence = 11;
	link.lifetime_ms = 1000;
	link.metric = 25;
	link.local_address[0] = 0x20;
	link.local_address[1] = 0x01;
	link.local_address[2] = 0x0d;
	link.local_address[3] = 0xb8;
	link.local_address[15] = 1;
	link.remote_address[0] = 0x20;
	link.remote_address[1] = 0x01;
	link.remote_address[2] = 0x0d;
	link.remote_address[3] = 0xb8;
	link.remote_address[15] = 2;
	assert(midr_wire_encode_object(&link, payload, sizeof(payload),
				       &payload_len) == 0);
	assert(midr_wire_decode_object(payload, payload_len, &decoded) == 0);
	assert(decoded.address_family == MIDR_CORE_AF_IPV6);
	assert(midr_core_object_semantic_equal(&link, &decoded));
	link.address_family = MIDR_CORE_AF_IPV4;
	memset(link.local_address + 4, 0, sizeof(link.local_address) - 4);
	memset(link.remote_address + 4, 0, sizeof(link.remote_address) - 4);
	assert(midr_wire_encode_object(&link, payload, sizeof(payload),
				       &payload_len) == 0);
	payload[64] = 1;
	assert(midr_wire_decode_object(payload, payload_len, &decoded) == -EINVAL);
	link.address_family = MIDR_CORE_AF_IPV6;
	link.metric = 0;
	assert(midr_wire_encode_object(&link, payload, sizeof(payload),
				       &payload_len) == -EINVAL);
	puts("midrd-wire-test: PASS");
	return 0;
}
