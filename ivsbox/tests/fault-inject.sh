#!/bin/sh
# IVSBox 故障注入执行清单与记录（M7-S7.4，架构 §16.1"故障注入"行 ＋ §16.2 场景 7/8/9）
#
# ============================ 这个脚本要解决的问题 ============================
# §16.1 的"故障注入"清单列了六类：弱网 / 双网断开 / 串口拔出 / SD 卡满·拔卡 /
# 进程卡死 / 掉电。清单本身没说"在哪儿注入、注入完看什么"，于是现场最容易发生的事是
# **"跑过一遍"和"没跑过"在记录上长得一样**。本脚本把每一类钉死到三种状态之一：
#
#   RUN       本机能真注入 ⇒ **真执行**，rc=0 记 PASS，否则记 FAIL
#   DELEGATE  已有自动化用例覆盖同一注入面 ⇒ 调它，**输出里出现 SKIP 一律记 SKIPPED**
#             （绝不能把"环境不足没跑"记成通过 —— 与本工程"SKIP 不能当通过"同一条纪律）
#   MANUAL    只有真板子能做（真拔电、真拔串口、真 4G 弱网、/dev/watchdog 复位、
#             真 RTSP 源录像）⇒ 只印**注入方法 + 预期行为 + 判据**，记 NOT-RUN，
#             **不计入通过**，留给现场照着做并在记录文件里回填
#
# 末尾打印 PASS / FAIL / SKIPPED / NOT-RUN 四个计数，并把整份记录同时写到
# `--record` 指定的文件（默认 /tmp/ivsbox-fault-inject-<时间戳>.log），可直接作为
# 测试记录附件。
#
# ============================ 宿主上真注入的是什么 ============================
# **进程卡死（场景 7）**：真起 `monitor-web.sh` ＋ `monitor-media.sh` 两个守护，
# 各拉起真的 `ivsbox-web` / `ivsbox-media`，然后 `kill -STOP` 其中之一。断言：
#   ① 被卡死的那一个**真的被重启**（pid 变了、心跳/HTTP 探针恢复）；
#   ② 守护在告警日志里**明确把这一次判成 HANG**（不是"进程没了"的崩溃路径）；
#   ③ 另一个**完全没被动过**（pid 不变、心跳照常前进）⇒ 这就是"**只**重启对应服务"；
#   ④ 反方向也要干净：Web 卡死时不得出现 media 的 HANG/CRASH LOOP 记录，反之亦然。
#
# ⚠ 注意**不要**拿"monitor 状态文件里的重拉计数有没有涨"当判据：那个计数是
#   **连续**重拉计数（健康一拍就清零），卡死触发的那一次重拉恰好把它从 0 置 1，
#   读数前后都可能是 1。判据要用告警日志里的 HANG 行（首版就是栽在这条上）。
#
# 主控（ivsboxd）那一半的"卡死 ⇒ 硬件狗复位"宿主机没有 /dev/watchdog，归 MANUAL。
#
# **掉电（写库中）**：委派 `tests/integration/test_fault_db_power`（真 fork + 真 SIGKILL
# 在事务中途）。**注意**：SIGKILL 只近似"进程未及清理就消失"，不等于真掉电；
# 真拔电属 MANUAL。
#
# ============================ 用法 ============================
#   sh tests/fault-inject.sh                 # 全部宿主项
#   sh tests/fault-inject.sh --build-dir build/host
#   sh tests/fault-inject.sh --record /tmp/fa.log
#   sh tests/fault-inject.sh --keep          # 保留临时目录，便于事后查现场
#
# 依赖：`make host`（集成用例也由它一起构建）。缺产物时对应项记 SKIPPED。
set -u

BLD=build/host
KEEP=0
RECORD=""
SCRIPT_DIR=$(dirname "$0")
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT" || exit 1

while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) BLD=$2; shift 2 ;;
    --record)    RECORD=$2; shift 2 ;;
    --keep)      KEEP=1; shift ;;
    -h|--help)   sed -n '2,50p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

