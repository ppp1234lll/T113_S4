#!/bin/sh
# IVSBox 受控远程通道 —— frpc 起停脚本（M8；方式与 BIRD 运维箱 scripts/frp.sh 同构）
#
# 位置（部署后）：/opt/ivsbox/scripts/frp.sh
#
# ============================ 它管什么 ============================
# 只管 **frpc 进程的起与停**。通道"该不该开"由 `ivsboxd` 里的 `iv_tunnel` 决定
# （写好 `/etc/frp/frpc.toml` 后置起标记文件）。**标记文件是唯一真相**：
#   标记在 ⇒ 进程该在；标记不在 ⇒ 进程必须不在。
# 本脚本不造标记、不删标记（除 `stop`）—— 造标记等于让脚本自己决定"要不要开远程
# 入口"，那道闸门就名存实亡了（这也是与 BIRD 版唯一有意不同的地方：BIRD 的
# `start` 自己 touch 标记，它的开关由 Go 侧的 SetDebugMode 管；本工程把开关与
# **有效期**一起交给 iv_tunnel，到期自动清标记，脚本跟着把进程收掉）。
#
# ============================ 动作 ============================
#   up    标记存在 ⇒ 拉起 frpc（幂等：已在跑就什么都不做）；标记不在 ⇒ 静默退出 0
#         （monitor 每圈都调，不能因为"本来就该关"而报错刷屏）
#   down  停掉 frpc（不动标记）
#   stop  删标记 + 停进程：**运维手工关闭**用，等价于 iv_tunnel_disable() 的兜底
#   status 诊断：打印标记 / 配置 / 进程三者的状态
#
# ============================ 为什么用 stcp 而不是 tcp ============================
# 配置里的代理项一律 `type = "stcp"`：访问方必须也跑 frpc 并持有同一个 secretKey，
# 端口不会开在 frps 上被任何人扫到。改回 `tcp` 等于把设备端口直接暴露到公网 ——
# 那不是"受控通道"，是开洞。
#
# ============================ 可调参数（环境变量覆盖，便于验证） ============================
#   IVSBOX_FRP_CONF    frpc 配置路径（默认 /etc/frp/frpc.toml）
#   IVSBOX_FRPC_BIN    frpc 二进制（默认 /usr/bin/frpc）
#   IVSBOX_FRP_PIDFILE pid 文件（默认 /var/run/frpc.pid）
#   IVSBOX_TUNNEL_FLAG 标记文件（默认 /tmp/ivsbox-tunnel.enabled）

FRP_CONF=${IVSBOX_FRP_CONF:-/etc/frp/frpc.toml}
FRPC_BIN=${IVSBOX_FRPC_BIN:-/usr/bin/frpc}
PID_FILE=${IVSBOX_FRP_PIDFILE:-/var/run/frpc.pid}
ENABLED_FLAG=${IVSBOX_TUNNEL_FLAG:-/tmp/ivsbox-tunnel.enabled}

frpc_alive() {
    # `-o comm` 只看进程名；frpc 是 ELF 二进制，判定可靠（对 shell 脚本会看到解释器，
    # 那正是本项目 monitor 里 `ivsboxd_alive` 不用 -f 的原因）。
    ps -o comm | grep -x frpc >/dev/null 2>&1
}

# 只清进程，不动标记文件
stop_process_only() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE" 2>/dev/null)
        [ -n "$PID" ] && kill "$PID" 2>/dev/null
        rm -f "$PID_FILE"
    fi
    # 兜底：pid 文件丢失/陈旧时按名字清（frpc 是单实例）
    pkill -x frpc 2>/dev/null
    return 0
}

do_up() {
    # 闸门优先：没有标记就不许起。这是"远程入口默认关闭"的物理保证。
    if [ ! -f "$ENABLED_FLAG" ]; then
        return 0
    fi
    if frpc_alive; then
        return 0
    fi
    if [ ! -f "$FRP_CONF" ]; then
        echo "frp.sh: up 跳过，配置不存在：$FRP_CONF"
        return 1
    fi
    if [ ! -x "$FRPC_BIN" ]; then
        echo "frp.sh: up 跳过，frpc 不可执行：$FRPC_BIN"
        return 1
    fi
    # `( ... ) &` + exec：让 $! 就是 frpc 自己的 pid（写成 `cmd &` 时若 cmd 是
    # 复合列表，$! 可能拿到子 shell；这里 exec 保证 pid 与进程一一对应）。
    ( exec "$FRPC_BIN" -c "$FRP_CONF" >/dev/null 2>&1 ) &
    echo $! > "$PID_FILE" 2>/dev/null || true
    return 0
}

do_stop() {
    # 先删标记再杀进程：monitor 下一圈看到标记没了，不会把它重新拉起来。
    rm -f "$ENABLED_FLAG"
    stop_process_only
}

case "$1" in
up)
    do_up
    ;;
down)
    stop_process_only
    ;;
stop)
    do_stop
    ;;
status)
    [ -f "$ENABLED_FLAG" ] && echo "flag: present" || echo "flag: absent"
    [ -f "$FRP_CONF" ] && echo "conf: present" || echo "conf: absent"
    frpc_alive && echo "proc: running" || echo "proc: stopped"
    ;;
*)
    echo "Usage: $0 {up|down|stop|status}"
    exit 1
    ;;
esac
exit 0
