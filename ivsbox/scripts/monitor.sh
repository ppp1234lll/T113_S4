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

# ---- OTA / 升级自检（M7-S7.2 ＋ S7.3）----
# 升级器与就绪标记的路径与 scripts/ivsbox-run.sh、src/app/main.c 保持一致。
IVSBOX_UPD=${IVSBOX_UPD:-$IVSBOX_APP_DIR/bin/ivsbox-updater}
IVSBOX_READY_MARK=${IVSBOX_READY_MARK:-/tmp/ivsbox-ready}
# 升级自检窗口：新版拉起后多久还没落下就绪标记，就判定失败并回滚。
# 必须显著大于"装配完成所需时间"（配置/DB/通道/agent/健康线程），也不能大到让
# 现场干等；120 s 是"慢速存储 + 冷启动"的宽松上界。
IVSBOX_UPGRADE_TIMEOUT=${IVSBOX_UPGRADE_TIMEOUT:-120}
# 自检判失败后停止当前实例的宽容期：先 TERM，等这么久再不退就 KILL。
# 为什么不能只 TERM：卡死的实例（T 态 / 死循环不响应信号）TERM 不动它，而"停不掉"
# 会让回滚**只在下次开机才生效** —— 与 S7.3"尽快把业务拉回旧版本"的意图正好相反。
IVSBOX_BOOT_KILL_GRACE_S=${IVSBOX_BOOT_KILL_GRACE_S:-5}
# ivsboxd 落下的"请装这个包"请求（由本地通道的 ota.request 方法写）。
OTA_REQUEST=$IVSBOX_HOME/ota/request

# ---- 受控远程通道（M8）----
# 标记文件由 ivsboxd 的 iv_tunnel 按**有效期**写/清（M8-S8.2）；本脚本只按它起停
# frpc 进程。**标记是唯一真相**：进程在不在不算数，因为进程崩溃是常态，而"标记没了
# 进程还在"是绕过有效期闸门的远程入口常开 —— 后者危险得多。
IVSBOX_TUNNEL_FLAG=${IVSBOX_TUNNEL_FLAG:-/tmp/ivsbox-tunnel.enabled}
TUNNEL_SCRIPT=${TUNNEL_SCRIPT:-$IVSBOX_APP_DIR/scripts/frp.sh}
# 通道告警节流：frpc 起不来（例如板端根本没装 frpc）时不能每 5 s 刷一条日志。
TUNNEL_ALERT_GAP=${TUNNEL_ALERT_GAP:-300}
TUNNEL_LAST_ALERT=0

# 状态文件是单行四列：
# `<window_start_mono> <restarts> <failsafe_until_mono> <boot_id 前 8 位>`。
# 前三列是 /proc/uptime 的整数秒（见文件头"时间基准"），第 4 列 2026-10-29 加。
#
# 为什么需要 boot_id：原来判"窗口属不属于上一次开机"用的是 `WS > boot`（秒数比
# 大小）。它只在"上次开机跑得比这次久"时才成立 —— 上次没跑多久就被复位，WS 是
# 个小值，本次 boot 几秒就超过它 ⇒ 判据为假 ⇒ 窗口没清、RC 跨复位继续累加 ⇒
# 人工复位几次就被当成 crash loop 熔断 30 分钟（现场表现是"板子好的、页面全空"）。
# boot_id 整机一复位必变，与跑了多久无关。旧格式文件没有第 4 列 ⇒ BID 为空 ⇒
# 判据天然为真、按"上一次开机"处理，向后兼容。
#
# 读入后逐列做"是不是纯数字"的兜底：状态文件被写坏/复位打断写半截时按"零历史"
# 处理，绝不让坏文件卡死守护循环。
WS=0
RC=0
FS=0
BID=''
# 本实例的启动时刻（单调秒），用于升级自检超时判定；0 表示"还没拉起过"。
BOOT_T0=0

mono_now() {
    cut -d. -f1 /proc/uptime 2>/dev/null
}

# 本开机周期的身份。整机一复位就变，比 uptime 秒数可靠（见状态文件那段注释）。
# 取前 8 位足够区分，也不至于让状态文件行长漂移。取不到就回空串 —— 空串会让
# "与文件里的 BID 不同"恒成立，最坏是按"上一次开机"处理，比误熔断安全。
boot_id_now() {
    cut -c1-8 /proc/sys/kernel/random/boot_id 2>/dev/null
}

