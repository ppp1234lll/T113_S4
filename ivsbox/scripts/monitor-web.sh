#!/bin/sh
# IVSBox Web 服务守护循环（M5-S5.1；M7-S7.4 补"卡死检测"）
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
# ============================ 卡死检测（M7-S7.4，架构 §16.2 场景 7） ============================
# 场景 7 要求"**Web、媒体单独卡死时只重启对应服务**"。只判"进程还在不在"接不住这一条：
# `kill -STOP`、死锁、长阻塞的 web 进程在 /proc 里**照样存在**，comm 也照样是 ivsbox-web
# ⇒ 老实现把它当健康，于是页面永远打不开而没人重启它。故判活分两层：
#   ① 进程层：pidfile + `/proc/<pid>/comm` + `/proc/<pid>/stat` 状态位（Z＝僵尸不算活）；
#   ② 循环层：**主循环心跳**是否还在前进。
# 心跳由 ivsbox-web 的**主循环**（web/src/main.c 的 heartbeat_tick()，**不是独立线程**）
# 按 IVSBOX_WEB_HEARTBEAT_MS 刷新。为什么不能用线程刷：循环卡死时线程照常前进、心跳一直
# 新鲜 ⇒ 等于没有卡死检测。"判活"与"判卡死"的分水岭就在这里。
#
# 判据用**"内容还在不在变"**，而不是文件 mtime：mtime 只能跟墙钟比，而墙钟会因对时跳变
# （与 monitor.sh 文件头"时间基准"同一条纪律）。内容变化只需本脚本自己的单调秒计时。
#
# 处置：判卡死后 **TERM → 宽限 → KILL** 再重拉。为什么必须有 KILL 兜底：SIGTERM 对处于
# T（stopped）态的进程只会挂起、不会生效；而 **SIGKILL 能直接回收**被 STOP 的进程。
# 只在"卡死"这一条路径上补 KILL —— 正常崩溃路径保持原样（TERM 就够，也别改变既有语义）。
#
# ============ 可调参数（环境变量覆盖，便于验证） ============
#   IVSBOX_WEB_MAX_RESTARTS     窗口内允许的重拉次数（默认 5）
#   IVSBOX_WEB_RESTART_WINDOW   统计窗口秒数（默认 300）
#   IVSBOX_WEB_COOLDOWN         failsafe 持续秒数（默认 600，比主控的 1800 短：
#                               Web 是附属服务，恢复要更快）
#   IVSBOX_WEB_POLL_S           轮询间隔秒数（默认 5）
#   IVSBOX_WEB_PORT             监听端口（默认 80，与 ivsbox-web 的内置默认一致；
#                               **显式传给进程**：验证时用非特权端口起非 root 实例）
#   IVSBOX_WEB_HEARTBEAT        心跳文件路径（默认 /tmp/ivsbox-web.heartbeat；
#                               须与 ivsbox-web 的 --heartbeat / IVSBOX_WEB_HEARTBEAT 一致）
#   IVSBOX_WEB_HANG_S           心跳内容多久没变化即判卡死（默认 30；
#                               必须 ≫ IVSBOX_WEB_HEARTBEAT_MS，且 ≥ 3×POLL_S，
#                               否则慢启动/偶发写失败会被误判）
#   IVSBOX_WEB_KILL_GRACE_S     判卡死后 TERM 到 KILL 的宽限秒数（默认 5）
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

# ---- 卡死检测（S7.4）----
IVSBOX_WEB_PORT=${IVSBOX_WEB_PORT:-80}
IVSBOX_WEB_HEARTBEAT=${IVSBOX_WEB_HEARTBEAT:-/tmp/ivsbox-web.heartbeat}
IVSBOX_WEB_HANG_S=${IVSBOX_WEB_HANG_S:-30}
IVSBOX_WEB_KILL_GRACE_S=${IVSBOX_WEB_KILL_GRACE_S:-5}

