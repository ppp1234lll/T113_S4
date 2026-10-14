#!/bin/sh
# IVSBox 长稳测试脚本（M7-S7.5）—— ≥72 h 连续运行 ＋ 周期扰动 ＋ 资源斜率监测
#
# ============================ 它对什么负责 ============================
# 计划 §S7.5 的判据："**72h 无重启、无 OOM、资源曲线平稳**"。这句话拆成三件可测的事：
#
#   ① **无重启**：主控 pid 全程不变（没有崩溃重拉）；`/proc/uptime` 单调不倒退
#      （没有整机复位）；monitor 状态文件里的重拉计数不增。
#   ② **无 OOM**：`dmesg` 里没有 oom-kill 记录（取不到 dmesg 时**明确记"不可判"**，
#      不默认当成通过）。
#   ③ **资源曲线平稳**：对 RSS / fd / 线程数 / `control.db` 大小做**最小二乘斜率**
#      （单位/小时），与阈值比较。斜率只在**足够长的连续运行**上才有意义
#      —— 短跑出来的斜率是噪声，故设 `--slope-min-hours`（默认 6）：运行时长不到
#      这个数就**只采样、不下斜率结论**（打印"斜率判据未生效"），绝不拿 3 分钟的
#      曲线宣称"平稳"。这是本脚本最重要的诚实约束。
#
# 周期扰动（§S7.5"脚本化周期触发"）：
#   网络切换  —— 只有显式给 `--net-iface` 才做（默认不动网络：现场设备上擅自
#                `ip link set down` 会切断管理链路，必须由人决定）；
#   媒体启停  —— 走媒体进程自己的 HTTP 口 `/record/start` → `/record/stop`；
#   通道查询  —— 走 Web 的 `/api/overview`（内含 `sys.overview`）→ 真正打到本地通道。
#
# ============================ 诚实降级 ============================
# 每一个周期动作都单独计数 ok/fail/skip，末尾一并报出：
#   - 没有 `wget` ⇒ 通道查询与媒体启停记 `skip`（**不算通过**）；
#   - 登录失败 / Web 未监听 ⇒ 通道查询记 `fail`（这是真故障，不是环境不足）；
#   - 媒体口无应答 ⇒ 媒体启停记 `fail`。
# "跳过"与"失败"是两回事，报告里分开写，别让环境不足把真故障盖住。
#
# ============================ 用法 ============================
#   sh tests/soak.sh --minutes 3                 # 冒烟：只验脚本与采样口径
#   sh tests/soak.sh --hours 72                  # 现场：完整长稳
#   sh tests/soak.sh --hours 72 --net-iface usb0 # 现场：连网络切换一起扰
#   sh tests/soak.sh --hours 72 --media-url 'rtsp%3A%2F%2F192.168.1.10%3A554%2Fstream'
#   sh tests/soak.sh --hours 72 --outdir /mnt/UDISK/ivsbox/soak
#
# 产物：`<outdir>/soak-<时间戳>.csv`（每次采样一行）＋ `...report.txt`
# 退出码：0＝无硬性违规；1＝有违规（pid 变过 / uptime 倒退 / 出现 OOM / 斜率超阈）
set -u

HOURS=72
MINUTES=""
SAMPLE_S=60
CYCLE_S=300
NET_IFACE=""
MEDIA_URL="rtsp%3A%2F%2F127.0.0.1%3A1%2Fnone"
# 通道号：bird_media 的 MEDIA_MAX_CHANNELS=8 ⇒ 合法范围 **0..7**，越界一律 400。
MEDIA_ID=0
WEB_PORT=80
WEB_PASS=admin
MEDIA_PORT=18000
SLOPE_MIN_H=6
RSS_SLOPE_MAX=256      # KB/小时
FD_SLOPE_MAX=1         # 个/小时
THR_SLOPE_MAX=1        # 个/小时
DB_SLOPE_MAX=4194304   # 字节/小时（仅告警，不判失败：审计库本来就该长）
OUTDIR=""
APP_DIR=/opt/ivsbox
DATA_ROOT=/mnt/UDISK/ivsbox

