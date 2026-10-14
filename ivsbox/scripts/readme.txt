============================================================
scripts  ——  部署与运行时脚本目录（拉起链路 + sysvinit 服务脚本）
============================================================

【目录作用】
  本目录存放 IVSBox 在板端运行所需的启动/守护脚本。板上有三条**互相独立**的拉起链路
  （2026-10-14 M6-S6.1 起）：
    · ivsboxd（主控）：init.d/S65ivsboxd → start_ivsbox.sh → monitor.sh
    · ivsbox-web（Web）：init.d/S66ivsboxweb → monitor-web.sh
    · ivsbox-media（媒体）：init.d/S67ivsboxmedia → monitor-media.sh
  各自 monitor 是各自进程的**唯一拉起方**，并内含熔断：窗口内重拉超限则进 failsafe
  停止拉起。脚本随程序落 rootfs 的 /opt/ivsbox/scripts，不落 UDISK。

【文件说明】
  start_ivsbox.sh     启动入口：nohup 后台拉起 /opt/ivsbox/scripts/monitor.sh
  monitor.sh          ivsboxd 的唯一拉起方：5s 轮询守护 + 熔断（failsafe）熔断状态落 UDISK
  monitor-web.sh      ivsbox-web 的唯一拉起方：5s 轮询守护 + 熔断，状态独立落 UDISK
                      （.monitor-web.state，与主控互不干扰）；**不碰看门狗**
  monitor-media.sh    ivsbox-media 的唯一拉起方：同 monitor-web.sh 模式
                      （.monitor-media.state）；**不碰看门狗**（媒体崩溃只停抓拍/录像，
                      控制面不受牵连；计划 S6.1"崩溃只重启自身"）
  init.d/             sysvinit 开机服务脚本子目录（见 init.d/readme.txt）
  ci/                 CI 辅助脚本子目录（当前为空占位）

【约定与注意事项】
  - 部署落点：程序（bin/scripts）落 rootfs 的 /opt/ivsbox；数据（config/db/queue/ota/
    releases/secure + 熔断状态 + 日志）落 UDISK 的 /mnt/UDISK/ivsbox。
  - sysvinit 脚本编号必须 > 60：/mnt/UDISK 由 S60mount_udisk 挂载，编号过小会先于挂载启动。
  - Web 与主控的守护**刻意分开**（各自状态文件 / pidfile），且 monitor-web.sh **绝不碰看门狗**：
    看门狗是 ivsboxd 独占的整机兜底，Web 崩溃不该触发整机复位。S66ivsboxweb 编号 > 65，
    让主控先建好本地通道 socket（/var/run/ivsbox/ctl）。
  - 熔断计时一律用 /proc/uptime 单调秒（不用墙钟 date），以跨整机复位存活。
  - IVSBOX_POLL_S 必须显著小于看门狗超时 16s（建议 ≤ 10），否则普通崩溃会升级为整机复位。
  - 本目录脚本由顶层 Makefile 的 package-copy 归集进 packaging/scripts，供 OTA 升级包使用；
    构建产物不入库（见 .gitignore）。
  - 可调参数经环境变量覆盖（IVSBOX_MAX_RESTARTS / IVSBOX_RESTART_WINDOW /
    IVSBOX_FAILSAFE_COOLDOWN / IVSBOX_POLL_S / IVSBOX_APP_DIR / IVSBOX_HOME 等）。

【相关文档】
  - 仓库根 AGENTS.md（规则 2 修改记录、规则 3 提交推送、规则 6 清理）
  - ivsbox/docs/功能开发计划.md（M1-S10 骨架装配 + init/守护脚本；M5-S5.1 Web 进程与守护）
  - ivsbox/docs/操作手册/日常开发闭环流程.md（编译 → 上板 → 串口验证链路）
  - 各脚本文件头注释（落点、熔断语义、时间基准的权威说明）