# 记录文件的默认位置**刻意放在临时目录之外**：收尾会删掉临时目录，
# 记录要是落在里面，跑完就没了（现场最恨这种"跑过但找不着证据"）。
[ -n "$RECORD" ] || RECORD="/tmp/ivsbox-fault-inject-$(date '+%Y%m%d-%H%M%S').log"

n_pass=0
n_fail=0
n_skip=0
n_manual=0

say() {
    printf '%s\n' "$*" | tee -a "$RECORD"
}

# ---------------------------------------------------------------------------
# 结果登记
# ---------------------------------------------------------------------------
item_pass() { say "  [PASS]    $1"; n_pass=$((n_pass + 1)); }
item_fail() { say "  [FAIL]    $1"; n_fail=$((n_fail + 1)); }
item_skip() { say "  [SKIPPED] $1"; n_skip=$((n_skip + 1)); }
item_manual() {
    n_manual=$((n_manual + 1))
    say "  [NOT-RUN] $1"
    shift
    while [ $# -gt 0 ]; do
        say "            $1"
        shift
    done
}

# 真跑一个命令，按 rc 判定；输出里出现 SKIP 一律记 SKIPPED。
run_item() {
    _name=$1
    shift
    _out=$("$@" 2>&1)
    _rc=$?
    printf '%s\n' "$_out" | sed 's/^/            | /' >> "$RECORD"
    case "$_out" in
    *SKIP*) item_skip "$_name (用例自报 SKIP)"; return ;;
    esac
    if [ "$_rc" -eq 0 ]; then item_pass "$_name"; else item_fail "$_name (rc=$_rc)"; fi
}

# 委派给已有用例：产物不存在 ⇒ SKIPPED（不是 FAIL，也不是 PASS）。
delegate() {
    _name=$1
    _bin=$2
    shift 2
    if [ ! -x "$_bin" ]; then
        item_skip "$_name (缺产物 $_bin，先 make host)"
        return
    fi
    run_item "$_name" "$_bin" "$@"
}

# ---------------------------------------------------------------------------
# 卡死注入的宿主工装
# ---------------------------------------------------------------------------
TMP=$(mktemp -d /tmp/ivfault.XXXXXX) || exit 1
APP="$TMP/app"
DATA="$TMP/data"
LOGD="$TMP/log"
ALERT="$LOGD/monitor-alert.log"
WEB_HB="$TMP/web.heartbeat"
WEB_PIDFILE="$TMP/web.pid"
MEDIA_PIDFILE="$TMP/media.pid"
WEB_PORT=${IVSBOX_WEB_PORT:-18080}   # 非 root 不能绑 80，验证用非特权端口
WEB_POLL=1
WEB_HANG=5
MEDIA_POLL=1
MEDIA_HANG_N=3

MON_WEB_PID=""
MON_MEDIA_PID=""
W_STOPPED=""
M_STOPPED=""

harness_up() {
    mkdir -p "$APP/bin" "$APP/scripts" "$APP/web/static" "$DATA" "$LOGD" || return 1
    cp "$ROOT/$BLD/ivsbox-web" "$APP/bin/" || return 1
    cp "$ROOT/$BLD/ivsbox-media" "$APP/bin/" || return 1
    cp "$ROOT/scripts/monitor-web.sh" "$ROOT/scripts/monitor-media.sh" "$APP/scripts/" || return 1

    IVSBOX_APP_DIR=$APP IVSBOX_HOME=$DATA IVSBOX_LOG_ROOT=$LOGD \
    IVSBOX_WEB_PIDFILE=$WEB_PIDFILE IVSBOX_WEB_ROOT=$APP/web/static \
    IVSBOX_WEB_PORT=$WEB_PORT IVSBOX_WEB_HEARTBEAT=$WEB_HB \
    IVSBOX_WEB_POLL_S=$WEB_POLL IVSBOX_WEB_HANG_S=$WEB_HANG \
    IVSBOX_WEB_KILL_GRACE_S=1 IVSBOX_WEB_COOLDOWN=600 \
        setsid sh "$APP/scripts/monitor-web.sh" >> "$TMP/mon-web.out" 2>&1 &
    MON_WEB_PID=$!

    IVSBOX_APP_DIR=$APP IVSBOX_HOME=$DATA IVSBOX_LOG_ROOT=$LOGD \
    IVSBOX_MEDIA_PIDFILE=$MEDIA_PIDFILE \
    IVSBOX_MEDIA_POLL_S=$MEDIA_POLL IVSBOX_MEDIA_HANG_N=$MEDIA_HANG_N \
    IVSBOX_MEDIA_PROBE_T_S=2 IVSBOX_MEDIA_KILL_GRACE_S=1 IVSBOX_MEDIA_COOLDOWN=600 \
        setsid sh "$APP/scripts/monitor-media.sh" >> "$TMP/mon-media.out" 2>&1 &
    MON_MEDIA_PID=$!
    return 0
}

