# IVSBox —— 顶层唯一构建入口
# 架构依据：T113运维终端-系统架构设计.md §15

TARGET  ?= host
CROSS   ?=
SYSROOT ?=
DEBUG   ?=
SANITIZE?=
ANALYZE ?=

BUILD_DIR := build/$(TARGET)

ifeq ($(TARGET),arm)
include mk/toolchain-tina-arm.mk
else
include mk/toolchain-host.mk
endif
include mk/rules.mk

# -----------------------------------------------------------------------------
# 版本注入（计划 M1-S2 第 4 件）
# 把 Git 短哈希以 -DIV_GIT_VER="<sha>" 传给全部编译单元。
# 脱离 .git 的离线构建（例如只把源码树拷到 VM 上编译）取不到哈希，退回 "unknown"，
# iv_version 据此降级，不影响编译。
# 注意引号写法：必须是 '"$(GIT_VER)"' —— 直接写 \"$(GIT_VER)\" 会被 shell 吃掉
# 双引号，gcc 收到的是未加引号的 token，宏值就不是字符串字面量了。
# -----------------------------------------------------------------------------
GIT_VER := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_VER := $(if $(strip $(GIT_VER)),$(strip $(GIT_VER)),unknown)
CFLAGS  += -DIV_GIT_VER='"$(GIT_VER)"'

# -----------------------------------------------------------------------------
# vendored 第三方库（third_party/<lib>/）
# -----------------------------------------------------------------------------
# 头文件搜索路径：让 src/modules/** 能 `#include "minmea.h"`。core/hal 不用它，
# 但这里是通用规则、按目录分流代价大，故统一放开 —— 纪律改由"目录可见性"守：
# 谁引了第三方，一眼能从 `#include` 的名字看出来（第三方头名不叫 iv_*）。
THIRD_PARTY_INC := third_party/minmea
CFLAGS += $(addprefix -I,$(THIRD_PARTY_INC))

# third_party 源码**按上游原样编译**：**去掉本工程的 `-Werror`**，否则为了编过
# 就得去改上游文件，而那会让"vendored 文件一字未改、可用 SHA256 复核"不再成立。
# 保留 `-Wall -Wextra` 只是为了看得见上游的警告（不阻塞构建）。
# 用递归展开（`=`）而不是立即展开（`:=`）：这样后续任何对 CFLAGS 的追加都会带进来。
CFLAGS_THIRD_PARTY = $(filter-out -Werror,$(CFLAGS))

# -----------------------------------------------------------------------------
# 源文件发现（非递归，按目录通配）
# -----------------------------------------------------------------------------
CORE_SRCS     := $(wildcard src/core/*.c)
HAL_SRCS      := $(wildcard src/hardware/*.c)
MODULES_SRCS  := $(wildcard src/modules/*.c src/modules/*/*.c)

