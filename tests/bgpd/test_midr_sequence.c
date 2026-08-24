// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MIDR persistent sequence allocator tests.
 */

#include <zebra.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "libfrr.h"
#include "privs.h"

#include "bgpd/bgp_midr_sequence.h"

/* Required variables when linking against bgpd/libbgp.a. */
struct zebra_privs_t bgpd_privs = {};
struct event_loop *master;

struct memory_store {
	bool found;
	uint32_t epoch;
	int load_result;
	int save_result;
	uint32_t load_node_id;
	uint32_t save_node_id;
	uint32_t saved_epoch;
	unsigned int load_count;
	unsigned int save_count;
};

static int memory_load_epoch(void *arg, uint32_t node_id, uint32_t *epoch, bool *found)
{
	struct memory_store *store = arg;

	store->load_count++;
	store->load_node_id = node_id;
	if (store->load_result)
		return store->load_result;
	*epoch = store->epoch;
	*found = store->found;
	return 0;
}

static int memory_save_epoch(void *arg, uint32_t node_id, uint32_t epoch)
{
	struct memory_store *store = arg;

	store->save_count++;
	store->save_node_id = node_id;
	store->saved_epoch = epoch;
	if (store->save_result)
		return store->save_result;
	store->epoch = epoch;
	store->found = true;
	return 0;
}

static const struct midr_sequence_store_ops memory_store_ops = {
	.load_epoch = memory_load_epoch,
	.save_epoch = memory_save_epoch,
};

static uint32_t router_id(const char *text)
{
	struct in_addr address;

	assert(inet_pton(AF_INET, text, &address) == 1);
	return address.s_addr;
}

static struct midr_sequence_allocator initialized_allocator(struct memory_store *store)
{
	struct midr_sequence_allocator allocator;

	assert(midr_sequence_allocator_init(&allocator, router_id("1.1.1.1"), &memory_store_ops,
					    store) == 0);
	return allocator;
}

static void test_first_start_and_restart(void)
{
	struct memory_store store = {};
	struct midr_sequence_allocator allocator;
	uint64_t sequence;
	uint32_t node_id = router_id("1.1.1.1");

	allocator = initialized_allocator(&store);
	assert(midr_sequence_allocator_is_ready(&allocator));
	assert(store.load_count == 1);
	assert(store.save_count == 1);
	assert(store.load_node_id == node_id);
	assert(store.save_node_id == node_id);
	assert(store.saved_epoch == 1);
	assert(allocator.boot_epoch == 1);
	assert(allocator.origin_counter == 0);

	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)1 << 32 | 1));
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)1 << 32 | 2));

	allocator = initialized_allocator(&store);
	assert(store.saved_epoch == 2);
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)2 << 32 | 1));
}

static void test_rollover(void)
{
	struct memory_store store = {
		.found = true,
		.epoch = 9,
	};
	struct midr_sequence_allocator allocator = initialized_allocator(&store);
	uint64_t sequence;

	assert(allocator.boot_epoch == 10);
	allocator.origin_counter = UINT32_MAX;
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(store.saved_epoch == 11);
	assert(sequence == ((uint64_t)11 << 32 | 1));

	allocator.boot_epoch = UINT32_MAX;
	allocator.origin_counter = UINT32_MAX;
	assert(midr_sequence_allocator_next(&allocator, &sequence) == -EOVERFLOW);
	assert(!midr_sequence_allocator_is_ready(&allocator));
}

static void test_advance_past(void)
{
	struct memory_store store = {};
	struct midr_sequence_allocator allocator = initialized_allocator(&store);
	uint64_t sequence;

	assert(midr_sequence_allocator_advance_past(&allocator, (uint64_t)1 << 32) == 0);
	assert(allocator.origin_counter == 0);
	assert(midr_sequence_allocator_advance_past(&allocator, ((uint64_t)1 << 32 | 20)) == 0);
	assert(allocator.boot_epoch == 1);
	assert(allocator.origin_counter == 20);
	assert(store.save_count == 1);
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)1 << 32 | 21));

	assert(midr_sequence_allocator_advance_past(&allocator, ((uint64_t)5 << 32 | 30)) == 0);
	assert(allocator.boot_epoch == 5);
	assert(allocator.origin_counter == 30);
	assert(store.saved_epoch == 5);
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)5 << 32 | 31));

	assert(midr_sequence_allocator_advance_past(&allocator,
						    ((uint64_t)6 << 32 | UINT32_MAX)) == 0);
	assert(allocator.boot_epoch == 7);
	assert(allocator.origin_counter == 0);
	assert(store.saved_epoch == 7);
	assert(midr_sequence_allocator_next(&allocator, &sequence) == 0);
	assert(sequence == ((uint64_t)7 << 32 | 1));

	assert(midr_sequence_allocator_advance_past(&allocator, UINT64_MAX) == -EOVERFLOW);
	assert(!midr_sequence_allocator_is_ready(&allocator));
}