harness_down() {
    [ -n "$MON_WEB_PID" ] && kill -TERM "$MON_WEB_PID" 2>/dev/null
    [ -n "$MON_MEDIA_PID" ] && kill -TERM "$MON_MEDIA_PID" 2>/dev/null
    # 守护是「唯一拉起方」，所以先停守护，再收拾它们拉起的子进程。
    for p in "$WEB_PIDFILE" "$MEDIA_PIDFILE"; do
        [ -f "$p" ] && kill -KILL "$(cat "$p" 2>/dev/null)" 2>/dev/null
    done
    # 被 STOP 过的进程对 SIGTERM 无反应（T 态下信号只挂起不生效），必须补 KILL，
    # 否则本工装会往宿主机里漏一堆永久卡死的 ivsbox-* 进程（实测漏过一次）。
    [ -n "$W_STOPPED" ] && kill -KILL "$W_STOPPED" 2>/dev/null
    [ -n "$M_STOPPED" ] && kill -KILL "$M_STOPPED" 2>/dev/null
    W_STOPPED=""
    M_STOPPED=""
}

cleanup() {
    harness_down
    if [ "$KEEP" -eq 0 ]; then
        rm -rf "$TMP" 2>/dev/null
    else
        echo "workdir kept: $TMP"
    fi
}
trap 'cleanup; exit 130' INT TERM

# pid 是否"活着"（存在且不是僵尸）
pid_alive() {
    [ -n "${1:-}" ] || return 1
    [ -r "/proc/$1/comm" ] || return 1
    [ "$(cut -d' ' -f3 "/proc/$1/stat" 2>/dev/null)" != "Z" ]
}

# 等函数返回 0，最多 W 秒
wait_ok() {
    _w=$1
    shift
    _i=0
    while [ "$_i" -lt "$_w" ]; do
        "$@" && return 0
        sleep 1
        _i=$((_i + 1))
    done
    return 1
}

pid_of() { cat "$1" 2>/dev/null; }

web_up()   { pid_alive "$(pid_of "$WEB_PIDFILE")"; }
media_up() { pid_alive "$(pid_of "$MEDIA_PIDFILE")"; }
web_hb_present() { [ -s "$WEB_HB" ]; }

web_pid_changed() {
    _now=$(pid_of "$WEB_PIDFILE")
    [ -n "$_now" ] && [ "$_now" != "$1" ] && pid_alive "$_now"
}
media_pid_changed() {
    _now=$(pid_of "$MEDIA_PIDFILE")
    [ -n "$_now" ] && [ "$_now" != "$1" ] && pid_alive "$_now"
}

# 心跳内容与上一拍不同 ⇒ 主循环在前进
HB_PREV=""
web_hb_advancing() {
    _cur=$(cat "$WEB_HB" 2>/dev/null)
    if [ -n "$_cur" ] && [ "$_cur" != "$HB_PREV" ]; then
        HB_PREV=$_cur
        return 0
    fi
    return 1
}

media_probe_ok() {
    printf 'GET /record/status HTTP/1.0\r\n\r\n' \
        | nc -w 2 127.0.0.1 18000 2>/dev/null | grep -q 'HTTP/'
}

