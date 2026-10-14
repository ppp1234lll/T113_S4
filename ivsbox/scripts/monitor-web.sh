#!/bin/sh
# IVSBox Web 服务守护循环（M5-S5.1）
#
# 与 monitor.sh（ivsboxd 的守护）**刻意分开**：ivsbox-web 是附属进程，崩了不影响
# 采集/上报主链路，因此：
#   - 不共用状态文件、不共用 pidfile（否则一方的崩溃计数会算到另一方头上）；
#   - **不碰看门狗**（看门狗是 ivsboxd 独占的整机兜底，Web 崩溃不该触发整机复位）。
#
# ============ 与 monitor.sh 共有的纪律（细节见 monitor.sh 文件头） ============
# 1) 计时一律用 /proc/uptime 的**单调秒**，不用墙钟 date +%s（开机时钟不可信、对时会跳变）。
# 2) 熔断状态落 UDISK（/var/run 是 tmpfs，复位即丢），写后 sync。
# 3) 本脚本是该进程的**唯一拉起方**：init 脚本只拉起本脚本，不直接拉起 ivsbox-web，
#    否则复位后 init 送的一轮会让跨复位熔断失效。
#
# Web 进程本身没有看门狗，崩溃只表现为"页面打不开"，所以这里的熔断更朴素：窗口内
# 重拉超限 ⇒ 进 failsafe 停止拉起并告警落盘，冷却到期自动恢复。
#
# ============ 可调参数（环境变量覆盖，便于验证） ============
#   IVSBOX_WEB_MAX_RESTARTS     窗口内允许的重拉次数（默认 5）
#   IVSBOX_WEB_RESTART_WINDOW   统计窗口秒数（默认 300）
#   IVSBOX_WEB_COOLDOWN         failsafe 持续秒数（默认 600，比主控的 1800 短：
#                               Web 是附属服务，恢复要更快）
#   IVSBOX_WEB_POLL_S           轮询间隔秒数（默认 5）
#   IVSBOX_APP_DIR / IVSBOX_LOG_ROOT / IVSBOX_WEB_PIDFILE  路径覆盖（仅验证用）

IVSBOX_HOME=${IVSBOX_HOME:-/mnt/UDISK/ivsbox}
IVSBOX_APP_DIR=${IVSBOX_APP_DIR:-/opt/ivsbox}
IVSBOX_LOG_ROOT=${IVSBOX_LOG_ROOT:-/mnt/UDISK/log}
IVSBOX_WEB_PIDFILE=${IVSBOX_WEB_PIDFILE:-/var/run/ivsbox-web.pid}
# 静态资源落点：与 ivsbox-web 的 --root 默认值、packaging/web/static 保持一致。
IVSBOX_WEB_ROOT=${IVSBOX_WEB_ROOT:-/opt/ivsbox/web/static}
STATE=$IVSBOX_HOME/.monitor-web.state
ALERT=$IVSBOX_LOG_ROOT/monitor-alert.log

MAX_RESTARTS=${IVSBOX_WEB_MAX_RESTARTS:-5}
WINDOW_S=${IVSBOX_WEB_RESTART_WINDOW:-300}
COOLDOWN_S=${IVSBOX_WEB_COOLDOWN:-600}
POLL_S=${IVSBOX_WEB_POLL_S:-5}

WS=0
RC=0
FS=0

mono_now() {
    cut -d. -f1 /proc/uptime 2>/dev/null
}

state_read() {
    if [ -f "$STATE" ]; then
        read -r WS RC FS < "$STATE" 2>/dev/null || true
    fi
    case "$WS" in ''|*[!0-9]*) WS=0 ;; esac
    case "$RC" in ''|*[!0-9]*) RC=0 ;; esac
    case "$FS" in ''|*[!0-9]*) FS=0 ;; esac
}

state_write() {
    printf '%s %s %s\n' "$1" "$2" "$3" > "$STATE" 2>/dev/null || true
    sync 2>/dev/null || true
}

alert() {
    printf '%s monitor-web: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >> "$ALERT" 2>/dev/null || true
    sync 2>/dev/null || true
}

# 判活用 pidfile + /proc 存在性，不用 `ps | grep`：进程名与参数都可能是 ivsbox-web，
# 抓错目标就会"永远判活"或"永远判死"。pidfile 由本脚本回写，是唯一权威来源。
web_alive() {
    [ -f "$IVSBOX_WEB_PIDFILE" ] || return 1
    read -r pid < "$IVSBOX_WEB_PIDFILE" 2>/dev/null || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    # /proc/<pid>/comm 必须是本进程名，避免 pid 被复用后误判为"还活着"。
    [ -r "/proc/$pid/comm" ] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "ivsbox-web" ] || return 1
    return 0
}

state_read
alert "web monitor started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s)"

# 跨开机消毒（与 monitor.sh 同口径）：uptime 归零后状态文件里的旧值不可直接比较。
boot=$(mono_now)
case "$boot" in ''|*[!0-9]*) boot=0 ;; esac
if [ "$FS" -ne 0 ]; then
    FS=$((boot + COOLDOWN_S))
    alert "web monitor resuming failsafe from persisted state, cooldown restarted (${COOLDOWN_S}s), ivsbox-web will NOT be started"
    state_write "$WS" "$RC" "$FS"
elif [ "$WS" -gt "$boot" ]; then
    WS=$boot
    RC=0
    state_write "$WS" "$RC" "$FS"
fi

while true; do
    now=$(mono_now)
    case "$now" in ''|*[!0-9]*) sleep "$POLL_S"; continue ;; esac

    if [ "$FS" -eq 0 ] && [ $((now - WS)) -ge "$WINDOW_S" ]; then
        WS=$now
        RC=0
        state_write "$WS" "$RC" "$FS"
    fi

    if [ "$FS" -gt "$now" ]; then
        sleep "$POLL_S"
        continue
    fi

    if [ "$FS" -ne 0 ]; then
        alert "web failsafe expired after ${COOLDOWN_S}s, resuming normal supervision"
        WS=$now
        RC=0
        FS=0
        state_write "$WS" "$RC" "$FS"
    fi

    if web_alive; then
        RC=0
    else
        if [ "$RC" -ge "$MAX_RESTARTS" ]; then
            FS=$((now + COOLDOWN_S))
            state_write "$WS" "$RC" "$FS"
            alert "web CRASH LOOP: ${RC} restart(s) within ${WINDOW_S}s reached the limit ${MAX_RESTARTS} -- entering failsafe for ${COOLDOWN_S}s, ivsbox-web will NOT be started"
        else
            RC=$((RC + 1))
            state_write "$WS" "$RC" "$FS"
            # 与 monitor.sh 同款：`( cd ... && exec ... ) &` 让子 shell 被 exec 成进程本体，
            # $! 才是 ivsbox-web 的真实 pid（`cd x && y &` 的 $! 会落在空壳上）。
            # 口令从环境变量取（IVSBOX_WEB_PASS），不写死在命令行 —— `ps` 能看到命令行参数。
            ( cd "$IVSBOX_APP_DIR" && exec ./bin/ivsbox-web \
                --root "$IVSBOX_WEB_ROOT" --chan /var/run/ivsbox/ctl ) &
            echo $! > "$IVSBOX_WEB_PIDFILE" 2>/dev/null || true
        fi
    fi

    sleep "$POLL_S"
done
