/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <zebra.h>

#include "midr-context-private.h"
#include "midr-session-private.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct session_state {
	unsigned int connecting;
	unsigned int established;
	unsigned int mismatch;
	unsigned int down;
	unsigned int protocol_established;
	unsigned int protocol_closed;
	unsigned int frames;
	uint32_t remote_node_id;
	uint64_t generation;
};

struct session_side {
	struct midr_context ctx;
	struct midr_session_manager *manager;
	struct session_state state;
};

static void state_changed(struct midr_context *ctx,
			  const struct midr_session_status *status, void *arg)
{
	struct session_state *state = arg;

	assert(ctx && status);
	state->remote_node_id = status->remote_node_id;
	state->generation = status->generation;
	switch (status->state) {
	case MIDR_SESSION_DOWN:
		state->down++;
		break;
	case MIDR_SESSION_CONNECTING:
		state->connecting++;
		break;
	case MIDR_SESSION_ESTABLISHED:
		state->established++;
		break;
	case MIDR_SESSION_IDENTITY_MISMATCH:
		state->mismatch++;
		break;
	}
}

static void protocol_established(
	void *arg, const struct midr_transport_endpoint *peer,
	uint32_t remote_node_id, uint64_t generation)
{
	struct session_state *state = arg;

	assert(peer && remote_node_id && generation);
	state->protocol_established++;
	state->remote_node_id = remote_node_id;
	state->generation = generation;
}

static void protocol_closed(void *arg,
			    const struct midr_transport_endpoint *peer,
			    uint32_t remote_node_id, uint64_t generation,
			    int reason)
{
	struct session_state *state = arg;

	assert(peer && remote_node_id && generation && reason <= 0);
	state->protocol_closed++;
}

static int protocol_frame(void *arg,
			  const struct midr_transport_endpoint *peer,
			  const struct midr_transport_frame *frame)
{
	struct session_state *state = arg;

	assert(peer && frame);
	assert(frame->type == MIDR_FRAME_UPDATE);
	assert(frame->payload_len == 1 && frame->payload[0] == 0x5a);
	state->frames++;
	return 0;
}

static struct midr_transport_endpoint transport_endpoint(uint8_t family,
							 uint16_t port)
{
	struct midr_transport_endpoint endpoint = {
		.family = family,
		.port = port,
	};

	if (family == MIDR_TRANSPORT_AF_IPV4) {
		endpoint.address[0] = 127;
		endpoint.address[3] = 1;
	} else {
		endpoint.address[15] = 1;
	}
	return endpoint;
}

static struct midr_session_endpoint session_endpoint(uint8_t family,
						     uint16_t port)
{
	struct midr_session_endpoint endpoint = {.port = port};

	if (family == MIDR_TRANSPORT_AF_IPV4) {
		SET_IPADDR_V4(&endpoint.address);
		endpoint.address.ipaddr_v4.s_addr = htonl(INADDR_LOOPBACK);
	} else {
		SET_IPADDR_V6(&endpoint.address);
		endpoint.address.ipaddr_v6 = in6addr_loopback;
	}
	return endpoint;
}

static void side_create(struct session_side *side, uint32_t node_id,
			uint8_t family, uint16_t port)
{
	struct midr_session_protocol_ops protocol = {
		.established = protocol_established,
		.closed = protocol_closed,
		.frame = protocol_frame,
		.arg = &side->state,
	};
	struct midr_session_manager_config config = {
		.local_node_id = node_id,
		.hello_interval_ms = 20,
		.transport = {
			.local = transport_endpoint(family, port),
			.hello_interval_ms = 20,
			.hold_time_ms = 1000,
			.tx_budget_ms = 500,
			.max_frame_size = 4096,
		},
	};
	struct midr_session_observer_ops observer = {
		.state_changed = state_changed,
	};

	memset(side, 0, sizeof(*side));
	protocol.arg = &side->state;
	assert(midr_session_manager_create(&config, &protocol,
					   &side->manager) == 0);
	side->ctx.sessions = side->manager;
	assert(midr_session_observer_register(&side->ctx, &observer,
					      &side->state) == 0);
}

static void side_destroy(struct session_side *side)
{
	midr_session_observer_unregister(&side->ctx);
	midr_session_manager_destroy(&side->manager);
	side->ctx.sessions = NULL;
}

static void poll_pair(struct session_side *left, struct session_side *right)
{
	(void)midr_session_manager_poll(left->manager, 5);
	(void)midr_session_manager_poll(right->manager, 5);
}

static void wait_established(struct session_side *left,
			     struct session_side *right)
{
	for (unsigned int i = 0;
	     i < 400 && (!left->state.protocol_established ||
			 right->state.protocol_established == 0);
	     i++)
		poll_pair(left, right);
	assert(left->state.protocol_established > 0);
	assert(right->state.protocol_established > 0);
}

static uint16_t test_port(unsigned int offset)
{
	return (uint16_t)(40000U + ((unsigned int)getpid() % 1000U) * 10U +
			  offset);
}

