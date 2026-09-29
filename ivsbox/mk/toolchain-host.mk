# Host 工具链配置
# 默认宿主机 gcc，用于单元测试、ASan/UBSan、静态分析

CC  := gcc
AR  := ar

CFLAGS_COMMON  := -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS_COMMON := -Wl,--gc-sections -Wl,--no-undefined

# 主机侧启用 _GNU_SOURCE，便于本机跑单测（clock_gettime、signalfd 等）
CFLAGS  := $(CFLAGS_COMMON) -D_GNU_SOURCE -Iinclude
LDFLAGS := $(LDFLAGS_COMMON)
# -lpthread：libivmodules 里的 iv_taskpool（M1-S5）需要 pthread。
# 位置在链接行末尾（... -livcore $(LDLIBS)），对系统库是正确位置。
# -ljson-c：M1-S8 配置模块（src/modules/config/，进 libivmodules）需要。
# 本变量被全部 13 处链接行共用（主程序 + 全部单测 + fuzz），落这一处即全生效。
# 宿主头文件来自 libjson-c-dev 0.15（libjson-c.so -> .so.5）；板端 sysroot 是
# json-c 0.13.1（libjson-c.so -> .so.4）⇒ 代码只准用 0.13 的 API 子集。
LDLIBS  := -lpthread -ljson-c

# 默认 host 带调试信息
ifeq ($(DEBUG),)
DEBUG := 1
endif

ifeq ($(DEBUG),1)
CFLAGS += -g -O0
else
CFLAGS += -O2
endif

# 消毒器：SANITIZE=<值> 直接透传给 -fsanitize=
#   SANITIZE=1        -> address,undefined（asan 目标的旧口径，保留兼容）
#   SANITIZE=thread   -> TSan（tsan 目标，只跑并发相关的用例）
#   **ASan 与 TSan 不能同时启用**，必须二选一；-fanalyzer 与消毒器也别混用。
ifeq ($(SANITIZE),1)
CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
else ifeq ($(SANITIZE),thread)
CFLAGS  += -fsanitize=thread -g
LDFLAGS += -fsanitize=thread -g
endif

ifeq ($(ANALYZE),1)
CFLAGS += -fanalyzer
endif
