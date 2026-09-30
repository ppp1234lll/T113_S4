#!/bin/sh
# IVSBox 崩溃自拉起守护循环（M1-S10）
# init 脚本只负责开机拉起一次；本循环每 5 秒检查 ivsboxd 是否存活，
# 不在就重新拉起。sysvinit 没有 procd 的 respawn，这层是必须的。

IVSBOX_HOME=/mnt/UDISK/ivsbox
CURRENT=$IVSBOX_HOME/current

while true; do
    if ! ps -o comm | grep -x ivsboxd >/dev/null 2>&1; then
        cd "$CURRENT" && ./bin/ivsboxd >/dev/null 2>&1 &
    fi

    if [ -f /tmp/ivsbox-upgrade.ready ]; then
        # OTA 触发点：后续由 OTA 模块实现，当前保留占位
        :
    fi

    sleep 5
done