static void test_public_ipv4_and_sources(void)
{
	struct session_side left, right;
	struct midr_session_endpoint left_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(0));
	struct midr_session_endpoint right_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(1));
	struct midr_transport_endpoint right_transport =
		transport_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(1));
	struct midr_session_status status;
	uint8_t payload = 0x5a;

	side_create(&left, 101, MIDR_TRANSPORT_AF_IPV4, test_port(0));
	side_create(&right, 202, MIDR_TRANSPORT_AF_IPV4, test_port(1));
	assert(midr_session_manager_request_static(left.manager,
						   &right_transport) == 0);
	assert(midr_session_connect(&left.ctx, &right_public) == 0);
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	assert(left.state.protocol_established == 0);
	assert(right.state.protocol_established == 0);
	assert(midr_session_manager_send(left.manager, &right_transport,
					 MIDR_FRAME_UPDATE, &payload,
					 sizeof(payload), 0) == -ENOTCONN);
	wait_established(&left, &right);
	assert(left.state.remote_node_id == 202);
	assert(right.state.remote_node_id == 101);
	assert(left.state.established == left.state.protocol_established);
	assert(right.state.established == right.state.protocol_established);
	assert(midr_session_status_get(&left.ctx, &right_public, &status) == 0);
	assert(status.state == MIDR_SESSION_ESTABLISHED);
	assert(status.remote_node_id == 202 && status.generation != 0);
	assert(midr_session_manager_send(left.manager, &right_transport,
					 MIDR_FRAME_UPDATE, &payload,
					 sizeof(payload), 0) == 0);
	for (unsigned int i = 0; i < 100 && !right.state.frames; i++)
		poll_pair(&left, &right);
	assert(right.state.frames == 1);
	assert(midr_session_disconnect(&left.ctx, &right_public,
				       MIDR_SESSION_CLOSE_ADMIN) == 0);
	assert(midr_session_disconnect(&left.ctx, &right_public,
				       MIDR_SESSION_CLOSE_ADMIN) == 0);
	assert(midr_session_status_get(&left.ctx, &right_public, &status) ==
	       -ENOENT);
	/* The static source still owns the same session. */
	assert(left.state.protocol_closed == 0);
	assert(midr_session_manager_send(left.manager, &right_transport,
					 MIDR_FRAME_UPDATE, &payload,
					 sizeof(payload), 0) == 0);
	side_destroy(&left);
	side_destroy(&right);
}

static void test_ipv6(void)
{
	struct session_side left, right;
	struct midr_session_endpoint left_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV6, test_port(2));
	struct midr_session_endpoint right_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV6, test_port(3));

	side_create(&left, 301, MIDR_TRANSPORT_AF_IPV6, test_port(2));
	side_create(&right, 302, MIDR_TRANSPORT_AF_IPV6, test_port(3));
	assert(midr_session_connect(&left.ctx, &right_public) == 0);
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	wait_established(&left, &right);
	assert(left.state.remote_node_id == 302);
	assert(right.state.remote_node_id == 301);
	side_destroy(&left);
	side_destroy(&right);
}

static void test_unregistered_inbound(void)
{
	struct session_side left, right;
	struct midr_session_endpoint right_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(5));

	side_create(&left, 401, MIDR_TRANSPORT_AF_IPV4, test_port(4));
	side_create(&right, 402, MIDR_TRANSPORT_AF_IPV4, test_port(5));
	assert(midr_session_connect(&left.ctx, &right_public) == 0);
	for (unsigned int i = 0; i < 80; i++)
		poll_pair(&left, &right);
	assert(left.state.protocol_established == 0);
	assert(right.state.protocol_established == 0);
	side_destroy(&left);
	side_destroy(&right);
}

static void test_reconnect_identity_binding(void)
{
	struct session_side left, right;
	struct midr_session_endpoint left_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(6));
	struct midr_session_endpoint right_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(7));
	uint64_t first_generation;

	side_create(&left, 501, MIDR_TRANSPORT_AF_IPV4, test_port(6));
	side_create(&right, 502, MIDR_TRANSPORT_AF_IPV4, test_port(7));
	assert(midr_session_connect(&left.ctx, &right_public) == 0);
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	wait_established(&left, &right);
	first_generation = left.state.generation;
	side_destroy(&right);
	for (unsigned int i = 0; i < 40 && !left.state.protocol_closed; i++)
		(void)midr_session_manager_poll(left.manager, 5);
	assert(left.state.protocol_closed == 1);

	side_create(&right, 502, MIDR_TRANSPORT_AF_IPV4, test_port(7));
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	for (unsigned int i = 0;
	     i < 400 && left.state.protocol_established < 2; i++)
		poll_pair(&left, &right);
	assert(left.state.protocol_established == 2);
	assert(left.state.generation > first_generation);
	side_destroy(&right);

	side_create(&right, 503, MIDR_TRANSPORT_AF_IPV4, test_port(7));
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	for (unsigned int i = 0; i < 400 && !left.state.mismatch; i++)
		poll_pair(&left, &right);
	assert(left.state.mismatch > 0);
	assert(left.state.protocol_established == 2);
	side_destroy(&left);
	side_destroy(&right);
}

