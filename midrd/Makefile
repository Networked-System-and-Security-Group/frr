CC ?= cc
CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= build

.PHONY: all test clean

all: $(BUILD_DIR)/midrd-contract-test

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/midrd-contract-test: contract-test.c midr-core.h midr-transport.h 		midr-prefix-provider.h midr-consumer.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) contract-test.c -o $@

test: $(BUILD_DIR)/midrd-contract-test
	$(BUILD_DIR)/midrd-contract-test

clean:
	rm -rf $(BUILD_DIR)