while [ $# -gt 0 ]; do
    case "$1" in
    --hours)           HOURS=$2; shift 2 ;;
    --minutes)         MINUTES=$2; shift 2 ;;
    --sample-s)        SAMPLE_S=$2; shift 2 ;;
    --cycle-s)         CYCLE_S=$2; shift 2 ;;
    --net-iface)       NET_IFACE=$2; shift 2 ;;
    --media-url)       MEDIA_URL=$2; shift 2 ;;
    --media-id)        MEDIA_ID=$2; shift 2 ;;
    --web-port)        WEB_PORT=$2; shift 2 ;;
    --web-pass)        WEB_PASS=$2; shift 2 ;;
    --media-port)      MEDIA_PORT=$2; shift 2 ;;
    --slope-min-hours) SLOPE_MIN_H=$2; shift 2 ;;
    --rss-slope-max)   RSS_SLOPE_MAX=$2; shift 2 ;;
    --fd-slope-max)    FD_SLOPE_MAX=$2; shift 2 ;;
    --thr-slope-max)   THR_SLOPE_MAX=$2; shift 2 ;;
    --outdir)          OUTDIR=$2; shift 2 ;;
    --app-dir)         APP_DIR=$2; shift 2 ;;
    --data-root)       DATA_ROOT=$2; shift 2 ;;
    -h|--help)         sed -n '2,45p' "$0"; exit 0 ;;
    *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

if [ -n "$MINUTES" ]; then
    TOTAL_S=$((MINUTES * 60))
    MODE="smoke(${MINUTES}min)"
else
    TOTAL_S=$((HOURS * 3600))
    MODE="soak(${HOURS}h)"
fi

TS=$(date '+%Y%m%d-%H%M%S')
if [ -z "$OUTDIR" ]; then
    OUTDIR=$(mktemp -d "/tmp/ivsoak-$TS.XXXXXX") || exit 1
else
    mkdir -p "$OUTDIR" || exit 1
fi
CSV="$OUTDIR/soak-$TS.csv"
REPORT="$OUTDIR/soak-$TS.report.txt"
COOKIE="$OUTDIR/.cookies"

have_wget=0
command -v wget >/dev/null 2>&1 && have_wget=1

# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------
say() { printf '%s\n' "$*" | tee -a "$REPORT"; }

mono_now() { cut -d. -f1 /proc/uptime 2>/dev/null; }

# 按进程名找 pid（不依赖 pidof/ps：纯 /proc 扫描，busybox 与发行版通吃）
find_pid() {
    for d in /proc/[0-9]*; do
        [ -r "$d/comm" ] || continue
        if [ "$(cat "$d/comm" 2>/dev/null)" = "$1" ]; then
            printf '%s' "${d#/proc/}"
            return 0
        fi
    done
    return 1
}

proc_metrics() {   # <comm> -> "rss_kb fds threads"（取不到给 -1，绝不假装 0）
    _pid=$(find_pid "$1") || { printf '%s' '-1 -1 -1'; return; }
    _rss=$(awk '/^VmRSS:/{print $2}' "/proc/$_pid/status" 2>/dev/null)
    _thr=$(awk '/^Threads:/{print $2}' "/proc/$_pid/status" 2>/dev/null)
    _fds=$(ls -1 "/proc/$_pid/fd" 2>/dev/null | wc -l)
    [ -n "$_rss" ] || _rss=-1
    [ -n "$_thr" ] || _thr=-1
    printf '%s %s %s' "$_rss" "$_thr" "$_fds"
}

read_rc() {   # 读 monitor 状态文件的第二列（重拉计数）
    [ -f "$1" ] || { printf '0'; return; }
    read -r _a _b _c < "$1" 2>/dev/null || true
    case "$_b" in ''|*[!0-9]*) printf '0' ;; *) printf '%s' "$_b" ;; esac
}

rss_total_rc() {
    _s=$(read_rc "$DATA_ROOT/.monitor.state")
    _w=$(read_rc "$DATA_ROOT/.monitor-web.state")
    _m=$(read_rc "$DATA_ROOT/.monitor-media.state")
    printf '%s' "$((_s + _w + _m))"
}

# 探活/查询类请求一律 `-t 1`（只试一次）：wget 默认重试 20 次，一旦对端"连得上但
# 不回字节"，单个请求就会阻塞几十秒，把长稳循环的采样/扰动节奏整个拖歪。
http_get() {   # http_get <url> [cookie-file] -> 0/1
    if [ -n "${2:-}" ]; then
        wget -q -t 1 -O /dev/null -T 10 --load-cookies "$2" "$1" >/dev/null 2>&1
    else
        wget -q -t 1 -O /dev/null -T 10 "$1" >/dev/null 2>&1
    fi
}

