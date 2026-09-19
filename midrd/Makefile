CC ?= cc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= build
PREFIX ?= /usr/local
SBINDIR ?= $(PREFIX)/sbin
FRR_CPPFLAGS ?= -DHAVE_CONFIG_H -I.. -I../lib
FRR_CFLAGS ?= -std=gnu11 -Wno-pedantic -Wno-missing-field-initializers \
	-Wno-unused-parameter
FRR_LIBDIR ?= $(abspath ../lib/.libs)
FRR_LIB ?= $(FRR_LIBDIR)/libfrr.so
FRR_LDFLAGS ?= -Wl,-rpath,$(FRR_LIBDIR)

.PHONY: all test clean boundary install native-smoke hardening-smoke

CORE_OBJECTS = $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o \
	$(BUILD_DIR)/midr-transport.o $(BUILD_DIR)/midr-session.o \
	$(BUILD_DIR)/midr-consumer.o \
	$(BUILD_DIR)/midr-prefix-provider.o $(BUILD_DIR)/midr-engine.o \
	$(BUILD_DIR)/midr-spf.o $(BUILD_DIR)/midr-prefix-ipc.o \
	$(BUILD_DIR)/midr-owned.o $(BUILD_DIR)/midr-scope.o \
	$(BUILD_DIR)/midr-cost.o $(BUILD_DIR)/midr-local-provider.o \
	$(BUILD_DIR)/midr-local-ipc.o $(BUILD_DIR)/midr-lsdb.o \
	$(BUILD_DIR)/midr-ted.o

INSTALL_OBJECTS = $(BUILD_DIR)/midr-spf-install.o \
	$(BUILD_DIR)/midr-zebra.o

all: $(BUILD_DIR)/midrd-contract-test $(BUILD_DIR)/midrd-core-test \
	$(BUILD_DIR)/midr-prefix-provider-test $(BUILD_DIR)/midrd-wire-test \
	$(BUILD_DIR)/midrd-transport-test \
	$(BUILD_DIR)/midrd-transport-budget-test \
	$(BUILD_DIR)/midrd-session-test \
	$(BUILD_DIR)/midrd-consumer-test \
	$(BUILD_DIR)/midrd-spf-test $(BUILD_DIR)/midrd-engine-test \
	$(BUILD_DIR)/midrd-prefix-ipc-test \
	$(BUILD_DIR)/midrd-owned-test \
	$(BUILD_DIR)/midrd-prefix-transaction-test \
	$(BUILD_DIR)/midrd-scope-test $(BUILD_DIR)/midrd-cost-test \
	$(BUILD_DIR)/midrd-local-ipc-test \
	$(BUILD_DIR)/midrd-local-transaction-test \
	$(BUILD_DIR)/midrd-lsdb-test \
	$(BUILD_DIR)/midrd-ted-test \
	$(BUILD_DIR)/midrd-interface-test \
	$(BUILD_DIR)/midrd-spf-install-test \
	$(BUILD_DIR)/midrd-sync-test $(BUILD_DIR)/midrd-scale-test \
	$(BUILD_DIR)/midrd

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/midrd-contract-test: contract-test.c midr-core.h midr-transport.h midr-session.h midr-prefix-provider.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) contract-test.c -o $@

$(BUILD_DIR)/midr-core.o: midr-core.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-core.c -o $@

$(BUILD_DIR)/midr-wire.o: midr-wire.c midr-wire.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-wire.c -o $@

$(BUILD_DIR)/midr-transport.o: midr-transport.c midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-transport.c -o $@

$(BUILD_DIR)/midr-session.o: midr-session.c midr-session.h midr-session-private.h midr-context-private.h midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-session.c -o $@

$(BUILD_DIR)/midr-consumer.o: midr-consumer.c midr-consumer.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-consumer.c -o $@

$(BUILD_DIR)/midr-engine.o: midr-engine.c midr-engine.h midr-core.h midr-consumer.h midr-prefix-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-engine.c -o $@

$(BUILD_DIR)/midr-owned.o: midr-owned.c midr-owned.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-owned.c -o $@

$(BUILD_DIR)/midr-scope.o: midr-scope.c midr-scope.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-scope.c -o $@

$(BUILD_DIR)/midr-cost.o: midr-cost.c midr-cost.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-cost.c -o $@

$(BUILD_DIR)/midr-local-provider.o: midr-local-provider.c midr-local-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-local-provider.c -o $@

$(BUILD_DIR)/midr-local-ipc.o: midr-local-ipc.c midr-local-ipc.h midr-local-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-local-ipc.c -o $@

