============================================================
modules/upd  ——  升级安装 / 双版本切换 / 回滚（M7-S7.2 ＋ S7.3）
============================================================

【目录作用】
  应用级 OTA 的**唯一下盘逻辑**：升级包的验签、暂存、准入检查、双版本安装、
  current/previous 原子切换、bootcount 自检确认与自动回滚。进 libivmodules，
  因此可以单测、也可以被升级器与其它目标复用。

  与 `modules/ota` 的分工是硬的：
    modules/ota   只回答"**这个包合法吗**"（格式 ＋ Ed25519 验签），**只读不写**；
    modules/upd   只回答"**改盘怎么改、坏了怎么回去**"，不自己判签名，
                  而是调用 iv_ota 验签后再动盘。
  谁都不越界：验签器不许碰文件系统，安装器不许自己实现密码学。

【落点口径（2026-09-30 用户定稿，勿改）】
  程序（bin/ ＋ scripts/）→ rootfs 的 /opt/ivsbox/，版本目录在其 releases/<版本>/
  数据（ota/、db/、log/、config/…）→ UDISK 的 /mnt/UDISK/ivsbox/
  这样安排的理由：`S60mount_udisk` 挂载失败会直接 `mkfs.ext4` 清空 UDISK，
  程序放 rootfs 才不会被连带清掉。

【文件说明】
  iv_upd.c    全部实现（零 malloc、调用方持有结构体、手写 JSON、本层不打日志）
  见 include/ivsbox/iv_upd.h 的文件头：落盘顺序、掉电窗口、返回码表都在那里

【关键设计（改代码前先读 iv_upd.h 的对应段落）】
  1. **落盘顺序固定**：验签 → adopt → 空间检查 → 暂存 → 装版本目录 →
     **先落 pending.json** → previous 链 → current 链（tmp+rename 原子） → 事件。
     pending 必须早于 current：崩在两者之间，重启后还能按 pending 判该回滚谁。
  2. **引导副本不参与轮换**：`<app_root>/bin/ivsbox-updater` 是裸布局的那一份，
     `adopt` 刻意不删它 —— 恢复路径不能依赖"正在被替换的那个程序"。
  3. **掉电注入**：`iv_upd_cfg_t::fault_at` = verify|stage|install|switch|pending
     时立刻返回 IV_UPD_EFAULT 且**不做任何清理**，用来复现"升级到一半掉电"。
  4. **chmod 失败不报错，但装完要回读**：`bin/` 前缀文件装完逐个 stat，
     没有执行位就报 IV_UPD_EPERM（只读/无 mode 的文件系统会静默吞掉 chmod）。

【运行方式】
  由 `ivsbox-updater`（updater/）经命令行调用；`scripts/monitor.sh` 拉起安装进程，
  `scripts/ivsbox-run.sh` 用 `boot-begin/boot-ok/boot-fail` 决定起哪个版本。
  单测：`make test`（tests/unit/test_upd.c）

【约定与注意事项】
  - 本层**不打日志**：错误原因走返回值 ＋ `iv_upd_ret_name()`/`iv_upd_reason_name()`，
    由调用方决定怎么记（升级器打 stderr、monitor 记 monitor-alert.log）。
  - 单写者：切换 current/previous 与写 pending.json 只允许本模块做；
    `iv_ctl` 的 `ota.request` 只写一行请求文件，**不碰** pending 与符号链接。
  - 量产公钥未部署 ⇒ `iv_ota_builtin_pubkey()` 返 IV_ENOTSUP ＋ 全零 ⇒ 拒一切包
    （fail-closed）。公钥由发布侧产出后写入，**私钥绝不入库**。

【相关文档】
  - include/ivsbox/iv_upd.h（契约与落盘顺序的权威描述）
  - docs/功能开发计划.md §S7.2 / §S7.3；docs/修改记录.md 对应条目
  - T113运维终端-系统架构设计.md §10.1/§10.2/§11.2（落点与双版本流程）
