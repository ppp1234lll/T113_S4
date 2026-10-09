============================================================
scripts/init.d  ——  sysvinit 开机服务脚本目录
============================================================

【目录作用】
  存放板端 sysvinit 使用的开机服务脚本，供 rcS 按编号顺序执行。板端为经典 sysvinit
  （无 procd、无 systemd），故不用 USE_PROCD，改用 /sbin/start-stop-daemon 后台拉起。
  本目录脚本随程序落 rootfs 的 /opt/ivsbox，打包时归集进 packaging/init.d。

【文件说明】
  S65ivsboxd       ivsboxd 主控 sysvinit 脚本：start 只拉起 monitor.sh（主控由 monitor
                   唯一拉起，避免绕过熔断）；stop 先停 monitor 再停主控；restart 清熔断
                   状态（人工解除 G8 failsafe）后重启
  .gitkeep         目录占位文件（保留本目录入 git）

【约定与注意事项】
  - 编号必须 > 60：UDISK 由 S60mount_udisk 挂载，编号过小会在挂载前启动，配置/日志会写进
    rootfs 上的同名空目录（重启即丢）。
  - start() 只拉起 monitor.sh，不直启主控：init 直启会绕过 monitor 的 failsafe 熔断，
    跨复位的崩溃计数就攒不起来，持续崩溃会变成 30 分钟一轮的整机复位风暴。
  - restart() 才清熔断状态文件（.monitor.state）：rcS 开机只调 start，故在 restart 清是安全的，
    不能放进 start()。
  - 路径口径（2026-09-30 用户定稿）：程序 /opt/ivsbox；数据 /mnt/UDISK/ivsbox；pidfile 在
    /var/run（tmpfs）。

【相关文档】
  - 仓库根 AGENTS.md（规则 2 / 3）
  - ivsbox/docs/功能开发计划.md（§S10、§0 数据落点与守护方式口径）
  - ivsbox/scripts/monitor.sh 文件头（熔断语义、唯一拉起方说明）
  - ivsbox/scripts/readme.txt（上级目录脚本链路）
