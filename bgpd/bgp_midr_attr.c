// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR path-attribute containers and parser adaptation.
 */

#include <zebra.h>

#include <assert.h>
#include <string.h>

#include "hash.h"
#include "jhash.h"
#include "memory.h"
#include "stream.h"

#include "bgpd/bgp_midr_attr.h"
#include "bgpd/bgp_route.h"

DEFINE_MTYPE_STATIC(BGPD, MIDR_LS_ATTR, "MIDR link-state attribute");

struct bgp_midr_ls_attr {
	unsigned long refcnt;
	struct midr_ls_attributes attributes;
	enum midr_instance_state state;
	uint32_t age_ms;
};

static struct hash *midr_ls_attr_hash;

static unsigned int
midr_ls_attributes_hash(const struct midr_ls_attributes *attributes)
{
	uint32_t key = 0;

#define MIDR_MIX(value) key = jhash_1word((value), key)
	MIDR_MIX(attributes->present);
	MIDR_MIX((uint32_t)(attributes->ls_sequence >> 32));
	MIDR_MIX((uint32_t)attributes->ls_sequence);
	MIDR_MIX((uint32_t)(attributes->policy_tags >> 32));
	MIDR_MIX((uint32_t)attributes->policy_tags);
	MIDR_MIX(attributes->group_id);
	MIDR_MIX(ipaddr_hash(&attributes->transport_address));
	MIDR_MIX((uint32_t)(attributes->cap_flags >> 32));
	MIDR_MIX((uint32_t)attributes->cap_flags);
	MIDR_MIX(ipaddr_hash(&attributes->link_local_address));
	MIDR_MIX(ipaddr_hash(&attributes->link_remote_address));
	MIDR_MIX(attributes->link_canonical_cost);
#undef MIDR_MIX
	return key;
}

static unsigned int
midr_instance_hash(const struct midr_ls_attributes *attributes,
			   enum midr_instance_state state)
{
	return jhash_1word(state, midr_ls_attributes_hash(attributes));
}

static bool
midr_ls_attributes_same(const struct midr_ls_attributes *a,
			const struct midr_ls_attributes *b)
{
	return a->present == b->present && a->ls_sequence == b->ls_sequence
	       && a->policy_tags == b->policy_tags
	       && a->group_id == b->group_id
	       && ipaddr_is_same(&a->transport_address,
				 &b->transport_address)
	       && a->cap_flags == b->cap_flags
	       && ipaddr_is_same(&a->link_local_address,
				 &b->link_local_address)
	       && ipaddr_is_same(&a->link_remote_address,
				 &b->link_remote_address)
	       && a->link_canonical_cost == b->link_canonical_cost;
}

unsigned int
bgp_midr_ls_attr_hash_key(const struct bgp_midr_ls_attr *attr)
{
	return attr ? midr_instance_hash(&attr->attributes, attr->state) : 0;
}

bool bgp_midr_ls_attr_same(const struct bgp_midr_ls_attr *a,
			   const struct bgp_midr_ls_attr *b)
{
	if (a == b)
		return true;
	if (!a || !b)
		return false;
	return a->state == b->state &&
	       midr_ls_attributes_same(&a->attributes, &b->attributes);
}

static unsigned int midr_ls_attr_hash_key(const void *arg)
{
	return bgp_midr_ls_attr_hash_key(arg);
}

static bool midr_ls_attr_hash_cmp(const void *a, const void *b)
{
	return bgp_midr_ls_attr_same(a, b);
}

static void *midr_ls_attr_hash_alloc(void *arg)
{
	const struct bgp_midr_ls_attr *source = arg;
	struct bgp_midr_ls_attr *attr;

	attr = XCALLOC(MTYPE_MIDR_LS_ATTR, sizeof(*attr));
	attr->attributes = source->attributes;
	attr->state = source->state;
	attr->age_ms = source->age_ms;
	return attr;
}

static void
midr_instance_attributes_from_object(const struct midr_instance *instance,
				     struct midr_ls_attributes *attributes)
{
	const struct midr_ls_object *object = &instance->object;

	memset(attributes, 0, sizeof(*attributes));
	attributes->present = MIDR_LS_ATTR_HAS_SEQUENCE;
	attributes->ls_sequence = object->ls_sequence;
	if (instance->state == MIDR_INSTANCE_WITHDRAWN)
		return;
	if (object->policy_tags) {
		attributes->present |= MIDR_LS_ATTR_HAS_POLICY_TAGS;
		attributes->policy_tags = object->policy_tags;
	}

