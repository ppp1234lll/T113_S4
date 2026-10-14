/*
 * IVSBox 出厂身份与工装支撑（libivmodules，功能开发计划 M5-S5.5）
 *
 * ============================ 它是什么 ============================
 * 量产出厂时要把"这台设备是谁"写死到板子上：设备 ID、MAC、调试服务器地址、
 * 加密套件号。写入通道是**UDP 组播**（用户 2026-10-13 口径更新）：产线上一台
 * 工装把身份报文播到组播组，产线上的设备各自收下、各自写进自己的 eMMC 裸分区，
 * **不再要求逐台 USB/串口连接**。
 *
 * 本模块只放**可复用、可单测**的那几块：
 *   - 身份记录的**编解码**（＋累加校验、整块 XOR 混淆）；
 *   - 组播身份报文的**解析与校验**；
 *   - `private` 裸分区的**读/写**；
 *   - 工厂标记文件的存在性判断。
 * 真正的组播监听循环与命令行在 `tools/provision/`（那是工厂工具，不进固件
 * 常驻进程）；而将来 `ivsboxd` 要读 `devid` 时（架构 §10.2「身份不进配置」）
 * **直接调本模块的 `iv_prov_private_read()`** —— 这就是它没写进 tools/ 的原因。
 *
 * ============================ 两组待冻结的输入（用户给定后替换默认值） ============================
 *   ① 组播默认地址与端口：当前用参考项目默认值 `239.255.255.249:65000`
 *      （本地监听端口 12360）**占位**，以用户给的为准。
 *   ② **报文协议格式**：当前用下面这套**自定义最小格式**占位。字段定义、校验
 *      方式、是否加密，全部待用户给定。这里的定义只保证"自己发的自己能收"，
 *      **不声称与任何现网格式兼容**。
 *
 * ============================ 记录格式（56 字节明文，落盘时整块 XOR） ============================
 *   偏移  0  magic   4B   0x42565349（内存序 "IVSB"）
 *   偏移  4  rec_ver 1B   IV_PROV_REC_VER
 *   偏移  5  suite   1B   加密套件 ID（姊妹工程 bird 用 SuiteID=5）
 *   偏移  6  rsv     2B   恒 0
 *   偏移  8  devid   4B   小端
 *   偏移 12  mac     6B
 *   偏移 18  rsv     2B   恒 0
 *   偏移 20  server 32B   ASCII，NUL 填充
 *   偏移 52  sum     4B   小端：前 52 字节逐字节累加
 *
 * ============================ 组播报文格式（56 字节明文） ============================
 *   偏移  0  magic   4B   "IVPR"
 *   偏移  4  ver     1B   1
 *   偏移  5  type    1B   1 = 身份
 *   偏移  6  rsv     2B   恒 0
 *   偏移  8  devid   4B   小端
 *   偏移 12  mac     6B
 *   偏移 18  suite   1B
 *   偏移 19  rsv     1B
 *   偏移 20  server 32B
 *   偏移 52  sum     4B   前 52 字节累加
 * （与记录格式**字段次序刻意不同**：报文的 suite 在 mac 之后，记录里在 mac 之前。
 *  两者是不同年代的两种东西，将来报文格式以用户给的为准，不要互相套用。）
 *
 * ============================ 安全边界（说清楚，不夸大） ============================
 * XOR 混淆**只防"明文浏览"**（拿 hexdump 一眼看不出内容），**不构成加密**：
 * key 在源码里，且是固定 4 字节循环。真正的机密性依赖"组播只在产线内网可达"
 * 与工厂标记（量产后删除即关闭入口）。这一条与计划 S5.5 的原文口径一致。
 */
#ifndef IVSBOX_IV_PROVISION_H
#define IVSBOX_IV_PROVISION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * 常量与默认值
 * -------------------------------------------------------------------------- */
#define IV_PROV_REC_MAGIC  ((uint32_t)0x42565349u) /* 内存序 "IVSB" */
#define IV_PROV_PKT_MAGIC  "IVPR"
#define IV_PROV_REC_VER    1u
#define IV_PROV_PKT_VER    1u

