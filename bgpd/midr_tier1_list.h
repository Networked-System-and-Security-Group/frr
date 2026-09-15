// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_MIDR_TIER1_LIST_H
#define _FRR_MIDR_TIER1_LIST_H

#include "bgpd/midr_tier1.h"

#define MIDR_TIER1_LIST_MAX 4096U
#define MIDR_TIER1_LIST_FILE_MAX (128U * 1024U)
#define MIDR_TIER1_LIST_LINE_MAX 256U

struct vty;
struct midr_tier1_list_status {
	bool loaded;
	uint64_t generation;
	size_t count;
	size_t duplicates;
	const char *source_path;
	const char *version;
};

/* Main-event-thread only. active/status pointers are borrowed until the next
 * successful load/clear/fini. Never retain them in an asynchronous request.
 * validate_only checks the file without activating it or incrementing generation.
 */
int midr_tier1_list_load_file(const char *path, bool validate_only,
			    char *errmsg, size_t errmsg_len);
int midr_tier1_list_clear(char *errmsg, size_t errmsg_len);
const struct midr_tier1_list *midr_tier1_list_active(void);
uint64_t midr_tier1_list_generation(void);
void midr_tier1_list_get_status(struct midr_tier1_list_status *out);
int midr_tier1_list_config_write(struct vty *vty);
void midr_tier1_list_fini(void);
/* -2 means no configured list; output version and generation are owned values. */
int midr_tier1_active_path_check(const as_t *asns, size_t count,
				struct midr_tier1_result *result);

#endif
