============================================================
tests/integration  ——  集成测试：验端到端装配（部分需外部服务已在跑）
============================================================

【目录作用】
  集成测试层，验的是"装好之后的进程 / 整条链路"而非孤立模块，与 tests/unit 分工明确：
  unit 验模块本身（不依赖外部进程），integration 验真实的端到端装配是否成立。
  本层用例按"对端由谁起"分三类，边界必须写清：
    - **连外部服务**：用例只当客户端，连本机通道与已运行的 ivsboxd（test_ctl_snapshot）。
    - **自包含**：用例自己起全部对端（pty、回环 TCP、临时目录），**前置条件为零**，
      任何环境都能跑（test_e2e_report）。
    - **自包含 ＋ 私有 netns**：除 pty / 回环 / 临时目录外，还自己 `unshare -rmn`
      进私有网络命名空间造 dummy 网卡（test_agent_switch）；`unshare` 不可用时
      **优雅 SKIP（退出 0）**，不弄红 `make itest`。
    - **自包含 ＋ 私有 user/mount ns**：自己 `unshare -rm` + tmpfs 造出私有
      `/mnt`、`/var/run`，再**真拉起 ivsboxd**（test_cfg_media_e2e）——它验的是
      app 层接线，而 app 层 `.c` 不进静态库、单测链接不到，只能用真进程 + 落盘日志
      做断言。同样 `unshare` 不可用即 SKIP。

