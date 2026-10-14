#!/bin/sh
# =============================================================================
# IVSBox 升级 / 启动链路 端到端用例（M7-S7.2 ＋ S7.3 的装配层证据）
#
# 为什么需要它：`tests/unit/test_upd.c` 测的是「C 函数在给定盘面下的行为」，而
# `monitor.sh → scripts/ivsbox-run.sh → boot-begin → 换目录 exec` 这条**接线**在 C 单测里
# 根本不存在。项目教训：接线在装配层，单测测不到，只有真把进程拉起来的用例抓得到。
# 本脚本就是那一层 —— 真跑 `build/host/ivsbox-updater`、真跑 `scripts/ivsbox-run.sh`、
# 真跑 `scripts/monitor.sh`，盘面（app_root / releases / 链接 / pending.json）在 /tmp 下现造。
#
# 覆盖：
#   u1 裸布局        没有 releases/previous 时起裸布局 `bin/ivsboxd`
#   u2 链接生效      `current → releases/<ver>` 时起该版本目录
#   u3 目标缺主控    该目录没有可执行 `bin/ivsboxd` ⇒ 回滚也救不回 ⇒ 退出码 127
#   u4 bootcount 超限 手工造 pending(bootcount=3/max_boot=3) ⇒ 自动回滚到 previous
#                    （计划 §S7.3 验证条「bootcount 超限注入→回滚」的脚本化）
#   u5 超时回滚＋卡死 monitor 驱动：新版永远不落就绪标记 ⇒ 超时回滚；实例忽略 TERM ⇒
#                    升级为 KILL。同一用例顺带验证「上一实例落下的**陈旧就绪标记必须在
#                    拉起前清掉**」：清不掉的话新版会被瞬时误确认（state=confirmed 是终点，
#                    回滚再也发生不了），于是在途的坏版本被永久"确认"为健康。
#
# 怎么跑（在编译 VM 上，先 `make host`）：
#   sh tests/upgrade-boot.sh [--app-src DIR] [--updater BIN] [--cc CC] [--keep]
# 退出码：0 全过 / 1 有 FAIL / 3 缺 host 构建产物（**不算通过**）
#
# ⚠ 假主控必须是**编译出来的 ELF**：monitor.sh 的存活判定是 `ps -o comm | grep -x ivsboxd`，
# 而 shell 脚本的 comm 是解释器名（sh/ash），永远匹配不上 —— 拿脚本假装主控，守护逻辑
# 根本进不去，用例会"静默什么都没测"却报绿。
#
# ⚠ 本脚本会 KILL 所有 comm == ivsboxd 的进程（收尾清场）。请在**没有真 ivsboxd 在跑**的
# 机器上执行（编译 VM 符合）。
# =============================================================================
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd)
APP_SRC=""
UPD_BIN=""
CC_BIN=${CC:-cc}
KEEP=0

usage() {
    printf '用法: sh tests/upgrade-boot.sh [--app-src DIR] [--updater BIN] [--cc CC] [--keep]\n'
}

while [ $# -gt 0 ]; do
    case "$1" in
    --app-src) APP_SRC=${2:-}; shift 2 ;;
    --updater) UPD_BIN=${2:-}; shift 2 ;;
    --cc) CC_BIN=${2:-}; shift 2 ;;
    --keep) KEEP=1; shift ;;
    -h | --help)
        usage
        exit 0
        ;;
    *)
        printf '未知参数: %s\n' "$1" >&2
        usage >&2
        exit 1
        ;;
    esac
done

[ -n "$APP_SRC" ] || APP_SRC=$(cd "$SELF_DIR/.." && pwd)
[ -n "$UPD_BIN" ] || UPD_BIN=$APP_SRC/build/host/ivsbox-updater
RUN_SH=$APP_SRC/scripts/ivsbox-run.sh
MON_SH=$APP_SRC/scripts/monitor.sh

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$1"; }
chk() { # chk <名> <实得> <期望>
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want [$3] got [$2])"; fi
}
chk_has() { # chk_has <名> <文件> <子串>
    if grep -q -- "$3" "$2" 2>/dev/null; then ok "$1"; else bad "$1 (在 $2 里找不到 [$3])"; fi
}
chk_not() { # chk_not <名> <文件> <子串>
    if grep -q -- "$3" "$2" 2>/dev/null; then bad "$1 (不该出现却出现了 [$3])"; else ok "$1"; fi
}

