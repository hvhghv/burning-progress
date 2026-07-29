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

.PHONY: all clean test static static-debug test-cli

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

test-cli: $(BIN_DIR)/burning-progress
	rm -rf $(BUILD_DIR)/test-rootfs-config \
		$(BUILD_DIR)/test-rootfs-config-default \
		$(BUILD_DIR)/test-rootfs-config-entry-link \
		$(BUILD_DIR)/test-rootfs-config-entry-outside \
		$(BUILD_DIR)/test-rootfs-config-link \
		$(BUILD_DIR)/test-rootfs-config-outside \
		$(BUILD_DIR)/test-rootfs-invalid-config \
		$(BUILD_DIR)/test-rootfs-pack-auto \
		$(BUILD_DIR)/test-rootfs-pack-refresh \
		$(BUILD_DIR)/test-enable-check
	mkdir -p $(BUILD_DIR)/test-rootfs-config
	printf '%s\n' invalid handoff relative /sbin/init | \
		$(BIN_DIR)/burning-progress rootfs configure $(BUILD_DIR)/test-rootfs-config
	grep -q '^version=1$$' $(BUILD_DIR)/test-rootfs-config/etc/burning-progress.conf
	grep -q '^entryMode=handoff$$' $(BUILD_DIR)/test-rootfs-config/etc/burning-progress.conf
	grep -q '^entry=/sbin/init$$' $(BUILD_DIR)/test-rootfs-config/etc/burning-progress.conf
	printf '\n\n' | \
		$(BIN_DIR)/burning-progress rootfs configure $(BUILD_DIR)/test-rootfs-config
	grep -q '^entryMode=handoff$$' $(BUILD_DIR)/test-rootfs-config/etc/burning-progress.conf
	if : | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config; then exit 1; fi
	mkdir -p $(BUILD_DIR)/test-rootfs-config-default
	printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config-default
	test -x $(BUILD_DIR)/test-rootfs-config-default/entry.sh
	grep -q '^exec /bin/sh$$' $(BUILD_DIR)/test-rootfs-config-default/entry.sh
	printf '%s\n' '#!/bin/sh' 'echo preserved' > \
		$(BUILD_DIR)/test-rootfs-config-default/entry.sh
	chmod 0755 $(BUILD_DIR)/test-rootfs-config-default/entry.sh
	printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config-default
	grep -q '^echo preserved$$' $(BUILD_DIR)/test-rootfs-config-default/entry.sh
	mkdir -p $(BUILD_DIR)/test-rootfs-config-entry-link \
		$(BUILD_DIR)/test-rootfs-config-entry-outside
	ln -s ../test-rootfs-config-entry-outside/entry.sh \
		$(BUILD_DIR)/test-rootfs-config-entry-link/entry.sh
	if printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config-entry-link; then exit 1; fi
	test ! -e $(BUILD_DIR)/test-rootfs-config-entry-outside/entry.sh
	mkdir -p $(BUILD_DIR)/test-rootfs-config-link \
		$(BUILD_DIR)/test-rootfs-config-outside
	ln -s ../test-rootfs-config-outside $(BUILD_DIR)/test-rootfs-config-link/etc
	if printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config-link; then exit 1; fi
	test ! -e $(BUILD_DIR)/test-rootfs-config-outside/burning-progress.conf
	if printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-config-link/; then exit 1; fi
	mkdir -p $(BUILD_DIR)/test-rootfs-invalid-config/etc/burning-progress.conf
	if printf '\n\n' | $(BIN_DIR)/burning-progress rootfs configure \
		$(BUILD_DIR)/test-rootfs-invalid-config; then exit 1; fi
	mkdir -p $(BUILD_DIR)/test-rootfs-pack-auto/rootfs/bin \
		$(BUILD_DIR)/test-rootfs-pack-auto/out
	printf '%s\n' '#!/bin/sh' > \
		$(BUILD_DIR)/test-rootfs-pack-auto/rootfs/bin/sh
	chmod 0755 $(BUILD_DIR)/test-rootfs-pack-auto/rootfs/bin/sh
	$(BIN_DIR)/burning-progress rootfs pack \
		$(BUILD_DIR)/test-rootfs-pack-auto/rootfs \
		--output $(BUILD_DIR)/test-rootfs-pack-auto/out/rootfs.cpio
	test -f $(BUILD_DIR)/test-rootfs-pack-auto/rootfs/sbin/burning-progress
	test -f $(BUILD_DIR)/test-rootfs-pack-auto/out/rootfs.cpio
	mkdir -p $(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/bin \
		$(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/sbin \
		$(BUILD_DIR)/test-rootfs-pack-refresh/out
	printf '%s\n' '#!/bin/sh' > \
		$(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/bin/sh
	chmod 0755 $(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/bin/sh
	printf '%s\n' stale > \
		$(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/sbin/burning-progress
	$(BIN_DIR)/burning-progress rootfs pack \
		$(BUILD_DIR)/test-rootfs-pack-refresh/rootfs \
		--output $(BUILD_DIR)/test-rootfs-pack-refresh/out/rootfs.cpio
	cmp -s $(BIN_DIR)/burning-progress \
		$(BUILD_DIR)/test-rootfs-pack-refresh/rootfs/sbin/burning-progress
	mkdir -p $(BUILD_DIR)/test-enable-check
	if $(BIN_DIR)/burning-progress --root $(BUILD_DIR)/test-enable-check enable; \
		then exit 1; fi

test: $(TEST_DIR)/test_config $(TEST_DIR)/test_install $(TEST_DIR)/test_cpio test-cli
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