	switch (object->key.type) {
	case MIDR_NLRI_TYPE_MEMBERSHIP:
		attributes->present |= MIDR_LS_ATTR_HAS_GROUP_ID |
				       MIDR_LS_ATTR_HAS_CAP_FLAGS;
		attributes->group_id = object->payload.membership.group_id;
		attributes->cap_flags = object->payload.membership.cap_flags;
		if (object->payload.membership.has_transport_address) {
			attributes->present |= MIDR_LS_ATTR_HAS_TRANSPORT_ADDRESS;
			attributes->transport_address =
				object->payload.membership.transport_address;
		}
		break;
	case MIDR_NLRI_TYPE_LINK:
		attributes->present |= MIDR_LS_ATTR_HAS_LINK_LOCAL_ADDRESS |
				       MIDR_LS_ATTR_HAS_LINK_REMOTE_ADDRESS |
				       MIDR_LS_ATTR_HAS_LINK_CANONICAL_COST;
		attributes->link_local_address = object->payload.link.link_local_address;
		attributes->link_remote_address = object->payload.link.link_remote_address;
		attributes->link_canonical_cost = object->payload.link.canonical_cost;
		break;
	case MIDR_NLRI_TYPE_NODE_PREFIX:
	case MIDR_NLRI_TYPE_GROUP_PREFIX:
	case MIDR_NLRI_TYPE_RESERVED:
		break;
	}
}

static struct bgp_midr_ls_attr *
bgp_midr_ls_attr_alloc(const struct midr_ls_attributes *attributes,
			       enum midr_instance_state state, uint32_t age_ms)
{
	struct bgp_midr_ls_attr *attr;

	if (!attributes || (state != MIDR_INSTANCE_ACTIVE &&
			    state != MIDR_INSTANCE_WITHDRAWN))
		return NULL;
	attr = XCALLOC(MTYPE_MIDR_LS_ATTR, sizeof(*attr));
	attr->attributes = *attributes;
	attr->state = state;
	attr->age_ms = age_ms;
	return attr;
}

struct bgp_midr_ls_attr *
bgp_midr_instance_attr_new(const struct midr_instance *instance,
				   uint32_t age_ms)
{
	if (!instance || midr_instance_validate(instance))
		return NULL;
	{
		struct midr_ls_attributes attributes = {};

		midr_instance_attributes_from_object(instance, &attributes);
		return bgp_midr_ls_attr_alloc(&attributes, instance->state, age_ms);
	}
}

struct bgp_midr_ls_attr *
bgp_midr_instance_attr_intern(const struct midr_instance *instance,
				       uint32_t age_ms)
{
	struct bgp_midr_ls_attr *fresh;
	struct bgp_midr_ls_attr *interned;

	fresh = bgp_midr_instance_attr_new(instance, age_ms);
	if (!fresh || !midr_ls_attr_hash) {
		if (fresh)
			XFREE(MTYPE_MIDR_LS_ATTR, fresh);
		return NULL;
	}
	interned = hash_get(midr_ls_attr_hash, fresh, midr_ls_attr_hash_alloc);
	interned->refcnt++;
	XFREE(MTYPE_MIDR_LS_ATTR, fresh);
	return interned;
}

struct bgp_midr_ls_attr *
bgp_midr_instance_attr_intern_attributes(
	const struct midr_instance_attributes *attributes)
{
	struct bgp_midr_ls_attr *fresh;
	struct bgp_midr_ls_attr *interned;

	if (!attributes ||
	    (attributes->state != MIDR_INSTANCE_ACTIVE &&
	     attributes->state != MIDR_INSTANCE_WITHDRAWN) ||
	    !attributes->ls.ls_sequence || !midr_ls_attr_hash)
		return NULL;
	fresh = bgp_midr_ls_attr_alloc(&attributes->ls, attributes->state,
					      attributes->age_ms);
	if (!fresh)
		return NULL;
	interned = hash_get(midr_ls_attr_hash, fresh, midr_ls_attr_hash_alloc);
	interned->refcnt++;
	XFREE(MTYPE_MIDR_LS_ATTR, fresh);
	return interned;
}

void bgp_midr_ls_attr_intern_ref(struct bgp_midr_ls_attr **attrp)
{
	struct bgp_midr_ls_attr *attr;
	struct bgp_midr_ls_attr *interned;

	if (!attrp || !*attrp)
		return;
	attr = *attrp;
	if (attr->refcnt) {
		bgp_midr_ls_attr_lock(attr);
		return;
	}
	interned =
		hash_get(midr_ls_attr_hash, attr, midr_ls_attr_hash_alloc);
	interned->refcnt++;
	XFREE(MTYPE_MIDR_LS_ATTR, attr);
	*attrp = interned;
}

void bgp_midr_ls_attr_lock(struct bgp_midr_ls_attr *attr)
{
	assert(attr && attr->refcnt);
	attr->refcnt++;
}