# 告警日志里**本次注入之后新增**的行里有没有某条判定（区别于"进程没了"的崩溃路径）。
# **必须带起始行号**：告警日志是两个守护共写的**同一个**文件，直接 grep 整个文件会把
# 前一项注入留下的行也命中 —— 首版 h2d 就是这样假红的（它命中的是 h1 写下的 web HANG）。
alert_new_has() {   # alert_new_has <起始行数> <子串>
    _from=$1
    _pat=$2
    [ -f "$ALERT" ] || return 1
    tail -n "+$((_from + 1))" "$ALERT" 2>/dev/null | grep -q "$_pat"
}

# 当前告警日志行数（注入前记一次，作为"只看见新增行"的起点）
alert_lines() {
    _n=$(wc -l < "$ALERT" 2>/dev/null)
    case "$_n" in ''|*[!0-9]*) printf '0' ;; *) printf '%s' "$_n" ;; esac
}

dump_site() {
    say "            --- 现场（$TMP）---"
    for f in mon-web.out mon-media.out; do
        [ -f "$TMP/$f" ] || continue
        say "            [$f 尾部]"
        tail -n 12 "$TMP/$f" | sed 's/^/              /' | tee -a "$RECORD"
    done
    if [ -f "$ALERT" ]; then
        say "            [monitor-alert.log 尾部]"
        tail -n 15 "$ALERT" | sed 's/^/              /' | tee -a "$RECORD"
    fi
    say "            state-web=$(cat "$DATA/.monitor-web.state" 2>/dev/null)  state-media=$(cat "$DATA/.monitor-media.state" 2>/dev/null)"
}

# ---------------------------------------------------------------------------
# 逐项：进程卡死（场景 7）
# ---------------------------------------------------------------------------
HCASE_FAILED=0

item_hang_web() {
    say "  注入：kill -STOP ivsbox-web（主循环被冻结，进程仍存在于 /proc）"
    _w0=$(pid_of "$WEB_PIDFILE")
    _m0=$(pid_of "$MEDIA_PIDFILE")
    W_STOPPED=$_w0
    _al0=$(alert_lines)

    kill -STOP "$_w0" 2>/dev/null
    if wait_ok $((WEB_HANG + 20)) web_pid_changed "$_w0"; then
        say "            web pid: $_w0 -> $(pid_of "$WEB_PIDFILE")"
        if wait_ok 15 web_hb_advancing; then
            item_pass "h1  Web 卡死被检出并重启（心跳恢复前进）"
        else
            item_fail "h1  Web 被重启了，但新实例心跳没有恢复前进"; HCASE_FAILED=1
        fi
    else
        item_fail "h1  Web 卡死未被检出（pid 仍是 $(pid_of "$WEB_PIDFILE")）"; HCASE_FAILED=1
    fi

    # 判据必须是"守护把它判成了卡死"，而不是"它恰好被重拉了"
    if alert_new_has "$_al0" 'monitor-web: web HANG'; then
        item_pass "h1c 告警日志明确判成 HANG（不是走「进程没了」的崩溃路径）"
    else
        item_fail "h1c 告警日志里没有 web HANG 判定"; HCASE_FAILED=1
    fi
    if alert_new_has "$_al0" 'monitor-media: media HANG' || \
       alert_new_has "$_al0" 'monitor-media: media CRASH'; then
        item_fail "h1d Web 卡死却把媒体也判成异常了（违反「只对应服务」的约束）"; HCASE_FAILED=1
    else
        item_pass "h1d 媒体侧没有任何 HANG/CRASH 判定"
    fi

    _m1=$(pid_of "$MEDIA_PIDFILE")
    if [ "$_m0" = "$_m1" ] && pid_alive "$_m1"; then
        item_pass "h1b [场景 7] Web 卡死期间媒体未被牵连（media pid 始终 $_m1）"
    else
        item_fail "h1b Web 卡死把媒体也一起重启了（media $_m0 -> $_m1）"; HCASE_FAILED=1
    fi
}