WS=0
RC=0
FS=0
# 心跳判据的两次采样：HB_SEEN＝上一轮看到的内容，HB_T0＝该内容首次被看到的单调秒。
HB_SEEN=""
HB_T0=0

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
    # 状态位 Z（僵尸）**不算活**：进程已经死了、只是还没被收尸，comm 仍然是 ivsbox-web
    # （S7.4 补）。少了这一条，僵尸会一直"判活"，卡死/崩溃后的重拉被永久压住。
    [ "$(cut -d' ' -f3 "/proc/$pid/stat" 2>/dev/null)" != "Z" ] || return 1
    return 0
}

# 返回 0＝主循环在前进（或判据不可用）；1＝判定卡死。
# 只在 web_alive 为真时才有意义：心跳文件缺失/为空同样按"没变化"处理，
# 时间轴由 HB_T0 起算，进程起不来时本来也走重启路径。
web_loop_alive() {
    HB_CUR=$(cat "$IVSBOX_WEB_HEARTBEAT" 2>/dev/null)
    if [ "$HB_CUR" != "$HB_SEEN" ]; then
        HB_SEEN=$HB_CUR
        HB_T0=$now
        return 0
    fi
    [ $((now - HB_T0)) -lt "$IVSBOX_WEB_HANG_S" ]
}

state_read
alert "web monitor started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s hang=${IVSBOX_WEB_HANG_S}s hb=$IVSBOX_WEB_HEARTBEAT)"

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
HB_T0=$boot

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

    # 先判"要不要重拉"，再统一走同一段重拉/熔断逻辑：崩溃与卡死在这里是同一件事
    # （都要重拉、都计入同一个崩溃计数），差别只在重拉**之前**要不要补一刀 KILL。
    NEED_RESTART=0
    if web_alive; then
        if web_loop_alive; then
            RC=0
        else
            alert "web HANG: 进程仍在（pid=$pid）但主循环 ${IVSBOX_WEB_HANG_S}s 无心跳（$IVSBOX_WEB_HEARTBEAT），按卡死处理：TERM -> ${IVSBOX_WEB_KILL_GRACE_S}s -> KILL"
            kill -TERM "$pid" 2>/dev/null || true
            sleep "$IVSBOX_WEB_KILL_GRACE_S"
            kill -KILL "$pid" 2>/dev/null || true
            HB_SEEN=""
            HB_T0=$now
            NEED_RESTART=1
        fi
    else
        NEED_RESTART=1
    fi

    if [ "$NEED_RESTART" -ne 0 ]; then
        if [ "$RC" -ge "$MAX_RESTARTS" ]; then
            FS=$((now + COOLDOWN_S))
            state_write "$WS" "$RC" "$FS"
            alert "web CRASH LOOP: ${RC} restart(s) within ${WINDOW_S}s reached the limit ${MAX_RESTARTS} -- entering failsafe for ${COOLDOWN_S}s, ivsbox-web will NOT be started"
        else
            RC=$((RC + 1))
            state_write "$WS" "$RC" "$FS"
            # 每次重拉前清掉心跳文件：新实例的"第一拍"必须从零起算，否则残留的旧内容
            # 与 HB_SEEN 恰好相等时，会把新进程的启动期误算成"已经卡死 HANG_S 秒"。
            rm -f "$IVSBOX_WEB_HEARTBEAT" 2>/dev/null || true
            # 与 monitor.sh 同款：`( cd ... && exec ... ) &` 让子 shell 被 exec 成进程本体，
            # $! 才是 ivsbox-web 的真实 pid（`cd x && y &` 的 $! 会落在空壳上）。
            # 口令从环境变量取（IVSBOX_WEB_PASS），不写死在命令行 —— `ps` 能看到命令行参数。
            ( cd "$IVSBOX_APP_DIR" && exec ./bin/ivsbox-web \
                --port "$IVSBOX_WEB_PORT" \
                --root "$IVSBOX_WEB_ROOT" --chan /var/run/ivsbox/ctl \
                --heartbeat "$IVSBOX_WEB_HEARTBEAT" ) &
            echo $! > "$IVSBOX_WEB_PIDFILE" 2>/dev/null || true
            HB_SEEN=""
            HB_T0=$now
        fi
    fi

    sleep "$POLL_S"
done
