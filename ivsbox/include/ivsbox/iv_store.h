/*
 * IVSBox control.db 表结构与落盘（计划 S9 延伸交付物，2026-10-15 定稿）
 *
 * ============================ 它是什么 ============================
 * `iv_db`（M1-S9）只给了"打开/执行/事务"，**全库此前没有任何 CREATE TABLE** ——
 * S4.2 设备发现、S4.3 SNMP 采集、S5.2 写操作审计、网络历史四处都登记着
 * "落 control.db（表结构待 S9 定义）"。本模块补齐这一块：**表结构 ＋ 写 API ＋
 * 有界清理 ＋ 只读出口**，并把四个生产者接到表上（接线在装配层，见各表注释）。
 *
 * ============================ 单写者与失败口径 ============================
 *   - **只有 ivsboxd 写**（架构 §10.4）；Web/平台经本地通道的只读方法读，
 *     本模块只暴露**模块内部用的**只读出口（单测断言用；Web 展示方法后续按需加）。
 *   - **fail-open（登记）**：store 建表失败（磁盘满/只读）⇒ 句柄为 NULL，
 *     各接线点 NULL 守卫后降级为"只写日志" —— 档案/审计是辅助数据，**不得**
 *     因为它们落不了盘而拖死主控（与 iv_db 打不开即退出不同：那是主数据）。
 *
 * ============================ 四张表 ============================
 *   device      S4.2 发现档案。主键＝MAC（计划 §S4.2"主键＝MAC/序列号，IP 为可变
 *               属性"）；同 MAC 改 IP＝更新不重复建档；上限 64 行，满删最旧 last_seen。
 *   snmp_latest S4.3 采集结果，**最近值语义**（主键 target+slot，INSERT OR REPLACE），
 *               供平台上报与 Web 展示；无应答落 ok=0（**不乱填**，S4.3 纪律）。
 *   audit       S5.2 写操作审计（config.apply / system.reboot / system.settime）。
 *               detail 只含键名/计数等摘要，**按构造不含任何口令/令牌**；上限 512 行。
 *   net_event   网络历史（计划 §S9"档案/审计/网络历史"）：出口切换等事件；
 *               生产者＝agent_reconnect（装配层）。上限 512 行。
 *
 * ============================ schema 版本 ============================
 *   `PRAGMA user_version`：0＝新库（建表后写 1）；1＝当前（跳过，快路径）；
 *   >1＝未来版本（本进程太老）⇒ IV_EPROTO，**不猜、不降级读写**。
 */
#ifndef IVSBOX_IV_STORE_H
#define IVSBOX_IV_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_db.h"
#include "ivsbox/iv_device.h"
#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 各表行数上限（有界：嵌入式 UDISK 不是垃圾场，任何追加型表都必须封顶） */
#define IV_STORE_DEVICE_ROWS_MAX 64
#define IV_STORE_AUDIT_ROWS_MAX  512
#define IV_STORE_NET_ROWS_MAX    512

/* 不透明句柄（内含 iv_db_t*，不拥有 db） */
typedef struct iv_store iv_store_t;

/*
 * 打开并确保 schema 就位（幂等：CREATE TABLE IF NOT EXISTS ＋ user_version 门）。
 * db 为 NULL ⇒ IV_EINVAL；同一 iv_db_t 上重复 open 会得到独立句柄（不推荐，
 * 但不禁止 —— SQLite 连接级串行由 busy_timeout 兜底）。
 */
int iv_store_open(iv_db_t *db, iv_store_t **out);

/* 关闭句柄（只释放本模块内存，**不关 db**）。NULL 安全。 */
void iv_store_close(iv_store_t *st);

/*
 * S4.2 设备档案 upsert：主键 MAC（小写冒号分隔文本）。
 * 同 MAC：更新 ip/brand/model/serial/port/found_by/last_seen，**first_seen 保留**。
 * d 为 NULL ⇒ IV_EINVAL。ip_be 按全工程"数值按大端读"约定。
 */
int iv_store_device_upsert(iv_store_t *st, const iv_dev_desc_t *d, int64_t now_s);

/*
 * S4.3 采集结果落库（最近值语义）。target："camera" / "switch"；
 * ok=0 表示该槽位无应答（value 置空串，**不乱填**）。
 */
int iv_store_snmp_put(iv_store_t *st, const char *target, const char *slot,
                      const char *oid, const char *value, int ok, int64_t ts);

/* S5.2 审计追加。method/detail 不为 NULL；detail 按构造不含口令。 */
int iv_store_audit_add(iv_store_t *st, const char *method, const char *detail,
                       int result, int64_t ts);

/* 网络历史追加。event："up"/"down"/"switch"；wan："wireless"/"wired"/自定义文本。 */
int iv_store_net_event_add(iv_store_t *st, const char *wan, const char *event,
                           const char *detail, int64_t ts);

/* ---- 只读出口（单测断言；Web 展示方法后续按需加）---- */

/* 读 device 表某 MAC 的当前 IP（无该行 ⇒ IV_ENOENT）。 */
int iv_store_device_ip(iv_store_t *st, const char *mac, char *out, size_t cap);

/* 读 snmp_latest 某槽位的 value（ok=0 或无行 ⇒ IV_ENOENT）。 */
int iv_store_snmp_value(iv_store_t *st, const char *target, const char *slot,
                        char *out, size_t cap);

/* 行数统计：table 只认 "device"/"snmp_latest"/"audit"/"net_event"（白名单，
 * 其余 IV_EINVAL —— 绝不把外部字符串拼进 SQL）。 */
int iv_store_rows(iv_store_t *st, const char *table, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_STORE_H */
