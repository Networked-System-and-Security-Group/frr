// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Test-only JSON provider for MIDR TED snapshots.
 */

#ifndef _FRR_TEST_MIDR_TED_MOCK_PROVIDER_H
#define _FRR_TEST_MIDR_TED_MOCK_PROVIDER_H

#include <stddef.h>

struct midr_context;

extern int midr_ted_mock_provider_publish_file(struct midr_context *ctx, const char *path,
					       char *error, size_t error_size);

#endif /* _FRR_TEST_MIDR_TED_MOCK_PROVIDER_H */
