/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_CONTEXT_H
#define MIDRD_CONTEXT_H

/*
 * Process-wide MIDR runtime.  The daemon owns the object; protocol modules
 * and the first/third-group adapters receive only this opaque handle.  Public
 * context APIs are called from the midrd event thread unless stated otherwise.
 */
struct midr_context;

/* Available after midrd finishes runtime initialization. */
struct midr_context *midr_context_get_default(void);

#endif /* MIDRD_CONTEXT_H */
