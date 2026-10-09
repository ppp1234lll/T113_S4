#!/bin/sh
# IVSBox 启动入口（M1-S10）
# 后台启动 monitor.sh，由 monitor 负责拉起/守护 ivsboxd。
# 程序（bin/scripts）自 2026-09-30 起落 rootfs 的 /opt/ivsbox，不再放 UDISK（见计划 §0）。

nohup /opt/ivsbox/scripts/monitor.sh >/dev/null 2>&1 &