item_hang_media() {
    say "  注入：kill -STOP ivsbox-media（bird_media 一行不改，靠外部 HTTP 探针判死）"
    _m0=$(pid_of "$MEDIA_PIDFILE")
    _w0=$(pid_of "$WEB_PIDFILE")
    M_STOPPED=$_m0
    _al0=$(alert_lines)
    HB_PREV=$(cat "$WEB_HB" 2>/dev/null)

    kill -STOP "$_m0" 2>/dev/null
    if wait_ok $((MEDIA_POLL * MEDIA_HANG_N + 25)) media_pid_changed "$_m0"; then
        say "            media pid: $_m0 -> $(pid_of "$MEDIA_PIDFILE")"
        if wait_ok 15 media_probe_ok; then
            item_pass "h2  媒体卡死被检出并重启（HTTP 探针恢复应答）"
        else
            item_fail "h2  媒体被重启了，但探针仍无应答"; HCASE_FAILED=1
        fi
    else
        item_fail "h2  媒体卡死未被检出（pid 仍是 $(pid_of "$MEDIA_PIDFILE")）"; HCASE_FAILED=1
    fi

    if alert_new_has "$_al0" 'monitor-media: media HANG'; then
        item_pass "h2c 告警日志明确判成 HANG（探针判据生效，非「进程没了」）"
    else
        item_fail "h2c 告警日志里没有 media HANG 判定"; HCASE_FAILED=1
    fi
    if alert_new_has "$_al0" 'monitor-web: web HANG' || \
       alert_new_has "$_al0" 'monitor-web: web CRASH'; then
        item_fail "h2d 媒体卡死却把 Web 也判成异常了（违反「只对应服务」的约束）"; HCASE_FAILED=1
    else
        item_pass "h2d Web 侧没有任何 HANG/CRASH 判定"
    fi

    _w1=$(pid_of "$WEB_PIDFILE")
    if [ "$_w0" = "$_w1" ] && pid_alive "$_w1"; then
        item_pass "h2b [场景 7] 媒体卡死期间 Web 未被牵连（web pid 始终 $_w1）"
    else
        item_fail "h2b 媒体卡死把 Web 也一起重启了（web $_w0 -> $_w1）"; HCASE_FAILED=1
    fi
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
say "============= IVSBox 故障注入执行记录（M7-S7.4 / 架构 §16.1·§16.2） ============="
say "时间      : $(date '+%Y-%m-%d %H:%M:%S')"
say "主机      : $(uname -srm)"
say "构建目录  : $BLD"
say "记录文件  : $RECORD"

say ""
say "---- 一、宿主可真注入（RUN）----"
if [ -x "$ROOT/$BLD/ivsbox-web" ] && [ -x "$ROOT/$BLD/ivsbox-media" ] && \
   command -v nc >/dev/null 2>&1; then
    if harness_up; then
        if wait_ok 20 web_up && wait_ok 20 media_up && wait_ok 20 web_hb_present; then
            item_hang_web
            item_hang_media
        else
            item_fail "h1/h2 工装未能在 20 s 内拉起 web+media"; HCASE_FAILED=1
        fi
    else
        item_fail "h1/h2 工装准备失败（无法拷贝产物？）"; HCASE_FAILED=1
    fi
    if [ "$HCASE_FAILED" -ne 0 ]; then
        dump_site
    fi
else
    item_skip "h1 Web 卡死注入（缺 $BLD/ivsbox-web、$BLD/ivsbox-media 或 nc，先 make host）"
    item_skip "h2 媒体卡死注入（同上）"
fi

say ""
say "---- 二、已有用例覆盖同一注入面（DELEGATE）----"
delegate "h4 掉电 · 写库中（事务中途被 SIGKILL，重启不得采纳半截事务）" \
         "$ROOT/$BLD/tests/integration/test_fault_db_power"
delegate "h5 掉电 · 升级中（5 个落盘相位逐个钉住「下一轮启动哪个版本」）" \
         "$ROOT/$BLD/tests/unit/test_upd"
delegate "h6 SD 卡满 / 拔卡（场景 6，需私有 ns）" \
         "$ROOT/$BLD/tests/integration/test_sd_fault_e2e"
delegate "h7 弱网 / 双网断开（场景 1·2，私有 netns 等价）" \
         "$ROOT/$BLD/tests/integration/test_agent_switch"
delegate "h8 UART 半包 / 粘包 / 超长（串口拔出的软件侧等价：读写错误路径）" \
         "$ROOT/$BLD/tests/unit/test_serial"

say ""
say "---- 三、只有真板子能做（NOT-RUN，必须回填现场结果）----"
item_manual 'm1 弱网（4G 弱信号 / 有意丢包）' \
    '注入：板端 `tc qdisc add dev usb0 root netem loss 30% delay 300ms`；或把 4G 天线拔到临界' \
    '预期：iv_probe 判活失败达阈值 → 出口切到 eth0（§7.2 六态），平台连接从新口重建' \
    '判据：monitor-alert.log 无 CRASH LOOP；平台侧确认会话重建；未确认数据在补传队列里不丢不重'
item_manual 'm2 双网断开（真拔网线 + 断 4G）' \
    '注入：拔掉 eth0 网线，同时 `ip link set usb0 down`' \
    '预期：状态机进入全断 → iv_recctl hold 10 s 后自动开录；恢复后 debounce 60 s 停录并封存片段' \
    '判据：录播目录出现片段且索引条目完整；恢复后平台收到补传数据'
item_manual 'm3 串口拔出（采集板 UART）' \
    '注入：直接拔掉 ttySAC5 的接线（硬件侧无对应 sysfs 开关，只能拔线）' \
    '预期：iv_serial 读失败 → 重连循环，主控不退出、不重启整机；平台业务继续' \
    '判据：日志有串口重连行且**没有**整机复位；门事件在重连后恢复上报'
item_manual 'm4 SD 卡满 / 拔卡' \
    '注入：把 SD 灌满（`dd if=/dev/zero of=/mnt/sdcard/fill bs=1M`）或在录像中直接拔卡' \
    '预期：层次告警（85 清 / 90 拒 / 95 停），控制与平台业务继续；sys.overview 的 storage 段带 fault/alarm' \
    '判据：Web 页面看到告警；采集/上报不受影响（§16.2 场景 6 的真机部分）'
item_manual 'm5 主 Reactor 卡死 ⇒ 硬件狗复位' \
    '注入：`kill -STOP $(pidof ivsboxd)`（冻结主循环，不再喂狗）' \
    '预期：约 16 s 后整机被 /dev/watchdog 复位；复位后 monitor.sh 按熔断状态决定是否再拉起' \
    '判据：串口能看到复位日志；**不得**出现"没复位但服务一直停着"（那说明狗没武装）' \
    '注意：宿主机没有 /dev/watchdog，本项只能在真机做'
item_manual 'm6 掉电 · 升级中' \
    '注入：`ivsbox-updater install <pkg>` 执行到一半直接断电（或按 5 个落盘相位分别断电）' \
    '预期：重启后**旧版本可运行**；半截的新版本既不被确认也不被当成 current' \
    '判据：`ivsbox-updater status` 的 version 仍是旧版本；boot-begin 给出的目录里 ivsboxd 可执行'
item_manual 'm7 掉电 · 录像中' \
    '注入：真 RTSP 源在录（record.start）时断电' \
    '预期：重启后不完整分片**不被索引为有效片段**，已封存分片可播放' \
    '判据：录像索引条数与磁盘上完整分片一致；媒体进程重启后能继续录新片段'
item_manual 'm8 掉电 · 写库中' \
    '注入：主控正在写 control.db 时断电（宿主近似见 h4，真掉电丢 page cache）' \
    '预期：重启后 integrity_check 通过；已提交事务在、未提交事务不在；损坏时现场被改名 *.corrupt' \
    '判据：`sqlite3 control.db "PRAGMA integrity_check;"` 回 ok；无 half transaction'

say ""
say "================ 汇总 ================"
say "PASS      : $n_pass"
say "FAIL      : $n_fail"
say "SKIPPED   : $n_skip"
say "NOT-RUN   : $n_manual   （真机项，见上文 m1~m8，**不计入通过**）"
say ""
if [ "$n_fail" -eq 0 ]; then
    say "宿主部分结论：无 FAIL。**M7-S7.4 尚未关闭** —— 还有 $n_manual 项真机注入未做。"
else
    say "宿主部分结论：有 $n_fail 项 FAIL，必须先修。"
fi

cleanup
trap - INT TERM
if [ "$n_fail" -eq 0 ]; then
    exit 0
fi
exit 1
