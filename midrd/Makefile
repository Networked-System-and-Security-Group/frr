CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD_DIR ?= build

CORE_OBJS = $(BUILD_DIR)/midr-core.o

.PHONY: all test clean

all: $(BUILD_DIR)/midr-core-test

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/midr-core.o: midr-core.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/midr-core-test.o: midr-core-test.c midr-core.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/midr-core-test: $(CORE_OBJS) $(BUILD_DIR)/midr-core-test.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD_DIR)/midr-core-test
	$(BUILD_DIR)/midr-core-test

clean:
	rm -rf $(BUILD_DIR)
