# ARM 交叉工具链配置（天嵌 TQT113 / Tina Linux）
# 工具链前缀可通 CROSS= 覆盖，默认 arm-linux-gnueabi-

ifeq ($(CROSS),)
CROSS := arm-linux-gnueabi-
endif

CC := $(CROSS)gcc
AR := $(CROSS)ar

CFLAGS_COMMON  := -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS_COMMON := -Wl,--gc-sections -Wl,--no-undefined

CFLAGS  := $(CFLAGS_COMMON) -Iinclude
LDFLAGS := $(LDFLAGS_COMMON)
LDLIBS  :=

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
