# IVSBox —— 顶层唯一构建入口
# 架构依据：T113运维终端-系统架构设计.md §15

TARGET  ?= host
CROSS   ?=
SYSROOT ?=
DEBUG   ?=
SANITIZE?=
ANALYZE ?=

BUILD_DIR := build/$(TARGET)

include mk/toolchain-$(TARGET).mk
include mk/rules.mk

# -----------------------------------------------------------------------------
# 源文件发现（非递归，按目录通配）
# -----------------------------------------------------------------------------
CORE_SRCS     := $(wildcard src/core/*.c)
HAL_SRCS      := $(wildcard src/hal/*.c)
MODULES_SRCS  := $(wildcard src/modules/*/*.c)
APP_SRCS      := $(wildcard src/app/*.c)

MEDIA_SRCS    := $(wildcard media/src/*.c)
WEB_SRCS      := $(wildcard web/src/*.c)
UPDATER_SRCS  := $(wildcard updater/src/*.c)
PROVISION_SRCS:= $(wildcard tools/provision/src/*.c)

TEST_SRCS     := $(wildcard tests/unit/*.c)
FUZZ_SRCS     := $(wildcard tests/fuzz/*.c)

# -----------------------------------------------------------------------------
# 对象文件
# -----------------------------------------------------------------------------
CORE_OBJS     := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
HAL_OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(HAL_SRCS))
MODULES_OBJS  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(MODULES_SRCS))
APP_OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SRCS))

MEDIA_OBJS    := $(patsubst %.c,$(BUILD_DIR)/%.o,$(MEDIA_SRCS))
WEB_OBJS      := $(patsubst %.c,$(BUILD_DIR)/%.o,$(WEB_SRCS))
UPDATER_OBJS  := $(patsubst %.c,$(BUILD_DIR)/%.o,$(UPDATER_SRCS))
PROVISION_OBJS:= $(patsubst %.c,$(BUILD_DIR)/%.o,$(PROVISION_SRCS))

# -----------------------------------------------------------------------------
# 目标产物
# -----------------------------------------------------------------------------
LIBCORE       := $(BUILD_DIR)/libivcore.a
LIBHAL        := $(BUILD_DIR)/libivhal.a
LIBMODULES    := $(BUILD_DIR)/libivmodules.a

APP_BINS      := $(BUILD_DIR)/ivsboxd
MEDIA_BIN     := $(BUILD_DIR)/ivsbox-media
WEB_BIN       := $(BUILD_DIR)/ivsbox-web
UPDATER_BIN   := $(BUILD_DIR)/ivsbox-updater
PROVISION_BIN := $(BUILD_DIR)/ivsbox-provision

BINS          := $(APP_BINS)
ifneq ($(MEDIA_SRCS),)
BINS += $(MEDIA_BIN)
endif
ifneq ($(WEB_SRCS),)
BINS += $(WEB_BIN)
endif
ifneq ($(UPDATER_SRCS),)
BINS += $(UPDATER_BIN)
endif
ifneq ($(PROVISION_SRCS),)
BINS += $(PROVISION_BIN)
endif

TEST_BINS     := $(patsubst tests/unit/%.c,$(BUILD_DIR)/tests/unit/%,$(TEST_SRCS))
FUZZ_BINS     := $(patsubst tests/fuzz/%.c,$(BUILD_DIR)/tests/fuzz/%,$(FUZZ_SRCS))

# -----------------------------------------------------------------------------
# 公共伪目标
# -----------------------------------------------------------------------------
.PHONY: all host arm test asan fuzz analyze package clean help

all: host

host:
	$(MAKE) TARGET=host build

arm:
	$(MAKE) TARGET=arm build

build: $(BINS) $(TEST_BINS)

test:
	$(MAKE) TARGET=host $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "RUN $$t"; \
		"$$t" || exit 1; \
	done

asan:
	$(MAKE) TARGET=host SANITIZE=1 test

fuzz:
	$(MAKE) TARGET=host $(FUZZ_BINS)

analyze:
	$(MAKE) TARGET=host ANALYZE=1 $(BINS)

package: arm
	@mkdir -p packaging
	@cp $(BINS) packaging/
	@echo "Packaged ARM binaries to packaging/"

clean:
	rm -rf build/ packaging/

help:
	@echo "IVSBox Makefile"
	@echo ""
	@echo "Usage: make [TARGET=host|arm] [CROSS=prefix] [SYSROOT=path] [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all      - host build (default)"
	@echo "  host     - host build"
	@echo "  arm      - ARM cross build"
	@echo "  test     - build and run unit tests"
	@echo "  asan     - build and run tests with ASan/UBSan"
	@echo "  fuzz     - build fuzz targets"
	@echo "  analyze  - build with GCC -fanalyzer"
	@echo "  package  - copy ARM binaries to packaging/"
	@echo "  clean    - remove build/ and packaging/"
	@echo "  help     - show this help"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS    - cross toolchain prefix (arm default: arm-linux-gnueabi-)"
	@echo "  SYSROOT  - cross compile sysroot"
	@echo "  DEBUG    - 1=enable -g -O0, 0=disable"
	@echo "  SANITIZE - 1=enable ASan/UBSan"

# -----------------------------------------------------------------------------
# 模式规则与显式规则（依赖方向在链接顺序中显式写死）
# -----------------------------------------------------------------------------
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(LIBCORE): $(CORE_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LIBHAL): $(HAL_OBJS) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(HAL_OBJS)

$(LIBMODULES): $(MODULES_OBJS) $(LIBCORE) $(LIBHAL)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(MODULES_OBJS)

$(APP_BINS): $(APP_OBJS) $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJS) -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

$(MEDIA_BIN): $(MEDIA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(WEB_BIN): $(WEB_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(UPDATER_BIN): $(UPDATER_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(PROVISION_BIN): $(PROVISION_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/tests/unit/%: $(BUILD_DIR)/tests/unit/%.o $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livcore $(LDLIBS)

$(BUILD_DIR)/tests/fuzz/%: $(BUILD_DIR)/tests/fuzz/%.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS)

# -----------------------------------------------------------------------------
# 自动依赖
# -----------------------------------------------------------------------------
DEPS := $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
-include $(DEPS)
