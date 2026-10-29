/*
 * SNMP v2c 最小客户端（libivmodules，功能开发计划 M4-S4.3）
 *
 * ============================ 它是什么 ============================
 * **自写**的最小 SNMP**客户端**：只做 **GET / GETNEXT**（不做 SET / TRAP / v3 / USM）。
 * 采集摄像机（1 路）与交换机的标量值与表列值，结果供平台上报与 Web 展示。
 *
 * ============================ 为什么不引 net-snmp ============================
 * 三处环境实测**均无**：板端 sysroot 无 `libnetsnmp`、编译 VM 无 `libsnmp-dev`、
 * Tina 无 buildroot package 树 ⇒ 引它等于先移植一套一万多行的库。而需求面只有
 * "读"，用不到它的 Agent / Trap / v3 / USM。
 * 参考项目 `snmp_udp.c`（905 行、零依赖）本身就是自写 v2c 客户端，口径直接对齐。
 *
 * **MIB ≠ 实现**：厂家 `.mib/.my` 只回答"查哪些 OID"，BER 编解码与 UDP 收发
 * 仍得自己写。三份厂家 MIB 的用途＝**抽取 OID 蓝本**，不是"拿来即用"的驱动。
 *
 * ============================ 报文口径（对齐参考实现 + RFC 3416） ============================
 *   SNMP 消息  ::= SEQUENCE { version INTEGER(1=SNMPv2c), community OCTET STRING, PDU }
 *   GetRequest  ::= [0] 构造体 (0xA0) { request-id, error-status(0), error-index(0),
 *                                       variable-bindings }
 *   GetNext     ::= [0] 构造体 (0xA1) 同上
 *   GetResponse ::= [0] 构造体 (0xA2) 同上；varbind 的值位由 NULL 换成实值
 *   varbind     ::= SEQUENCE { name OBJECT IDENTIFIER, value ANY }
 *   **长度域按 BER 规范**（短式 <128 单字节；长式首字节 0x81/0x82 + 后续字节数）
 *   —— 参考实现**只写单字节长度**（>127 就错），本模块按规范实现（登记为对参考的
 *   一处**修正**，不是偏离：报文更长时参考实现会发出非法包）。
 *   OID 编码：前两级合并为 40*a+b；其后每级 base-128、非末字节最高位置 1。
 *
 * ============================ 取值 ============================
 *   - **标量 OID**（约定以 `.0` 结尾）→ 直接 GET；
 *   - **表列 OID**（值里不带末尾 `.0`，如 `.1.3.6.1.2.1.2.2.1.8`）→ 从该列首实例起
 *     GETNEXT 连读，直到**走出该列子树**为止（对齐参考项目 `Port Oper Status … .8.xx`
 *     口径）；`max_rows` 上限防跑飞。
 *   - request-id 由调用方给；**应答 reqid 不匹配一律丢弃**（继续等）。
 *   - 单个 OID 无应答记"该项无值"，**不拿相邻值乱填**（`noSuchObject`/
 *     `noSuchInstance`/`endOfMibView` 都映射为"无值"）。
 *
 * ============================ 传输与线程 ============================
 *   - 普通 UDP socket，目的端口默认 161；一次一事务，靠 request-id 配对；
 *   - **阻塞式收发**（`SO_RCVTIMEO` 到期即重试），**必须在慢任务池（M1-S5）内调用**，
 *     绝不阻塞 Reactor；
 *   - 全部系统调用经 `iv_snmp_iops_t` 注入点，单测零网络。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK / IV_EINVAL / IV_ERANGE（缓冲不足）/ IV_ETIMEDOUT（重试耗尽）/
 *   IV_EPROTO（报文非法）/ IV_ENOENT（该项无值）/ IV_EIO
 */
#ifndef IVSBOX_IV_SNMP_H
#define IVSBOX_IV_SNMP_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h> /* socklen_t */

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 常量
 * ------------------------------------------------------------------------- */
