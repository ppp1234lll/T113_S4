#!/bin/sh
# IVSBox 媒体进程守护循环（M6-S6.1；M7-S7.4 补"卡死检测"）
#
# 模式与 monitor-web.sh 相同（细节见其文件头）：独立状态文件/pidfile、
# **不碰看门狗**、/proc/uptime 单调秒计时、跨开机消毒、pidfile+comm 判活。
# 媒体进程崩溃只表现为"抓拍/录像停"，控制面（ivsboxd）不受牵连，由本脚本
# 循环拉起（计划 S6.1"崩溃只重启自身"）。
#
# ============================ 卡死检测（M7-S7.4，架构 §16.2 场景 7） ============================
# 场景 7 要求"**Web、媒体单独卡死时只重启对应服务**"。只判"进程还在不在"接不住：
# `kill -STOP`、死锁的媒体进程在 /proc 里照样存在、comm 照样是 ivsbox-media。
#
# 与 Web 的差别（也是本脚本比 monitor-web.sh 复杂的原因）：**bird_media 是收编的上游
# 代码、一行不改**（S6.0 硬约束，见 media/README.ivsbox.md）⇒ 不能像 Web 那样在它主循环里
# 埋心跳，只能在**外部**探。外部可用的真判据只有一个：**HTTP 往返**。
#
# ⚠ 特别注意：**TCP 连得上不等于没卡死**。卡死进程的监听队列由**内核**代答三次握手
# ⇒ `nc`/`connect()` 直接返回成功，但进程永远不会回一个字节。所以探针必须看
# **响应里有没有 `HTTP/` 状态行**，不能只看连接是否建立。这是本条注入最容易自欺的一点。
#
# 探针实现按可用性择优（启动时定一次并写进告警，便于现场核对）：
#   wget  → `wget -S -O /dev/null -T N <url>`，看 stderr 里的 `HTTP/` 状态行；
#   nc    → 手写 `GET ... HTTP/1.0` 请求，看回包里的 `HTTP/` 状态行；
#   都没有 → **降级为仅判存活**，并在告警里**明确写出"卡死检测不可用"**（绝不静默假装有检测）。
# 判定阈值用"**连续 N 次无应答**"而非单次：媒体在 remux/写盘时短暂不响应是正常的
# （默认 N=6 × POLL_S=5 ⇒ 需连续 ~30 s 完全不响应才动手，与 Web 的 30 s 心跳超时对齐）。
#
# 处置：判卡死 **TERM → 宽限 → KILL** 再重拉。必须有 KILL 兜底：SIGTERM 对处于 T
# （stopped）态的进程只会挂起、不生效；**SIGKILL 能直接回收**被 STOP 的进程。
#
# ============ 可调参数（环境变量覆盖，便于验证） ============
#   IVSBOX_MEDIA_MAX_RESTARTS / IVSBOX_MEDIA_RESTART_WINDOW / IVSBOX_MEDIA_COOLDOWN
#   IVSBOX_MEDIA_POLL_S
#   IVSBOX_MEDIA_PROBE_URL      探活 URL（默认 http://127.0.0.1:18000/record/status）
#   IVSBOX_MEDIA_PROBE_HOST/PORT  nc 分支用的地址/端口（默认 127.0.0.1 / 18000）
#   IVSBOX_MEDIA_PROBE_T_S      单次探活超时秒数（默认 3）
#   IVSBOX_MEDIA_HANG_N         连续失败多少次判卡死（默认 6）
#   IVSBOX_MEDIA_KILL_GRACE_S   判卡死后 TERM 到 KILL 的宽限秒数（默认 5）
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

# ---- 卡死检测（S7.4）----
IVSBOX_MEDIA_PROBE_URL=${IVSBOX_MEDIA_PROBE_URL:-http://127.0.0.1:18000/record/status}
IVSBOX_MEDIA_PROBE_HOST=${IVSBOX_MEDIA_PROBE_HOST:-127.0.0.1}
IVSBOX_MEDIA_PROBE_PORT=${IVSBOX_MEDIA_PROBE_PORT:-18000}
IVSBOX_MEDIA_PROBE_T_S=${IVSBOX_MEDIA_PROBE_T_S:-3}
IVSBOX_MEDIA_HANG_N=${IVSBOX_MEDIA_HANG_N:-6}
IVSBOX_MEDIA_KILL_GRACE_S=${IVSBOX_MEDIA_KILL_GRACE_S:-5}

WS=0
RC=0
FS=0
# 连续探活失败计数（成功一次即清零）。
PROBE_MISS=0

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
    # 状态位 Z（僵尸）**不算活**：进程已死、只是还没被收尸，comm 仍是 ivsbox-media
    # （S7.4 补）。少了这一条，僵尸会一直"判活"，崩溃后的重拉被永久压住。
    [ "$(cut -d' ' -f3 "/proc/$pid/stat" 2>/dev/null)" != "Z" ] || return 1
    return 0
}

