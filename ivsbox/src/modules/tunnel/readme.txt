============================================================
src/modules/tunnel  ——  受控远程访问通道（M8-S8.1~S8.3）
============================================================

【目录作用】
  受控的**反向远程访问通道**的**设备侧闸门**：决定"现在该不该开远程入口、开放哪些
  服务、钥匙怎么派生、什么时候自动关"。属模块层（`libivmodules.a`）。

【文件说明】
  iv_tunnel.c  通道闸门实现：服务名白名单解析、secretKey 派生、frpc.toml 渲染与
               原子落盘（0600）、标记文件的置与清、有效期到期自动关闭、会话留档。
  readme.txt   本文件。

  相关（不在此目录）：
    scripts/frp.sh     frpc 进程起停（只按标记文件动作，不自己造标记）＋
                       独立判标记里的到期时刻（主控不在时唯一的有效期闸门）
    scripts/monitor.sh 通道守护：标记在 ⇒ 保证进程在；标记不在 ⇒ 保证进程不在；
                       标记在但已过期 ⇒ 收掉（判过期用 date +%s，monitor 的 $now 是 uptime）

【实现状态】
  S8.1~S8.3 已实现（2026-10-15）：模块 ＋ 装配接线（`main.c`：SN 读身份分区、令牌读
  `<secure>/tunnel.token`、resolver 落 127.0.0.1、5 s 周期 tick、配置热更新作废、
  退出即关）＋ 配置键 `sys.tunnel.*` ＋ `iv_ctl` 的 `tunnel.status/enable/disable`。
  标记文件内容＝到期时刻，`frp.sh`/`monitor.sh` 可独立判过期（有效期不依赖主控存活）。
  摄像机受控代理（S8.5）未落地 ⇒ 通道里看摄像机走访问端 `ssh -L`。

  ⚠ **2026-10-30 收口后本文件有过时描述，2026-11-04 已改**（之前写的是 10-15 的
  "按服务开代理"版）：现在**没有 resolver、没有服务名白名单**，通道只有一条代理、
  固定转发本机 sshd(22)。三处历史描述与代码不一致，凡与本文件冲突一律以代码为准：
    ① 服务名白名单（`web`/`camera:<id>`/`media:<id>`）—— **已删除**；
    ② 派生式含 `svc` —— **不含**，实际是 `md5(sn + token)` 前 8 字节；
    ③ 标记文件 `/tmp/ivsbox-tunnel.enabled` —— 实际是 **`/tmp/frp.enabled`**。

  2026-11-04 新增：`iv_tunnel_token_valid()`（令牌字符白名单，防 TOML 注入，渲染前
  与 `set_identity` 各挡一次）＋ ctl 的 `tunnel.configure`（存令牌 + 有效期 + 开关
  一次交付，Web「远程通道」页用它）。

【实现方式（2026-10-15 用户指定：与 BIRD 运维箱同款）】
  不自己写隧道、不自己发明通道协议，按参考项目
  `bird_20260623/code/bird` 已跑通的方式：
    frpc（设备侧）── 外连 ──> frps（运维服务器），代理类型 **stcp**
    —— 访问方必须也跑 frpc 且持有同一个 secretKey 才连得上来，端口不在公网暴露。
  对照关系：
    - 配置：`/etc/frp/frpc.toml`（0600，内含 secretKey）；
    - 开关：`/tmp/ivsbox-tunnel.enabled` 标记文件（BIRD 用 `/tmp/frp.enabled`）；
    - 守护：`monitor.sh` 只在标记存在时守护 frpc（BIRD 同款第 4 段）；
    - 钥匙：`secretKey = hex(MD5(sn + token + svc)[0..8))`，与 BIRD 的
      `utils.GenerateSTCPKey` 同源（本工程多服务，故把 svc 也拼进派生式）。

【约定与注意事项】
  - **通道只有一条 stcp 代理**（2026-10-30 对齐 BIRD）：`name = <sn>`、
    `localIP = 127.0.0.1`、`localPort = 22`。**没有服务名白名单、没有 resolver** ——
    设备侧不需要知道"世界上有哪些服务"，看摄像机/取文件由访问端 `ssh -L` 按需建。
    架构 §9.4「禁止任意 ip:port」因此天然成立：能进配置的目标只有 127.0.0.1:22。
  - **令牌字符白名单**（2026-11-04）：`iv_tunnel_token_valid()` 只收
    `0-9A-Za-z-_.~+/=@`、1~63 字节。令牌会被拼进 `auth.token = "%s"` 的双引号里，
    带引号/换行/`#` 分别对应"提前闭合""注入任意键""后面整段变注释"。`frps 地址`
    同样过一道（域名/IPv4/IPv6 字面量字符集）。
  - **钥匙**：`secretKey = hex(MD5(sn + token)[0..8))`，**不含 svc**。访问端必须用
    同一公式；两侧算错一位的症状是"都连上了 frps、握手永远失败"，谁都不报"钥匙错"。
  - **有效期闸门**：`enable()` 必须带有效期（默认 3600 s、上限 86400 s），到期由
    `tick()` 自动清标记 —— 远程入口不能靠人记得去关。
  - **fail-closed**：缺 SN / 令牌 / frps 地址 中的任何一项，一律开不了。
  - **不做的事**：不碰 socket、不起停进程、不管数据面（都归 frp 与脚本）。
  - 进程启停与数据面不在本层，正是为了**让拒绝条件可以在单测里被钉死**。

【卡用户 / 卡平台项】
  - ~~板端是否有 `/usr/bin/frpc`~~ —— 2026-11-04 现场核对：**有**（15073464 B，随镜像出厂）。
  - frps 服务器地址 / 端口（默认 7000）/ `auth.token` 由谁下发：**2026-11-04 起由现场在
    Web「远程通道」页填**（地址/端口进配置、令牌进 `secure/tunnel.token`）；平台侧自动
    下发仍未接（D 系列 D-11「frps」）。
  - 运维侧的 visitor 是否用同一条 secretKey 公式：**同一条**（`md5(sn+token)` 前 8 字节），
    生成工具 `tools/tunnel-keygen.html` 内置同一份实现，用设备单测的黄金向量钉住。
  - 客户是否允许远程入站、安全评审（架构 §9.4 前置 ②③）：仍未走完，现场按"临时开、
    带有效期、到期自动关"使用。

【相关文档】
  ivsbox/docs/功能开发计划.md：§M8（远程通道）
  T113运维终端-系统架构设计.md：§9.3（摄像机 Web 受控代理）、§9.4、§18.8
  参考实现：bird_20260623/code/bird/scripts/frp.sh、internal/utils/md5.go、
            internal/config/factory/factory_manager.go（writeFrpConfig）