【文件说明】
  test_ctl_snapshot.c  ivsboxd 本地通道端到端往返（M1 退出条件第 2 条）：连
                       IV_CHAN_PATH_DEFAULT，验 system.snapshot 应答、request_id
                       原样回填、同连接二次请求、未实现方法无应答、连接可复用。
                       **需外部服务**。
  test_e2e_report.c    S3.6 上报装配层端到端联动（**自包含，零 mock**）：假采集板＝
                       pty 对（master 由用例持有，slave 路径交给装配层开），假平台＝
                       127.0.0.1 临时端口上的真 TCP listener，持久队列＝临时目录。
                       验 0xE1 轮询帧字节级黄金、0xE1 应答→状态镜像刷新、立即上报→
                       平台收到（字段顺序 / `##` 壳 / `&&数据区&&` CRC）、断开→入队
                       积压、恢复→按序补发且队列清空、E3 查询→qn 原值回填、未注册
                       命令→回 ACK(0x74)（**不回 0x01**：2026-10-20 起明确失败，
                       回 OK 等于对平台撒谎）。**无需外部服务**。
  test_agent_switch.c  iv_agent 装配层「切出口」端到端联动（**自包含 ＋ 私有 netns**，
                       对应架构 §16.2 场景 1/2）：自己 `unshare -rmn` 重启进私有
                       user+net+mount ns，在 ns 内建 `usb0`/`eth0` 两张 dummy 网卡；
                       采集板仍走 pty、平台走 127.0.0.1 TCP、队列走临时目录。
                       覆盖：真内核 LINK 事件→probe 状态（摸底）、基线 `##` 上报、
                       **拔线→切有线**（事务真成功 ⇒ 真 netlink 改默认路由 ＋
                       `iv_report_reconnect` 真重建 TCP）、平台断→入队积压、
                       **跨过 hold_s 回切 → 按序补传**、收口无 fd 泄漏。
                       **无需外部服务**，但需 `unshare` 与 `iproute2`（见下）。
                       **两种模式**：默认＝上面的私有 netns（VM 用，网卡是造的）；设
                       `IV_AGENT_TEST_REALNIC=1` 则改走**真网卡模式** —— 跳过 netns 与
                       造卡，直接在**真实 `usb0`/`eth0`** 上跑同一套 k01~k07（板端用；
                       接口必须已存在，否则 SKIP；**须先 stop 4G 看护 `S62usb4g`**，它与
                       "拔线"互斥；会真改板子默认路由，收尾自己删干净）。
                       **开局链路状态两种模式都由 `iv_agent_open()` 的"开局摸底"播出**
                       （同步查一次真实状态；真接口本来就 UP、内核不会自播事件）；
                       netns 模式 k02 另用 `ip link set ... up` 造一条真内核 LINK 事件。
  test_web_http.c      ivsbox-web HTTP 服务端到端（M5，自包含）：自己 fork+execl 拉
                       起静态服务，裸 socket 分段发送复现"头体分两次到达"，验静态页/
                       鉴权/目录穿越/400/413/fd 稳定性。无需 ivsboxd。
  test_media_e2e.c     ivsbox-media（收编 bird_media）× iv_media 桥接端到端（M6-S6.1，
                       **自包含**）：自己 fork+execl 拉起 build/host/ivsbox-media（固定
                       端口 18000，二进制缺失或端口被占 ⇒ SKIP 退出 0），经桥接模块打
                       真实接口：status 往返、无效 RTSP 受理→对端自动停→保活把会话表
                       清掉（桥接↔对端状态同步的端到端证据）、capture 结论与磁盘一致
                       （VM 无 /dev/video0 ⇒ 判 fail，环境有摄像头 ⇒ 判 ok，两种环境
                       都不假红）。无需 ivsboxd。**并行跑两份会因端口冲突假红**。
  test_prov_e2e.c      ivsbox-provision 设备侧 × 工装侧 端到端（M6/S5.5 协议定稿，
                       **自包含**）：自己 fork+execl 拉起设备侧（`--dry-run`，不写裸
                       分区），自己扮演工装侧（回环 UDP ＋ libivmodules 组包函数）。
                       e01 搜索应答字段与启动参数一致；e02 update_mac 回 success 且
                       身份已换成新 ID/MAC；e03 坏 MAC 回 fault；e04 未实现指令
                       （update_relay）**一个字节都不回**；e05 垃圾包静默且进程不罢工。
                       端口自 37201 起自动挑（占用则顺延，全占 ⇒ SKIP 退出 0）；
                       二进制缺失 ⇒ SKIP。**不需要工厂标记、不需要 ivsboxd**。
  test_cfg_media_e2e.c S6.5 前置「配置接线」端到端（**自包含 ＋ 私有 user+mount ns**）：
                       把自己 `unshare -rm` 重启进私有 user+mount ns，把 `/mnt` 与
                       `/var/run` 挂成 tmpfs，写一份**含 8 路 `camera.ch` 数组**的
                       ivsbox.json，**真拉起 `build/host/ivsboxd`**，再从它落盘的日志里
                       断言 agent 装配那一行
                       `media bridge up, rec urls=<有效>/<表长> hold=<ms> debounce=<ms>`。
                       一行同时钉死四处：① 8 个数组元素被收下（IV_CFG_ARR_MAX 6→8，
                       截断则第 8 路那条 URL 消失）；② 只有 **url 非空 且 record 为真**
                       的那一路进表（另设两路反例）；③ hold/debounce 取自
                       `camera.rec.*`（用 7000/20000 与出厂默认区分开）；④ 不得出现
                       `extras rejected`。**这条是 app 层 `cfg_rec_sources()` 的唯一
                       证据**（app 层 .c 不进静态库，单测链接不到）。需
                       `build/host/ivsboxd`（缺 ⇒ SKIP）；`unshare -rm` 不可用 ⇒ SKIP。
  test_web_media_e2e.c M6-S6.4 媒体下载（HTTP Range）端到端（**自包含**）：自己
                       fork+execl 拉起 `build/host/ivsbox-web`（二进制缺失 ⇒ SKIP 退出 0），
                       `--media-root` 指向自建临时目录（内放一份确定内容的伪随机文件）、
                       `--media-bps` 给 64 KiB/s，再用裸 socket 逐条打：未登录 401、登录取
                       会话、全量 `?sha=1`（`X-SHA256` 必须与本地 `iv_sha256_file()` 一致，
                       正文逐字节比对）、中段/后缀 Range 的 206 与 `Content-Range` 边界、
                       越界 416、`If-Range` 失配退 200、`..` 穿越 403、**指向媒体根之外的
                       符号链接也必须 403**（这是路径围栏第二道 `realpath()` 复核的真实
                       攻击面 —— 符号链接文件名完全合法，第一道逐段检查拦不住它）、缺失
                       404、限速用墙钟证明（去掉限速会立刻变红）、中途 cancel 后服务仍可用、
                       反复下载无 fd 泄漏。**不需要 ivsboxd**。
  test_sd_fault_e2e.c  M6 退出条件 §16.2 场景 6 端到端（**自包含 ＋ 私有 user+mount ns**）：
                       "SD 卡拔出或写满时，控制和平台业务继续运行并产生明确告警"。
                       自己 `unshare -rm` 进私有 user+mount ns，把 `/mnt` 与 `/var/run`
                       挂成 tmpfs，用 `IVSBOX_SD_ROOT`（`iv_storage_root_resolve()` 的
                       开发/测试口子）把存储根指到三种受控位置，每种位置**真拉起一次
                       `build/host/ivsboxd`**，再从**本地通道**问 `sys.overview` /
                       `system.snapshot`：① 根存在（空目录）⇒ `enabled=true/fault=0/
                       alarm=0/level=0`；② 根**不存在**（＝拔卡）⇒ `fault=1/alarm=1/
                       allow_new=0/stop_rec=1`，日志出现 `storage fault: mounted=0`
                       （带 `(recording gated)`）；③ 根挂在 `size=1M` 的 tmpfs 上并
                       **灌满**（＝写满）⇒ `level=3`(stop)/`alarm=1`，日志出现
                       `storage level ok -> stop`。**三种情形都断言 `system.snapshot`
                       仍然应答 `"status":"ok"`** —— 这是"控制业务不受影响"的可观测
                       证据。首轮巡检不必等 30 s（`iv_storage_step()` 见 `last_run_ms==0`
                       即真跑一轮）。这条同时是**存储告警对外出口的回归网**：本次改动前
                       `iv_storage_alarm()` 除单测无任何调用者、`sys.overview` 也没有
                       `storage` 段，SD 异常只有本地日志、上层看不到。需
                       `build/host/ivsboxd`（缺 ⇒ SKIP）；`unshare -rm` 不可用 ⇒ SKIP。
                       平台侧 `ERR` 故障码上报**未接**（SD 码值在 `iv_err.h` 无槽位，
                       类型段 `0x4~0xF` 归属待与单片机侧协商）⇒ 属真机/协议现场。
  test_fault_db_power.c M7-S7.4 故障注入「掉电 · **写库中**」端到端（**自包含**）：
                       验证"事务写到一半被强行拿掉，重启后库必须自洽"。三种掉电形状：
                       ① **提交点前后被打死**——子进程提交 N 批（每批 `BEGIN IMMEDIATE`
                       …`COMMIT`）后落一个标记，再开一批**永不提交**的事务并 `pause()`
                       等死；父进程见到标记即 `SIGKILL`，重开库断言「已提交的一行不少、
                       未提交的一行不多、`integrity_check` 通过、没被误判成 `*.corrupt`、
                       恢复后还能继续写、`-wal` 仍在（WAL 模式没被回退）」，并连续 5 轮
                       累积（证明反复掉电不会越掉越坏）；
                       ② **WAL 残留**——趁子进程静止在中途事务时把 `db`+`-wal`+`-shm`
                       **整组**快照，重放后行数必须恰好等于已提交批（未提交帧被丢弃）；
                       ③ **库被写成 0 字节**——能开、可用、表为空，且原文件被改名
                       `*.corrupt` 保全（对照：全新路径不得凭空产生 `.corrupt`）。
                       **诚实边界**：`SIGKILL` 只近似"进程未及清理就消失"，**不等于真掉电**
                       （page cache 里的数据仍会写回）；真拔电只能在板端做，见
                       `tests/fault-inject.sh` 的 MANUAL 项 m6/m7/m8。无需任何外部服务。
  thresh_e2e.py        阈值下发 0xF1 的**装配层接线**取证（Python；本目录唯一的非 C 用例）。
                       为什么非它不可：`iv_agent_apply_thresh()` 的单测与
                       `test_agent_board` b06b 只证明"给它一份阈值能发对帧"，而
                       **"改配置会不会走到它"取决于 `main.c` 的订阅掩码** —— 掩码漏了
                       class，分支再对也永不触发，编译/单测/C 集成用例全绿、运行期零反应
                       （2026-10-21 `probe.*` 就这么踩过；本次两条反向证伪也就是删掩码与
                       空驱动）。做法：以 root 自己拉起 `build/host/ivsboxd`，经
                       `/var/run/ivsbox/ctl` 下发 `config.apply`，再从落盘日志断言
                       ① 只设 `sensor.temp_high` ⇒ `thresh queued` 载荷 `{"tu":55}` 且
                       **不含** `hv`（0 ＝ 不下发该键）；② 再设 `elec.volt_high` ⇒
                       `{"hv":250,…,"tu":55}` 九键以配置为准、键序固定；③ 全清零后不再
                       排队；④ 无采集板时出现 `thresh not sent yet`（重试而非静默丢）；
                       ⑤ 6 s 内重试日志新增 ≤ 2（退避 ＋ 降噪）。**需 root**（ctl 是
                       `root:root 0660`）与可写 `/mnt/UDISK/log`；自己起/收 daemon，
                       故跑之前不能有别的 ivsboxd 在跑。