static void poll_trio(struct session_side *a, struct session_side *b,
		      struct session_side *c)
{
	(void)midr_session_manager_poll(a->manager, 5);
	(void)midr_session_manager_poll(b->manager, 5);
	(void)midr_session_manager_poll(c->manager, 5);
}

/* Three nodes on one address: B peers A and C, so an inbound stream at B
 * cannot be attributed to a configured peer from its kernel-chosen source.
 * Identity comes from HELLO.  Before the fix C's stream was bound to A's peer
 * slot, produced MIDR_SESSION_IDENTITY_MISMATCH and was reset in a storm. */
static void test_shared_address_three_nodes(void)
{
	struct session_side a, b, c;
	struct midr_transport_endpoint a_ep =
		transport_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(8));
	struct midr_transport_endpoint b_ep =
		transport_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(9));
	struct midr_transport_endpoint c_ep =
		transport_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(10));
	unsigned int a_est, b_est, c_est;

	side_create(&a, 601, MIDR_TRANSPORT_AF_IPV4, test_port(8));
	side_create(&b, 602, MIDR_TRANSPORT_AF_IPV4, test_port(9));
	side_create(&c, 603, MIDR_TRANSPORT_AF_IPV4, test_port(10));
	assert(midr_session_manager_request_static(a.manager, &b_ep) == 0);
	assert(midr_session_manager_request_static(b.manager, &a_ep) == 0);
	assert(midr_session_manager_request_static(b.manager, &c_ep) == 0);
	assert(midr_session_manager_request_static(c.manager, &b_ep) == 0);
	for (unsigned int i = 0;
	     i < 800 && (!a.state.protocol_established ||
			 b.state.protocol_established < 2 ||
			 !c.state.protocol_established);
	     i++)
		poll_trio(&a, &b, &c);
	assert(a.state.protocol_established > 0);
	assert(b.state.protocol_established == 2);
	assert(c.state.protocol_established > 0);
	assert(a.state.remote_node_id == 602);
	assert(c.state.remote_node_id == 602);
	/* Once converged the shared-address rendezvous must not flap: the
	 * counts stay put and no identity mismatch is ever reported. */
	a_est = a.state.protocol_established;
	b_est = b.state.protocol_established;
	c_est = c.state.protocol_established;
	for (unsigned int i = 0; i < 100; i++)
		poll_trio(&a, &b, &c);
	assert(a.state.protocol_established == a_est);
	assert(b.state.protocol_established == b_est);
	assert(c.state.protocol_established == c_est);
	assert(a.state.mismatch == 0 && b.state.mismatch == 0 &&
	       c.state.mismatch == 0);
	side_destroy(&a);
	side_destroy(&b);
	side_destroy(&c);
}

static void test_internal_reset_preserves_intent(void)
{
	struct session_side left, right;
	struct midr_session_endpoint left_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(8));
	struct midr_session_endpoint right_public =
		session_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(9));
	struct midr_transport_endpoint right_transport =
		transport_endpoint(MIDR_TRANSPORT_AF_IPV4, test_port(9));
	struct midr_session_status status;
	unsigned int left_established;
	unsigned int right_established;
	uint64_t first_generation;

	side_create(&left, 601, MIDR_TRANSPORT_AF_IPV4, test_port(8));
	side_create(&right, 602, MIDR_TRANSPORT_AF_IPV4, test_port(9));
	assert(midr_session_connect(&left.ctx, &right_public) == 0);
	assert(midr_session_connect(&right.ctx, &left_public) == 0);
	wait_established(&left, &right);
	left_established = left.state.protocol_established;
	right_established = right.state.protocol_established;
	first_generation = left.state.generation;
	assert(midr_session_manager_reset(left.manager, &right_transport,
					  -ENOMEM) == 0);
	assert(left.state.protocol_closed == 1);
	assert(midr_session_status_get(&left.ctx, &right_public, &status) == 0);
	assert(status.state == MIDR_SESSION_DOWN && status.last_error == -ENOMEM);
	for (unsigned int i = 0;
	     i < 500 &&
	     (left.state.protocol_established == left_established ||
	      right.state.protocol_established == right_established);
	     i++)
		poll_pair(&left, &right);
	assert(left.state.protocol_established > left_established);
	assert(right.state.protocol_established > right_established);
	assert(left.state.generation > first_generation);
	assert(midr_session_status_get(&left.ctx, &right_public, &status) == 0);
	assert(status.state == MIDR_SESSION_ESTABLISHED);
	side_destroy(&left);
	side_destroy(&right);
}

int main(void)
{
	struct midr_session_endpoint invalid = {0};

	assert(midr_session_endpoint_validate(&invalid) == -EINVAL);
	test_public_ipv4_and_sources();
	test_ipv6();
	test_unregistered_inbound();
	test_reconnect_identity_binding();
	test_shared_address_three_nodes();
	test_internal_reset_preserves_intent();
	puts("midrd session tests: PASS");
	return 0;
}
