============================================================
web/src  ——  ivsbox-web 进程源码目录（自研最小 HTTP/1.1 服务）
============================================================

【目录作用】
  ivsbox-web 管理进程的 C 源码目录。顶层 Makefile 用 `$(wildcard web/src/*.c)` 收集
  本目录源码，编译链接为独立可执行 build/<target>/ivsbox-web。本目录独立编译，
  **只链 libivcore**（不链 libivmodules / libivhal）—— 它自己不读配置、不碰数据库、
  不发平台报文（这三件事归 ivsboxd，架构 §10.4 单写者原则）；数据一律经本地通道
  （iv_chan）向 ivsboxd 取。

【文件说明】
  iv_web.h         进程内部接口：连接/请求/响应结构、各类限额、各层函数声明。
                   文件头写清 HTTP 子集选型理由、S6.4 媒体下载设计与"分层偏离"登记。
                   2026-10-16 起另含账号/口令相关限额、ctx 字段与两处提示语宏。
  main.c           入口与事件循环：建监听 socket → poll → 收全一个请求 → 交给
                   api/media/static 生成响应 → 发回 → 关连接（一连接一请求）。
                   S6.4 起增加媒体文件的**流式发送**（pread→send，正文不进 wbuf）
                   与**限速**（1 s 窗口配额；配额用尽把等待折进 poll 超时，
                   **不是 sleep**，否则会连累其它连接）。
                   M7-S7.4 起主循环每拍调 `heartbeat_tick()` 刷**心跳文件**：
                   monitor-web.sh 用它判"主循环是不是卡死了"（`kill -STOP`/死锁的
                   进程在 /proc 里照样存在，只看 pid 接不住）。心跳**必须由主循环
                   推进**——改成独立线程刷，则卡死时线程照常前进，等于没有检测。
                   2026-10-16 起启动时调 `iv_web_pass_init()` 装载账号/口令
                   （见 iv_web_pass.c），新增 `--user` / `--pass-file` 与
                   IVSBOX_WEB_USER / IVSBOX_WEB_PASS / IVSBOX_WEB_PASS_FILE。
  iv_web_api.c     REST 路由 + 会话 Cookie + 本地通道转发（M5-S5.2/S5.4）。
                   2026-10-16 起含账号/口令登录（POST /api/login）、改密
                   （POST /api/passwd）与 must_change 闸门。
  iv_web_static.c  静态资源（直接从 web/static 读盘 —— 改页面不必重编固件）。
  iv_web_media.c   S6.4 媒体下载：`/media/...` → media_root 下的普通文件。路径围栏
                   三道（逐段拒 `.`/`..` → realpath 前缀复核 → open(O_NOFOLLOW)）；
                   Range（含后缀区间）/ETag/If-Range；`?sha=1` 时附 X-SHA256。
                   2026-10-16 起改密闸门同样盖住这里（默认口令期不给下录像）。
  iv_web_pass.c    2026-10-16 账号与口令（**只存摘要**）：`iv_web_pass_hash()` 是
                   **域分隔**的 `sha256("ivsbox-web|1|" + user + "|" + pass)`；
                   `iv_web_pass_rule()` 要求 8-12 位且大写/小写/数字**都要有**，并
                   **拒控制字符**（参考实现的 `[\s\S]` 会放过 `\t`，此处**有意偏离**
                   —— 落盘格式是行式的，理由写在文件头）；`load/save` 落
                   `<secure>/web.passwd`，一行 `v1 <user> <sha256hex>`，**强制 0600**
                   （非 0600 则整份拒收 ⇒ 回落出厂凭据，且**不回写**该文件）；
                   写入走 tmp → fsync → rename，**先磁盘后内存**；`init()` 判定
                   "是不是出厂默认口令对"来置 must_change，并在算完摘要后
                   **擦掉内存里的明文口令**。
  iv_web_chan.c    本地通道客户端（连 ivsboxd 的 unix socket，一问一答）。
  iv_web_cam.c     S8.5 摄像机受控代理：/cam/<N>/... → 反代 camera.ch.<N>.ip。
                   只挂在**只绑 127.0.0.1** 的专用监听口上（与主口完全隔离，连接带
                   is_cam 标记路由隔离）；目标 IP 只经本地通道查配置、上游端口只有
                   cam_upstream 一个、环回护栏拒自指 —— "白名单外地址不可达"是物理
                   事实。改写三形态（正文绝对 URL／Location／Set-Cookie Path），凭据
                   零下发（SM4 密文本进程读都不读）。chunked/超限 502；MJPEG 长流、
                   WebSocket 升级、插件页**不承诺**（登记口径）。

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-web、build/arm/ivsbox-web
  运行：ivsbox-web [--port N] [--root DIR] [--chan PATH] [--user NAME] [--pass PW]
                   [--pass-file PATH] [--media-root DIR] [--media-bps N]
                   [--cam-port N] [--cam-upstream N]
                   [--heartbeat PATH] [--heartbeat-ms N]
  账号/口令（2026-10-16 起）：`--user` 默认 `admin`；`--pass` 默认出厂口令
  `88888888`，**启动日志不回显**（只打 `must_change` 与口令文件路径）。
  `--pass-file` 默认 `/mnt/UDISK/ivsbox/secure/web.passwd`；**传空值即"不落盘"**
  （改密只在内存里活到进程退出，专供测试）。
  S8.5：--cam-port（默认 8081，只绑 127.0.0.1；0 = 关闭代理入口）与 sys.tunnel.cam.port
  同源（frp stcp visitor 落点）；--cam-upstream（默认 80）是摄像机侧 Web 端口（唯一
  允许被代理的上游端口）。也可用 IVSBOX_WEB_CAM_PORT / IVSBOX_WEB_CAM_UPSTREAM 覆盖。
  账号/口令/口令文件可用 IVSBOX_WEB_USER / IVSBOX_WEB_PASS / IVSBOX_WEB_PASS_FILE
  覆盖；媒体根/限速可用 IVSBOX_WEB_MEDIA_ROOT / IVSBOX_WEB_MEDIA_BPS 覆盖
  （产线走命令行，环境变量留给开发/测试）。
  心跳路径/周期可用 IVSBOX_WEB_HEARTBEAT / IVSBOX_WEB_HEARTBEAT_MS 覆盖，**须与
  monitor-web.sh 的同名变量指同一个文件**（两边不一致 ⇒ 健康进程会被判成卡死）；
  `--heartbeat-ms 0` 关闭心跳，仅用于验证。

