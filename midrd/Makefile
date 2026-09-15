CC ?= cc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= build
PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin

.PHONY: all test clean boundary install

CORE_OBJECTS = $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o \
	$(BUILD_DIR)/midr-transport.o $(BUILD_DIR)/midr-consumer.o \
	$(BUILD_DIR)/midr-prefix-provider.o $(BUILD_DIR)/midr-engine.o \
	$(BUILD_DIR)/midr-spf.o $(BUILD_DIR)/midr-prefix-ipc.o \
	$(BUILD_DIR)/midr-owned.o $(BUILD_DIR)/midr-scope.o

all: $(BUILD_DIR)/midrd-contract-test $(BUILD_DIR)/midrd-core-test \
	$(BUILD_DIR)/midr-prefix-provider-test $(BUILD_DIR)/midrd-wire-test \
	$(BUILD_DIR)/midrd-transport-test \
	$(BUILD_DIR)/midrd-transport-budget-test \
	$(BUILD_DIR)/midrd-consumer-test \
	$(BUILD_DIR)/midrd-spf-test $(BUILD_DIR)/midrd-engine-test \
	$(BUILD_DIR)/midrd-prefix-ipc-test \
	$(BUILD_DIR)/midrd-owned-test \
	$(BUILD_DIR)/midrd-prefix-transaction-test \
	$(BUILD_DIR)/midrd-scope-test \
	$(BUILD_DIR)/midrd

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/midrd-contract-test: contract-test.c midr-core.h midr-transport.h 		midr-prefix-provider.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) contract-test.c -o $@

$(BUILD_DIR)/midr-core.o: midr-core.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-core.c -o $@

$(BUILD_DIR)/midr-wire.o: midr-wire.c midr-wire.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-wire.c -o $@

$(BUILD_DIR)/midr-transport.o: midr-transport.c midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-transport.c -o $@

$(BUILD_DIR)/midr-consumer.o: midr-consumer.c midr-consumer.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-consumer.c -o $@

$(BUILD_DIR)/midr-engine.o: midr-engine.c midr-engine.h midr-core.h midr-consumer.h midr-prefix-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-engine.c -o $@

$(BUILD_DIR)/midr-owned.o: midr-owned.c midr-owned.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-owned.c -o $@

$(BUILD_DIR)/midr-scope.o: midr-scope.c midr-scope.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-scope.c -o $@

$(BUILD_DIR)/midrd-core-test.o: core-test.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c core-test.c -o $@

$(BUILD_DIR)/midrd-core-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midrd-core-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midr-prefix-provider-test.o: prefix-provider-test.c midr-prefix-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c prefix-provider-test.c -o $@

$(BUILD_DIR)/midr-prefix-provider-test: $(BUILD_DIR)/midr-prefix-provider.o $(BUILD_DIR)/midr-prefix-provider-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-wire-test.o: wire-test.c midr-wire.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c wire-test.c -o $@

$(BUILD_DIR)/midrd-wire-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o $(BUILD_DIR)/midrd-wire-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-transport-test.o: transport-test.c midr-transport.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c transport-test.c -o $@

$(BUILD_DIR)/midrd-transport-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o $(BUILD_DIR)/midr-transport.o $(BUILD_DIR)/midrd-transport-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-transport-budget-test.o: transport-budget-test.c midr-transport.c midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c transport-budget-test.c -o $@

$(BUILD_DIR)/midrd-transport-budget-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o $(BUILD_DIR)/midrd-transport-budget-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-consumer-test.o: consumer-test.c midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c consumer-test.c -o $@

$(BUILD_DIR)/midrd-consumer-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-consumer.o $(BUILD_DIR)/midrd-consumer-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd.o: midrd.c midr-engine.h midr-core.h midr-prefix-provider.h midr-prefix-ipc.h midr-spf.h midr-owned.h midr-scope.h midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midrd.c -o $@

$(BUILD_DIR)/midr-prefix-provider.o: midr-prefix-provider.c midr-prefix-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-prefix-provider.c -o $@

$(BUILD_DIR)/midr-spf.o: midr-spf.c midr-spf.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-spf.c -o $@

$(BUILD_DIR)/midr-prefix-ipc.o: midr-prefix-ipc.c midr-prefix-ipc.h midr-prefix-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-prefix-ipc.c -o $@

$(BUILD_DIR)/midrd-prefix-ipc-test.o: prefix-ipc-test.c midr-prefix-ipc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c prefix-ipc-test.c -o $@

$(BUILD_DIR)/midrd-prefix-ipc-test: $(BUILD_DIR)/midr-prefix-ipc.o $(BUILD_DIR)/midrd-prefix-ipc-test.o $(BUILD_DIR)/midr-prefix-provider.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-owned-test.o: owned-test.c midr-owned.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c owned-test.c -o $@

$(BUILD_DIR)/midrd-owned-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-owned.o $(BUILD_DIR)/midrd-owned-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-prefix-transaction-test.o: prefix-transaction-test.c midrd.c midr-engine.h midr-core.h midr-prefix-provider.h midr-prefix-ipc.h midr-spf.h midr-owned.h midr-scope.h midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c prefix-transaction-test.c -o $@

$(BUILD_DIR)/midrd-prefix-transaction-test: $(CORE_OBJECTS) $(BUILD_DIR)/midrd-prefix-transaction-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-scope-test.o: scope-test.c midr-scope.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c scope-test.c -o $@

$(BUILD_DIR)/midrd-scope-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-scope.o $(BUILD_DIR)/midrd-scope-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-spf-test.o: spf-test.c midr-spf.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c spf-test.c -o $@

$(BUILD_DIR)/midrd-spf-test: $(BUILD_DIR)/midr-spf.o $(BUILD_DIR)/midrd-spf-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-engine-test.o: engine-test.c midr-engine.h midr-spf.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c engine-test.c -o $@

$(BUILD_DIR)/midrd-engine-test: $(CORE_OBJECTS) $(BUILD_DIR)/midrd-engine-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd: $(CORE_OBJECTS) $(BUILD_DIR)/midrd.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test: all
	$(BUILD_DIR)/midrd-contract-test
	$(BUILD_DIR)/midrd-core-test
	$(BUILD_DIR)/midr-prefix-provider-test
	$(BUILD_DIR)/midrd-wire-test
	$(BUILD_DIR)/midrd-transport-test
	$(BUILD_DIR)/midrd-transport-budget-test
	$(BUILD_DIR)/midrd-consumer-test
	$(BUILD_DIR)/midrd-spf-test
	$(BUILD_DIR)/midrd-engine-test
	$(BUILD_DIR)/midrd-prefix-ipc-test
	$(BUILD_DIR)/midrd-owned-test
	$(BUILD_DIR)/midrd-prefix-transaction-test
	$(BUILD_DIR)/midrd-scope-test
	./extraction-boundary-test.sh

clean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/midrd
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(BUILD_DIR)/midrd $(DESTDIR)$(SBINDIR)/midrd
