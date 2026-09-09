// SPDX-License-Identifier: GPL-2.0-or-later
#include <zebra.h>
#include <errno.h>

#include "bgpd/bgp_midr_canonical.h"
#include "bgpd/bgp_midr_codec.h"

struct fixture {
	uint64_t now;
	size_t calls;
	size_t fail_at;
	size_t live;
};

static uint64_t now_ns(void *arg)
{
	return ((struct fixture *)arg)->now;
}

static void *allocate(size_t size, void *arg)
{
	struct fixture *f = arg;
	void *p;

	if (++f->calls == f->fail_at)
		return NULL;
	p = calloc(1, size);
	assert(p);
	f->live++;
	return p;
}

static void deallocate(void *p, void *arg)
{
	struct fixture *f = arg;

	if (p) {
		assert(f->live);
		f->live--;
		free(p);
	}
}

static struct midr_canonical *create(struct fixture *f, size_t ids, size_t events)
{
	struct midr_canonical *store = NULL;
	struct midr_canonical_config c = {
		.identity_limit = ids,
		.event_limit = events,
		.max_age_ms = 1000,
		.now_ns = now_ns,
		.clock_arg = f,
		.alloc = allocate,
		.free = deallocate,
		.alloc_arg = f,
	};

	assert(midr_canonical_create(&c, &store) == 0);
	return store;
}

static struct midr_instance sample(enum midr_nlri_type type, bool ipv6)
{
	struct midr_instance i = {
		.state = MIDR_INSTANCE_ACTIVE,
		.object.key.type = type,
		.object.key.originator_node_id = htonl(1),
		.object.ls_sequence = 100,
	};
	struct midr_ls_prefix_key *p;

	switch (type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		i.object.payload.membership.group_id = 1;
		break;
	case MIDR_NLRI_TYPE_LINK:
		i.object.key.u.link.remote_node_id = htonl(2);
		i.object.key.u.link.link_id = 7;
		i.object.payload.link.canonical_cost = 123;
		i.object.payload.link.link_local_address.ipa_type = ipv6 ? IPADDR_V6 : IPADDR_V4;
		i.object.payload.link.link_remote_address.ipa_type = ipv6 ? IPADDR_V6 : IPADDR_V4;
		assert(inet_pton(ipv6 ? AF_INET6 : AF_INET, ipv6 ? "2001:db8::1" : "192.0.2.1",
				 i.object.payload.link.link_local_address.ip.addrbytes) == 1);
		assert(inet_pton(ipv6 ? AF_INET6 : AF_INET, ipv6 ? "2001:db8::2" : "192.0.2.2",
				 i.object.payload.link.link_remote_address.ip.addrbytes) == 1);
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
		p = type == MIDR_NLRI_TYPE_NODE_PREFIX ? &i.object.key.u.node_prefix
						      : &i.object.key.u.group_prefix.prefix;
		p->afi = ipv6 ? AFI_IP6 : AFI_IP;
		p->safi = SAFI_UNICAST;
		assert(str2prefix(ipv6 ? "2001:db8::/65" : "192.0.2.0/24", &p->prefix));
		if (type == MIDR_NLRI_TYPE_GROUP_PREFIX)
			i.object.key.u.group_prefix.group_id = 1;
		break;
	case MIDR_NLRI_TYPE_RESERVED:
	default:
		assert(0);
	}
	assert(midr_instance_validate(&i) == 0);
	return i;
}

static struct midr_instance withdraw(struct midr_instance i)
{
	i.state = MIDR_INSTANCE_WITHDRAWN;
	i.object.policy_tags = 0;
	memset(&i.object.payload, 0, sizeof(i.object.payload));
	return i;
}

static void expect_accept(struct midr_canonical *s, struct midr_instance *i,
		   uint32_t age, enum midr_canonical_result expected)
{
	enum midr_canonical_result result;

	assert(midr_canonical_accept(s, i, age, &result) == 0);
	assert(result == expected);
}

