CC ?= gcc
AR ?= ar

CPPFLAGS ?=
CPPFLAGS += -D_GNU_SOURCE -Iinclude
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Werror
LDFLAGS ?=
LDLIBS ?=
ROOTFS_LDLIBS ?= -larchive -lz

MUSL_CC ?= musl-gcc
MUSL_STRIP ?= strip
STATIC_SECTION_FLAGS := -ffunction-sections -fdata-sections
STATIC_WARNING_FLAGS := -std=c99 -Wall -Wextra -Wpedantic -Werror
STATIC_DEBUG_CFLAGS ?= -Og -g3 -fno-omit-frame-pointer $(STATIC_SECTION_FLAGS) $(STATIC_WARNING_FLAGS)
STATIC_RELEASE_CFLAGS ?= -Os -g0 -fno-asynchronous-unwind-tables -fno-unwind-tables $(STATIC_SECTION_FLAGS) $(STATIC_WARNING_FLAGS)
STATIC_LDFLAGS ?= -static -Wl,--gc-sections -Wl,--build-id=none

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
TEST_DIR := $(BUILD_DIR)/tests
STATIC_DEBUG_DIR := $(BUILD_DIR)/static-debug
STATIC_RELEASE_DIR := $(BUILD_DIR)/static

COMMON_SRCS := src/common.c src/config.c src/sha256.c src/install.c src/cpio.c src/tar.c src/rootfs.c src/runtime.c
COMMON_OBJS := $(COMMON_SRCS:src/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean test static static-debug

all: $(BIN_DIR)/burning-init $(BIN_DIR)/burning-progress

$(BIN_DIR)/burning-init: $(OBJ_DIR)/burning_init.o | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR)/burning-progress: $(OBJ_DIR)/burning_progress.o $(COMMON_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(ROOTFS_LDLIBS)

$(TEST_DIR)/test_config: $(OBJ_DIR)/tests/test_config.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(ROOTFS_LDLIBS)

$(TEST_DIR)/test_install: $(OBJ_DIR)/tests/test_install.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(ROOTFS_LDLIBS)

$(TEST_DIR)/test_cpio: $(OBJ_DIR)/tests/test_cpio.o $(COMMON_OBJS) | $(TEST_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(ROOTFS_LDLIBS)

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

static-debug:
	rm -rf $(STATIC_DEBUG_DIR)
	$(MAKE) BUILD_DIR=$(STATIC_DEBUG_DIR) CC="$(MUSL_CC)" \
		CFLAGS="$(STATIC_DEBUG_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(STATIC_LDFLAGS)" all

static: static-debug
	rm -rf $(STATIC_RELEASE_DIR)
	$(MAKE) BUILD_DIR=$(STATIC_RELEASE_DIR) CC="$(MUSL_CC)" \
		CFLAGS="$(STATIC_RELEASE_CFLAGS)" \
		LDFLAGS="$(LDFLAGS) $(STATIC_LDFLAGS)" all
	$(MUSL_STRIP) --strip-all \
		$(STATIC_RELEASE_DIR)/bin/burning-init \
		$(STATIC_RELEASE_DIR)/bin/burning-progress

clean:
	rm -rf $(BUILD_DIR)
