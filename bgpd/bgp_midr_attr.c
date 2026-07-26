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
DEFINE_MTYPE_STATIC(BGPD, MIDR_PROPAGATION_PATH_ATTR,
		    "MIDR propagation path attribute");

struct bgp_midr_ls_attr {
	unsigned long refcnt;
	struct midr_ls_attributes attributes;
};

struct bgp_midr_propagation_path_attr {
	unsigned long refcnt;
	struct midr_propagation_path path;
};

static struct hash *midr_ls_attr_hash;
static struct hash *midr_propagation_path_attr_hash;

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
	MIDR_MIX(attributes->link_metrics.present_flags);
	MIDR_MIX(attributes->link_metrics.rtt_us);
	MIDR_MIX(attributes->link_metrics.loss_ppm);
	MIDR_MIX(attributes->link_metrics.available_bandwidth_kbps);
#undef MIDR_MIX
	return key;
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
	       && a->link_metrics.present_flags
			  == b->link_metrics.present_flags
	       && a->link_metrics.rtt_us == b->link_metrics.rtt_us
	       && a->link_metrics.loss_ppm == b->link_metrics.loss_ppm
	       && a->link_metrics.available_bandwidth_kbps
			  == b->link_metrics.available_bandwidth_kbps;
}

unsigned int
bgp_midr_ls_attr_hash_key(const struct bgp_midr_ls_attr *attr)
{
	return attr ? midr_ls_attributes_hash(&attr->attributes) : 0;
}

bool bgp_midr_ls_attr_same(const struct bgp_midr_ls_attr *a,
			   const struct bgp_midr_ls_attr *b)
{
	if (a == b)
		return true;
	if (!a || !b)
		return false;
	return midr_ls_attributes_same(&a->attributes, &b->attributes);
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
	return attr;
}

