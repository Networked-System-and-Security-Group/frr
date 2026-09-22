/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIDRD_CONTEXT_H
#define MIDRD_CONTEXT_H

/*
 * Process-wide MIDR runtime.  The daemon owns the object; protocol modules
 * and the first/third-group adapters receive only this opaque handle.  Public
 * context APIs are called from the midrd event thread unless stated otherwise.
 */
#include <stdbool.h>
#include <stdint.h>

struct midr_context;
struct ipaddr;

/* Available after midrd finishes runtime initialization. */
struct midr_context *midr_context_get_default(void);

/* 第一组新增：本节点身份查询。第一组的 Node/Link 需要本节点 node_id，
 * 会话 endpoint 需要 MIDR 端口，准入校验需要会话源地址（即监听地址）。 */
uint32_t midr_context_node_id(const struct midr_context *ctx);
/* Either output may be NULL.  Returns false before the listen endpoint is
 * known. */
bool midr_context_listen_endpoint(const struct midr_context *ctx,
				  struct ipaddr *address, uint16_t *port);

#endif /* MIDRD_CONTEXT_H */
