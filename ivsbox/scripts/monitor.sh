#!/bin/sh
# IVSBox 崩溃自拉起守护循环（M1-S10；2026-09-30 补 G8 熔断，同日二次修正 F1/F2/F3）
#
# sysvinit 没有 procd 的 respawn，这层是必须的。**本脚本是 ivsboxd 的唯一拉起方**：
# init 脚本（S65ivsboxd）只拉起本脚本、不再直接拉起主控 —— 主控要不要启动由本脚本
# 按熔断状态决定（开机后至多晚一个轮询周期，默认 5 s）。若 init 直启主控，复位后
# 每次开机都会"免费"跑一轮 ivsboxd、每次都重新武装看门狗，跨复位熔断就被架空，
# 持续的运行期崩溃会变成 30 分钟一轮的整机复位风暴。
#
# ============================ 熔断语义（G8） ============================
# 没有熔断时，"进程反复起不来"与"设备一切正常"在日志里几乎一个样 —— 都是安静的，
# 只有一行行重复的启动记录。于是现场看到的是"设备在用"，实际是崩溃循环。
#
# 窗口（默认 300 s）内重拉超过上限（默认 5 次）⇒ 进 **failsafe**：不再拉起、告警落盘。
# 此时是否发生整机复位取决于最后一次崩溃的形态（S6 板端实证）：
#   - 崩溃前已打开 /dev/watchdog（运行期崩溃）⇒ 异常死亡没有写 'V'，内核继续计时，
#     约 16 s 后复位整机 —— 这才是"硬件兜底"（与计划 §S6"判死路径绝不写 'V'"同源）；
#   - 启动早期就退出（还没走到看门狗初始化）⇒ 从没人打开过看门狗，板子**不会**复位，
#     服务静默停止，等冷却到期 —— 行为同样可接受，只是没有硬件复位这一步。
# 复位/重启后本脚本重新启动：只要状态文件仍标记 failsafe，就**不拉起 ivsboxd**
# （不拉起 ⇒ 看门狗无人武装 ⇒ 不会再触发下一次复位），冷却到期后自动恢复监护。
# 为此状态文件放 UDISK（/var/run 是 tmpfs，复位即丢）。
#
# ============================ 程序 / 数据落点（2026-09-30 用户定稿） ============================
# **程序（bin/scripts）落 rootfs 的 `/opt/ivsbox`**（与 Go 版运维箱同口径）；
# **数据（config/db/queue/ota/releases/secure ＋ 本脚本的熔断状态 ＋ 日志）留 UDISK**
# 的 `/mnt/UDISK/ivsbox`。程序在 rootfs 的直接收益：不受 `S60mount_udisk` 挂载失败
# 即 `mkfs.ext4` 清空 UDISK 的牵连（程序本体不会被连带清掉）。
#
# ============================ 时间基准（G9 同款纪律） ============================
# 窗口与冷却的计时一律用**单调秒**（/proc/uptime 的整数部分），不用墙钟 date +%s：
# 板子开机时钟不可信、后续对时会跳变，墙钟会让熔断提前到期、崩溃计数提前清零
# （与 C 代码"时长测量一律 CLOCK_MONOTONIC"同一条纪律）。
# 代价：uptime 每次复位归零，状态文件里的旧值跨开机不可直接比较 ⇒ 启动时做两步消毒：
#   ① 仍处 failsafe（FS≠0）⇒ 只继承"在熔断"这个事实，冷却从本次启动重新计满一整轮
#      （既保证跨复位仍有完整冷却，也封死"长寿命设备断电重启后 FS 巨大 ⇒ 永久熔断"）；
#   ② 不在 failsafe 但 WS > 当前 uptime ⇒ 窗口属上一开机周期，作废重开。
#
# 人工恢复：`/etc/init.d/S65ivsboxd restart`（restart 会清状态文件），
# 或直接删除状态文件后重启服务。
#
# ============================ 可调参数（环境变量覆盖，便于验证） ============================
#   IVSBOX_MAX_RESTARTS        窗口内允许的重拉次数（默认 5）
#   IVSBOX_RESTART_WINDOW      统计窗口秒数（默认 300）
#   IVSBOX_FAILSAFE_COOLDOWN   failsafe 持续秒数（默认 1800）
#   IVSBOX_POLL_S              轮询间隔秒数（默认 5）
#   IVSBOX_APP_DIR / IVSBOX_HOME / IVSBOX_LOG_ROOT / IVSBOX_PIDFILE
#                              路径覆盖（默认同生产部署；仅验证/联调用）
#   注意 1：改短计数类参数只用于验证（例如把上限设成 2、窗口 30 s），生产保持默认。
#   注意 2：**IVSBOX_POLL_S 必须显著小于看门狗超时（16 s，建议 ≤ 10）** ——
#           崩溃后必须在狗咬下来之前完成重拉（新实例 open 看门狗才重新计时），
#           轮询一旦 ≥ 16 s，一次普通崩溃也会升级成整机复位。