【运行方式】
  # 只跑自包含用例（无需先起任何服务）
  make TARGET=host build/host/tests/integration/test_e2e_report
  ./build/host/tests/integration/test_e2e_report
  make TARGET=host build/host/tests/integration/test_agent_switch
  ./build/host/tests/integration/test_agent_switch

  # 板端真链路（真网卡模式）：先停 4G 看护，跑完起回来并核对 ip route
  /etc/init.d/S62usb4g stop
  IV_AGENT_TEST_REALNIC=1 ./build/arm/tests/integration/test_agent_switch
  /etc/init.d/S62usb4g start

  # 跑整个目录（前提：ivsboxd 必须先在跑，否则 test_ctl_snapshot 会报"没在跑"）
  build/host/ivsboxd &      # 宿主机：先起服务
  make itest                # 再构建并运行 tests/integration/*
  # 板端：/etc/init.d/S65ivsboxd start 后，直接跑 build/arm/tests/integration/*
  # 用例可选参数：test_ctl_snapshot [socket_path]（默认 /var/run/ivsbox/ctl）

  # thresh_e2e.py：装配层接线取证。自己起/收 daemon（**跑之前不能有 ivsboxd 在跑**）
  sudo mkdir -p /mnt/UDISK/log
  sudo python3 tests/integration/thresh_e2e.py