static void drain(struct midr_canonical *s)
{
	while (midr_canonical_event_peek(s))
		midr_canonical_event_ack(s);
}

static void test_age(void)
{
	uint32_t age;

	assert(midr_instance_age(10, 50, 50, 0, 1000, &age) == 0 && age == 10);
	assert(midr_instance_age(10, 50, 51, 3, 1000, &age) == 0 && age == 14);
	assert(midr_instance_age(10, 0, 1000000, 0, 1000, &age) == 0 && age == 11);
	assert(midr_instance_age(999, 0, 1, 0, 1000, &age) == 0 && age == 1000);
	assert(midr_instance_age(0, 0, UINT64_MAX, 0, UINT32_MAX, &age) == 0 && age == UINT32_MAX);
	assert(midr_instance_age(UINT32_MAX, 0, 0, UINT32_MAX, 1000, &age) == 0 && age == 1000);
	assert(midr_instance_age(0, 2, 1, 0, 1000, &age) == -ERANGE);
	assert(midr_instance_age(0, 0, 0, 0, 0, &age) == -EINVAL);
}

static void test_versions(void)
{
	struct fixture f = {};
	struct midr_canonical *s = create(&f, 4, 16);
	struct midr_instance i = sample(MIDR_NLRI_TYPE_LINK, true), old = i;
	struct midr_canonical_view view;
	const struct midr_canonical_event *event;
	const struct midr_instance_ref *held;

	expect_accept(s, &i, 100, MIDR_CANONICAL_ACCEPTED);
	assert(midr_canonical_identity_count(s) == 1);
	assert(midr_canonical_event_count(s) == 1);
	event = midr_canonical_event_peek(s);
	assert(midr_canonical_event_change(event) == MIDR_CANONICAL_REPLACE);
	assert(midr_canonical_event_sequence(event) == 100);
	assert(midr_ls_object_key_same(midr_canonical_event_key(event), &i.object.key));
	assert(!midr_canonical_event_before(event));
	held = midr_canonical_event_after(event);
	assert(midr_instance_ref_acquire(held) == 0);
	drain(s);
	f.now = 200000000;
	expect_accept(s, &i, 0, MIDR_CANONICAL_DUPLICATE);
	size_t allocations = f.calls;

	for (unsigned int n = 0; n < 4096; n++)
		expect_accept(s, &i, 0, MIDR_CANONICAL_DUPLICATE);
	assert(f.calls == allocations);
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0);
	assert(view.age_ms == 300 && view.observed_ns == 0);
	assert(!midr_canonical_event_count(s));
	i.object.ls_sequence--;
	expect_accept(s, &i, 0, MIDR_CANONICAL_OLDER);
	i = withdraw(old);
	i.object.ls_sequence++;
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	assert(midr_instance_ref_value(held)->state == MIDR_INSTANCE_ACTIVE);
	assert(midr_canonical_event_before(midr_canonical_event_peek(s)) == held);
	expect_accept(s, &old, 0, MIDR_CANONICAL_OLDER);
	drain(s);
	f.now = 400000000;
	expect_accept(s, &i, 0, MIDR_CANONICAL_DUPLICATE);
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0 && view.age_ms == 200);
	f.now = 1200000000;
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0);
	assert(view.state == MIDR_CANONICAL_FLOOR && !view.current);
	expect_accept(s, &i, 0, MIDR_CANONICAL_EXPIRED);
	assert(midr_canonical_expire(s, &i.object.key) == 0);
	assert(midr_canonical_event_change(midr_canonical_event_peek(s)) == MIDR_CANONICAL_EXPIRE);
	drain(s);
	assert(midr_canonical_expire(s, &i.object.key) == 0);
	assert(!midr_canonical_event_count(s));
	assert(midr_canonical_identity_count(s) == 1);
	i = old;
	i.object.ls_sequence += 2;
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	midr_canonical_destroy(&s);
	assert(f.live == 1);
	assert(midr_instance_ref_value(held)->object.ls_sequence == 100);
	midr_instance_ref_release(&held);
	assert(!f.live);
}