# ---------------------------------------------------------------------------
# 周期扰动
# ---------------------------------------------------------------------------
c_net_ok=0; c_net_fail=0; c_net_skip=0
c_med_ok=0; c_med_fail=0; c_med_skip=0
c_ctl_ok=0; c_ctl_fail=0; c_ctl_skip=0

cycle_net() {
    # 网络切换：**必须显式给 --net-iface**。现场设备上擅自把 WAN 口 down 掉会切断
    # 管理链路（ssh 也会断），这是人的决定，不是脚本该替人做的决定。
    if [ -z "$NET_IFACE" ]; then
        c_net_skip=$((c_net_skip + 1))
        return
    fi
    if command -v ip >/dev/null 2>&1; then
        if ip link set "$NET_IFACE" down 2>/dev/null && sleep 3 && \
           ip link set "$NET_IFACE" up 2>/dev/null; then
            c_net_ok=$((c_net_ok + 1))
        else
            c_net_fail=$((c_net_fail + 1))
        fi
    else
        c_net_skip=$((c_net_skip + 1))
    fi
}

cycle_media() {
    if [ "$have_wget" -eq 0 ]; then
        c_med_skip=$((c_med_skip + 1))
        return
    fi
    if http_get "http://127.0.0.1:$MEDIA_PORT/record/start?id=$MEDIA_ID&url=$MEDIA_URL" && \
       http_get "http://127.0.0.1:$MEDIA_PORT/record/stop?id=$MEDIA_ID"; then
        c_med_ok=$((c_med_ok + 1))
    else
        c_med_fail=$((c_med_fail + 1))
    fi
}

cycle_ctl() {
    if [ "$have_wget" -eq 0 ]; then
        c_ctl_skip=$((c_ctl_skip + 1))
        return
    fi
    if http_get "http://127.0.0.1:$WEB_PORT/api/overview" "$COOKIE"; then
        c_ctl_ok=$((c_ctl_ok + 1))
    else
        c_ctl_fail=$((c_ctl_fail + 1))
    fi
}

login_web() {
    [ "$have_wget" -eq 1 ] || return 1
    wget -q -O /dev/null -T 10 --save-cookies "$COOKIE" --keep-session-cookies \
         --post-data "{\"pass\":\"$WEB_PASS\"}" \
         "http://127.0.0.1:$WEB_PORT/api/login" >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
# 开跑
# ---------------------------------------------------------------------------
t_start=$(date +%s)
up_start=$(mono_now)
case "$up_start" in ''|*[!0-9]*) up_start=0 ;; esac

say "================ IVSBox 长稳测试（M7-S7.5） ================"
say "模式        : $MODE   （总时长 $((TOTAL_S / 60)) 分钟）"
say "主机        : $(uname -srm)"
say "采样周期    : ${SAMPLE_S}s    扰动周期: ${CYCLE_S}s"
say "网络口      : ${NET_IFACE:-（未指定，网络切换不注入）}"
say "媒体地址    : $MEDIA_URL"
say "程序/数据根 : $APP_DIR / $DATA_ROOT"
say "判据        : 无重启 + 无 OOM + 资源斜率（时长 >= ${SLOPE_MIN_H}h 才判定）"
say "CSV         : $CSV"
say "报告        : $REPORT"
say ""

pid0=$(find_pid ivsboxd) || pid0=""
up_last=$up_start
reboot_count=0
pid_changes=0
rc0=$(rss_total_rc)
cycle=0
next_cycle=0

if [ "$have_wget" -eq 0 ]; then
    say "WARNING: 本机没有 wget ⇒ 通道查询与媒体启停无法注入，二者一律记 skip；"
    say "         请勿把这理解为「扰动都做了」。"
fi
if ! login_web; then
    say "WARNING: Web 登录失败（端口 $WEB_PORT 未监听 / 口令不对 / wget 不支持 cookie）"
    say "         ⇒ 通道查询会一直记 fail；请先确认 ivsbox-web 在跑且口令正确。"
fi

printf '%s\n' "ts,elapsed_h,cycle,rss_ivsboxd,fds_ivsboxd,thr_ivsboxd,rss_web,fds_web,thr_web,rss_media,fds_media,thr_media,db_bytes,uptime_s,ivsboxd_pid,pid_changes" > "$CSV"

