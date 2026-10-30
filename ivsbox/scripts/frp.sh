#!/bin/sh
# IVSBox 受控远程通道 —— frpc 起停脚本（M8；方式与 BIRD 运维箱 scripts/frp.sh 同构）
#
# 位置（部署后）：/opt/ivsbox/scripts/frp.sh
#
# ============================ 它管什么 ============================
# 只管 **frpc 进程的起与停**。通道"该不该开"由 `ivsboxd` 里的 `iv_tunnel` 决定
# （写好 `/etc/frp/frpc.toml` 后置起标记文件）。**标记文件是唯一真相**：
#   标记在 ⇒ 进程该在；标记不在 ⇒ 进程必须不在。
#
# 与 BIRD（GO 版 `scripts/frp.sh`）的对应关系逐条如下：
#   BIRD `start`  = 本脚本 `start`：先清进程 → `touch` 标记 → 起 frpc → 记 pid
#   BIRD `stop`   = 本脚本 `stop` ：先删标记 → 杀进程（**顺序不能反**）
#   BIRD `restart`= 本脚本 `restart`（等价 start）
# 唯一有意保留的差异：BIRD 的 `start` 无条件 `touch`，本脚本只在**标记不存在**时
# 补建 —— 因为本工程的标记内容就是有效期到期时刻（见下节），覆盖等于把 TTL 闸门
# 冲掉。标记因"补建"而出现的场景只有一种：运营手工删了标记但 frpc 还在跑，此时
# 补建＝恢复不变量，不是新开远程入口。
#
# ============================ 有效期为什么要在这一层也判一次 ============================
# `iv_tunnel_tick()` 跑在 `ivsboxd` 进程里 —— 主控不在（被 kill -9、崩溃、停在 monitor
# 熔断窗口外）时它就没人跑了。若只有那一层，"开 1 小时后主控崩了"留下的就是**一条永久
# 开着的远程入口**。
# 故 `iv_tunnel_enable()` 把**到期时刻**（unix 秒，一行十进制）写进标记文件内容；本脚本
# 每次被调用都先判过期，过期就删标记 + 停进程。这样有效期闸门**不依赖主控存活**。
# 标记里没有数字（例如运维手工 `touch`）⇒ 视为**未过期**：不擅自关掉别人手工开的通道
# （能 touch 这个文件的人本来就已经是 root，不是本闸门要防的对象）。
#
# ============================ 动作（与 BIRD 同名的三件套 + 本工程诊断） ============================
#   start   标记存在且未过期 ⇒ 拉起 frpc（幂等：已在跑就什么都不做）；已过期 ⇒ 收掉；
#           标记不在 ⇒ **补建标记**后再拉起（BIRD 的 `start` 自己 `touch`）。
#           补建只在"标记不存在"时做，绝不覆盖已有内容 —— 覆盖会把到期时刻冲掉，
#           TTL 闸门就漏了。
#   stop    删标记 + 停进程（BIRD `disable_frp`，运维手工关闭走这个）
#   restart 等价于 start（BIRD 同）
#   up      等价于 start（本工程 monitor 旧名，保留兼容）
#   down    只停进程、**不动标记**（BIRD 没有这个动作；留给"临时断连"用）
#   status  诊断：打印标记 / 到期 / 配置 / 进程四者的状态
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
#   IVSBOX_TUNNEL_FLAG 标记文件（默认 /tmp/frp.enabled，与 BIRD 同名）

FRP_CONF=${IVSBOX_FRP_CONF:-/etc/frp/frpc.toml}
FRPC_BIN=${IVSBOX_FRPC_BIN:-/usr/bin/frpc}
PID_FILE=${IVSBOX_FRP_PIDFILE:-/var/run/frpc.pid}
ENABLED_FLAG=${IVSBOX_TUNNEL_FLAG:-/tmp/frp.enabled}

frpc_alive() {
    # `-o comm` 只看进程名；frpc 是 ELF 二进制，判定可靠（对 shell 脚本会看到解释器，
    # 那正是本项目 monitor 里 `ivsboxd_alive` 不用 -f 的原因）。
    ps -o comm | grep -x frpc >/dev/null 2>&1
}

# 标记里的到期时刻（只留十进制数字；读不到/没数字 ⇒ 输出空）
flag_expire() {
    [ -f "$ENABLED_FLAG" ] || return 1
    tr -cd '0-9' < "$ENABLED_FLAG" 2>/dev/null
}

# 0 ＝ 已过期；非 0 ＝ 未过期或判不出来（判不出来时不关，见文件头说明）
flag_expired() {
    EXP=$(flag_expire)
    [ -n "$EXP" ] || return 1
    NOW=$(date +%s 2>/dev/null) || return 1
    [ "$NOW" -ge "$EXP" ] 2>/dev/null
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
    # 幂等短路：进程在、标记在、没过期 ⇒ 什么都不做。BIRD 的 start 不具备幂等性
    # （它无条件 stop+start，由 monitor 的 `! is_running` 保证不被反复调）；本脚本
    # 也可能被 ctl / 人手直接调，所以自带这一层，避免每调一次就把通道重连一次。
    if frpc_alive && [ -f "$ENABLED_FLAG" ] && ! flag_expired; then
        return 0
    fi
    # 第一道闸门：标记在但已过期 ⇒ 收掉（主控不在时只剩这一层能管）。先删标记再杀
    # 进程，免得 monitor 下一圈看到标记还在又把它拉起来。
    if flag_expired; then
        echo "frp.sh: 标记已过期（主控不在或已挂），停掉 frpc 并清标记"
        do_stop
        return 0
    fi
    if [ ! -f "$FRP_CONF" ]; then
        echo "frp.sh: start 跳过，配置不存在：$FRP_CONF"
        return 1
    fi
    if [ ! -x "$FRPC_BIN" ]; then
        echo "frp.sh: start 跳过，frpc 不可执行：$FRPC_BIN"
        return 1
    fi
    # 配置与二进制都就位才补建标记：与 BIRD 的 `touch "$ENABLED_FLAG"` 同一个位置
    # （在起进程之前，让 monitor 看到标记后即使进程还没起来也不会误删）。
    # **只在标记不存在时建** —— 已有内容就是到期时刻，覆盖等于把 TTL 闸门冲掉。
    [ -f "$ENABLED_FLAG" ] || : > "$ENABLED_FLAG"
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
start|up|restart)
    # BIRD 的 `start` / `restart` 都是"要它开"，本工程旧名 `up` 同义（monitor 兼容）。
    do_up
    ;;
down)
    # BIRD 里没有这个动作：只断连、**不动标记**，下一圈 monitor 会把它拉回来。
    # 留给"临时掐断看现场反应"用，不是关闭通道的正规入口。
    stop_process_only
    ;;
stop)
    do_stop
    ;;
status)
    if [ -f "$ENABLED_FLAG" ]; then
        echo "flag: present ($ENABLED_FLAG, expire=$(flag_expire))"
        if flag_expired; then echo "expired: yes"; else echo "expired: no"; fi
    else
        echo "flag: absent ($ENABLED_FLAG)"
        echo "expired: -"
    fi
    if [ -f "$FRP_CONF" ]; then echo "conf: present ($FRP_CONF)"; else echo "conf: absent ($FRP_CONF)"; fi
    if frpc_alive; then echo "proc: running"; else echo "proc: stopped"; fi
    ;;
*)
    echo "Usage: $0 {start|stop|restart|up|down|status}"
    exit 1
    ;;
esac
exit 0