static void test_conflict(void)
{
	struct fixture f = {};
	struct midr_canonical *s = create(&f, 1, 16);
	struct midr_instance i = sample(MIDR_NLRI_TYPE_MEMBERSHIP, false);
	struct midr_instance original = i;
	struct midr_canonical_view view;

	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	drain(s);
	f.now = 500000000;
	i.object.payload.membership.group_id = 2;
	expect_accept(s, &i, 0, MIDR_CANONICAL_CONFLICT);
	assert(midr_canonical_event_change(midr_canonical_event_peek(s)) == MIDR_CANONICAL_ISOLATE);
	assert(!midr_canonical_event_after(midr_canonical_event_peek(s)));
	drain(s);
	expect_accept(s, &original, 0, MIDR_CANONICAL_CONFLICT);
	assert(!midr_canonical_event_count(s));
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0);
	assert(view.state == MIDR_CANONICAL_QUARANTINED && !view.current && view.observed_ns == 0);
	f.now = 1000000000;
	assert(midr_canonical_expire(s, &i.object.key) == 0);
	expect_accept(s, &original, 0, MIDR_CANONICAL_EXPIRED);
	i.object.ls_sequence++;
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0 && view.current);
	midr_canonical_destroy(&s);
	assert(!f.live);
}

static void test_failure_and_limits(void)
{
	for (size_t fail = 1; fail <= 3; fail++) {
		struct fixture f = {};
		struct midr_canonical *s = create(&f, 1, 2);
		struct midr_instance i = sample(MIDR_NLRI_TYPE_NODE_PREFIX, false);
		enum midr_canonical_result result;
		struct midr_canonical_view view;

		f.fail_at = f.calls + fail;
		assert(midr_canonical_accept(s, &i, 0, &result) == -ENOMEM);
		assert(!midr_canonical_identity_count(s) && !midr_canonical_event_count(s));
		assert(midr_canonical_lookup(s, &i.object.key, &view) == -ENOENT && f.live == 1);
		f.fail_at = 0;
		expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
		drain(s);
		i.object.ls_sequence++;
		for (size_t update_fail = 1; update_fail <= 2; update_fail++) {
			f.fail_at = f.calls + update_fail;
			assert(midr_canonical_accept(s, &i, 0, &result) == -ENOMEM);
			assert(midr_canonical_lookup(s, &i.object.key, &view) == 0 && view.sequence == 100);
			assert(!midr_canonical_event_count(s));
		}
		f.fail_at = 0;
		i = withdraw(i);
		expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED); /* existing key at identity limit */
		i.object.ls_sequence++;
		expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
		i.object.ls_sequence++;
		assert(midr_canonical_accept(s, &i, 0, &result) == -ENOSPC);
		drain(s);
		i.object.key.originator_node_id = htonl(2);
		assert(midr_canonical_accept(s, &i, 0, &result) == -ENOSPC);
		i.object.ls_sequence = 0;
		assert(midr_canonical_accept(s, &i, 0, &result) == -EINVAL);
		midr_canonical_destroy(&s);
		assert(!f.live);
	}
}

static void test_families(void)
{
	struct fixture f = {};
	struct midr_canonical *s = create(&f, 16, 32);
	struct midr_canonical_view view;

	for (int type = MIDR_NLRI_TYPE_MEMBERSHIP; type <= MIDR_NLRI_TYPE_GROUP_PREFIX; type++) {
		struct midr_instance i = withdraw(sample(type, false));

		expect_accept(s, &i, 1000, MIDR_CANONICAL_EXPIRED);
		expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED); /* unknown withdrawal */
	}
	assert(midr_canonical_identity_count(s) == 4);
	for (int type = MIDR_NLRI_TYPE_NODE_PREFIX; type <= MIDR_NLRI_TYPE_GROUP_PREFIX; type++) {
		struct midr_instance i = sample(type, true);

		expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
		assert(midr_canonical_lookup(s, &i.object.key, &view) == 0 && view.current);
	}
	assert(midr_canonical_identity_count(s) == 6);
	midr_canonical_destroy(&s);
	assert(!f.live);
}