static void test_fail_closed(void)
{
	struct memory_store store = {
		.save_result = -EIO,
	};
	struct midr_sequence_allocator allocator;
	uint64_t sequence = 123;

	assert(midr_sequence_allocator_init(&allocator, router_id("1.1.1.1"), &memory_store_ops,
					    &store) == -EIO);
	assert(!midr_sequence_allocator_is_ready(&allocator));
	assert(midr_sequence_allocator_next(&allocator, &sequence) == -EAGAIN);
	assert(sequence == 123);

	store = (struct memory_store){
		.load_result = -EINVAL,
	};
	assert(midr_sequence_allocator_init(&allocator, router_id("1.1.1.1"), &memory_store_ops,
					    &store) == -EINVAL);
	assert(!midr_sequence_allocator_is_ready(&allocator));

	store = (struct memory_store){
		.found = true,
		.epoch = UINT32_MAX,
	};
	assert(midr_sequence_allocator_init(&allocator, router_id("1.1.1.1"), &memory_store_ops,
					    &store) == -EOVERFLOW);
	assert(!midr_sequence_allocator_is_ready(&allocator));
}

static void test_advance_save_failure(void)
{
	struct memory_store store = {};
	struct midr_sequence_allocator allocator = initialized_allocator(&store);
	uint64_t sequence;

	store.save_result = -ENOSPC;
	assert(midr_sequence_allocator_advance_past(&allocator, ((uint64_t)2 << 32 | 1)) ==
	       -ENOSPC);
	assert(!midr_sequence_allocator_is_ready(&allocator));
	assert(allocator.boot_epoch == 1);
	assert(allocator.origin_counter == 0);
	assert(midr_sequence_allocator_next(&allocator, &sequence) == -EAGAIN);

	store = (struct memory_store){};
	allocator = initialized_allocator(&store);
	allocator.origin_counter = UINT32_MAX;
	store.save_result = -ENOSPC;
	sequence = 123;
	assert(midr_sequence_allocator_next(&allocator, &sequence) == -ENOSPC);
	assert(!midr_sequence_allocator_is_ready(&allocator));
	assert(sequence == 123);
}

static void test_invalid_arguments(void)
{
	struct memory_store store = {};
	struct midr_sequence_allocator allocator;
	struct midr_sequence_store_ops incomplete_ops = {};
	struct json_object *state = NULL;
	uint64_t sequence;

	assert(frr_daemon_state_save_status(NULL) == -EINVAL);
	assert(frr_daemon_state_save_status(&state) == -EINVAL);
	assert(midr_sequence_allocator_init(NULL, router_id("1.1.1.1"), &memory_store_ops,
					    &store) == -EINVAL);
	assert(midr_sequence_allocator_init(&allocator, 0, &memory_store_ops, &store) == -EINVAL);
	assert(midr_sequence_allocator_init(&allocator, router_id("1.1.1.1"), &incomplete_ops,
					    &store) == -EINVAL);
	assert(midr_sequence_allocator_next(NULL, &sequence) == -EINVAL);
	assert(midr_sequence_allocator_next(&allocator, NULL) == -EINVAL);
	assert(midr_sequence_allocator_advance_past(NULL, 1) == -EINVAL);
	assert(!midr_sequence_allocator_is_ready(NULL));
}

static void test_frr_store_persistence(void)
{
	char directory[] = "/tmp/midr-sequence-XXXXXX";
	char state_path[PATH_MAX];
	char *state_paths[] = {
		state_path,
		NULL,
	};
	char program[] = "test_midr_sequence";
	char *argv[] = {
		program,
		NULL,
	};
	struct frr_daemon_info daemon = {
		.name = "test_midr_sequence",
		.logname = "MIDR sequence test",
		.state_paths = state_paths,
	};
	struct midr_sequence_allocator allocator;
	uint32_t node_id = router_id("1.1.1.1");
	int fd;

	assert(mkdtemp(directory) != NULL);
	assert(snprintf(state_path, sizeof(state_path), "%s/bgpd.json", directory) > 0);
	frr_preinit(&daemon, 1, argv);

	assert(midr_sequence_allocator_init(&allocator, node_id, &midr_sequence_frr_store_ops,
					    NULL) == 0);
	assert(allocator.boot_epoch == 1);
	assert(midr_sequence_allocator_init(&allocator, node_id, &midr_sequence_frr_store_ops,
					    NULL) == 0);
	assert(allocator.boot_epoch == 2);

	fd = open(state_path, O_WRONLY | O_TRUNC);
	assert(fd >= 0);
	assert(write(fd, "{bad", 4) == 4);
	assert(close(fd) == 0);
	assert(midr_sequence_allocator_init(&allocator, node_id, &midr_sequence_frr_store_ops,
					    NULL) == -EINVAL);

	assert(unlink(state_path) == 0);
	assert(chmod(directory, 0500) == 0);
	assert(midr_sequence_allocator_init(&allocator, node_id, &midr_sequence_frr_store_ops,
					    NULL) == -EACCES);
	assert(chmod(directory, 0700) == 0);
	assert(rmdir(directory) == 0);
}

int main(void)
{
	test_first_start_and_restart();
	test_rollover();
	test_advance_past();
	test_fail_closed();
	test_advance_save_failure();
	test_invalid_arguments();
	test_frr_store_persistence();
	printf("MIDR sequence allocator tests passed\n");
	return 0;
}