#define IV_SNMP_COMMUNITY_MAX 64u
#define IV_SNMP_HOST_MAX 64u
#define IV_SNMP_OID_STR_MAX 160u /* 与配置模块 IV_CFG_STR_MAX 同量级 */
#define IV_SNMP_STR_MAX 128u     /* OCTET STRING 取值缓冲 */
#define IV_SNMP_PKT_MAX 512u     /* 单包上限（本工程查询报文很短，512 足够） */
#define IV_SNMP_PORT_DEFAULT 161u
#define IV_SNMP_TIMEOUT_MS_DEFAULT 1500u
#define IV_SNMP_RETRIES_DEFAULT 1u
#define IV_SNMP_MAX_ROWS_DEFAULT 64u

/* PDU 标签 */
#define IV_SNMP_PDU_GET 0xA0u
#define IV_SNMP_PDU_GETNEXT 0xA1u
#define IV_SNMP_PDU_GETRESP 0xA2u

/* BER 类型标签 */
#define IV_SNMP_T_INTEGER 0x02u
#define IV_SNMP_T_OCTETSTR 0x04u
#define IV_SNMP_T_NULL 0x05u
#define IV_SNMP_T_OID 0x06u
#define IV_SNMP_T_IPADDR 0x40u
#define IV_SNMP_T_COUNTER32 0x41u
#define IV_SNMP_T_GAUGE32 0x42u
#define IV_SNMP_T_TIMETICKS 0x43u
#define IV_SNMP_T_COUNTER64 0x46u
#define IV_SNMP_T_NOSUCHOBJECT 0x80u
#define IV_SNMP_T_NOSUCHINSTANCE 0x81u
#define IV_SNMP_T_ENDOFMIBVIEW 0x82u

/* ---------------------------------------------------------------------------
 * 槽位名规范表（OID 蓝本，见功能开发计划 §S4.3）
 *
 * **键名即语义**：配置里 `camera.snmp.<品牌>.<槽位>` / `switch.snmp.oid.<槽位>`
 * 的 `<槽位>` 就是下面这两张表里的字符串，**顺序固定**。表只回答"语义叫什么"，
 * **不含任何厂家 OID 值**（值全部来自配置，换机型只改配置、不改固件）。
 * 装配层按本表顺序读配置键、也按本表顺序回报槽位名 ⇒ 三处（默认值表、
 * 装配读取、Web 展示）不会各写一套名字而错位。
 * ------------------------------------------------------------------------- */
#define IV_SNMP_SLOT_NAME_MAX 32u /* ≥ IV_CFG_SEG_MAX(32)，含结尾 NUL */
#define IV_SNMP_CAM_SLOT_N    9u  /* 摄像机 OID 槽位数 */
#define IV_SNMP_SW_SLOT_N     12u /* 交换机 OID 槽位数 */

/* 摄像机槽位（厂家中立；标量以 `.0` 结尾走 GET，否则按表列 GETNEXT）。 */
extern const char *const iv_snmp_cam_slots[IV_SNMP_CAM_SLOT_N];
/* 交换机槽位（前 2 条为标量，其余 IF-MIB / LLDP 表列，走 GETNEXT）。 */
extern const char *const iv_snmp_sw_slots[IV_SNMP_SW_SLOT_N];

/* ---------------------------------------------------------------------------
 * 取值
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t type;   /* 原始 BER 标签；0 = 无值（无应答/异常值） */
    int64_t num;    /* INTEGER / Counter32 / Gauge32 / TimeTicks / Counter64 */
    char    str[IV_SNMP_STR_MAX]; /* OCTET STRING（IpAddress 文本化；OID 点分文本） */
    char    oid[IV_SNMP_OID_STR_MAX]; /* 本 varbind 的 OID 点分文本 */
} iv_snmp_value_t;

/* 该值是否"有效"：排除了无值/异常值哨兵。 */
int iv_snmp_value_valid(const uint8_t type);

/* 诊断名（永不返回 NULL）。 */
const char *iv_snmp_type_name(uint8_t type);

/* ---------------------------------------------------------------------------
 * I/O 注入点
 * ------------------------------------------------------------------------- */