static void roundtrip(struct midr_instance *i)
{
	struct stream *s = stream_new(1024);
	struct midr_instance decoded;
	struct midr_instance_attributes attributes;
	struct midr_ls_object_key key;

	assert(midr_nlri_encode(s, &i->object.key) == MIDR_CODEC_OK);
	assert(midr_nlri_decode(s, &key) == MIDR_CODEC_OK);
	assert(midr_ls_object_key_same(&key, &i->object.key));
	stream_reset(s);
	assert(midr_instance_attribute_encode(s, i, 123) == MIDR_CODEC_OK);
	for (size_t len = 0; len < stream_get_endp(s); len++) {
		/* Never install a partial object, even if its TLV prefix parses. */
		if (midr_instance_attribute_decode(s, len, &attributes) == MIDR_CODEC_OK)
			assert(midr_instance_from_wire(&key, &attributes, &decoded) != MIDR_CODEC_OK);
		stream_set_getp(s, 0);
	}
	assert(midr_instance_attribute_decode(s, stream_get_endp(s), &attributes) == MIDR_CODEC_OK);
	assert(attributes.age_ms == 123);
	assert(midr_instance_from_wire(&key, &attributes, &decoded) == MIDR_CODEC_OK);
	assert(midr_instance_same(i, &decoded));
	stream_free(s);
}

static void test_expiry_failure_and_history(void)
{
	struct fixture f = {.now = 1};
	struct midr_canonical *s = create(&f, 8, 2);
	struct midr_instance i = sample(MIDR_NLRI_TYPE_NODE_PREFIX, true);
	struct midr_canonical_view view;
	const struct midr_instance_ref *w;
	enum midr_canonical_result result;
	uint32_t age;

	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	f.now = 0;
	assert(midr_canonical_accept(s, &i, 0, &result) == -ERANGE);
	f.now = 1;
	assert(midr_canonical_expire(s, &i.object.key) == -EAGAIN);
	i.object.ls_sequence++;
	i = withdraw(i);
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	f.now += 1000000000;
	assert(midr_canonical_expire(s, &i.object.key) == -ENOSPC);
	assert(midr_canonical_lookup(s, &i.object.key, &view) == 0 && !view.current);
	drain(s);
	f.fail_at = f.calls + 1;
	assert(midr_canonical_expire(s, &i.object.key) == -ENOMEM);
	assert(!midr_canonical_event_count(s));
	f.fail_at = 0;
	assert(midr_canonical_expire(s, &i.object.key) == 0);
	drain(s);
	i.object.ls_sequence++;
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	w = midr_canonical_event_after(midr_canonical_event_peek(s));
	assert(midr_instance_ref_acquire(w) == 0);
	drain(s);
	i = sample(MIDR_NLRI_TYPE_NODE_PREFIX, true);
	i.object.ls_sequence = 103;
	expect_accept(s, &i, 0, MIDR_CANONICAL_ACCEPTED);
	assert(midr_instance_ref_value(midr_canonical_event_before(midr_canonical_event_peek(s)))->state == MIDR_INSTANCE_WITHDRAWN);
	drain(s);
	assert(midr_instance_ref_age(w, f.now + 123000000, 5, 1000, &age) == 0 && age == 128);
	midr_canonical_destroy(&s);
	assert(f.live == 1);
	midr_instance_ref_release(&w);
	assert(!f.live);
}