# vendored 第三方源码（third_party/<lib>/*.c）编进 libivmodules —— 依赖方向允许
# modules 层使用外部代码（架构 §15.5），core / hal 仍然只允许 libc。
# 目前只有 minmea（NMEA 解析，M2-S2.5）：来源、commit 与 SHA256 见
# third_party/minmea/README.ivsbox.md。
THIRD_PARTY_SRCS := $(wildcard third_party/*/*.c)
MODULES_SRCS     += $(THIRD_PARTY_SRCS)
APP_SRCS      := $(wildcard src/app/*.c)

MEDIA_SRCS    := $(wildcard media/src/*.c)
WEB_SRCS      := $(wildcard web/src/*.c)
UPDATER_SRCS  := $(wildcard updater/src/*.c)
PROVISION_SRCS:= $(wildcard tools/provision/src/*.c)

TEST_SRCS     := $(wildcard tests/unit/*.c)
FUZZ_SRCS     := $(wildcard tests/fuzz/*.c)
ITEST_SRCS    := $(wildcard tests/integration/*.c)

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

TEST_OBJS     := $(patsubst tests/unit/%.c,$(BUILD_DIR)/tests/unit/%.o,$(TEST_SRCS))
FUZZ_OBJS     := $(patsubst tests/fuzz/%.c,$(BUILD_DIR)/tests/fuzz/%.o,$(FUZZ_SRCS))
ITEST_OBJS    := $(patsubst tests/integration/%.c,$(BUILD_DIR)/tests/integration/%.o,$(ITEST_SRCS))

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
ITEST_BINS    := $(patsubst tests/integration/%.c,$(BUILD_DIR)/tests/integration/%,$(ITEST_SRCS))

# -----------------------------------------------------------------------------
# 公共伪目标
# -----------------------------------------------------------------------------
.PHONY: all host arm test itest asan tsan fuzz analyze analyze-build package package-copy clean help

all: host

host:
	$(MAKE) TARGET=host build

arm:
	$(MAKE) TARGET=arm build

build: $(BINS) $(TEST_BINS) $(ITEST_BINS)

test:
	$(MAKE) TARGET=host $(TEST_BINS)
	@for t in $(TEST_BINS); do \
		echo "RUN $$t"; \
		"$$t" || exit 1; \
	done

# 集成测试。**刻意不并入 make test**：单测自包含（任何环境都能跑），而本目标
# 的前提是"外部服务已在跑"（ivsboxd 已在接听点监听）。并进 make test 会让 CI
# 因"没起服务"而假红，那是测试体系的坏味道。
#   VM：`build/host/ivsboxd &` 之后 `make itest`
#   板端：`/etc/init.d/S65ivsboxd start` 之后直接跑 build/arm/tests/integration/*
itest:
	$(MAKE) TARGET=host $(ITEST_BINS)
	@for t in $(ITEST_BINS); do \
		echo "RUN $$t"; \
		"$$t" || exit 1; \
	done

asan:
	$(MAKE) TARGET=host SANITIZE=1 BUILD_DIR=build/asan test

# TSan 只跑宿主机：armv7 交叉目标上 ThreadSanitizer 通常不可用。
# 与 asan 互斥（不能同时开），故用独立的 BUILD_DIR，避免复用旧 .o。
tsan:
	$(MAKE) TARGET=host SANITIZE=thread BUILD_DIR=build/tsan test

fuzz:
	$(MAKE) TARGET=host $(FUZZ_BINS)

analyze:
	$(MAKE) TARGET=host ANALYZE=1 BUILD_DIR=build/analyze analyze-build

# 必须在带 BUILD_DIR=build/analyze 的子 make 内展开 $(BINS)。若在外层
# analyze 配方中直接写 $(BINS)，make 会提前将它展开成 build/host/*，
# 子 make 随后只有 build/analyze/* 的规则，导致“没有规则可创建目标”。
analyze-build: $(BINS)

# S1-01：package 原先在外层 make 里直接 cp $(BINS)，而外层 TARGET 默认 host，
# 复制的是 x86 产物（arm 子 make 只影响它自己那次调用里的变量）。改为把复制
# 也丢进一次 TARGET=arm 的子 make（package-copy），并加 readelf 断言防再犯：
# 不是 ARM ELF 就打包失败，而不是悄悄发出一个跑不起来的包。
package: arm
	$(MAKE) TARGET=arm package-copy

package-copy:
	@mkdir -p packaging/bin packaging/scripts packaging/init.d
	@cp $(BINS) packaging/bin/
	@cp scripts/start_ivsbox.sh scripts/monitor.sh packaging/scripts/
	@cp scripts/init.d/S65ivsboxd packaging/init.d/
	@chmod +x packaging/scripts/start_ivsbox.sh packaging/scripts/monitor.sh packaging/init.d/S65ivsboxd
	@for bin in $(BINS); do \
		LC_ALL=C $(CROSS)readelf -h "$$bin" | grep -q "Machine:.*ARM" || { \
			echo "package: ERROR, not ARM ELF: $$bin"; exit 1; \
		}; \
	done
	@echo "Packaged ARM binaries + scripts -> packaging/"

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
	@echo "  tsan     - build and run tests with TSan (host only, exclusive with asan)"
	@echo "  itest    - build and run integration tests (requires a running ivsboxd)"
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
	@echo "  SANITIZE - 1=ASan/UBSan, thread=TSan (host only, mutually exclusive)"

# -----------------------------------------------------------------------------
# 模式规则与显式规则（依赖方向在链接顺序中显式写死）
# -----------------------------------------------------------------------------
# 保护单测/模糊测试的 .o 不被当垃圾清掉：
# 它们只由下面的通用模式规则生成，makefile 里没有显式规则提到它们，于是 make 把它们
# 判成"隐式规则链的中间文件"，在链出可执行文件后即删除。现象：make clean 后第一次构建
# 只有 11 个 .o，第二次才补齐 13 个，且每次 clean 后都要把单测白重编一遍。
# .PRECIOUS 只禁止删除、不改变其他任何语义。
# 注意：不要写成不带前置条件的 `.SECONDARY:` —— 那会让 make 认为所有目标都不需重编，
# `make arm` 会直接报"对 build 无需做任何事"而什么都不产出（2026-09-24 实测踩过）。
.PRECIOUS: $(TEST_OBJS) $(FUZZ_OBJS) $(ITEST_OBJS)

# 版本戳：只在 Git 短哈希真的变化时刷新 mtime，从触发相关 .o 重编。
# 少了这一步会出现"提交后哈希变了、二进制里仍是旧哈希"的静默不一致。
VER_STAMP := $(BUILD_DIR)/.gitver

$(VER_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo "$(GIT_VER)" | cmp -s - $@ || echo "$(GIT_VER)" > $@

FORCE:
.PHONY: FORCE

$(BUILD_DIR)/%.o: %.c $(VER_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# third_party 更有针对性（stem 更短），make 会优先匹配这条：
# 用不带 -Werror 的标志编译上游源码，理由见文件头 THIRD_PARTY_INC 那段注释。
$(BUILD_DIR)/third_party/%.o: third_party/%.c $(VER_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_THIRD_PARTY) -MMD -MP -c -o $@ $<

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

$(BUILD_DIR)/tests/unit/%: $(BUILD_DIR)/tests/unit/%.o $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

$(BUILD_DIR)/tests/integration/%: $(BUILD_DIR)/tests/integration/%.o $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

$(BUILD_DIR)/tests/fuzz/%: $(BUILD_DIR)/tests/fuzz/%.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< $(LDLIBS)

# -----------------------------------------------------------------------------
# 自动依赖
# -----------------------------------------------------------------------------
DEPS := $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
-include $(DEPS)