#define IV_PROV_SERVER_MAX 32u  /* 调试服务器地址字段长度 */
#define IV_PROV_REC_LEN    56u  /* 记录明文长度 */
#define IV_PROV_PKT_LEN    56u  /* 报文长度 */
#define IV_PROV_SECTOR     4096u /* 裸分区写入粒度（一个块） */

#define IV_PROV_PKT_TYPE_ID 1u  /* 报文类型：身份 */

/* **待用户冻结**：组播参数（当前为参考项目默认值占位） */
#define IV_PROV_MCAST_IP_DEFAULT         "239.255.255.249"
#define IV_PROV_MCAST_PORT_DEFAULT       65000
#define IV_PROV_MCAST_LOCAL_PORT_DEFAULT 12360

/* 落点 */
#define IV_PROV_PRIVATE_DEV_DEFAULT "/dev/by-name/private"
#define IV_PROV_SECURE_DIR_DEFAULT  "/mnt/UDISK/ivsbox/secure"
#define IV_PROV_MARKER_DEFAULT      "/mnt/UDISK/ivsbox/factory/enable_provision"

/* --------------------------------------------------------------------------
 * 身份
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t devid;                      /* 设备 ID（TID） */
    uint8_t  mac[6];                     /* 设备 MAC */
    uint8_t  suite_id;                   /* 加密套件 ID */
    char     server[IV_PROV_SERVER_MAX]; /* 调试服务器地址（ASCII，含 NUL） */
} iv_prov_id_t;

/* 累加校验：逐字节求和（与记录/报文里的 sum 字段同口径）。 */
uint32_t iv_prov_checksum(const uint8_t *buf, size_t len);

/* 原地 XOR 混淆/还原（同一函数即是其逆运算）。 */
void iv_prov_xor(uint8_t *buf, size_t len);

/* 编码成 56 字节**明文**记录（含 sum）。out 容量至少 IV_PROV_REC_LEN。
 * 返回 IV_OK / IV_EINVAL。 */
int iv_prov_id_encode(const iv_prov_id_t *id, uint8_t out[IV_PROV_REC_LEN]);

/* 解析 56 字节明文记录：校验 magic / rec_ver / sum。
 * 返回 IV_OK / IV_EINVAL（参数）/ IV_EPROTO（格式或校验不符）。 */
int iv_prov_id_decode(const uint8_t in[IV_PROV_REC_LEN], iv_prov_id_t *out);

/* --------------------------------------------------------------------------
 * 组播报文
 * -------------------------------------------------------------------------- */

/* 解析一条组播报文。返回 IV_OK（*out 已填）/ IV_EINVAL / IV_EPROTO（含校验不过）。
 * **长度必须精确等于 IV_PROV_PKT_LEN**：多几个字节的包一律拒收，免得把尾巴
 * 当成"另一个字段"。 */
int iv_prov_pkt_parse(const uint8_t *pkt, size_t len, iv_prov_id_t *out);

/* 按报文格式打包一条（工装侧发送 / 单测往返用）。
 * out 容量至少 IV_PROV_PKT_LEN。返回 IV_OK / IV_EINVAL。 */
int iv_prov_pkt_build(const iv_prov_id_t *id, uint8_t out[IV_PROV_PKT_LEN]);

/* --------------------------------------------------------------------------
 * 裸分区读写
 * -------------------------------------------------------------------------- */

/* 把身份写入裸分区（open+write 4 KiB 块+fsync，整块 XOR 混淆）。
 * 允许重写（不设锁定位，2026-09-30 口径）。失败返回 IV_EIO / IV_ENOENT /
 * IV_EPERM（非 root）等。 */
int iv_prov_private_write(const char *dev, const iv_prov_id_t *id);

/* 从裸分区读回身份（读 4 KiB，XOR 还原后解析）。
 * 未写入过（magic 不符）返回 IV_ENOENT。 */
int iv_prov_private_read(const char *dev, iv_prov_id_t *out);

/* --------------------------------------------------------------------------
 * 工厂标记
 * -------------------------------------------------------------------------- */

/* 标记文件存在 ⇒ 1；不存在 ⇒ 0（含参数为 NULL、stat 出错）。 */
int iv_prov_marker_present(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_PROVISION_H */
