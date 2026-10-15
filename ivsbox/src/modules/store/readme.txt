============================================================
src/modules/store  ——  control.db 表结构与落盘（iv_store）
============================================================

【目录作用】
  iv_store.c 实现 control.db 的四张业务表（device / snmp_latest / audit /
  net_event）：表结构（CREATE TABLE IF NOT EXISTS）＋ 写 API ＋ 有界清理 ＋
  只读出口。计划 §S9 的"表结构待定义"交付物（S4.2 建档、S4.3 采集落盘、
  S5.2 写操作审计、§S9 网络历史四处登记的落点）。

【文件说明】
  iv_store.c            四表 schema ＋ 写路径（全 prepared statement，防注入）
                        ＋ trim 有界清理 ＋ 只读出口 ＋ schema 版本门
  （头文件在 include/ivsbox/iv_store.h）

【表结构与口径】
  device        S4.2 发现档案。主键＝MAC（小写冒号分隔文本，计划 §S4.2
                "主键＝MAC/序列号，IP 为可变属性"）；同 MAC 改 IP＝更新不重复
                建档；first_seen 只在建档时写、UPDATE 不碰（保留）；上限
                IV_STORE_DEVICE_ROWS_MAX=64，满删最旧 last_seen。
                ⚠ device 表没有 id 列——trim 的键列/排序列必须按表给
                （mac/last_seen），不能一刀切（首轮单测当场抓过）。
  snmp_latest   S4.3 采集结果，最近值语义（主键 target+slot，INSERT OR
                REPLACE）；无应答/未配槽位落 ok=0（值空串，不乱填），
                读侧 ok=0 一律 IV_ENOENT。
  audit         S5.2 写操作审计（config.apply / system.reboot /
                system.settime）。detail 只含键名/计数等摘要，按构造不含
                口令/令牌；上限 512 行。
  net_event     网络历史（出口切换等）。生产者＝agent_reconnect（装配层）；
                上限 512 行。

【schema 版本门】
  PRAGMA user_version：0＝新库（建表后写 1）；1＝当前（跳过快路径）；
  >1＝库比程序新 ⇒ IV_EPROTO，不猜、不降级读写。

【失败口径（与 iv_db 不同）】
  fail-open：store 建表失败（磁盘满/只读）⇒ 句柄为 NULL，各接线点 NULL
  守卫后降级为"只写日志"。档案/审计是辅助数据，不得拖死主控；iv_db 打不开
  才是退出级（主数据）。

【接线（生产者都在装配层）】
  device_upsert   src/modules/agent/iv_agent.c on_dev_found（S4.2 发现回调）
  snmp_put        src/modules/agent/iv_agent.c on_snmp_done（S4.3 完成回调）
  audit_add       src/modules/ctl/iv_ctl.c（config.apply / system.reboot /
                  system.time 成功路径）
  net_event_add   src/modules/agent/iv_agent.c agent_reconnect（出口切换）
  装配点＝src/app/main.c 第 2b 步（iv_store_open，失败告警置 NULL），
  关停先 iv_store_close 再 iv_db_close。

【测试】
  tests/unit/test_store.c     模块级 9 组（schema 幂等/版本门/device upsert
                              与上限/最近值/审计与网络历史上限/白名单/防御）
  tests/integration/test_agent_m4.c     m09：SNMP 落库端到端（真实 SNMP 轮次
                              后逐槽位读回 ＋ 行数 ＋ store==档案一致性）
  tests/integration/test_agent_switch.c k04：net_event 落库端到端（真切换一
                              次 ⇒ net_event 恰一行）
  tests/unit/test_ctl.c       c11a/c11b：config.apply 成功 ⇒ audit 恰一行

【约定与注意事项】
  - 写路径全部 sqlite3_prepare_v2 + bind_*，绝不拼串（外部字符串只进绑定
    参数）；iv_store_rows 的表名走白名单（防注入形态字符串拼进 SQL）。
  - device 的 UPDATE-then-INSERT 不用 UPSERT 语法（板端 SQLite 版本未知，
    UPSERT 要 >= 3.24）。
  - 墙钟用 time()：这些是跨重启的长期事实，不用单调毫秒。
