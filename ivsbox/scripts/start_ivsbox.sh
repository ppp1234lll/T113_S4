#!/bin/sh
# IVSBox 启动入口（M1-S10）
# 后台启动 monitor.sh，由 monitor 负责拉起/守护 ivsboxd。

nohup /mnt/UDISK/ivsbox/current/scripts/monitor.sh >/dev/null 2>&1 &
