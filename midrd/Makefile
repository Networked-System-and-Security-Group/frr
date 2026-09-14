CC ?= cc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= build

.PHONY: all test clean

all: $(BUILD_DIR)/midrd-contract-test $(BUILD_DIR)/midrd-core-test $(BUILD_DIR)/midr-prefix-provider-test $(BUILD_DIR)/midrd

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/midrd-contract-test: contract-test.c midr-core.h midr-transport.h 		midr-prefix-provider.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) contract-test.c -o $@

$(BUILD_DIR)/midr-core.o: midr-core.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-core.c -o $@

$(BUILD_DIR)/midrd-core-test.o: core-test.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c core-test.c -o $@

$(BUILD_DIR)/midrd-core-test: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midrd-core-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midr-prefix-provider-test.o: prefix-provider-test.c midr-prefix-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c prefix-provider-test.c -o $@

$(BUILD_DIR)/midr-prefix-provider-test: $(BUILD_DIR)/midr-prefix-provider.o $(BUILD_DIR)/midr-prefix-provider-test.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

$(BUILD_DIR)/midrd.o: midrd.c midr-core.h midr-transport.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midrd.c -o $@

$(BUILD_DIR)/midr-prefix-provider.o: midr-prefix-provider.c midr-prefix-provider.h midr-core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c midr-prefix-provider.c -o $@

$(BUILD_DIR)/midrd: $(BUILD_DIR)/midr-core.o $(BUILD_DIR)/midr-prefix-provider.o $(BUILD_DIR)/midrd.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@

test: $(BUILD_DIR)/midrd-contract-test $(BUILD_DIR)/midrd-core-test $(BUILD_DIR)/midr-prefix-provider-test $(BUILD_DIR)/midrd
	$(BUILD_DIR)/midrd-contract-test
	$(BUILD_DIR)/midrd-core-test
	$(BUILD_DIR)/midr-prefix-provider-test

clean:
	rm -rf $(BUILD_DIR)