void bgp_midr_ls_attr_unintern(struct bgp_midr_ls_attr **attrp)
{
	struct bgp_midr_ls_attr *attr;

	if (!attrp || !*attrp)
		return;
	attr = *attrp;
	assert(attr->refcnt);
	attr->refcnt--;
	if (!attr->refcnt) {
		assert(hash_release(midr_ls_attr_hash, attr) == attr);
		XFREE(MTYPE_MIDR_LS_ATTR, attr);
	}
	*attrp = NULL;
}

void bgp_midr_ls_attr_flush(struct bgp_midr_ls_attr **attrp)
{
	struct bgp_midr_ls_attr *attr;

	if (!attrp || !*attrp)
		return;
	attr = *attrp;
	if (!attr->refcnt)
		XFREE(MTYPE_MIDR_LS_ATTR, attr);
	*attrp = NULL;
}

const struct midr_ls_attributes *
bgp_midr_ls_attr_value(const struct bgp_midr_ls_attr *attr)
{
	return attr ? &attr->attributes : NULL;
}

enum midr_instance_state
bgp_midr_ls_attr_state(const struct bgp_midr_ls_attr *attr)
{
	return attr ? attr->state : 0;
}

uint32_t bgp_midr_ls_attr_age(const struct bgp_midr_ls_attr *attr)
{
	return attr ? attr->age_ms : 0;
}

static void midr_ls_attr_hash_free(void *arg)
{
	XFREE(MTYPE_MIDR_LS_ATTR, arg);
}

void bgp_midr_attr_init(void)
{
	assert(!midr_ls_attr_hash);
	midr_ls_attr_hash =
		hash_create(midr_ls_attr_hash_key, midr_ls_attr_hash_cmp,
			    "MIDR link-state attributes");
}

void bgp_midr_attr_finish(void)
{
	hash_clean_and_free(&midr_ls_attr_hash, midr_ls_attr_hash_free);
}

enum bgp_attr_parse_ret
bgp_midr_attr_codec_result(enum midr_codec_result result)
{
	switch (result) {
	case MIDR_CODEC_OK:
		return BGP_ATTR_PARSE_PROCEED;
	case MIDR_CODEC_UNKNOWN_NLRI_TYPE:
	case MIDR_CODEC_UNKNOWN_TLV:
	case MIDR_CODEC_MALFORMED_NLRI:
	case MIDR_CODEC_MALFORMED_ATTRIBUTE:
	case MIDR_CODEC_NO_SPACE:
	case MIDR_CODEC_OVERFLOW:
		return BGP_ATTR_PARSE_WITHDRAW;
	}
	return BGP_ATTR_PARSE_WITHDRAW;
}

int bgp_midr_nlri_codec_result(enum midr_codec_result result)
{
	switch (result) {
	case MIDR_CODEC_OK:
	case MIDR_CODEC_UNKNOWN_NLRI_TYPE:
		return BGP_NLRI_PARSE_OK;
	case MIDR_CODEC_UNKNOWN_TLV:
	case MIDR_CODEC_MALFORMED_NLRI:
	case MIDR_CODEC_MALFORMED_ATTRIBUTE:
	case MIDR_CODEC_NO_SPACE:
	case MIDR_CODEC_OVERFLOW:
		return BGP_NLRI_PARSE_ERROR;
	}
	return BGP_NLRI_PARSE_ERROR;
}

bool bgp_midr_attr_family_is_midr(bool has_mp_reach, afi_t afi,
				  safi_t safi)
{
	return has_mp_reach && afi == AFI_BGP_LS && safi == SAFI_MIDR_LS;
}

enum bgp_attr_parse_ret
bgp_midr_attr_decode(struct attr *attr, uint8_t type, uint8_t flags,
		     const uint8_t *value, size_t length)
{
	const uint8_t required_flags =
		BGP_ATTR_FLAG_OPTIONAL | BGP_ATTR_FLAG_EXTLEN;
	enum midr_codec_result result;
	struct stream *stream;

	if (!attr || !value || flags != required_flags)
		return BGP_ATTR_PARSE_WITHDRAW;

	stream = stream_new(length ? length : 1);
	stream_put(stream, value, length);
	stream_set_getp(stream, 0);

	if (type == BGP_ATTR_MIDR_LS) {
		struct midr_instance_attributes attributes = {};

		result = midr_instance_attribute_decode(stream, length,
						  &attributes);
		if (result == MIDR_CODEC_OK) {
			attr->midr_ls =
				bgp_midr_instance_attr_intern_attributes(&attributes);
			if (!attr->midr_ls)
				result = MIDR_CODEC_NO_SPACE;
		}
	} else {
		result = MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}

	stream_free(stream);
	return bgp_midr_attr_codec_result(result);
}