$(BUILD_DIR)/midr-ted.o: midr-ted.c midr-ted.h midr-consumer.h midr-lsdb.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-ted.c -o $@

$(BUILD_DIR)/midr-lsdb.o: midr-lsdb.c midr-lsdb.h midr-core.h midr-scope.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-lsdb.c -o $@

$(BUILD_DIR)/midrd-core-test.o: core-test.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c core-test.c -o $@

$(BUILD_DIR)/midrd-core-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midrd-core-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-scale-test.o: scale-test.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c scale-test.c -o $@

$(BUILD_DIR)/midrd-scale-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midrd-scale-test.o
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
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-transport-budget-test.o: transport-budget-test.c midr-transport.c midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c transport-budget-test.c -o $@

$(BUILD_DIR)/midrd-transport-budget-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o $(BUILD_DIR)/midrd-transport-budget-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-session-test.o: session-test.c midr-session.h midr-session-private.h midr-context-private.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c session-test.c -o $@

$(BUILD_DIR)/midrd-session-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-wire.o $(BUILD_DIR)/midr-transport.o $(BUILD_DIR)/midr-session.o $(BUILD_DIR)/midrd-session-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-consumer-test.o: consumer-test.c midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c consumer-test.c -o $@

$(BUILD_DIR)/midrd-consumer-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-consumer.o $(BUILD_DIR)/midrd-consumer-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd.o: midrd.c midr-context.h midr-context-private.h midr-topology.h midr-cost.h midr-engine.h midr-core.h midr-local-ipc.h midr-local-provider.h midr-prefix-provider.h midr-prefix-ipc.h midr-session.h midr-session-private.h midr-spf.h midr-spf-install.h midr-ted.h midr-owned.h midr-scope.h midr-transport.h midr-wire.h midr-zebra.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midrd.c -o $@

$(BUILD_DIR)/midr-prefix-provider.o: midr-prefix-provider.c midr-prefix-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-prefix-provider.c -o $@

$(BUILD_DIR)/midr-spf.o: midr-spf.c midr-spf.h midr-consumer.h midr-ted.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-spf.c -o $@

$(BUILD_DIR)/midr-spf-install.o: midr-spf-install.c midr-spf-install.h midr-spf.h midr-zebra.h midr-context-private.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-spf-install.c -o $@

$(BUILD_DIR)/midr-zebra.o: midr-zebra.c midr-zebra.h midr-spf-install.h midr-context-private.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-zebra.c -o $@

$(BUILD_DIR)/midr-prefix-ipc.o: midr-prefix-ipc.c midr-prefix-ipc.h midr-prefix-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c midr-prefix-ipc.c -o $@

$(BUILD_DIR)/midrd-prefix-ipc-test.o: prefix-ipc-test.c midr-prefix-ipc.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c prefix-ipc-test.c -o $@

$(BUILD_DIR)/midrd-prefix-ipc-test: $(BUILD_DIR)/midr-prefix-ipc.o $(BUILD_DIR)/midrd-prefix-ipc-test.o $(BUILD_DIR)/midr-prefix-provider.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-cost-test.o: cost-test.c midr-cost.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c cost-test.c -o $@

$(BUILD_DIR)/midrd-cost-test: $(BUILD_DIR)/midr-cost.o $(BUILD_DIR)/midrd-cost-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-local-ipc-test.o: local-ipc-test.c midr-local-ipc.h midr-local-provider.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c local-ipc-test.c -o $@

$(BUILD_DIR)/midrd-local-ipc-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-local-provider.o $(BUILD_DIR)/midr-local-ipc.o $(BUILD_DIR)/midrd-local-ipc-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-local-transaction-test.o: local-transaction-test.c midrd.c midr-context.h midr-context-private.h midr-cost.h midr-local-ipc.h midr-local-provider.h midr-engine.h midr-owned.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c local-transaction-test.c -o $@

$(BUILD_DIR)/midrd-local-transaction-test: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd-local-transaction-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-interface-test.o: interface-test.c midrd.c midr-context.h midr-context-private.h midr-topology.h midr-spf.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c interface-test.c -o $@

$(BUILD_DIR)/midrd-interface-test: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd-interface-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-spf-install-test.o: spf-install-test.c midrd.c midr-context.h midr-context-private.h midr-spf.h midr-spf-install.h midr-zebra.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c spf-install-test.c -o $@

$(BUILD_DIR)/midrd-spf-install-test: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd-spf-install-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-ted-test.o: ted-test.c midr-ted.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c ted-test.c -o $@

