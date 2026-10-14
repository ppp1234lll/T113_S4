# ARM 交叉工具链配置（天嵌 TQT113 / Tina Linux）
# 工具链前缀可通 CROSS= 覆盖，默认 arm-linux-gnueabi-

ifeq ($(CROSS),)
CROSS := arm-linux-gnueabi-
endif

CC := $(CROSS)gcc
AR := $(CROSS)ar

CFLAGS_COMMON  := -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS_COMMON := -Wl,--gc-sections -Wl,--no-undefined

# 板端同样启用 _GNU_SOURCE：严格 -std=c11 下 localtime_r / clock_gettime 等 POSIX
# 接口不暴露（计划 §0 通用规则），核心层需要它们
CFLAGS  := $(CFLAGS_COMMON) -D_GNU_SOURCE -Iinclude
LDFLAGS := $(LDFLAGS_COMMON)
# -lpthread：libivmodules 里的 iv_taskpool（M1-S5）需要 pthread。
# **板端不能省**：glibc 2.25 的 pthread 符号在独立 libpthread 里，漏加直接链接失败；
# 而宿主机的新 glibc（>=2.34）已把 pthread 并入 libc，不加也能过
# —— 典型的"VM 绿、板端挂"。
# -ljson-c：M1-S8 配置模块需要。sysroot 里只有 libjson-c.so / .so.4、**没有 .a**
# ⇒ 板端对它是动态依赖，运行时由 rootfs 的 libjson-c.so.4 提供（已确认存在）。
LDLIBS  := -lpthread -ljson-c -lsqlite3 -lm -lcrypto
# -lm：M2-S2.5 的 iv_gps 用 cos/sqrt 做位移判定的近似换算（equirectangular）。
# 板端 rootfs 的 glibc 自带 libm.so.6（已随 libjson-c / libsqlite3 一并在板端验证）。
# -lcrypto：M7-S7.1 的 OTA 验签用 OpenSSL EVP 的 Ed25519。**版本敏感**：Ed25519
# 要 OpenSSL >= 1.1.1。sysroot 实测（2026-10-14）
#   /opt/EmbedSky/Tina/arm-buildroot-linux-gnueabi/sysroot/usr/lib/libcrypto.so.1.1
#   OPENSSL_VERSION_TEXT "OpenSSL 1.1.1a"、usr/include/openssl/evp.h 有 EVP_PKEY_ED25519
# ⇒ 交叉链接可用。**板端 rootfs 是否装着 libcrypto.so.1.1 待现场核对**（板子当时不在线），
# 已登记在修改记录；若板端缺库，改用 sysroot 里的 libcrypto.a 静态链（同目录已具备）。

ifdef SYSROOT
CFLAGS  += --sysroot=$(SYSROOT)
LDFLAGS += --sysroot=$(SYSROOT)
endif

# 板端默认 Release
ifeq ($(DEBUG),)
DEBUG := 0
endif

ifeq ($(DEBUG),1)
CFLAGS += -g -O0
else
CFLAGS += -O2
endif
