// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local IP-to-ASN snapshot lookup.
 */

#ifndef _FRR_MIDR_IP2ASN_H
#define _FRR_MIDR_IP2ASN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include "asn.h"
#include "prefix.h"

struct vty;

#define MIDR_IP2ASN_UPDATE_ID_MAX 64

/*
 * All APIs in this header are bgpd main-event-thread only.  The implementation
 * is intentionally lock-free; worker/helper code must not access its manager
 * or route tables.  Borrowed status/path pointers become invalid after the
 * next successful load, clear, or update.
 */

enum midr_ip2asn_replace_flags {
	MIDR_IP2ASN_REPLACE_NONE = 0,
	MIDR_IP2ASN_REPLACE_DISCARD_DIRTY = 1U << 0,
};

enum midr_ip2asn_reset_kind {
	MIDR_IP2ASN_RESET_NONE,
	MIDR_IP2ASN_RESET_FULL_LOAD_DISCARD,
	MIDR_IP2ASN_RESET_CLEAR_DISCARD,
};

struct midr_ip2asn_load_result {
	uint64_t old_generation;
	uint64_t new_generation;
	unsigned long entries;
	bool discarded_runtime_updates;
};

struct midr_ip2asn_update_result {
	char update_id[MIDR_IP2ASN_UPDATE_ID_MAX + 1];
	uint64_t base_generation;
	/* Actual generation after the call; unchanged for validate-only. */
	uint64_t new_generation;
	unsigned long added;
	unsigned long replaced;
	unsigned long deleted;
	unsigned long entries;
	bool validate_only;
};

/*
 * Path and ID pointers refer to manager-owned storage.  Callers must not retain
 * them across a load, clear, or update operation.
 */
struct midr_ip2asn_status {
	bool loaded;
	unsigned long entries;
	const char *source_path;
	bool dirty;
	uint64_t generation;
	const char *last_update_path;
	const char *last_update_id;
	struct timeval last_update_time;
	uint64_t process_updates_applied_total;
	unsigned long last_added;
	unsigned long last_replaced;
	unsigned long last_deleted;
	bool last_reset_valid;
	enum midr_ip2asn_reset_kind last_reset_kind;
	uint64_t last_reset_old_generation;
	const char *last_reset_discarded_update_id;
	struct timeval last_reset_time;
};

/*
 * Backward-compatible entry points.  They never implicitly discard dirty
 * runtime updates.  In particular, the legacy void clear is a safe no-op when
 * dirty; callers that need error reporting or explicit discard must use
 * midr_ip2asn_clear_ex().
 */
extern int midr_ip2asn_load_file(const char *path, char *errmsg,
				 size_t errmsg_len);
extern void midr_ip2asn_clear(void);

extern int midr_ip2asn_load_file_ex(const char *path, unsigned int flags,
				    struct midr_ip2asn_load_result *result,
				    char *errmsg, size_t errmsg_len);
extern int midr_ip2asn_clear_ex(unsigned int flags, char *errmsg,
			       size_t errmsg_len);
extern int midr_ip2asn_update_file(const char *path, bool validate_only,
				   struct midr_ip2asn_update_result *result,
				   char *errmsg, size_t errmsg_len);

extern bool midr_ip2asn_is_loaded(void);
extern const char *midr_ip2asn_source_path(void);
extern unsigned long midr_ip2asn_entry_count(void);
extern uint64_t midr_ip2asn_generation(void);
extern bool midr_ip2asn_is_dirty(void);
extern void midr_ip2asn_get_status(struct midr_ip2asn_status *status);
extern const char *
midr_ip2asn_reset_kind_name(enum midr_ip2asn_reset_kind kind);

extern bool midr_ip2asn_lookup(const struct prefix *addr, as_t *asn,
			       struct prefix *matched_prefix);
/* Exact lookup rejects non-canonical prefixes with host bits set. */
extern bool midr_ip2asn_lookup_exact(const struct prefix *prefix, as_t *asn);
extern int midr_ip2asn_config_write(struct vty *vty);

#endif /* _FRR_MIDR_IP2ASN_H */