$(BUILD_DIR)/midrd-ted-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-consumer.o $(BUILD_DIR)/midr-scope.o $(BUILD_DIR)/midr-lsdb.o $(BUILD_DIR)/midr-ted.o $(BUILD_DIR)/midrd-ted-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-lsdb-test.o: lsdb-test.c midr-lsdb.h midr-scope.h midr-core.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c lsdb-test.c -o $@

$(BUILD_DIR)/midrd-lsdb-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-consumer.o $(BUILD_DIR)/midr-scope.o $(BUILD_DIR)/midr-lsdb.o $(BUILD_DIR)/midrd-lsdb-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-sync-test.o: sync-test.c midrd.c midr-context.h midr-context-private.h midr-transport.h midr-engine.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c sync-test.c -o $@

$(BUILD_DIR)/midrd-sync-test: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd-sync-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-owned-test.o: owned-test.c midr-owned.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c owned-test.c -o $@

$(BUILD_DIR)/midrd-owned-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-owned.o $(BUILD_DIR)/midrd-owned-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-prefix-transaction-test.o: prefix-transaction-test.c midrd.c midr-context.h midr-context-private.h midr-cost.h midr-engine.h midr-core.h midr-local-ipc.h midr-local-provider.h midr-prefix-provider.h midr-prefix-ipc.h midr-spf.h midr-owned.h midr-scope.h midr-transport.h midr-wire.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(FRR_CPPFLAGS) $(CFLAGS) $(FRR_CFLAGS) -c prefix-transaction-test.c -o $@

$(BUILD_DIR)/midrd-prefix-transaction-test: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd-prefix-transaction-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd-scope-test.o: scope-test.c midr-scope.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c scope-test.c -o $@

$(BUILD_DIR)/midrd-scope-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-scope.o $(BUILD_DIR)/midrd-scope-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-spf-test.o: spf-test.c midr-spf.h midr-ted.h \
		midr-consumer.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c spf-test.c -o $@

$(BUILD_DIR)/midrd-spf-test: $(BUILD_DIR)/midr-spf.o $(BUILD_DIR)/midrd-spf-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd-engine-test.o: engine-test.c midr-engine.h midr-spf.h midr-ted.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c engine-test.c -o $@

$(BUILD_DIR)/midrd-engine-test: $(CORE_OBJECTS) $(BUILD_DIR)/midrd-engine-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

$(BUILD_DIR)/midrd: $(CORE_OBJECTS) $(INSTALL_OBJECTS) $(BUILD_DIR)/midrd.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ $(FRR_LDFLAGS) $(FRR_LIB) -o $@

test: all
	$(BUILD_DIR)/midrd-contract-test
	$(BUILD_DIR)/midrd-core-test
	$(BUILD_DIR)/midr-prefix-provider-test
	$(BUILD_DIR)/midrd-wire-test
	$(BUILD_DIR)/midrd-transport-test
	$(BUILD_DIR)/midrd-transport-budget-test
	$(BUILD_DIR)/midrd-session-test
	$(BUILD_DIR)/midrd-consumer-test
	$(BUILD_DIR)/midrd-spf-test
	$(BUILD_DIR)/midrd-engine-test
	$(BUILD_DIR)/midrd-prefix-ipc-test
	$(BUILD_DIR)/midrd-owned-test
	$(BUILD_DIR)/midrd-prefix-transaction-test
	$(BUILD_DIR)/midrd-scope-test
	$(BUILD_DIR)/midrd-cost-test
	$(BUILD_DIR)/midrd-local-ipc-test
	$(BUILD_DIR)/midrd-local-transaction-test
	$(BUILD_DIR)/midrd-lsdb-test
	$(BUILD_DIR)/midrd-ted-test
	$(BUILD_DIR)/midrd-interface-test
	$(BUILD_DIR)/midrd-spf-install-test
	$(BUILD_DIR)/midrd-sync-test
	$(BUILD_DIR)/midrd-scale-test
	./extraction-boundary-test.sh

native-smoke: $(BUILD_DIR)/midrd
	MIDRD_BIN=$(abspath $(BUILD_DIR)/midrd) ./r7-native-smoke.sh

hardening-smoke: $(BUILD_DIR)/midrd $(BUILD_DIR)/midrd-scale-test
	MIDRD_BIN=$(abspath $(BUILD_DIR)/midrd) \
	MIDRD_SCALE_BIN=$(abspath $(BUILD_DIR)/midrd-scale-test) \
	./r7-hardening-smoke.sh

clean:
	rm -rf $(BUILD_DIR)

install: $(BUILD_DIR)/midrd
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(BUILD_DIR)/midrd $(DESTDIR)$(SBINDIR)/midrd
