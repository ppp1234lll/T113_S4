#!/bin/sh
# IVSBox 崩溃自拉起守护循环（M1-S10）
# init 脚本只负责开机拉起一次；本循环每 5 秒检查 ivsboxd 是否存活，
# 不在就重新拉起。sysvinit 没有 procd 的 respawn，这层是必须的。

IVSBOX_HOME=/mnt/UDISK/ivsbox
CURRENT=$IVSBOX_HOME/current

while true; do
    if ! ps -o comm | grep -x ivsboxd >/dev/null 2>&1; then
        # 用 `( ... ) &` + `exec`：子 shell 被 exec 成 ivsboxd 本体，$! 即 ivsboxd 的真实 pid。
        # 不能写 `cd "$CURRENT" && ./bin/ivsboxd ... &` —— `&` 作用于整条 AND 列表，
        # 会多留一个常驻的 monitor.sh 空壳进程，且 $! 拿到的是空壳而不是 ivsboxd 的 pid。
        ( cd "$CURRENT" && exec ./bin/ivsboxd >/dev/null 2>&1 ) &
        # 回写 pidfile：monitor 才是重启方，必须让 /var/run/ivsboxd.pid 始终指向真实进程。
        # 不写这一步，崩溃重启后 pidfile 仍是旧 pid，init 脚本的 stop 会打空、停不掉服务。
        echo $! > /var/run/ivsboxd.pid
    fi

    if [ -f /tmp/ivsbox-upgrade.ready ]; then
        # OTA 触发点：后续由 OTA 模块实现，当前保留占位
        :
    fi

    sleep 5
done