state_read() {
    if [ -f "$STATE" ]; then
        read -r WS RC FS BID < "$STATE" 2>/dev/null || true
    fi
    case "$WS" in ''|*[!0-9]*) WS=0 ;; esac
    case "$RC" in ''|*[!0-9]*) RC=0 ;; esac
    case "$FS" in ''|*[!0-9]*) FS=0 ;; esac
}

# 写状态并 **sync**：本文件的唯一价值就是"跨整机复位存活"，停在 page cache 里
# 等于没写（计划 §S6 探针 v1 的踩坑记录：只 fflush 不 fsync，复位后文件是 0 字节）。
# 写入频率极低（每次重拉 / 状态跃迁），代价可接受。
state_write() {
    # 第 4 列固定写本开机周期的 `bid`（全局）：调用点不必传它。
    # 少一个"必须记得传"的参数，就少一类"某处漏传 ⇒ 跨开机判据错 ⇒ 误熔断"的坑，
    # 这也是它不跟着变成 `state_write <WS> <RC> <FS> <BID>` 的原因。
    printf '%s %s %s %s\n' "$1" "$2" "$3" "$bid" > "$STATE" 2>/dev/null || true
    sync 2>/dev/null || true
}

alert() {
    printf '%s monitor: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >> "$ALERT" 2>/dev/null || true
    sync 2>/dev/null || true
}

ivsboxd_alive() {
    ps -o comm | grep -x ivsboxd >/dev/null 2>&1
}

frpc_alive() {
    # frpc 是 ELF 二进制，`-o comm` 判定可靠（对 shell 脚本会看到解释器名，故不用 -f）。
    ps -o comm | grep -x frpc >/dev/null 2>&1
}

tunnel_alert() {
    # 节流：同一类告警在 TUNNEL_ALERT_GAP 秒内只记一次。
    if [ $((now - TUNNEL_LAST_ALERT)) -ge "$TUNNEL_ALERT_GAP" ]; then
        alert "tunnel: $1"
        TUNNEL_LAST_ALERT=$now
    fi
}

state_read
alert "started (max_restarts=$MAX_RESTARTS window=${WINDOW_S}s cooldown=${COOLDOWN_S}s poll=${POLL_S}s)"

# 跨开机消毒（见文件头"时间基准"）：uptime 归零后，状态文件里的旧值不可直接比较。
boot=$(mono_now)
case "$boot" in ''|*[!0-9]*) boot=0 ;; esac
bid=$(boot_id_now)
if [ "$FS" -ne 0 ]; then
    # **熔断不因复位而解除**：这里刻意只继承、不看开机周期。看门狗复位同样是
    # "整机复位"，一复位就清熔断的话，"崩 → 看门狗复位 → 再崩"就永远拦不住 ——
    # 而那正是这套 failsafe 存在的理由。人工复位后想立刻恢复，只能删状态文件
    # （`rm -f $STATE` 再 start），现场排障手册里写这一条。
    FS=$((boot + COOLDOWN_S))
    alert "resuming failsafe from persisted state, cooldown restarted (${COOLDOWN_S}s), ivsboxd will NOT be started"
    state_write "$WS" "$RC" "$FS"
elif [ -n "$bid" ] && [ "$BID" != "$bid" ]; then
    # 开机周期变了 ⇒ 上一轮的 restart 计数作废。原先靠 `WS > boot` 判，上次开机
    # 没跑多久就被复位时 WS 是个小值、本次 boot 几秒就超过它 ⇒ 漏判、计数跨复位
    # 累加、人工复位几次就熔断 30 分钟（见状态文件那段注释）。
    WS=$boot
    RC=0
    state_write "$WS" "$RC" "$FS"
