# 打包与安装（骨架）

当前为空目录，仅记录约定。实现放在 M5/M7 阶段。

## 目标产物

- `ivsbox.ipk` —— Tina/OpenWrt 包，安装到 `/opt/ivsbox/releases/<version>/`
- 三个常驻进程由 `scripts/init.d/` 下的 procd 脚本管理
- 按需特权进程 `ivsbox-updater` / `ivsbox-provision` 不随服务自动启动

## postinst 需要完成的事（尚未实现）

1. 创建运行用户与组 `ivsbox`（`ivsbox-web` / `ivsbox-media` 以此身份运行）。
2. 创建目录并设置属主与权限：

   | 目录 | 属主 | 权限 | 说明 |
   |---|---|---|---|
   | `/data/ivsbox/db/` | ivsboxd 用户 | 0750 | 控制库与媒体库 |
   | `/data/ivsbox/config/` | ivsboxd 用户 | 0750 | 运行配置 |
   | `/data/ivsbox/config/secure/` | ivsboxd 用户 | 0700 | 密钥与口令（架构文档 §10.2） |
   | `/data/ivsbox/factory/` | root | 0700 | 工厂身份 |
   | `/data/ivsbox/queue/` | ivsboxd 用户 | 0750 | 持久上报队列 |
   | `/mnt/sd/ivsbox/media/` | ivsbox 组 | 0770 | 媒体文件 |

3. 创建 IPC socket 目录（权限 0660，架构文档 §5.1）。
4. 安装 init 脚本并 `enable`（**注意**：骨架 stub 会立即退出，M1 完成前不要 enable）。
5. 写入 `/opt/ivsbox/current` 符号链接。

## prerm / postrm

- 停止并 disable 三个服务。
- **不删除** `/data/ivsbox/` 与 `/mnt/sd/ivsbox/media/`（保留现场证据与用户数据）。

## TODO

- 分区布局与 Flash 占用需与 `/data` 容量一起核算（缺口 G1）。
- `/opt` 所在分区是否可写、容量是否够放两个版本目录，需在 M0 的 BSP 能力清单中确认。
