// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef _FRR_BGP_MIDR_INSTANCE_H
#define _FRR_BGP_MIDR_INSTANCE_H

#include "bgpd/bgp_midr_ls.h"

enum midr_instance_state {
	MIDR_INSTANCE_ACTIVE = 1,
	MIDR_INSTANCE_WITHDRAWN = 2,
};

/* Age belongs to the transport envelope, not to semantic equality. */
struct midr_instance {
	struct midr_ls_object object;
	enum midr_instance_state state;
};

extern int midr_instance_validate(const struct midr_instance *instance);
extern bool midr_instance_same(const struct midr_instance *a,
			      const struct midr_instance *b);
extern int midr_instance_age(uint32_t received_ms, uint64_t received_ns,
			     uint64_t now_ns, uint32_t budget_ms,
			     uint32_t max_age_ms, uint32_t *age_ms);

#endif