elif [ "$WS" -gt "$boot" ]; then
    # 兜底：boot_id 取不到（裁剪内核 / 容器）时退回旧判据，聊胜于无。
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
            # 用 `( ... ) &` + `exec`：子 shell 被 exec 成**启动包装脚本**，包装脚本再
            # exec 成 ivsboxd 本体 —— `$!` 始终是同一个 pid（M7-S7.3 起改走包装脚本：
            # 由它按 bootcount 决定这一次该起哪个版本目录，见 scripts/ivsbox-run.sh）。
            # 不能写 `cd "$IVSBOX_APP_DIR" && ./scripts/ivsbox-run.sh &` —— `&` 作用于
            # 整条 AND 列表，会多留一个常驻的 monitor.sh 空壳进程，且 $! 拿到的是空壳。
            # 拉起前**同步**清掉就绪标记 —— 不能只靠包装脚本自己删。包装脚本的删除
            # 发生在被 fork 出来的子进程里，本圈轮询读标记时它可能还没被调度；若上一
            # 实例恰好"落下标记后立刻崩溃"，本圈就会把**它的**标记当成新版的战绩，给
            # 一个反复崩溃的新版盖上"已确认" —— bootcount 归零，回滚保护整个失效。
            rm -f "$IVSBOX_READY_MARK" 2>/dev/null || true
            # 把本脚本解析出来的落点**显式传给**包装脚本。包装脚本的同名变量默认值与这里
            # 恰好一致，但"恰好一致"不是契约：一旦现场用 IVSBOX_APP_DIR 改掉落点，包装
            # 脚本会继续读它自己的默认值 /opt/ivsbox —— monitor 守着一个目录、启动包装去起
            # 另一个目录（tests/upgrade-boot.sh u5 抓到的就是这条）。IVSBOX_READY_MARK 必须
            # 一起传：否则上面那行 rm 删的是 A 文件、包装脚本删的是 B 文件。
            # 变量赋值写在子 shell 里 —— 赋值进子 shell 环境、被 exec 出的进程继承，
            # `$!` 仍与 exec 出来的主控是同一个 pid（pidfile 语义不变）。
            ( IVSBOX_APP_ROOT="$IVSBOX_APP_DIR" \
              IVSBOX_DATA_ROOT="$IVSBOX_HOME" \
              IVSBOX_LOG_ROOT="$IVSBOX_LOG_ROOT" \
              IVSBOX_READY_MARK="$IVSBOX_READY_MARK" \
              exec "$IVSBOX_APP_DIR/scripts/ivsbox-run.sh" ) &
            # 回写 pidfile：monitor 是唯一拉起方，必须让 pidfile 始终指向真实进程。
            # 不写这一步，崩溃重启后 pidfile 仍是旧 pid，init 脚本的 stop 会打空、停不掉服务。
            echo $! > "$IVSBOX_PIDFILE" 2>/dev/null || true
            # 升级自检从这一刻开始计时。每次重拉都**重新计时**：新版崩溃后被重拉，应该
            # 按"这一次"算，而不是把上一实例已经等掉的时间累进来。
            BOOT_T0=$now
        fi
    fi

    # ---- 受控远程通道守护（M8）----
    # 三个方向都要兜：① 标记在、进程没了 ⇒ 通道静默失效，远程运维连不上，现象与
    # "设备掉线"一模一样，现场最难查；② 标记没了、进程还在 ⇒ 远程入口绕过有效期
    # 常开，这是安全事故，比失效危险得多；③ 标记在但**已过期** ⇒ 主控不在时
    # `iv_tunnel_tick()` 没人跑，只剩这一层能执行有效期闸门（标记内容就是到期时刻，
    # 由 iv_tunnel_enable 写入；⚠ 本脚本的 $now 是 /proc/uptime，判过期必须用 date）。
    # "该不该在跑"的最终裁决交给 frp.sh 的 up（它内部同样判过期，幂等）。
    if [ -f "$IVSBOX_TUNNEL_FLAG" ]; then
        tun_exp=$(tr -cd '0-9' < "$IVSBOX_TUNNEL_FLAG" 2>/dev/null)
        tun_now=$(date +%s 2>/dev/null)
        if [ -n "$tun_exp" ] && [ -n "$tun_now" ] && [ "$tun_now" -ge "$tun_exp" ] 2>/dev/null; then
            tunnel_alert "标记已过期（主控不在？），停掉 frpc 并清标记"
        elif ! frpc_alive; then
            tunnel_alert "标记存在但 frpc 不在，拉起"
        fi
        sh "$TUNNEL_SCRIPT" up >/dev/null 2>&1
    else
        if frpc_alive; then
            tunnel_alert "标记已清除，停掉 frpc（有效期闸门）"
            sh "$TUNNEL_SCRIPT" down >/dev/null 2>&1
        fi
    fi

    # ---- OTA 请求消费（M7-S7.2）----
    # 分工：ivsboxd 只负责"收命令 + 落一个请求文件"（本地通道 ota.request，路径必须是
    # 已经落在设备上的包）；**真正改盘的安装动作由本脚本拉起独立进程**完成。
    # 为什么放在 monitor 而不是 ivsboxd 里：安装要停/换正在运行的程序，这段危险动作
    # 需要它自己的生命周期 —— 不该继承主控的看门狗 fd，也不该在跑 reactor 的进程里阻塞。
    if [ -f "$OTA_REQUEST" ] && [ -x "$IVSBOX_UPD" ]; then
        req_pkg=""
        req_restart=""
        read -r req_pkg req_restart < "$OTA_REQUEST" 2>/dev/null || true
        rm -f "$OTA_REQUEST"
        case "$req_pkg" in
        /*)
            alert "OTA: 开始安装 $req_pkg (restart=${req_restart:-0})"
            if [ "$req_restart" = "1" ]; then
                "$IVSBOX_UPD" install "$req_pkg" --restart >> "$ALERT" 2>&1
            else
                "$IVSBOX_UPD" install "$req_pkg" >> "$ALERT" 2>&1
            fi
            alert "OTA: 安装结束 rc=$?"
            ;;
        *)
            alert "OTA: 请求里的路径不是绝对路径，已丢弃：$req_pkg"
            ;;
        esac
    fi

    # ---- 升级自检确认（M7-S7.3）----
    # 只在"存在待确认升级"时才判（status --pending）。判据用**就绪标记**而不是
    # "进程还活着"：活着不等于自检通过 —— 配置/DB/通道/agent/健康线程任何一路没起来
    # 都不该被确认；而"什么时候算真的起来了"只有 ivsboxd 自己知道（见 src/app/main.c）。
    if [ "$BOOT_T0" -ne 0 ] && [ -x "$IVSBOX_UPD" ] && \
       "$IVSBOX_UPD" status --pending --app-root "$IVSBOX_APP_DIR" --data-root "$IVSBOX_HOME" >/dev/null 2>&1; then
        if [ -f "$IVSBOX_READY_MARK" ]; then
            "$IVSBOX_UPD" boot-ok --app-root "$IVSBOX_APP_DIR" --data-root "$IVSBOX_HOME" >/dev/null 2>&1
            rm -f "$IVSBOX_READY_MARK"
            alert "upgrade confirmed: 就绪标记已出现，pending -> confirmed"
        elif [ $((now - BOOT_T0)) -ge "$IVSBOX_UPGRADE_TIMEOUT" ]; then
            alert "upgrade self-test TIMEOUT (${IVSBOX_UPGRADE_TIMEOUT}s) -- 回滚到 previous"
            "$IVSBOX_UPD" boot-fail --app-root "$IVSBOX_APP_DIR" --data-root "$IVSBOX_HOME" >> "$ALERT" 2>&1
            "$IVSBOX_UPD" status --app-root "$IVSBOX_APP_DIR" --data-root "$IVSBOX_HOME" >> "$ALERT" 2>&1
            rm -f "$IVSBOX_READY_MARK"
            # 停掉当前实例：下一圈轮询就会发现它没了，用回滚后的 current 重新拉起旧版本。
            inst_pid=$(cat "$IVSBOX_PIDFILE" 2>/dev/null)
            kill -TERM "$inst_pid" 2>/dev/null || true
            # TERM 不退就 KILL（宽容期见 IVSBOX_BOOT_KILL_GRACE_S）。只 TERM 的话，一个
            # "活着但永远不落标记"的半截实例会一直占着位子：monitor 看它还在就不再重拉，
            # 于是回滚后的旧版本**永远起不来**。
            sleep "$IVSBOX_BOOT_KILL_GRACE_S"
            if [ -n "$inst_pid" ] && [ -r "/proc/$inst_pid/stat" ] && \
               [ "$(cut -d' ' -f3 "/proc/$inst_pid/stat" 2>/dev/null)" != "Z" ]; then
                alert "self-test TIMEOUT: 实例 pid=$inst_pid 未响应 TERM，升级为 KILL"
                kill -KILL "$inst_pid" 2>/dev/null || true
            fi
            BOOT_T0=0
        fi
    fi

    sleep "$POLL_S"
done