sample_n=0
while :; do
    now=$(date +%s)
    el=$((now - t_start))
    [ "$el" -lt "$TOTAL_S" ] || break

    # ---- 采样 ----
    set -- $(proc_metrics ivsboxd); rss_a=$1; thr_a=$2; fds_a=$3
    set -- $(proc_metrics ivsbox-web); rss_w=$1; thr_w=$2; fds_w=$3
    set -- $(proc_metrics ivsbox-media); rss_m=$1; thr_m=$2; fds_m=$3
    # 库文件可能还没建（主控没跑 / 首次启动）：取不到记 0。
    # **不要写 `wc -c < 缺失文件 2>/dev/null`**：报错是 shell 自己发的、由输入重定向
    # 失败引起，`2>/dev/null` 拦不住，72h 跑下来会把 stderr 淹掉。先判存在最干净。
    if [ -f "$DATA_ROOT/db/control.db" ]; then
        db_bytes=$(wc -c < "$DATA_ROOT/db/control.db")
    else
        db_bytes=0
    fi
    case "$db_bytes" in ''|*[!0-9]*) db_bytes=0 ;; esac
    up_now=$(mono_now)
    case "$up_now" in ''|*[!0-9]*) up_now=$up_last ;; esac
    if [ "$up_now" -lt "$up_last" ]; then
        reboot_count=$((reboot_count + 1))
        say "[$(date '+%H:%M:%S')] *** uptime 倒退（$up_last -> $up_now）：疑似整机复位 ***"
    fi
    up_last=$up_now

    pid_now=$(find_pid ivsboxd) || pid_now=""
    if [ -n "$pid0" ] && [ "$pid_now" != "$pid0" ]; then
        pid_changes=$((pid_changes + 1))
        say "[$(date '+%H:%M:%S')] *** ivsboxd pid 变化（$pid0 -> ${pid_now:-<gone>}）：主控重启过 ***"
        [ -n "$pid_now" ] && pid0=$pid_now
    fi

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$(date '+%Y-%m-%dT%H:%M:%S')" \
        "$(awk -v s="$el" 'BEGIN{printf "%.3f", s/3600}')" \
        "$cycle" "$rss_a" "$fds_a" "$thr_a" "$rss_w" "$fds_w" "$thr_w" \
        "$rss_m" "$fds_m" "$thr_m" "$db_bytes" "$up_now" "${pid_now:-none}" \
        "$pid_changes" >> "$CSV"
    sample_n=$((sample_n + 1))

    # ---- 周期扰动 ----
    if [ "$el" -ge "$next_cycle" ]; then
        cycle=$((cycle + 1))
        say "[$(date '+%H:%M:%S')] cycle $cycle：网络切换 / 媒体启停 / 通道查询"
        cycle_net
        cycle_media
        cycle_ctl
        next_cycle=$((el + CYCLE_S))
    fi

    sleep "$SAMPLE_S"
done

# ---------------------------------------------------------------------------
# 斜率（最小二乘，x＝小时）与首末峰
# ---------------------------------------------------------------------------
# 列号与 CSV 表头一一对应：4=RSS 5=fd 6=线程 13=control.db 字节
slope_of() {
    awk -F, -v c="$1" '
        NR > 1 && $c != "" && $c >= 0 { n++; sx += $2; sy += $c; sxx += $2*$2; sxy += $2*$c }
        END {
            if (n < 2) { printf "nan"; exit }
            d = n*sxx - sx*sx
            if (d == 0) { printf "nan"; exit }
            printf "%.3f", (n*sxy - sx*sy) / d
        }' "$CSV"
}

stat_of() {
    awk -F, -v c="$1" '
        NR > 1 && $c != "" && $c >= 0 {
            if (n == 0) first = $c
            last = $c
            if (n == 0 || $c > max) max = $c
            n++
        }
        END { if (n == 0) printf "n/a n/a n/a"; else printf "%s %s %s", first, last, max }' "$CSV"
}

el_h=$(awk -v s="$(( $(date +%s) - t_start ))" 'BEGIN{printf "%.2f", s/3600}')
rc1=$(rss_total_rc)

say ""
say "================ 结果 ================"
say "采样点数      : $sample_n"
say "实际运行      : ${el_h} 小时"
say "扰动轮次      : $cycle"
say " 网络切换    : ok=$c_net_ok fail=$c_net_fail skip=$c_net_skip"
say " 媒体启停    : ok=$c_med_ok fail=$c_med_fail skip=$c_med_skip"
# 判据只说"Web 应答了"：Web 把 /api/overview 转成对本地通道的 sys.overview 调用，
# 通道不可达时它仍会以 200 回一个错误信封 ⇒ ok 只证明"查询链路被打到了且 Web 活着"，
# 不证明通道语义正确（那是集成用例的事）。别把这一列读成"业务正确性"。
say " 通道查询    : ok=$c_ctl_ok fail=$c_ctl_fail skip=$c_ctl_skip （ok ＝ Web 应答，非业务正确性）"
say ""

