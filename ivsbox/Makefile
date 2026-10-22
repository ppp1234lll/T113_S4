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
#
# 脱离 .git 的离线构建（例如只把源码树拷到 VM 上编译）取不到哈希，退回 "nogit"，
# iv_version 据此降级成 `1.0.0+nogit`，不影响编译。
#
# **D-11：降级只是把事实说清楚，不是解决**。板端产物必须可追溯，所以在非 git 仓
# 里构建**要显式注入**（GIT_VER 是普通赋值，命令行优先）：
#     make arm GIT_VER=$(git rev-parse --short HEAD)
# 忘了注入的后果不是编译失败，而是版本串变成 `+nogit`、`iv_version_git_valid()`
# 为 0 —— 计划 M-01「版本串与实际一致」会判失败，且现场查不到对应提交。
#
# 注意引号写法：必须是 '"$(GIT_VER)"' —— 直接写 \"$(GIT_VER)\" 会被 shell 吃掉
# 双引号，gcc 收到的是未加引号的 token，宏值就不是字符串字面量了。
# -----------------------------------------------------------------------------
GIT_VER := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_VER := $(if $(strip $(GIT_VER)),$(strip $(GIT_VER)),nogit)
CFLAGS  += -DIV_GIT_VER='"$(GIT_VER)"'

# 构建日期（YYYYMMDD）。
# 用途：平台 E3「查询设备版本号」应答里的 `sysver` 固定是 `1.0.0.<构建日期>`
# （用户 2026-10-20 拍板，对齐参考实现 SOFT_NO_STR 的"版本号+时间"格式）。
# 刻意不在源码里用 __DATE__：「Oct 20 2026」→「20261020」要在运行期换算，
# 而 iv_version 目前是零运行期状态（纯编译期常量、天然线程安全），不想为一个
# 日期串把它变成静态缓冲 + ready 标志。注入方式与 IV_GIT_VER 完全一致。
BUILD_DATE := $(shell date +%Y%m%d)
BUILD_DATE := $(if $(strip $(BUILD_DATE)),$(strip $(BUILD_DATE)),00000000)
CFLAGS     += -DIV_BUILD_DATE='"$(BUILD_DATE)"'

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
# `-fanalyzer` **只分析我们自己的代码**（2026-10-22 修 `make analyze` 不可用）
#
# 第三方与上游子工程的源码一律剥掉 `-fanalyzer`，理由不是"省事"，是三条硬事实：
#   ① **会 OOM**。`media/src/http/mongoose.c` 是单文件上万行的第三方 Web 服务器，
#      GCC 10/11 的 -fanalyzer 在这类"大函数 + 大量分支"文件上内存占用是数量级
#      放大。3.8 GiB 的构建 VM 上实测被内核 OOM killer 直接杀掉 cc1（日志只有
#      一行 `gcc: fatal error: 已杀死 signal terminated program cc1`，
#      `make analyze` 退出码 2）。这不是"慢"，是**跑不完**。
#   ② **分析了也没人去修**。`-fanalyzer` 的价值在于"查出我们会改的代码里的缺陷"；
#      上游代码我们不打算改（改了下次同步就冲突），它的告警只能被 ignore。
#   ③ **判据会失真**。旧口径用 `grep -c 'warning:'` 当判据，而 OOM 时这个计数
#      恰好是 0 —— 看起来"零告警"，其实一个文件都没分析完。这与 D-05
#      （`fuzz:` 从不执行）是同一类"门禁形同虚设"。
#
# 反过来，`src/`（core/hal/modules/app）**保留** `-fanalyzer` —— 那才是我们要
# 保证零缺陷的部分。`media/src/media/`、`media/src/http/http_server.c` 这类
# 我们自己写的儿子工程代码同样保留，只剥 pure 第三方那几处。
# -----------------------------------------------------------------------------
ANALYZE_OFF_PATHS = \
	$(BUILD_DIR)/media/src/http/mongoose.o \
	$(BUILD_DIR)/media/third_party/%.o \
	$(BUILD_DIR)/third_party/%.o

# 用中间变量而不是自引用赋值：`CFLAGS = $(filter-out -fanalyzer,$(CFLAGS))` 这种
# 写法要依赖 make 的"target-specific 右侧取全局值"规则才能成立，读起来像递归，
# 换个人维护迟早改成死循环。立即展开一次存起来，语义一目了然。
ANALYZE_CFLAGS_OFF := $(filter-out -fanalyzer,$(CFLAGS))

