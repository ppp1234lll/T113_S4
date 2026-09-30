#!/bin/sh
# IVSBox 崩溃自拉起守护循环（M1-S10，2026-09-30 补缺口清单 G8 熔断）
#
# sysvinit 没有 procd 的 respawn，这层是必须的：init 脚本只负责开机拉起一次，
# 之后由本循环每 5 秒检查 ivsboxd 是否存活，不在就重拉。
#
# ============================ 为什么必须带熔断（G8） ============================
# 没有熔断时，"进程反复起不来"与"设备一切正常"在日志里几乎一个样 —— 都是安静的，
# 只有一行行重复的启动记录。于是现场看到的是"设备在用"，实际是崩溃循环。
#
# 超限后进入 **failsafe**：**不再重拉**，并把告警落盘（先 fsync）。此时没有人持有
# /dev/watchdog，内核会在约 16 s 后复位整机 —— 这是刻意的"硬件兜底"，与计划 §S6
# 的纪律一致（判死路径绝不写 'V'）。复位后本脚本重新启动，从**持久**状态文件里
# 读到仍在窗口内的崩溃历史，**继续 failsafe**，不会变成"每 16 s 重启一次"的无尽
# 循环。failsafe 到期（默认 30 分钟）后自动清零、重新试一轮。
#
# 状态文件放在 UDISK 而不是 /var/run：/var/run 是 tmpfs，整机复位即丢失，
# 那样跨复位的崩溃历史就无从累计，熔断形同虚设。
#
# 人工恢复：`/etc/init.d/S65ivsboxd restart`（restart 会清状态文件），
# 或直接删除状态文件后重启服务。
#
# ============================ 可调参数（环境变量覆盖，便于测试） ============================
#   IVSBOX_MAX_RESTARTS        窗口内允许的重拉次数（默认 5）
#   IVSBOX_RESTART_WINDOW      统计窗口秒数（默认 300）
#   IVSBOX_FAILSAFE_COOLDOWN   failsafe 持续秒数（默认 1800）
#   IVSBOX_POLL_S              轮询间隔秒数（默认 5）
#   注意：改短这些值只用于验证（例如把上限设成 2、窗口 30 s），生产保持默认。

IVSBOX_HOME=/mnt/UDISK/ivsbox
CURRENT=$IVSBOX_HOME/current
STATE=$IVSBOX_HOME/.monitor.state
ALERT=/mnt/UDISK/log/monitor-alert.log

MAX_RESTARTS=${IVSBOX_MAX_RESTARTS:-5}
WINDOW_S=${IVSBOX_RESTART_WINDOW:-300}
COOLDOWN_S=${IVSBOX_FAILSAFE_COOLDOWN:-1800}
POLL_S=${IVSBOX_POLL_S:-5}

# 状态文件是单行三列：`<window_start_epoch> <restarts> <failsafe_until_epoch>`。
# 全部用整数 epoch 秒，避免依赖日期格式；读入后逐列做"是不是纯数字"的兜底，
# 状态文件被写坏时按"零历史"处理，绝不让一个坏文件把守护循环卡死。
WS=0
RC=0
FS=0

state_read() {
    if [ -f "$STATE" ]; then
        read -r WS RC FS < "$STATE" 2>/dev/null || true
    fi
    case "$WS" in ''|*[!0-9]*) WS=0 ;; esac
    case "$RC" in ''|*[!0-9]*) RC=0 ;; esac
    case "$FS" in ''|*[!0-9]*) FS=0 ;; esac
}

# 写状态并 **sync**：本文件的唯一价值就是"跨整机复位存活"，
# 停在 page cache 里等于没写（计划 §S6 探针 v1 的踩坑记录：只 fflush 不 fsync，
# 复位后文件是 0 字节）。写入频率极低（每次重拉 / 每次状态跃迁），代价可接受。
state_write() {
    printf '%s %s %s\n' "$1" "$2" "$3" > "$STATE" 2>/dev/null || true
    sync 2>/dev/null || true
}

alert() {
    printf '%s monitor: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >> "$ALERT" 2>/dev/null || true
    sync 2>/dev/null || true
}

ivsboxd_alive() {
    ps -o comm | grep -x ivsboxd >/dev/null 2>&1
}

state_read
echo "$(date '+%Y-%m-%d %H:%M:%S') monitor: started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s)" \
    >> "$ALERT" 2>/dev/null || true

while true; do
    now=$(date +%s)

    # 窗口过期 ⇒ 历史清零。设备稳定跑了整个窗口之后再崩一次，不必按"循环"论处。
    if [ "$FS" -eq 0 ] && [ $((now - WS)) -ge "$WINDOW_S" ]; then
        WS=$now
        RC=0
        state_write "$WS" "$RC" "$FS"
    fi

    # failsafe 未到期：不重拉、不喂狗，等硬件兜底或人工介入。
    if [ "$FS" -gt "$now" ]; then
        sleep "$POLL_S"
        continue
    fi

    # failsafe 到期：清零并回到正常守护，给它一轮新的机会。
    if [ "$FS" -ne 0 ]; then
        alert "failsafe expired after ${COOLDOWN_S}s, resuming normal supervision"
        WS=$now
        RC=0
        FS=0
        state_write "$WS" "$RC" "$FS"
    fi

    if ! ivsboxd_alive; then
        if [ "$RC" -ge "$MAX_RESTARTS" ]; then
            FS=$((now + COOLDOWN_S))
            state_write "$WS" "$RC" "$FS"
            alert "CRASH LOOP: ${RC} restart(s) within ${WINDOW_S}s reached the limit ${MAX_RESTARTS} -- entering failsafe for ${COOLDOWN_S}s, ivsboxd will NOT be restarted; the hardware watchdog will reset the board (no one holds it now)"
        else
            RC=$((RC + 1))
            state_write "$WS" "$RC" "$FS"
            # 用 `( ... ) &` + `exec`：子 shell 被 exec 成 ivsboxd 本体，$! 即 ivsboxd 的真实 pid。
            # 不能写 `cd "$CURRENT" && ./bin/ivsboxd ... &` —— `&` 作用于整条 AND 列表，
            # 会多留一个常驻的 monitor.sh 空壳进程，且 $! 拿到的是空壳而不是 ivsboxd 的 pid。
            ( cd "$CURRENT" && exec ./bin/ivsboxd >/dev/null 2>&1 ) &
            # 回写 pidfile：monitor 才是重启方，必须让 /var/run/ivsboxd.pid 始终指向真实进程。
            # 不写这一步，崩溃重启后 pidfile 仍是旧 pid，init 脚本的 stop 会打空、停不掉服务。
            echo $! > /var/run/ivsboxd.pid
        fi
    fi

    if [ -f /tmp/ivsbox-upgrade.ready ]; then
        # OTA 触发点：后续由 OTA 模块实现，当前保留占位
        :
    fi

    sleep "$POLL_S"
done
