# Host 工具链配置
# 默认宿主机 gcc，用于单元测试、ASan/UBSan、静态分析

CC  := gcc
AR  := ar

CFLAGS_COMMON  := -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS_COMMON := -Wl,--gc-sections -Wl,--no-undefined

# 主机侧启用 _GNU_SOURCE，便于本机跑单测（clock_gettime、signalfd 等）
CFLAGS  := $(CFLAGS_COMMON) -D_GNU_SOURCE -Iinclude
LDFLAGS := $(LDFLAGS_COMMON)
LDLIBS  :=

# 默认 host 带调试信息
ifeq ($(DEBUG),)
DEBUG := 1
endif

ifeq ($(DEBUG),1)
CFLAGS += -g -O0
else
CFLAGS += -O2
endif

ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

ifeq ($(ANALYZE),1)
CFLAGS += -fanalyzer
endif