【约定与注意事项】
  - 本目录**刻意不并入 make test**：目录里既有"需外部服务"的用例，并进去会让 CI 因
    "没起服务"而假红（见 Makefile `itest` 注释、架构 §16.1）。**需要按单用例粒度执行**
    时用上面第一条命令。
  - test_ctl_snapshot 对"服务尚未就绪"有重试（最多 5 s）；连接失败会明确提示
    "ivsboxd 没在跑"。
  - test_e2e_report / test_agent_switch 的 pty 只走 POSIX 接口
    （`posix_openpt/grantpt/unlockpt/ptsname_r`）；板端没有 `socat`，所以"串口对"
    一律用 pty（VM 上同理）。
  - 两者都会在 `$TMPDIR`（默认 `/tmp`）下建一个临时目录做持久队列，**用例结束前
    删除**；test_ctl_snapshot 不写文件。
  - **test_agent_switch 需要 `unshare -rmn`（非特权 user namespace）与 `ip`**：
    它按架构 §18.3 的冻结名造 `usb0`/`eth0`，并在私有 netns 内**真改默认路由**
    （`iv_agent` 把 `switch_route` 接到了 `iv_netmgr_ops_default()`，不打桩 ——
    否则测的就不是装配层实际接的那条路）。**两者任一不可用时用例打印 `SKIP:` 并
    返回 0**，不会让 `make itest` 假红。板端没有 `unshare` ⇒ 默认模式在板端 SKIP；
    要上板就置 `IV_AGENT_TEST_REALNIC=1` 走真网卡模式（见上），此时改用真接口。
  - 板端验证需先按 §S65ivsboxd 拉起服务，再运行"需外部服务"的用例。

【相关文档】
  - ivsbox/docs/功能开发计划.md（M1 退出条件第 2 条；§0.1 R1/S3.6/S3.8 行；§S3.6、§S3.8）
  - T113运维终端-系统架构设计.md §7.2、§15.3、§16.1、§16.2、§18.1、§18.3
