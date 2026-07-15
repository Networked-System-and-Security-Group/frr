// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR local IP-to-ASN snapshot lookup.
 */

#ifndef _FRR_MIDR_IP2ASN_H
#define _FRR_MIDR_IP2ASN_H

#include <stdbool.h>
#include <stddef.h>

#include "asn.h"
#include "prefix.h"

struct vty;

extern int midr_ip2asn_load_file(const char *path, char *errmsg,
				 size_t errmsg_len);
extern void midr_ip2asn_clear(void);
extern bool midr_ip2asn_is_loaded(void);
extern const char *midr_ip2asn_source_path(void);
extern unsigned long midr_ip2asn_entry_count(void);
extern bool midr_ip2asn_lookup(const struct prefix *addr, as_t *asn,
			       struct prefix *matched_prefix);
extern int midr_ip2asn_config_write(struct vty *vty);

#endif /* _FRR_MIDR_IP2ASN_H */