struct bgp_midr_ls_attr *
bgp_midr_ls_attr_intern(const struct midr_ls_attributes *attributes)
{
	struct bgp_midr_ls_attr lookup = {};
	struct bgp_midr_ls_attr *attr;

	if (!attributes || !midr_ls_attr_hash)
		return NULL;
	lookup.attributes = *attributes;
	attr = hash_get(midr_ls_attr_hash, &lookup,
			midr_ls_attr_hash_alloc);
	attr->refcnt++;
	return attr;
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

const struct midr_ls_attributes *
bgp_midr_ls_attr_value(const struct bgp_midr_ls_attr *attr)
{
	return attr ? &attr->attributes : NULL;
}

unsigned int bgp_midr_propagation_path_attr_hash_key(
	const struct bgp_midr_propagation_path_attr *attr)
{
	if (!attr)
		return 0;
	return jhash(attr->path.nodes,
		     attr->path.node_count * sizeof(*attr->path.nodes),
		     attr->path.node_count);
}

bool bgp_midr_propagation_path_attr_same(
	const struct bgp_midr_propagation_path_attr *a,
	const struct bgp_midr_propagation_path_attr *b)
{
	if (a == b)
		return true;
	if (!a || !b || a->path.node_count != b->path.node_count)
		return false;
	return memcmp(a->path.nodes, b->path.nodes,
		      a->path.node_count * sizeof(*a->path.nodes))
	       == 0;
}

static unsigned int midr_propagation_path_attr_hash_key(const void *arg)
{
	return bgp_midr_propagation_path_attr_hash_key(arg);
}

static bool midr_propagation_path_attr_hash_cmp(const void *a,
						const void *b)
{
	return bgp_midr_propagation_path_attr_same(a, b);
}

static void *midr_propagation_path_attr_hash_alloc(void *arg)
{
	const struct bgp_midr_propagation_path_attr *source = arg;
	struct bgp_midr_propagation_path_attr *attr;
	size_t size;

	attr = XCALLOC(MTYPE_MIDR_PROPAGATION_PATH_ATTR, sizeof(*attr));
	size = source->path.node_count * sizeof(*source->path.nodes);
	attr->path.nodes = XMALLOC(MTYPE_MIDR_PROPAGATION_PATH_ATTR, size);
	memcpy(attr->path.nodes, source->path.nodes, size);
	attr->path.node_count = source->path.node_count;
	attr->path.capacity = source->path.node_count;
	return attr;
}

struct bgp_midr_propagation_path_attr *
bgp_midr_propagation_path_attr_intern(
	const struct midr_propagation_path *path)
{
	struct bgp_midr_propagation_path_attr lookup = {};
	struct bgp_midr_propagation_path_attr *attr;

	if (!path || !path->nodes || !path->node_count
	    || !midr_propagation_path_attr_hash)
		return NULL;
	lookup.path = *path;
	attr = hash_get(midr_propagation_path_attr_hash, &lookup,
			midr_propagation_path_attr_hash_alloc);
	attr->refcnt++;
	return attr;
}

void bgp_midr_propagation_path_attr_lock(
	struct bgp_midr_propagation_path_attr *attr)
{
	assert(attr && attr->refcnt);
	attr->refcnt++;
}

void bgp_midr_propagation_path_attr_unintern(
	struct bgp_midr_propagation_path_attr **attrp)
{
	struct bgp_midr_propagation_path_attr *attr;

	if (!attrp || !*attrp)
		return;
	attr = *attrp;
	assert(attr->refcnt);
	attr->refcnt--;
	if (!attr->refcnt) {
		assert(hash_release(midr_propagation_path_attr_hash, attr)
		       == attr);
		XFREE(MTYPE_MIDR_PROPAGATION_PATH_ATTR, attr->path.nodes);
		XFREE(MTYPE_MIDR_PROPAGATION_PATH_ATTR, attr);
	}
	*attrp = NULL;
}

const struct midr_propagation_path *
bgp_midr_propagation_path_attr_value(
	const struct bgp_midr_propagation_path_attr *attr)
{
	return attr ? &attr->path : NULL;
}

static void midr_ls_attr_hash_free(void *arg)
{
	XFREE(MTYPE_MIDR_LS_ATTR, arg);
}

static void midr_propagation_path_attr_hash_free(void *arg)
{
	struct bgp_midr_propagation_path_attr *attr = arg;

	XFREE(MTYPE_MIDR_PROPAGATION_PATH_ATTR, attr->path.nodes);
	XFREE(MTYPE_MIDR_PROPAGATION_PATH_ATTR, attr);
}

void bgp_midr_attr_init(void)
{
	assert(!midr_ls_attr_hash && !midr_propagation_path_attr_hash);
	midr_ls_attr_hash =
		hash_create(midr_ls_attr_hash_key, midr_ls_attr_hash_cmp,
			    "MIDR link-state attributes");
	midr_propagation_path_attr_hash = hash_create(
		midr_propagation_path_attr_hash_key,
		midr_propagation_path_attr_hash_cmp,
		"MIDR propagation path attributes");
}

void bgp_midr_attr_finish(void)
{
	hash_clean_and_free(&midr_ls_attr_hash, midr_ls_attr_hash_free);
	hash_clean_and_free(&midr_propagation_path_attr_hash,
			    midr_propagation_path_attr_hash_free);
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
		struct midr_ls_attributes attributes = {};

		result = midr_ls_attribute_decode(stream, length,
						  &attributes);
		if (result == MIDR_CODEC_OK)
			attr->midr_ls =
				bgp_midr_ls_attr_intern(&attributes);
	} else if (type == BGP_ATTR_MIDR_PROPAGATION_PATH) {
		struct midr_propagation_path path = {};

		result = midr_propagation_path_decode(stream, length, &path);
		if (result == MIDR_CODEC_OK) {
			attr->midr_propagation_path =
				bgp_midr_propagation_path_attr_intern(
					&path);
			midr_propagation_path_fini(&path);
		}
	} else {
		result = MIDR_CODEC_MALFORMED_ATTRIBUTE;
	}

	stream_free(stream);
	return bgp_midr_attr_codec_result(result);
}