typedef struct {
    int      (*socket)(int domain, int type, int proto);
    int      (*setsockopt)(int fd, int level, int optname, const void *optval,
                           socklen_t optlen);
    ssize_t  (*sendto)(int fd, const void *buf, size_t len, int flags,
                       const struct sockaddr *addr, socklen_t alen);
    ssize_t  (*recvfrom)(int fd, void *buf, size_t len, int flags,
                         struct sockaddr *addr, socklen_t *alen);
    int      (*close)(int fd);
    uint64_t (*now_ms)(void);
} iv_snmp_iops_t;

const iv_snmp_iops_t *iv_snmp_default_iops(void);

/* ---------------------------------------------------------------------------
 * 目标
 * ------------------------------------------------------------------------- */
typedef struct {
    const iv_snmp_iops_t *iops;    /* NULL → 默认 */
    char     host[IV_SNMP_HOST_MAX];      /* 点分十进制 */
    uint16_t port;                 /* 0 → 161 */
    char     community[IV_SNMP_COMMUNITY_MAX]; /* 读团体字 */
    uint32_t timeout_ms;           /* 0 → 1500 */
    uint32_t retries;              /* 重试次数（0 → 1，共 2 发） */
    uint32_t max_rows;             /* walk 行数上限；0 → 64 */
    uint32_t reqid_seed;           /* request-id 起始值（默认 1） */
} iv_snmp_target_t;

/* 填默认（host/community 空、port=161、timeout=1500、retries=1、max_rows=64）。 */
void iv_snmp_target_default(iv_snmp_target_t *t);

/* ---------------------------------------------------------------------------
 * 纯 BER 函数（无 I/O）
 * ------------------------------------------------------------------------- */

/* OID 点分字符串 → BER **内容**（不含 tag/len）。返回内容字节数，负码为错误。 */
int iv_snmp_oid_encode(const char *dotted, uint8_t *out, size_t cap);

/* BER OID 内容 → 点分字符串（以 '.' 开头）。返回写入长度（不含 NUL），负码为错误。 */
int iv_snmp_oid_decode(const uint8_t *in, size_t inlen, char *out, size_t cap);

/* 构造一个完整 SNMP 请求（GetRequest/GetNext）。返回报文长度，负码为错误。 */
int iv_snmp_build_request(uint8_t pdu_tag, const char *community,
                          uint32_t reqid, const char *oid, uint8_t *out,
                          size_t cap);

/* 解析 GetResponse。reqid 不匹配返回 IV_EPROTO（调用方应丢弃该包）。
 * 成功返回 IV_OK 并填 *val；error-status 非 0 时经 *err_status 返回并不填值。 */
int iv_snmp_parse_response(const uint8_t *pkt, size_t len, uint32_t want_reqid,
                           iv_snmp_value_t *val, int *err_status);

/* ---------------------------------------------------------------------------
 * 事务 / 取值
 * ------------------------------------------------------------------------- */

/* 一次阻塞事务：发 req、等 reqid 匹配的应答（`retries+1` 次尝试）。
 * 返回应答长度（>=0）或负码（IV_ETIMEDOUT / IV_EIO / IV_EINVAL）。 */
int iv_snmp_transact(const iv_snmp_target_t *t, uint8_t pdu_tag, uint32_t reqid,
                     const char *oid, uint8_t *resp, size_t respcap);

/* 取一个标量（GET）。 */
int iv_snmp_get(const iv_snmp_target_t *t, const char *oid, iv_snmp_value_t *out);

/* 取表列的下一个实例（GETNEXT）。 */
int iv_snmp_getnext(const iv_snmp_target_t *t, const char *oid,
                    iv_snmp_value_t *out);

/* 走一列表列：从 column_oid 起 GETNEXT 连读，直到走出该列子树或达上限。
 * rows 至少 max_rows 项；*n_out 回填实际行数。 */
int iv_snmp_walk(const iv_snmp_target_t *t, const char *column_oid,
                 iv_snmp_value_t *rows, int max_rows, int *n_out);

/* OID 点分文本是否在基子树内（base=".1.3.6" 含 ".1.3.6.1" 但不含 ".1.3.60"）。 */
int iv_snmp_oid_in_subtree(const char *base, const char *oid);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_SNMP_H */