static void test_codec(void)
{
	for (int type = MIDR_NLRI_TYPE_MEMBERSHIP; type <= MIDR_NLRI_TYPE_GROUP_PREFIX; type++)
		for (int family = 0; family < 2; family++) {
			struct midr_instance i = sample(type, family);

			i.object.policy_tags = 7;
			roundtrip(&i);
			i = withdraw(i);
			roundtrip(&i);
		}
	for (int len = 0; len <= 128; len += 64) {
		struct midr_instance i = sample(MIDR_NLRI_TYPE_NODE_PREFIX, true);

		i.object.key.u.node_prefix.prefix.prefixlen = len;
		apply_mask(&i.object.key.u.node_prefix.prefix);
		roundtrip(&i);
	}
	struct midr_instance i = sample(MIDR_NLRI_TYPE_LINK, true);
	struct midr_instance decoded;
	struct midr_instance_attributes attributes;
	struct stream *s = stream_new(1024), *small = stream_new(1);

	assert(midr_instance_attribute_encode(small, &i, 0) == MIDR_CODEC_NO_SPACE);
	assert(!stream_get_endp(small));
	stream_free(small);
	i = withdraw(i);
	const uint8_t golden[] = {0, 1, 0, 8, 0, 0, 0, 0, 0, 0, 0, 100,
				  0, 4, 0, 1, 2, 0, 5, 0, 4, 0, 0, 0, 123};

	assert(midr_instance_attribute_encode(s, &i, 123) == MIDR_CODEC_OK);
	assert(stream_get_endp(s) == sizeof(golden));
	assert(memcmp(STREAM_DATA(s), golden, sizeof(golden)) == 0);
	stream_reset(s);
	assert(midr_instance_attribute_encode(s, &i, UINT32_MAX) == MIDR_CODEC_OK);
	assert(midr_instance_attribute_decode(s, stream_get_endp(s), &attributes) == MIDR_CODEC_OK);
	assert(stream_get_getp(s) == stream_get_endp(s));
	/* sequence TLV is 12 bytes, state TLV is 5, age TLV is 8. */
	assert(stream_get_endp(s) == 25);
	stream_putc_at(s, 16, 0);
	stream_set_getp(s, 0);
	assert(midr_instance_attribute_decode(s, 25, &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_putc_at(s, 16, MIDR_INSTANCE_WITHDRAWN);
	stream_putw_at(s, 19, 3);
	stream_set_getp(s, 0);
	assert(midr_instance_attribute_decode(s, 25, &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_putw_at(s, 19, 4);
	stream_putw_at(s, 17, MIDR_INSTANCE_TLV_STATE);
	stream_set_getp(s, 0);
	assert(midr_instance_attribute_decode(s, 25, &attributes) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	stream_putw_at(s, 17, 999);
	stream_set_getp(s, 0);
	assert(midr_instance_attribute_decode(s, 25, &attributes) == MIDR_CODEC_UNKNOWN_TLV);
	stream_putw_at(s, 17, MIDR_INSTANCE_TLV_AGE);
	stream_set_getp(s, 0);
	assert(midr_instance_attribute_decode(s, 25, &attributes) == MIDR_CODEC_OK);
	assert(attributes.age_ms == UINT32_MAX);
	attributes.ls.present |= MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
	assert(midr_instance_from_wire(&i.object.key, &attributes, &decoded) == MIDR_CODEC_MALFORMED_ATTRIBUTE);
	i.object.policy_tags = 1;
	assert(midr_instance_validate(&i) == -EINVAL);
	i.object.policy_tags = 0;
	i.object.payload.link.canonical_cost = 1;
	assert(midr_instance_validate(&i) == -EINVAL);
	stream_free(s);
}

int main(void)
{
	test_age();
	test_versions();
	test_conflict();
	test_failure_and_limits();
	test_families();
	test_codec();
	test_expiry_failure_and_history();
	puts("MIDR instance/canonical tests passed");
	return 0;
}
