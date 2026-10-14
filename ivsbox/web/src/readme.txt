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
  main.c           入口与事件循环：建监听 socket → poll → 收全一个请求 → 交给
                   api/media/static 生成响应 → 发回 → 关连接（一连接一请求）。
                   S6.4 起增加媒体文件的**流式发送**（pread→send，正文不进 wbuf）
                   与**限速**（1 s 窗口配额；配额用尽把等待折进 poll 超时，
                   **不是 sleep**，否则会连累其它连接）。
                   M7-S7.4 起主循环每拍调 `heartbeat_tick()` 刷**心跳文件**：
                   monitor-web.sh 用它判"主循环是不是卡死了"（`kill -STOP`/死锁的
                   进程在 /proc 里照样存在，只看 pid 接不住）。心跳**必须由主循环
                   推进**——改成独立线程刷，则卡死时线程照常前进，等于没有检测。
  iv_web_api.c     REST 路由 + 会话 Cookie + 本地通道转发（M5-S5.2/S5.4）。
  iv_web_static.c  静态资源（直接从 web/static 读盘 —— 改页面不必重编固件）。
  iv_web_media.c   S6.4 媒体下载：`/media/...` → media_root 下的普通文件。路径围栏
                   三道（逐段拒 `.`/`..` → realpath 前缀复核 → open(O_NOFOLLOW)）；
                   Range（含后缀区间）/ETag/If-Range；`?sha=1` 时附 X-SHA256。
  iv_web_chan.c    本地通道客户端（连 ivsboxd 的 unix socket，一问一答）。

【运行方式】
  make host / make arm    分别产出 build/host/ivsbox-web、build/arm/ivsbox-web
  运行：ivsbox-web [--port N] [--root DIR] [--chan PATH] [--pass PW]
                   [--media-root DIR] [--media-bps N]
                   [--heartbeat PATH] [--heartbeat-ms N]
  口令也可用环境变量 IVSBOX_WEB_PASS；媒体根/限速可用 IVSBOX_WEB_MEDIA_ROOT /
  IVSBOX_WEB_MEDIA_BPS 覆盖（产线走命令行，环境变量留给开发/测试）。
  心跳路径/周期可用 IVSBOX_WEB_HEARTBEAT / IVSBOX_WEB_HEARTBEAT_MS 覆盖，**须与
  monitor-web.sh 的同名变量指同一个文件**（两边不一致 ⇒ 健康进程会被判成卡死）；
  `--heartbeat-ms 0` 关闭心跳，仅用于验证。

【约定与注意事项】
  - 支持的 HTTP 子集：GET / POST，`Connection: close`（不做 keep-alive / pipeline /
    chunked / gzip）。**Range 只对 `/media/...` 启用**：静态资源都是小文件、整体读入后
    一把发完，给它 Range 只会多出一条能错的分支。
  - 鉴权：静态页面放行，`/api/...` 与 `/media/...` 一律要求会话 Cookie（login 除外）。
    会话 token 只存在进程内存里，进程重启即全部失效。
  - **分层偏离（登记）**：Web 进程原本不碰媒体目录，S6.4 起会读 `media_root` 下的录像
    文件（计划点名的方式二"供 Web/授权通道"要求如此）⇒ 用路径围栏把可读面钉死在媒体
    根内，并在 docs/修改记录.md 如实登记，不冒充"零偏离"。
  - 登录口令来源仍是命令行/环境变量，**欠账**：架构 §10 本应走 secure/ 密钥分离
    （F8 未实现，与 S4.3 的 SNMP community 同一笔账）。

【相关文档】
  - ivsbox/docs/功能开发计划.md §S5.1~§S5.4、§S6.4
  - T113运维终端-系统架构设计.md §10.4、§16.1、§16.2