【约定与注意事项】
  - 支持的 HTTP 子集：GET / POST，`Connection: close`（不做 keep-alive / pipeline /
    chunked / gzip）。**Range 只对 `/media/...` 启用**：静态资源都是小文件、整体读入后
    一把发完，给它 Range 只会多出一条能错的分支。
  - 鉴权（2026-10-16 起）：**账号 ＋ 口令**。静态页面放行、login 除外，`/api/...` 与
    `/media/...` 一律要求会话 Cookie。口令**只以 sha256 摘要存在**（`<secure>/web.passwd`，
    0600），比较**恒时**、**账号错与口令错回同一个 401**（不给账号探测）。会话 token
    只在进程内存里，进程重启即全部失效。
  - **首次强制改密**：出厂默认口令期内，除 `POST /api/passwd` 与 `POST /api/logout`
    外一律 **403 must change password**（前端据此弹改密页）。闸门与规则**都在服务端**
    —— 前端那条正则只做即时反馈，不是判据。
  - **分层偏离（登记）**：Web 进程原本不碰媒体目录，S6.4 起会读 `media_root` 下的录像
    文件（计划点名的方式二"供 Web/授权通道"要求如此）⇒ 用路径围栏把可读面钉死在媒体
    根内，并在 docs/修改记录.md 如实登记，不冒充"零偏离"。
  - **凭据存储仍是欠账**：2026-10-16 已把口令改成"只存摘要 ＋ 0600"，但文件仍在 UDISK
    的 `secure/` 下、由 root 读写，**不是**密钥分离的密钥库。架构 §10 要求的 secure/
    密钥分离（F8）**仍未实现** —— 与 S4.3 的 SNMP community、S8.3 的隧道令牌**同一笔
    账**，不因本次改动而销账。
  - S8.5 专用口**不再叠会话/鉴权层**：入口由 M8 通道（stcp secretKey）把守，且只绑
    环回。如安全评审提出再加。
  - S8.5 改写为尽力而为的文本替换：各家摄像机页面形态各异，真机兼容性不承诺（计划口径）。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S5.1（登录鉴权与会话）、§S5.2（REST API）、
    §S6.4（媒体下载）、§S8.5（摄像机代理）；§F8（密钥分离，**未销账的同一笔欠款**）
  - T113运维终端-系统架构设计.md §10.4、§16.1、§16.2
  - ivsbox/docs/修改记录.md 2026-10-16 16:35（账号/强制改密落地的当次记录）
