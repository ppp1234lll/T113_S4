#!/bin/sh
# IVSBox 启动包装（M7-S7.3：开机自检与自动回滚的落地处）
#
# ============================ 它解决什么问题 ============================
# 双版本切换之后，"该起哪个版本"本身成了一个需要判断的问题：新版本可能装上了、
# 链接也切过去了，但它**起不来**。这一步的判断不能放在被拉起的程序里（它可能已经
# 坏了），只能放在外面 —— 本脚本就是那个"外面"。
#
# 判据与记账**全部在 ivsbox-updater**（C 实现、有单测覆盖），本脚本只做三件事：
#   ① 清掉上一实例的就绪标记（否则同一开机周期内旧标记会把新版"确认"掉）；
#   ② 问 `boot-begin` "这次该起哪个目录"（它顺带做 bootcount 记账与超限自动回滚）；
#   ③ `exec <目录>/bin/ivsboxd`。
#
# ============================ 引导副本不在轮换范围内 ============================
# `boot-begin`/`boot-ok`/`boot-fail` 一律由 `$APP_ROOT/bin/ivsbox-updater` 执行 ——
# 这是**裸布局那份、不参与版本轮换**的副本（adopt 不删裸布局，升级只写 `releases/`
# 与 `current`/`previous` 链接）。理由：恢复路径不能依赖被测对象。若改用
# `current/bin/ivsbox-updater`，新版把 updater 一起弄坏时就**连"该回滚"都判不出来**，
# 那正是最需要这个包的时候。
#
# ============================ 与 monitor.sh 的关系 ============================
# monitor.sh 是唯一的拉起方（它管崩溃重拉与熔断），它拉起的是**本脚本**而不是
# `ivsboxd`：`( exec ivsbox-run.sh ) &` 之后 $! 与 exec 出来的 ivsboxd 仍是同一个
# pid，pidfile 语义不变。monitor 另外负责"自检超时 → boot-fail"（它才有轮询循环）。
#
# ============================ 退出码 ============================
#   126 切目录失败   127 该目录下没有可执行的 ivsboxd（且回滚也没救回来）
# 正常路径**不返回**：exec 成功即本进程变成 ivsboxd。
set -u

APP_ROOT=${IVSBOX_APP_ROOT:-/opt/ivsbox}
DATA_ROOT=${IVSBOX_DATA_ROOT:-/mnt/UDISK/ivsbox}
LOG_ROOT=${IVSBOX_LOG_ROOT:-/mnt/UDISK/log}
READY_MARK=${IVSBOX_READY_MARK:-/tmp/ivsbox-ready}
UPD=$APP_ROOT/bin/ivsbox-updater
BOOT_LOG=$LOG_ROOT/ivsbox-boot.log

log() {
    printf '%s ivsbox-run: %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >>"$BOOT_LOG" 2>/dev/null || true
    printf 'ivsbox-run: %s\n' "$1" >&2
}

mkdir -p "$LOG_ROOT" 2>/dev/null || true

# ① /tmp 是 tmpfs（开机即空），但同一开机周期内的重复拉起必须各算各的：
#    不清掉的话，崩溃重拉时上一实例的旧标记会让"还没起来的新版"直接被确认。
rm -f "$READY_MARK" 2>/dev/null || true

resolve_fallback() {
    d=$(readlink -f "$APP_ROOT/current" 2>/dev/null)
    [ -n "$d" ] || d=$APP_ROOT
    printf '%s' "$d"
}

DIR=""
if [ -x "$UPD" ]; then
    DIR=$("$UPD" boot-begin --app-root "$APP_ROOT" --data-root "$DATA_ROOT" 2>>"$BOOT_LOG")
    rc=$?
    if [ "$rc" -ne 0 ] || [ -z "$DIR" ]; then
        log "boot-begin 失败(rc=$rc)，退回 current/裸布局"
        DIR=$(resolve_fallback)
    else
        log "boot-begin -> $DIR"
    fi
else
    log "警告：没有引导用升级器 $UPD —— 本次不做 bootcount 记账，也就无法自动回滚"
    DIR=$(resolve_fallback)
fi

# ② 该目录下没有能跑的主控：这是"跑不起来"最硬的形态，直接走回滚而不是让 monitor
#    反复重拉（重拉只会消耗 bootcount，白等几轮）。回滚后**当次就起旧版本**，
#    而不是等下一次开机 —— 现场希望的是"尽快恢复业务"。
if [ ! -x "$DIR/bin/ivsboxd" ]; then
    log "错误：$DIR/bin/ivsboxd 不存在或不可执行，触发回滚"
    if [ -x "$UPD" ]; then
        "$UPD" boot-fail --app-root "$APP_ROOT" --data-root "$DATA_ROOT" >>"$BOOT_LOG" 2>&1
        log "boot-fail rc=$?"
    fi
    DIR=$(resolve_fallback)
    if [ ! -x "$DIR/bin/ivsboxd" ]; then
        log "错误：回滚后 $DIR/bin/ivsboxd 仍不可执行，放弃启动"
        exit 127
    fi
    log "回滚后改用 $DIR"
fi

cd "$DIR" || exit 126
exec ./bin/ivsboxd
