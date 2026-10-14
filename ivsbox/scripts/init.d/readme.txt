============================================================
scripts/init.d  ——  sysvinit 开机服务脚本目录
============================================================

【目录作用】
  存放板端 sysvinit 使用的开机服务脚本，供 rcS 按编号顺序执行。板端为经典 sysvinit
  （无 procd、无 systemd），故不用 USE_PROCD，改用 /sbin/start-stop-daemon 后台拉起。
  本目录脚本随程序落 rootfs 的 /opt/ivsbox，打包时归集进 packaging/init.d。

【文件说明】
  S62usb4g         4G（Quectel EC200G，CDC-ECM）数据面自动拉起与运行期看护：开机幂等拉起
                   （AT 激活 PDP → usb0 up → udhcpc）+ 常驻看护（每 30s 检查，丢 carrier
                   或丢 IPv4 就自动重拨）。编号 62 > 60 且晚于 S41dhcpcd；日志落
                   /var/log/usb4g.log；与镜像自带的 udev 热插拔脚本（usb-EC200G-inserted.sh）
                   用"已有 IPv4 即放过 + flock"共存，不重复拨号
  S65ivsboxd       ivsboxd 主控 sysvinit 脚本：start 只拉起 monitor.sh（主控由 monitor
                   唯一拉起，避免绕过熔断）；stop 先停 monitor 再停主控；restart 清熔断
                   状态（人工解除 G8 failsafe）后重启
  S66ivsboxweb     ivsbox-web sysvinit 脚本（M5-S5.1）：start 只拉起 monitor-web.sh；
                   stop 先停守护再停 Web；restart 清 .monitor-web.state。编号 66 > 65，
                   让主控先建好本地通道 socket（/var/run/ivsbox/ctl）
  .gitkeep         目录占位文件（保留本目录入 git）

【约定与注意事项】
  - 编号必须 > 60：UDISK 由 S60mount_udisk 挂载，编号过小会在挂载前启动，配置/日志会写进
    rootfs 上的同名空目录（重启即丢）。
  - start() 只拉起 monitor.sh，不直启主控：init 直启会绕过 monitor 的 failsafe 熔断，
    跨复位的崩溃计数就攒不起来，持续崩溃会变成 30 分钟一轮的整机复位风暴。
    S66ivsboxweb 同款：start() 只拉起 monitor-web.sh，不直启 ivsbox-web。
  - Web 与主控改用**各自独立**的熔断状态文件（.monitor.state / .monitor-web.state），
    且 monitor-web.sh 不碰看门狗 —— Web 是附属服务，崩溃不该触发整机复位。
  - restart() 才清熔断状态文件（.monitor.state / .monitor-web.state）：rcS 开机只调 start，
    故在 restart 清是安全的，不能放进 start()。
  - 路径口径（2026-09-30 用户定稿）：程序 /opt/ivsbox；数据 /mnt/UDISK/ivsbox；pidfile 在
    /var/run（tmpfs）。
  - 4G 拨号有两个触发源，靠"幂等 + flock"共存：镜像 udev 热插拔脚本（开机冷插拔通常即完成）
    与本目录 S62usb4g（开机兜底 + 运行期掉线自愈）。**不要**把它做进 ivsboxd 主控——4G 拉起
    属板端系统层，`iv_netmgr` 只消费 usb0 的链路状态。

【相关文档】
  - 仓库根 AGENTS.md（规则 2 / 3）
  - ivsbox/docs/功能开发计划.md（§S10、§S3.9、§0 数据落点与守护方式口径）
  - T113运维终端-系统架构设计.md（§18.3 平台侧网络与 4G 链路）
  - ivsbox/scripts/monitor.sh 文件头（熔断语义、唯一拉起方说明）
  - ivsbox/scripts/readme.txt（上级目录脚本链路）