for f in "$UPD_BIN" "$RUN_SH" "$MON_SH"; do
    if [ ! -f "$f" ]; then
        printf 'SKIP: 缺少 %s —— 先在 ivsbox/ 下跑 `make host`（本用例要 host 构建产物）\n' "$f" >&2
        exit 3
    fi
done

TMP=$(mktemp -d "${TMPDIR:-/tmp}/ivboot.XXXXXX") || exit 4
TMP=$(cd "$TMP" && pwd) # 消掉 /tmp 可能的符号链接：后面断言比的是真路径
FAKE_OUT=$TMP/fake.out
: >"$FAKE_OUT"

MON_PID=""
WRAP_PID=""
CR=""
RD=""
LD=""
WDIR=""
WRC=""

kill_fakes() {
    for d in /proc/[0-9]*; do
        [ -r "$d/comm" ] || continue
        [ "$(cat "$d/comm" 2>/dev/null)" = "ivsboxd" ] || continue
        kill -KILL "${d#/proc/}" 2>/dev/null
    done
}

count_fakes() {
    _n=0
    for d in /proc/[0-9]*; do
        [ -r "$d/comm" ] || continue
        [ "$(cat "$d/comm" 2>/dev/null)" = "ivsboxd" ] || continue
        # 状态位 Z 不算活（僵尸只是还没被收尸，comm 仍然是 ivsboxd）
        [ "$(cut -d' ' -f3 "$d/stat" 2>/dev/null)" != "Z" ] || continue
        _n=$((_n + 1))
    done
    printf '%s' "$_n"
}

stop_monitor() {
    [ -n "$MON_PID" ] || return 0
    kill -TERM "$MON_PID" 2>/dev/null
    sleep 1
    kill -KILL "$MON_PID" 2>/dev/null
    MON_PID=""
}

cleanup() {
    stop_monitor
    if [ -n "$WRAP_PID" ]; then
        kill -KILL "$WRAP_PID" 2>/dev/null
    fi
    kill_fakes
    if [ "$KEEP" = "1" ]; then
        printf '现场保留在 %s\n' "$TMP"
    else
        rm -rf "$TMP"
    fi
}
trap 'cleanup' EXIT INT TERM