$(ANALYZE_OFF_PATHS): CFLAGS = $(ANALYZE_CFLAGS_OFF)
$(ANALYZE_OFF_PATHS): CFLAGS_THIRD_PARTY = $(filter-out -Werror,$(ANALYZE_CFLAGS_OFF))

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

# 媒体进程（M6-S6.0，收编自姊妹工程 bird_media，见 media/README.ivsbox.md）：
# main.c 在 media/ 根，业务源在 media/src/<media|http|utils>/*.c。
MEDIA_SRCS    := media/main.c $(wildcard media/src/*/*.c)
WEB_SRCS      := $(wildcard web/src/*.c)
UPDATER_SRCS  := $(wildcard updater/src/*.c)
PROVISION_SRCS:= $(wildcard tools/provision/src/*.c)

# bird_media 头文件路径（只给媒体对象用，见下面专属模式规则；不进全局 CFLAGS，
# 避免 ffmpeg/mongoose 的头名泄漏给主工程源码）。
MEDIA_INC := -Imedia/include -Imedia/src -Imedia/src/media -Imedia/src/http \
             -Imedia/third_party/ffmpeg/include

# ffmpeg 预编译静态库（随 bird_media 携带，已验架构：lib_arm=ARM EABI5 soft-float、
# lib_x86=x86-64；与 VM 交叉/宿主 gcc 匹配）。
ifeq ($(TARGET),arm)
FFMPEG_LIB_DIR := media/third_party/ffmpeg/lib_arm
else
FFMPEG_LIB_DIR := media/third_party/ffmpeg/lib_x86
endif
# 链接顺序与 bird_media 的 CMakeLists 一致（avformat 依赖 avcodec…）
FFMPEG_LIBS := $(FFMPEG_LIB_DIR)/libavformat.a \
               $(FFMPEG_LIB_DIR)/libavcodec.a \
               $(FFMPEG_LIB_DIR)/libavutil.a \
               $(FFMPEG_LIB_DIR)/libavdevice.a
# -lz（avformat 的 gzip 头）/ -ldl（avutil 可能 dlopen）：上游 CMake 同款

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

# 模糊测试：**编了还要真跑**。
#
# 原先这里只有 `$(MAKE) TARGET=host $(FUZZ_BINS)` —— 只编译、从不执行，于是
# 「随机灌流无崩溃、无越界」这条准出判据**从未被验证过**（D-05，2026-10-21）。
# 两处一起改：
#   ① 按 SANITIZE=1（ASan+UBSan）重编 —— 不这样编，越界/UAF 根本抓不到，
#      灌十小时也只是"没崩"；独立 BUILD_DIR=build/fuzz，不污染 build/host 的 .o。
#   ② 编完**逐个真跑**：`FUZZ_SEC` 控制时长（默认 10 s；0 = 只跑一轮自检；
#      出测试报告前用 `make fuzz FUZZ_SEC=600`）。任一个非零退出即整体失败。
FUZZ_SEC       ?= 10
FUZZ_BUILD_DIR ?= build/fuzz
FUZZ_RUN_BINS   = $(patsubst tests/fuzz/%.c,$(FUZZ_BUILD_DIR)/tests/fuzz/%,$(FUZZ_SRCS))

fuzz:
	$(MAKE) TARGET=host SANITIZE=1 BUILD_DIR=$(FUZZ_BUILD_DIR) $(FUZZ_RUN_BINS)
	@for f in $(FUZZ_RUN_BINS); do \
		echo "RUN $$f $(FUZZ_SEC)s"; \
		"$$f" $(FUZZ_SEC) || exit 1; \
	done
	@echo "fuzz OK: $(words $(FUZZ_RUN_BINS)) 个 harness × $(FUZZ_SEC)s，ASan/UBSan 零报告"

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
	@cp scripts/start_ivsbox.sh scripts/monitor.sh scripts/monitor-web.sh scripts/monitor-media.sh packaging/scripts/
	@cp scripts/ivsbox-run.sh packaging/scripts/
	# 受控远程通道的 frpc 起停脚本（M8；monitor.sh 按标记文件调它）
	@cp scripts/frp.sh packaging/scripts/
	@cp scripts/init.d/S62usb4g scripts/init.d/S65ivsboxd scripts/init.d/S66ivsboxweb scripts/init.d/S67ivsboxmedia packaging/init.d/
	# Web 静态资源（M5-S5.3）：模板与数据分离，页面改版不必重编固件。
	@rm -rf packaging/web
	@mkdir -p packaging/web/static
	@cp -r web/static/. packaging/web/static/ 2>/dev/null || true
	@chmod +x packaging/scripts/start_ivsbox.sh packaging/scripts/monitor.sh packaging/scripts/monitor-web.sh packaging/scripts/monitor-media.sh packaging/scripts/ivsbox-run.sh packaging/scripts/frp.sh packaging/init.d/S62usb4g packaging/init.d/S65ivsboxd packaging/init.d/S66ivsboxweb packaging/init.d/S67ivsboxmedia
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
	@echo "  fuzz     - build (ASan) and RUN fuzz harnesses, FUZZ_SEC=10"
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

# 版本戳：只在 Git 短哈希/构建日期真的变化时刷新 mtime，从触发相关 .o 重编。
# 少了这一步会出现"提交后哈希变了、二进制里仍是旧哈希"的静默不一致；
# 同理，跨天重编若不换戳，`-DIV_BUILD_DATE` 变了但 .o 不重编，平台查到的
# sysver 日期会一直停在第一次编译那天。
VER_STAMP := $(BUILD_DIR)/.gitver
VER_TAG   := $(GIT_VER)-$(BUILD_DATE)

$(VER_STAMP): FORCE
	@mkdir -p $(dir $@)
	@echo "$(VER_TAG)" | cmp -s - $@ || echo "$(VER_TAG)" > $@

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

# 媒体进程对象（M6-S6.0）：bird_media 收编源码**按上游原样编译**（同 third_party
# 纪律：去 -Werror 保留 -Wall -Wextra），加 MEDIA_INC；stem 比 %.o 更短，优先匹配。
$(BUILD_DIR)/media/%.o: media/%.c $(VER_STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_THIRD_PARTY) $(MEDIA_INC) -MMD -MP -c -o $@ $<

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

# 媒体进程（M6-S6.0）：ffmpeg 静态库链在 LDLIBS 之前（静态库依赖动态库的位置
# 纪律）；-lz -ldl 是 avformat/avutil 的伴随依赖（上游 CMake 同款）。
# 注意前置只放 $(MEDIA_OBJS)——$^ 会全部喂给链接行，版本戳等辅助文件不可挂这里
# （否则 ld 把 .gitver 当输入文件报 syntax error）。
$(MEDIA_BIN): $(MEDIA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) $(LDLIBS) -lz -ldl

# Web 进程要用 libivcore 的本地通道客户端（iv_chan）与日志（iv_log），故必须链
# libivcore。**刻意不链 libivmodules**：Web 不直接碰配置 / 数据库，一切数据都发本地
# 通道问 ivsboxd（架构 §10.4 单写者原则）——链上 modules 就等于把"能直接开库"的
# 口子留给了 Web，"单写者"就成了纸面约定。
$(WEB_BIN): $(WEB_OBJS) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(WEB_OBJS) -L$(BUILD_DIR) -livcore $(LDLIBS)

# 升级器要调 libivmodules 里的 iv_upd（安装/双版本/回滚）与 iv_ota（验签），
# 故链三层库；`--gc-sections` 会把没用到的部分丢掉，不会把整个 modules 拖进产物。
# 注意：链了 modules 就意味着 libivsboxd-updater 的运行期依赖里多了 libcrypto
# （iv_ota 用它做 Ed25519），与 ivsboxd 自身是否用 OTA 无关。
$(UPDATER_BIN): $(UPDATER_OBJS) $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(UPDATER_OBJS) -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

# 出厂工装要用 libivmodules 里的身份编解码 / 裸分区读写（iv_provision），故链整个
# modules 静态库；`--gc-sections` 会把没用到的部分丢掉，不会把 sqlite3/json-c 的
# 代码实际带进最终二进制（依赖仍是动态库，板端已具备）。
$(PROVISION_BIN): $(PROVISION_OBJS) $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $(PROVISION_OBJS) -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

$(BUILD_DIR)/tests/unit/%: $(BUILD_DIR)/tests/unit/%.o $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

$(BUILD_DIR)/tests/integration/%: $(BUILD_DIR)/tests/integration/%.o $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

# fuzz 目标链接三个静态库：fuzz harness 会调用被测模块（如 fuzz_frame → iv_frame）。
# 原先不链库是"没有 fuzz 用例"时期的占位写法，首个 fuzz 用例落地后必须带上。
$(BUILD_DIR)/tests/fuzz/%: $(BUILD_DIR)/tests/fuzz/%.o $(LIBMODULES) $(LIBHAL) $(LIBCORE)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) -o $@ $< -L$(BUILD_DIR) -livmodules -livhal -livcore $(LDLIBS)

# -----------------------------------------------------------------------------
# 自动依赖
# -----------------------------------------------------------------------------
DEPS := $(shell find $(BUILD_DIR) -name '*.d' 2>/dev/null)
-include $(DEPS)
