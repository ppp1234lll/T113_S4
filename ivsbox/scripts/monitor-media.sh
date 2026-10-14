#!/bin/sh
# IVSBox 媒体进程守护循环（M6-S6.1）
#
# 模式与 monitor-web.sh 相同（细节见其文件头）：独立状态文件/pidfile、
# **不碰看门狗**、/proc/uptime 单调秒计时、跨开机消毒、pidfile+comm 判活。
# 媒体进程崩溃只表现为"抓拍/录像停"，控制面（ivsboxd）不受牵连，由本脚本
# 循环拉起（计划 S6.1"崩溃只重启自身"）。
#
# ============ 可调参数（环境变量覆盖，便于验证） ============
#   IVSBOX_MEDIA_MAX_RESTARTS / IVSBOX_MEDIA_RESTART_WINDOW / IVSBOX_MEDIA_COOLDOWN
#   IVSBOX_MEDIA_POLL_S
#   IVSBOX_APP_DIR / IVSBOX_LOG_ROOT / IVSBOX_MEDIA_PIDFILE

IVSBOX_HOME=${IVSBOX_HOME:-/mnt/UDISK/ivsbox}
IVSBOX_APP_DIR=${IVSBOX_APP_DIR:-/opt/ivsbox}
IVSBOX_LOG_ROOT=${IVSBOX_LOG_ROOT:-/mnt/UDISK/log}
IVSBOX_MEDIA_PIDFILE=${IVSBOX_MEDIA_PIDFILE:-/var/run/ivsbox-media.pid}
STATE=$IVSBOX_HOME/.monitor-media.state
ALERT=$IVSBOX_LOG_ROOT/monitor-alert.log

MAX_RESTARTS=${IVSBOX_MEDIA_MAX_RESTARTS:-5}
WINDOW_S=${IVSBOX_MEDIA_RESTART_WINDOW:-300}
COOLDOWN_S=${IVSBOX_MEDIA_COOLDOWN:-600}
POLL_S=${IVSBOX_MEDIA_POLL_S:-5}

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
    printf '%s monitor-media: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >> "$ALERT" 2>/dev/null || true
    sync 2>/dev/null || true
}

media_alive() {
    [ -f "$IVSBOX_MEDIA_PIDFILE" ] || return 1
    read -r pid < "$IVSBOX_MEDIA_PIDFILE" 2>/dev/null || return 1
    case "$pid" in ''|*[!0-9]*) return 1 ;; esac
    [ -r "/proc/$pid/comm" ] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "ivsbox-media" ] || return 1
    return 0
}

state_read
alert "media monitor started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s)"

# 跨开机消毒（与 monitor.sh/monitor-web.sh 同口径）
boot=$(mono_now)
case "$boot" in ''|*[!0-9]*) boot=0 ;; esac
if [ "$FS" -ne 0 ]; then
    FS=$((boot + COOLDOWN_S))
    alert "media monitor resuming failsafe from persisted state, cooldown restarted (${COOLDOWN_S}s), ivsbox-media will NOT be started"
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
        alert "media failsafe expired after ${COOLDOWN_S}s, resuming normal supervision"
        WS=$now
        RC=0
        FS=0
        state_write "$WS" "$RC" "$FS"
    fi

    if media_alive; then
        RC=0
    else
        if [ "$RC" -ge "$MAX_RESTARTS" ]; then
            FS=$((now + COOLDOWN_S))
            state_write "$WS" "$RC" "$FS"
            alert "media CRASH LOOP: ${RC} restart(s) within ${WINDOW_S}s reached the limit ${MAX_RESTARTS} -- entering failsafe for ${COOLDOWN_S}s, ivsbox-media will NOT be started"
        else
            RC=$((RC + 1))
            state_write "$WS" "$RC" "$FS"
            # `( cd ... && exec ... ) &`：子 shell 被 exec 成进程本体，$! 才是真 pid。
            # bird_media 无命令行参数（监听 127.0.0.1 语义由代码固定在 0.0.0.0:18000，
            # 本机无鉴权接口；网内暴露风险登记在 修改记录，M8 远程访问边界收口）。
            ( cd "$IVSBOX_APP_DIR" && exec ./bin/ivsbox-media ) &
            echo $! > "$IVSBOX_MEDIA_PIDFILE" 2>/dev/null || true
        fi
    fi

    sleep "$POLL_S"
done