viol=0

say "— (1) 无重启 —"
say "  ivsboxd 重启次数（pid 变化）: $pid_changes"
say "  整机复位次数（uptime 倒退）  : $reboot_count"
say "  monitor 重拉计数合计         : $rc0 -> $rc1"
if [ "$reboot_count" -ne 0 ]; then
    say "  ** 违规：发生了整机复位 **"; viol=1
fi
if [ "$pid_changes" -gt 1 ]; then
    say "  ** 违规：主控多次重启（共 $pid_changes 次）**"; viol=1
fi
if [ "$rc1" -gt "$rc0" ]; then
    say "  注意：守护侧重拉计数增加了（附属服务可能崩过，见 monitor-alert.log）"
fi

say ""
say "— (2) 无 OOM —"
if command -v dmesg >/dev/null 2>&1; then
    _dm=$(dmesg 2>/dev/null)
else
    _dm=""
fi
if [ -z "$_dm" ]; then
    say "  不可判：取不到 dmesg 内容（无权限或内核日志为空）——**不默认当成通过**"
else
    oom=$(printf '%s\n' "$_dm" | grep -i -c 'out of memory\|oom-kill')
    case "$oom" in
    ''|*[!0-9]*) say "  不可判：dmesg 输出无法解析" ;;
    *) if [ "$oom" -eq 0 ]; then
           say "  dmesg 无 oom-kill 记录"
       else
           say "  ** 违规：dmesg 出现 $oom 条 oom 记录 **"; viol=1
       fi ;;
    esac
fi

say ""
say "— (3) 资源曲线 —"
if awk -v h="$el_h" -v m="$SLOPE_MIN_H" 'BEGIN{exit !(h >= m)}'; then
    for spec in "4:RSS(ivsboxd)_KB:$RSS_SLOPE_MAX" "5:fd(ivsboxd):$FD_SLOPE_MAX" \
                "6:threads(ivsboxd):$THR_SLOPE_MAX" "13:control.db_bytes:$DB_SLOPE_MAX"; do
        col=${spec%%:*}
        rest=${spec#*:}
        name=${rest%%:*}
        lim=${rest##*:}
        sl=$(slope_of "$col")
        set -- $(stat_of "$col")
        say "  $name: first=$1 last=$2 peak=$3 slope=${sl}/h  limit=$lim"
        if [ "$sl" != "nan" ]; then
            over=$(awk -v s="$sl" -v l="$lim" 'BEGIN{print (s > l) ? 1 : 0}')
            if [ "$over" -eq 1 ]; then
                case "$name" in
                control.db_bytes) say "    （仅告警：审计库本来就会增长，不判失败）" ;;
                *) say "    ** 违规：斜率超阈，疑似泄漏 **"; viol=1 ;;
                esac
            fi
        fi
    done
else
    say "  斜率判据**未生效**：运行 ${el_h}h < ${SLOPE_MIN_H}h。"
    say "  短跑的曲线斜率是噪声，拿它下「平稳」结论是自欺 —— 请按 --hours 72 现场跑。"
    for spec in "4:RSS(ivsboxd)_KB" "5:fd(ivsboxd)" "6:threads(ivsboxd)" "13:control.db_bytes"; do
        col=${spec%%:*}
        name=${spec##*:}
        set -- $(stat_of "$col")
        say "  $name: first=$1 last=$2 peak=$3（斜率略）"
    done
fi

say ""
if [ "$viol" -eq 0 ]; then
    say "结论：本次运行未见硬性违规。"
    if [ -n "$MINUTES" ]; then
        say "**注意**：这是冒烟运行，只证明脚本与采样口径可用；"
        say "M7-S7.5 的「72h 无重启/无 OOM/曲线平稳」仍**未验收**，须现场按 --hours 72 跑满。"
    else
        say "运行时长已按 ${HOURS}h 配置；若本次跑满，则 S7.5 的判据在此次记录上成立。"
    fi
else
    say "结论：**存在硬性违规**，见上文标记。"
fi

if [ "$viol" -eq 0 ]; then
    exit 0
fi
exit 1