IVSBOX_HOME=${IVSBOX_HOME:-/mnt/UDISK/ivsbox}
# 程序目录（bin/ivsboxd 所在）：2026-09-30 起落 rootfs /opt/ivsbox；数据仍在 IVSBOX_HOME。
IVSBOX_APP_DIR=${IVSBOX_APP_DIR:-/opt/ivsbox}
IVSBOX_LOG_ROOT=${IVSBOX_LOG_ROOT:-/mnt/UDISK/log}
IVSBOX_PIDFILE=${IVSBOX_PIDFILE:-/var/run/ivsboxd.pid}
STATE=$IVSBOX_HOME/.monitor.state
ALERT=$IVSBOX_LOG_ROOT/monitor-alert.log

MAX_RESTARTS=${IVSBOX_MAX_RESTARTS:-5}
WINDOW_S=${IVSBOX_RESTART_WINDOW:-300}
COOLDOWN_S=${IVSBOX_FAILSAFE_COOLDOWN:-1800}
POLL_S=${IVSBOX_POLL_S:-5}

# 状态文件是单行三列：`<window_start_mono> <restarts> <failsafe_until_mono>`，
# 全部是 /proc/uptime 的整数秒（见文件头"时间基准"）。读入后逐列做"是不是纯数字"
# 的兜底：状态文件被写坏/复位打断写半截时按"零历史"处理，绝不让坏文件卡死守护循环。
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

# 写状态并 **sync**：本文件的唯一价值就是"跨整机复位存活"，停在 page cache 里
# 等于没写（计划 §S6 探针 v1 的踩坑记录：只 fflush 不 fsync，复位后文件是 0 字节）。
# 写入频率极低（每次重拉 / 状态跃迁），代价可接受。
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
alert "started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s)"

# 跨开机消毒（见文件头"时间基准"）：uptime 归零后，状态文件里的旧值不可直接比较。
boot=$(mono_now)
case "$boot" in ''|*[!0-9]*) boot=0 ;; esac
if [ "$FS" -ne 0 ]; then
    FS=$((boot + COOLDOWN_S))
    alert "resuming failsafe from persisted state, cooldown restarted (${COOLDOWN_S}s), ivsboxd will NOT be started"
    state_write "$WS" "$RC" "$FS"
elif [ "$WS" -gt "$boot" ]; then
    WS=$boot
    RC=0
    state_write "$WS" "$RC" "$FS"
fi

while true; do
    now=$(mono_now)
    case "$now" in ''|*[!0-9]*) sleep "$POLL_S"; continue ;; esac

    # 窗口过期 ⇒ 历史清零。设备稳定跑了整个窗口之后再崩一次，不必按"循环"论处。
    if [ "$FS" -eq 0 ] && [ $((now - WS)) -ge "$WINDOW_S" ]; then
        WS=$now
        RC=0
        state_write "$WS" "$RC" "$FS"
    fi

    # failsafe 未到期：不拉起主控（本脚本是唯一拉起方，不拉起 ⇒ 看门狗无人武装）。
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
            alert "CRASH LOOP: ${RC} restart(s) within ${WINDOW_S}s reached the limit ${MAX_RESTARTS} -- entering failsafe for ${COOLDOWN_S}s, ivsboxd will NOT be started; if the last instance had armed the watchdog, the board will be reset by hardware in ~16s (crash close never writes 'V')"
        else
            RC=$((RC + 1))
            state_write "$WS" "$RC" "$FS"
            # 用 `( ... ) &` + `exec`：子 shell 被 exec 成 ivsboxd 本体，$! 即 ivsboxd 的真实 pid。
            # 不能写 `cd "$IVSBOX_APP_DIR" && ./bin/ivsboxd ... &` —— `&` 作用于整条 AND 列表，
            # 会多留一个常驻的 monitor.sh 空壳进程，且 $! 拿到的是空壳而不是 ivsboxd 的 pid。
            ( cd "$IVSBOX_APP_DIR" && exec ./bin/ivsboxd >/dev/null 2>&1 ) &
            # 回写 pidfile：monitor 是唯一拉起方，必须让 pidfile 始终指向真实进程。
            # 不写这一步，崩溃重启后 pidfile 仍是旧 pid，init 脚本的 stop 会打空、停不掉服务。
            echo $! > "$IVSBOX_PIDFILE" 2>/dev/null || true
        fi
    fi

    if [ -f /tmp/ivsbox-upgrade.ready ]; then
        # OTA 触发点：后续由 OTA 模块实现，当前保留占位
        :
    fi

    sleep "$POLL_S"
done
