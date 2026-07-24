CC ?= gcc
AR ?= ar

CPPFLAGS ?=
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=
LDLIBS += -larchive -lz

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
TEST_DIR := $(BUILD_DIR)/tests

COMMON_SRCS := src/common.c src/config.c src/sha256.c src/install.c src/cpio.c src/tar.c src/rootfs.c src/runtime.c
COMMON_OBJS := $(COMMON_SRCS:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test static

all: $(BIN_DIR)/burning-init $(BIN_DIR)/burning-progress

$(BIN_DIR)/burning-init: $(OBJ_DIR)/burning_init.o $(COMMON_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/burning-progress: $(OBJ_DIR)/burning_progress.o $(COMMON_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_DIR)/test_config: $(OBJ_DIR)/tests/test_config.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_DIR)/test_install: $(OBJ_DIR)/tests/test_install.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_DIR)/test_cpio: $(OBJ_DIR)/tests/test_cpio.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJ_DIR)/%.o: src/%.c include/burning.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/tests/%.o: tests/%.c include/burning.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BIN_DIR) $(TEST_DIR):
	mkdir -p $@

test: $(TEST_DIR)/test_config $(TEST_DIR)/test_install $(TEST_DIR)/test_cpio
	$(TEST_DIR)/test_config
	$(TEST_DIR)/test_install
	$(TEST_DIR)/test_cpio

static:
	$(MAKE) clean
	$(MAKE) CC=musl-gcc LDFLAGS=-static all

clean:
	rm -rf $(BUILD_DIR)