# ---------------------------------------------------------------------------
# 假主控（编译期选默认模式；运行期可用 FAKE_MODE 覆盖）
# ---------------------------------------------------------------------------
cat >"$TMP/src_fake_ivsboxd.c" <<'CEOF'
/* 假主控（用例专用，不是产品代码）。
 *
 * 只做三件事：
 *   ① 把自己被 exec 出来的工作目录追加进 $FAKE_OUT —— 用例据此断言"这次起的是哪个目录"；
 *   ② 按 FAKE_MODE（或编译期 FAKE_MODE_DEFAULT）决定活法：ok 常驻 / crash 立刻退出 /
 *      hang 常驻且**忽略 SIGTERM**；
 *   ③ 若给了 $FAKE_READY，就落一个就绪标记（模拟 ivsboxd 装配完成后落标记）。
 *
 * hang 模式故意忽略 SIGTERM：S7.3 的"自检超时后停掉当前实例"必须能把它 KILL 掉。
 * 只做 TERM 的实现会在这里卡住 ⇒ 用例报红。这正是"半截实例占住位子、回滚后的旧版本
 * 永远起不来"那一类故障的复现。
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FAKE_MODE_DEFAULT
#define FAKE_MODE_DEFAULT "ok"
#endif

int main(void)
{
    const char *mode = getenv("FAKE_MODE");
    const char *out = getenv("FAKE_OUT");
    const char *ready = getenv("FAKE_READY");
    char        cwd[4096];

    if (mode == NULL)
        mode = FAKE_MODE_DEFAULT;

    if (out != NULL && getcwd(cwd, sizeof cwd) != NULL) {
        FILE *f = fopen(out, "a");
        if (f != NULL) {
            (void)fprintf(f, "%s\n", cwd);
            (void)fclose(f);
        }
    }
    if (ready != NULL && ready[0] != '\0') {
        FILE *f = fopen(ready, "w");
        if (f != NULL) {
            (void)fputs("ready\n", f);
            (void)fclose(f);
        }
    }

    if (strcmp(mode, "crash") == 0)
        return 1;
    if (strcmp(mode, "hang") == 0)
        (void)signal(SIGTERM, SIG_IGN);

    for (;;)
        (void)pause();
}
CEOF

"$CC_BIN" -O0 -Wall -Wextra -o "$TMP/ivsboxd" "$TMP/src_fake_ivsboxd.c" || exit 4
"$CC_BIN" -O0 -Wall -Wextra -DFAKE_MODE_DEFAULT='"hang"' \
    -o "$TMP/ivsboxd-hang" "$TMP/src_fake_ivsboxd.c" || exit 4

# ---------------------------------------------------------------------------
# 盘面构造
# ---------------------------------------------------------------------------
setup_case() { # setup_case <名>：造 app 根（裸布局）＋ 独立 data/log
    CR=$TMP/$1
    RD=$TMP/$1.data
    LD=$TMP/$1.log
    mkdir -p "$CR/bin" "$CR/scripts" "$RD/ota" "$LD" || return 1
    cp "$UPD_BIN" "$CR/bin/ivsbox-updater"
    cp "$TMP/ivsboxd" "$CR/bin/ivsboxd"
    cp "$RUN_SH" "$CR/scripts/ivsbox-run.sh"
    chmod 0755 "$CR/bin/ivsbox-updater" "$CR/bin/ivsboxd" "$CR/scripts/ivsbox-run.sh"
    return 0
}

mkrel() { # mkrel <版本> [hang]：造 releases/<版本>/bin/ivsboxd
    _d=$CR/releases/$1
    mkdir -p "$_d/bin"
    if [ "${2:-}" = "hang" ]; then
        cp "$TMP/ivsboxd-hang" "$_d/bin/ivsboxd"
    else
        cp "$TMP/ivsboxd" "$_d/bin/ivsboxd"
    fi
    chmod 0755 "$_d/bin/ivsboxd"
}

write_pending() { # write_pending <版本> <旧版本> <bootcount> <max_boot>
    printf '{"fmt":1,"state":"pending","version":"%s","previous":"%s","app_dir":"%s/releases/%s","bootcount":%s,"max_boot":%s,"reason":""}\n' \
        "$1" "$2" "$CR" "$1" "$3" "$4" >"$RD/ota/pending.json"
}

# 真跑启动包装（全局 CR/RD/LD）。结果放 WDIR（被 exec 出来的目录）与 WRC（退出码）。
run_wrap() {
    : >"$FAKE_OUT"
    FAKE_OUT="$FAKE_OUT" FAKE_MODE=ok \
        IVSBOX_APP_ROOT="$CR" IVSBOX_DATA_ROOT="$RD" IVSBOX_LOG_ROOT="$LD" \
        "$CR/scripts/ivsbox-run.sh" >"$TMP/wrap.err" 2>&1 &
    WRAP_PID=$!
    _i=0
    while [ "$_i" -lt 60 ]; do
        [ -s "$FAKE_OUT" ] && break
        kill -0 "$WRAP_PID" 2>/dev/null || break
        sleep 0.05
        _i=$((_i + 1))
    done
    if kill -0 "$WRAP_PID" 2>/dev/null; then
        # 还活着 = 假主控已在跑（预期）：收掉它，本次算"包装成功换目录 exec"。
        kill -KILL "$WRAP_PID" 2>/dev/null
        wait "$WRAP_PID" 2>/dev/null
        WRC=0
    else
        wait "$WRAP_PID" 2>/dev/null
        WRC=$?
    fi
    WRAP_PID=""
    WDIR=$(head -n 1 "$FAKE_OUT" 2>/dev/null)
}

# ---------------------------------------------------------------------------
# u1 裸布局
# ---------------------------------------------------------------------------
setup_case u1 || exit 4
run_wrap
chk "u1 裸布局：起的是 app_root 自身" "$WDIR" "$CR"

# ---------------------------------------------------------------------------
# u2 current 指向版本目录
# ---------------------------------------------------------------------------
setup_case u2 || exit 4
mkrel 2.0.0
ln -s releases/2.0.0 "$CR/current"
run_wrap
chk "u2 current 链接生效：起的是 releases/2.0.0" "$WDIR" "$CR/releases/2.0.0"

# ---------------------------------------------------------------------------
# u3 目标目录没有可执行主控 ⇒ 回滚也救不回 ⇒ 127
# ---------------------------------------------------------------------------
setup_case u3 || exit 4
mkdir -p "$CR/releases/broken" # 目录在、bin/ivsboxd 不在
ln -s releases/broken "$CR/current"
run_wrap
chk "u3 目标缺主控：退出码 127" "$WRC" "127"
chk_has "u3 日志说明放弃启动" "$TMP/wrap.err" "放弃启动"

# ---------------------------------------------------------------------------
# u4 bootcount 超限 ⇒ 自动回滚（计划 §S7.3 的验证条）
# ---------------------------------------------------------------------------
setup_case u4 || exit 4
mkrel 1.0.0
mkrel 2.0.0
ln -s releases/2.0.0 "$CR/current"
ln -s releases/1.0.0 "$CR/previous"
write_pending 2.0.0 1.0.0 3 3
# 先拿真解析器验一遍手工造的 pending.json：写错字段名的话这里立刻红，
# 而不是让 u4 "什么都没测到"却看着像通过。
if "$UPD_BIN" status --pending --app-root "$CR" --data-root "$RD" >/dev/null 2>&1; then
    ok "u4 前置：手工造的 pending.json 被真解析器认作待确认"
else
    bad "u4 前置：手工造的 pending.json 没被认作待确认（字段名/格式不对）"
fi
run_wrap
chk "u4 bootcount 超限：改起旧版本" "$WDIR" "$CR/releases/1.0.0"
chk "u4 current 已切回 previous" "$(cd "$CR" && realpath current)" "$CR/releases/1.0.0"
chk_has "u4 events.log 记下 rollback 及原因" "$RD/ota/events.log" "bootcount-exceeded"
"$UPD_BIN" status --app-root "$CR" --data-root "$RD" >"$TMP/u4.status" 2>&1
chk_has "u4 状态为 rolled-back" "$TMP/u4.status" "state=rolled-back"
chk_has "u4 原因可查" "$TMP/u4.status" "last_reason=bootcount-exceeded"

# ---------------------------------------------------------------------------
# u5 自检超时 ⇒ 回滚；实例忽略 TERM ⇒ KILL；陈旧就绪标记必须在拉起前清掉
# ---------------------------------------------------------------------------
setup_case u5 || exit 4
mkrel 1.0.0
mkrel 2.0.0 hang
ln -s releases/2.0.0 "$CR/current"
ln -s releases/1.0.0 "$CR/previous"
write_pending 2.0.0 1.0.0 0 3

READY=$TMP/u5.ready
printf 'stale\n' >"$READY" # 模拟：上一实例落下标记后立刻崩溃，标记留在 /tmp
: >"$FAKE_OUT"

IVSBOX_APP_DIR=$CR IVSBOX_HOME=$RD IVSBOX_LOG_ROOT=$LD \
    IVSBOX_PIDFILE=$TMP/u5.pid IVSBOX_READY_MARK=$READY \
    IVSBOX_POLL_S=1 IVSBOX_UPGRADE_TIMEOUT=3 IVSBOX_BOOT_KILL_GRACE_S=1 \
    IVSBOX_MAX_RESTARTS=5 IVSBOX_RESTART_WINDOW=600 IVSBOX_FAILSAFE_COOLDOWN=60 \
    FAKE_OUT="$FAKE_OUT" FAKE_READY="" \
    sh "$MON_SH" >"$TMP/u5.monitor.out" 2>&1 &
MON_PID=$!

# 等回滚落地（current 切回 1.0.0），最多 40 s
_i=0
while [ "$_i" -lt 80 ]; do
    [ "$(cd "$CR" && realpath current 2>/dev/null)" = "$CR/releases/1.0.0" ] && break
    kill -0 "$MON_PID" 2>/dev/null || break
    sleep 0.5
    _i=$((_i + 1))
done
# 再等旧版本被重新拉起
_i=0
while [ "$_i" -lt 40 ]; do
    [ "$(tail -n 1 "$FAKE_OUT" 2>/dev/null)" = "$CR/releases/1.0.0" ] && break
    sleep 0.5
    _i=$((_i + 1))
done

ALERT=$LD/monitor-alert.log
chk "u5 回滚发生在自检超时后：current 指向旧版本" \
    "$(cd "$CR" && realpath current 2>/dev/null)" "$CR/releases/1.0.0"
chk_has "u5 告警记录了自检超时" "$ALERT" "self-test TIMEOUT"
chk_has "u5 忽略 TERM 的实例被升级为 KILL" "$ALERT" "未响应 TERM，升级为 KILL"
chk_not "u5 陈旧就绪标记没把新版误确认" "$ALERT" "upgrade confirmed"
chk "u5 业务已由旧版本接管（最后起来的目录）" \
    "$(tail -n 1 "$FAKE_OUT" 2>/dev/null)" "$CR/releases/1.0.0"
chk "u5 收尾：只剩一个假主控在跑" "$(count_fakes)" "1"

"$UPD_BIN" status --app-root "$CR" --data-root "$RD" >"$TMP/u5.status" 2>&1
chk_has "u5 状态为 rolled-back" "$TMP/u5.status" "state=rolled-back"

# ---------------------------------------------------------------------------
stop_monitor
kill_fakes
chk "收尾：假主控全部收干净" "$(count_fakes)" "0"

# ---------------------------------------------------------------------------
# u6 启动包装缺失 ＋ 陈旧就绪标记 ⇒ 绝不许被"确认"
# ---------------------------------------------------------------------------
# 这条把 u5 里那个竞态窗口变成**确定性**判据：包装脚本整份缺失（部署不全 / rootfs 损坏）
# 时，拉起动作根本不会产生任何进程，于是陈旧标记只可能被 monitor 自己读到。若 monitor
# 不在拉起前同步清掉它，"一个字节都没跑起来的新版"会被判成自检通过 —— 这是绕过回滚
# 保护的最短路径（state=confirmed 是终点，之后连回滚都判不动）。
setup_case u6 || exit 4
mkrel 1.0.0
mkrel 2.0.0
ln -s releases/2.0.0 "$CR/current"
ln -s releases/1.0.0 "$CR/previous"
write_pending 2.0.0 1.0.0 0 3
rm -f "$CR/scripts/ivsbox-run.sh" # 包装脚本缺失
READY6=$TMP/u6.ready
printf 'stale\n' >"$READY6"
: >"$FAKE_OUT"

IVSBOX_APP_DIR=$CR IVSBOX_HOME=$RD IVSBOX_LOG_ROOT=$LD \
    IVSBOX_PIDFILE=$TMP/u6.pid IVSBOX_READY_MARK=$READY6 \
    IVSBOX_POLL_S=1 IVSBOX_UPGRADE_TIMEOUT=3 IVSBOX_BOOT_KILL_GRACE_S=1 \
    IVSBOX_MAX_RESTARTS=5 \
    IVSBOX_RESTART_WINDOW=600 IVSBOX_FAILSAFE_COOLDOWN=60 \
    FAKE_OUT="$FAKE_OUT" FAKE_READY="" \
    sh "$MON_SH" >"$TMP/u6.monitor.out" 2>&1 &
MON_PID=$!

# 等它停下来（连包装都 exec 不起来 ⇒ 复位次数到顶后进熔断）。**不要等"自检超时"**：
# BOOT_T0 每次重拉都重新计时（见 monitor.sh 注释），"反复起不来"这种形态由 bootcount
# 兜住，自检超时只在"活着但起不完"（u5）时才可能触发。u6 里 boot-begin 根本跑不到，
# bootcount 不动 ⇒ 既不超时也不回滚，如实进熔断才是对的。
_i=0
while [ "$_i" -lt 40 ]; do
    grep -q "CRASH LOOP" "$LD/monitor-alert.log" 2>/dev/null && break
    kill -0 "$MON_PID" 2>/dev/null || break
    sleep 0.5
    _i=$((_i + 1))
done

ALERT6=$LD/monitor-alert.log
chk_not "u6 陈旧标记没把没起来的新版确认掉" "$ALERT6" "upgrade confirmed"
if [ -f "$READY6" ]; then
    chk "u6 monitor 在拉起前清掉了陈旧就绪标记" "present" "gone"
else
    chk "u6 monitor 在拉起前清掉了陈旧就绪标记" "gone" "gone"
fi
chk_has "u6 连包装都起不来 ⇒ 如实进熔断，不无限重拉" "$ALERT6" "CRASH LOOP"
"$UPD_BIN" status --app-root "$CR" --data-root "$RD" >"$TMP/u6.status" 2>&1
chk_has "u6 状态仍是 pending（bootcount 没动，不该被误判成回滚）" "$TMP/u6.status" \
    "state=pending"
chk "u6 一个假主控都没被拉起" "$(count_fakes)" "0"
stop_monitor

printf '\nPASS: %d  FAIL: %d\n' "$PASS" "$FAIL"
if [ "$FAIL" -ne 0 ]; then
    for _c in u5 u6; do
        printf -- '--- %s.monitor.out ---\n' "$_c"
        sed -n '1,40p' "$TMP/$_c.monitor.out" 2>/dev/null
        printf -- '--- %s alert ---\n' "$_c"
        sed -n '1,60p' "$TMP/$_c.log/monitor-alert.log" 2>/dev/null
    done
    exit 1
fi
exit 0