# 返回 0＝媒体进程**在 HTTP 上应答了**（或判据不可用/已降级）；1＝没应答。
# 判据是"响应里出现 HTTP/ 状态行"，不是"连得上"（内核会代答握手，见文件头）。
#
# ⚠ `-t 1`（只试一次）**不能省**：wget 默认 `--tries=20`，对一个"连得上但永不回字节"
#   的卡死进程，每次探活会重试 20 次 × PROBE_T_S ⇒ 一次探活就阻塞 **60 s**，
#   比 POLL_S 大两个数量级，守护循环被整段拖住、卡死反而检不出来。
#   （这条是 S7.4 真注入 `kill -STOP` 时抓出来的：首轮 h2 报"卡死未检出"，
#     根因不是判据错，而是探针自己把循环堵死了。）
media_responds() {
    case "$PROBE" in
    wget)
        wget -S -t 1 -O /dev/null -T "$IVSBOX_MEDIA_PROBE_T_S" "$IVSBOX_MEDIA_PROBE_URL" 2>&1 \
            | grep -q 'HTTP/'
        ;;
    nc)
        printf 'GET /record/status HTTP/1.0\r\n\r\n' \
            | nc -w "$IVSBOX_MEDIA_PROBE_T_S" "$IVSBOX_MEDIA_PROBE_HOST" "$IVSBOX_MEDIA_PROBE_PORT" 2>/dev/null \
            | grep -q 'HTTP/'
        ;;
    *)
        return 0
        ;;
    esac
}

state_read

# 探针择优（一次定下来，并把结论写进告警日志，现场可直接核对用了哪条判据）。
if command -v wget >/dev/null 2>&1; then
    PROBE=wget
elif command -v nc >/dev/null 2>&1; then
    PROBE=nc
else
    PROBE=none
fi

alert "media monitor started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s probe=$PROBE url=$IVSBOX_MEDIA_PROBE_URL hang_n=$IVSBOX_MEDIA_HANG_N)"
if [ "$PROBE" = "none" ]; then
    alert "media WARNING: 既无 wget 也无 nc ⇒ **卡死检测不可用**，降级为仅判存活（进程存在即算健康）；若要覆盖 §16.2 场景 7 的媒体部分，请在该 rootfs 上提供 wget 或 nc"
fi

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

    # 先判"要不要重拉"，再统一走同一段重拉/熔断逻辑（崩溃与卡死在这里是同一件事）。
    NEED_RESTART=0
    if media_alive; then
        if media_responds; then
            PROBE_MISS=0
            RC=0
        else
            PROBE_MISS=$((PROBE_MISS + 1))
            alert "media 探活无应答 ${PROBE_MISS}/${IVSBOX_MEDIA_HANG_N}（probe=$PROBE url=$IVSBOX_MEDIA_PROBE_URL）"
            if [ "$PROBE_MISS" -ge "$IVSBOX_MEDIA_HANG_N" ]; then
                alert "media HANG: 连续 ${PROBE_MISS} 次探活无应答（pid=$pid），按卡死处理：TERM -> ${IVSBOX_MEDIA_KILL_GRACE_S}s -> KILL"
                kill -TERM "$pid" 2>/dev/null || true
                sleep "$IVSBOX_MEDIA_KILL_GRACE_S"
                kill -KILL "$pid" 2>/dev/null || true
                PROBE_MISS=0
                NEED_RESTART=1
            fi
        fi
    else
        PROBE_MISS=0
        NEED_RESTART=1
    fi

    if [ "$NEED_RESTART" -ne 0 ]; then
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
            # 起完给探针一点热身时间：新进程要建监听 socket 才能应答，头几次失败照常计数，
            # 但 HANG_N×POLL_S（默认 30 s）已经远大于冷启动耗时。
            ( cd "$IVSBOX_APP_DIR" && exec ./bin/ivsbox-media ) &
            echo $! > "$IVSBOX_MEDIA_PIDFILE" 2>/dev/null || true
        fi
    fi

    sleep "$POLL_S"
done
