/*
 * IVSBox 出厂身份与组播工装支撑（libivmodules，功能开发计划 M6/S5.5 收口）
 *
 * ============================ 它是什么 ============================
 * 出厂时要把"这台设备是谁"（设备 ID ＋ MAC）写死在板子上，并让产线工装能**搜到**
 * 它。用户 2026-10-14 给定《蜂鸟物联打印软件协议20250220.xlsx》作为**协议权威**，
 * 本模块据此实现（原自定义 `IVPR` 二进制帧整套作废）：
 *
 *   - **报文＝JSON over UDP 组播**（明文 JSON 字符串，不带换行符）；
 *   - **组播地址/端口**：搜索、查询、配置设备ID·MAC、配置继电器、程序跳转 =
 *     `239.255.255.252:37200`；**配置 IP / 配置服务器地址另走**
 *     `239.255.255.249:65000`（后者本轮未实现，仅登记常量）；
 *   - **骨架**：`{"info":{"tid":"<6位HEX 设备ID>","date":"<YYYYMMDD>"},
 *     "data":{"types":"<指令>",...}}`；
 *   - **本轮只实现两条**（用户 2026-10-14 指令：「只需要实现 设备搜索 、设备ID
 *     MAC配置就行，其他的按照你的建议编写」）：`search`（设备搜索）与
 *     `update_mac`（配置设备 ID、MAC）。其余 6 条（inquiry / update_relay /
 *     update_ip / update_server / update_np / update_runaddr）**收到一律不回复、
 *     不动作**，只留 `types` 常量与注释，等后续步骤。
 *
 * ============================ 两条已实现指令的口径 ============================
 * **① search（设备搜索）**
 *   软件→组播：`{"info":{"tid":"000000","date":"<日期>"},"data":{"types":"search"}}`
 *   （文档：打开软件发一次、之后每 60s 一次；tid 是软件自己的 ID，未指定设备时 000000）
 *   设备→组播：`... "data":{"types":"search","tnam":..,"mcu":..,"mod":..,"sv":..,
 *   "mac":..,"ip":..,"nm":..,"gw":..,"inip":..,"outip":..,"iccid":..}`
 *   **应答条件**：`info.tid` 为 `000000`（广播）**或**与本机设备 ID 相同（定点搜索）。
 *
 * **② update_mac（配置设备 ID、MAC）**
 *   软件→组播：`{"info":{"tid":"12DEFF",...},"data":{"types":"update_mac",
 *   "mac":"02-00-00-44-6c-40"}}`
 *   设备→组播：`... {"types":"update_mac","result":"success|fault",<设备信息...>}`
 *
 *   ⚠ **`info.tid` 在本报文里同时承担两个角色，本模块按"它就是要写入的新设备 ID"
 *   实现**（`000000` 例外＝只改 MAC、不动 ID）。依据：文档《配置设备IDMAC》示例
 *   请求 `tid=12DEFF`、而**应答**示例里 `tid=1234FF` —— 若 tid 只是寻址用的旧 ID，
 *   应答不可能换成另一个值；换成新值恰是"设备 ID 已被改成新值"的自证。字段表里
 *   `data` 下也确实**没有**独立的"新 ID"字段。
 *   **该口径属推断，登记待用户确认**；现场如只需刷 MAC，工具侧用 `--keep-id`
 *   安全阀（忽略报文里的新 ID）。
 *
 *   `mac` 字段**三种写法都认**（文档请求示例用 `-`、应答示例用 `:`）：
 *   `aa-bb-cc-dd-ee-ff` / `aa:bb:cc:dd:ee:ff` / 无分隔 12 位 HEX；非法即
 *   `result:"fault"` 且**不**落盘（长度不足/非 HEX/超范围一律拒）。
 *
 * ============================ 为什么不用 json-c ============================
 * 分层纪律（架构 Reading A）：**json-c 的解析只落 `src/modules/config/`**。
 * 本模块属协议侧，故**手写**一个只认这两条报文的**容错扫描器**：按 `"info"` /
 * `"data"` 两个对象分别取键、字符串值支持 `\"` `\\` 转义、键序无关、空白与
 * **尾逗号**（文档示例里就有 `"date":"20230531",}`）一律容忍。它**不是**通用
 * JSON 解析器，也**不**做嵌套树 —— 只服务这两条报文，这一点写在实现文件头。
 *
 * ============================ 记录格式（56 字节明文，落盘时整块 XOR） ============================
 *   偏移  0  magic   4B   0x42565349（内存序 "IVSB"）
 *   偏移  4  rec_ver 1B   IV_PROV_REC_VER
 *   偏移  5  suite   1B   加密套件 ID（姊妹工程 bird 用 SuiteID=5）
 *   偏移  6  rsv     2B   恒 0
 *   偏移  8  devid   4B   小端（＝`info.tid` 的数值形式）
 *   偏移 12  mac     6B
 *   偏移 18  rsv     2B   恒 0
 *   偏移 20  server 32B   ASCII，NUL 填充
 *   偏移 52  sum     4B   小端：前 52 字节逐字节累加
 *   落点：`private` 裸分区（4 KiB 块直写＋fsync，**刻意不 O_CREAT**）。
 *
 * ============================ 安全边界（说清楚，不夸大） ============================
 * XOR 混淆**只防"明文浏览"**，**不构成加密**：key 在源码里且固定 4 字节循环。
 * 机密性依赖"组播只在产线内网可达"＋工厂标记文件（量产后删除即关闭入口）。
 * 另：**报文本身明文**（协议文档如此），组播网段内任何主机都能看到设备信息与
 * 配置内容 —— 与本模块无关，属协议固有属性，登记在案。
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
#define IV_PROV_REC_VER    1u

#define IV_PROV_SERVER_MAX 32u   /* 记录里调试服务器地址字段长度 */
#define IV_PROV_REC_LEN    56u   /* 记录明文长度 */
#define IV_PROV_SECTOR     4096u /* 裸分区写入粒度（一个块） */

/* 组播参数（协议文档定稿，2026-10-14） */
#define IV_PROV_MCAST_ADDR      "239.255.255.252" /* 搜索/查询/配置ID·MAC/继电器/跳转 */
#define IV_PROV_MCAST_PORT      37200
#define IV_PROV_MCAST_ADDR_NET  "239.255.255.249" /* 配置IP/配置服务器地址（未实现，仅登记） */
#define IV_PROV_MCAST_PORT_NET  65000
#define IV_PROV_TTL             1   /* 组播 TTL：产线同一网段，不跨路由 */

/* 指令 `types` 字符串（协议文档《指令索引》共 8 条；★＝本轮已实现） */
#define IV_PROV_TYPE_SEARCH         "search"         /* ★ 设备搜索 */
#define IV_PROV_TYPE_UPDATE_MAC     "update_mac"     /* ★ 配置设备 ID、MAC */
#define IV_PROV_TYPE_INQUIRY        "inquiry"        /* 未实现 */
#define IV_PROV_TYPE_UPDATE_RELAY   "update_relay"   /* 未实现 */
#define IV_PROV_TYPE_UPDATE_IP      "update_ip"      /* 未实现（另走 249:65000） */
#define IV_PROV_TYPE_UPDATE_SERVER  "update_server"  /* 未实现（另走 249:65000） */
#define IV_PROV_TYPE_UPDATE_NP      "update_np"      /* 未实现 */
#define IV_PROV_TYPE_UPDATE_RUNADDR "update_runaddr" /* 未实现 */

/* 字段容量 */
#define IV_PROV_TID_MAX   8u    /* "1234FF" + NUL */
#define IV_PROV_TSN_MAX   13u   /* 12 位大写 HEX + NUL（对齐参考实现 "%04X%04X%04X"） */
#define IV_PROV_DATE_MAX  16u   /* "20230531" / "20230531125625" */
#define IV_PROV_TYPE_MAX  24u
#define IV_PROV_MAC_MAX   20u   /* "aa:bb:cc:dd:ee:ff" + NUL */
#define IV_PROV_TEXT_MAX  64u   /* 通用文本字段（tnam/mcu/mod/sv/inip/outip） */
#define IV_PROV_JSON_MAX  1024u /* 单条报文体上限（文档示例 < 512 B） */

/* `info.tid` 的广播值：搜索未指定设备、配置时"不改 ID 只改 MAC" */
#define IV_PROV_TID_BROADCAST "000000"

/* 结果字符串（update_mac 应答） */
#define IV_PROV_RESULT_OK    "success"
#define IV_PROV_RESULT_FAULT "fault"

/* 落点 */
#define IV_PROV_PRIVATE_DEV_DEFAULT "/dev/by-name/private"
#define IV_PROV_SECURE_DIR_DEFAULT  "/mnt/UDISK/ivsbox/secure"
#define IV_PROV_MARKER_DEFAULT      "/mnt/UDISK/ivsbox/factory/enable_provision"

/* --------------------------------------------------------------------------
 * 身份（持久化记录）
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t devid;                      /* 设备 ID（＝协议里的 tid） */
    uint8_t  mac[6];                     /* 设备 MAC */
    uint8_t  suite_id;                   /* 加密套件 ID */
    char     server[IV_PROV_SERVER_MAX]; /* 调试服务器地址（ASCII，含 NUL） */
} iv_prov_id_t;

/* 累加校验：逐字节求和（与记录里的 sum 字段同口径）。 */
uint32_t iv_prov_checksum(const uint8_t *buf, size_t len);

/* 原地 XOR 混淆/还原（同一函数即是其逆运算）。 */
void iv_prov_xor(uint8_t *buf, size_t len);

/* 编码成 56 字节**明文**记录（含 sum）。out 容量至少 IV_PROV_REC_LEN。
 * 返回 IV_OK / IV_EINVAL。 */
int iv_prov_id_encode(const iv_prov_id_t *id, uint8_t out[IV_PROV_REC_LEN]);

/* 解析 56 字节明文记录：校验 magic / rec_ver / sum。
 * 返回 IV_OK / IV_EINVAL（参数）/ IV_EPROTO（格式或校验不符）。 */
int iv_prov_id_decode(const uint8_t in[IV_PROV_REC_LEN], iv_prov_id_t *out);

/* 把身份写入裸分区（open+write 4 KiB 块+fsync，整块 XOR 混淆）。
 * 允许重写（不设锁定位）。失败返回 IV_EIO / IV_ENOENT / IV_EINVAL 等。 */
int iv_prov_private_write(const char *dev, const iv_prov_id_t *id);

/* 从裸分区读回身份（读 4 KiB，XOR 还原后解析）。
 * 未写入过（magic 不符）返回 IV_ENOENT。 */
int iv_prov_private_read(const char *dev, iv_prov_id_t *out);

/* --------------------------------------------------------------------------
 * 设备信息（search / update_mac 应答里那组字段）
 * -------------------------------------------------------------------------- */
#define IV_PROV_IP_MAX   24u
#define IV_PROV_SMALL_MAX 32u

typedef struct {
    char tid[IV_PROV_TID_MAX];    /* 设备 ID，6 位大写 HEX */
    char tnam[IV_PROV_TEXT_MAX];  /* 设备名称 */
    char mcu[IV_PROV_TEXT_MAX];   /* MCU 序列号 */
    char mod[IV_PROV_TEXT_MAX];   /* 硬件型号 */
    char sv[IV_PROV_TEXT_MAX];    /* 系统版本 */
    char mac[IV_PROV_MAC_MAX];    /* aa:bb:cc:dd:ee:ff */
    char ip[IV_PROV_IP_MAX];
    char nm[IV_PROV_IP_MAX];
    char gw[IV_PROV_IP_MAX];
    char inip[IV_PROV_TEXT_MAX];  /* 内网服务器地址 */
    char outip[IV_PROV_TEXT_MAX]; /* 外网服务器地址 */
    char iccid[IV_PROV_SMALL_MAX];
    char name[IV_PROV_SMALL_MAX];
    char password[IV_PROV_SMALL_MAX];
} iv_prov_devinfo_t;

/* 从身份记录填 tid/mac，其余字段清空（由调用方按需覆盖；文档允许字段为空串）。 */
void iv_prov_devinfo_from_id(const iv_prov_id_t *id, iv_prov_devinfo_t *out);

/* --------------------------------------------------------------------------
 * 工具函数：tid / mac / date 的字符串形式
 * -------------------------------------------------------------------------- */
/* 0x1234FF → "1234FF"（大写、零填充 6 位）。out 容量至少 IV_PROV_TID_MAX。 */
void iv_prov_tid_from_u32(uint32_t devid, char out[IV_PROV_TID_MAX]);

/*
 * 终端序列号（平台 E3 应答的 `tsn`）：**12 位大写 HEX**，格式对齐参考实现
 * `start_get_device_id_str()` 的 `%04X%04X%04X`（那边取 MCU 的 3×u16 chipid）。
 *
 * 数据源（**运行时事实，不进配置**）：
 *   1) `/sys/class/sunxi_info/sys_info` 里的 `sunxi_chipid : <32 hex>`，取前 12 位；
 *   2) 读不到再退 `/proc/cpuinfo` 的 `Serial`（16 hex），同样取前 12 位；
 *   3) 都读不到 ⇒ 写**空串**。不编造：`tid` 是配置身份，`tsn` 是芯片唯一号，
 *      两者混用会让平台把"同一台设备"认成两台（或反之）。
 *
 * 返回值：1 = 取到，0 = 取不到（out 为空串）。out 容量至少 IV_PROV_TSN_MAX。
 */
int iv_prov_tsn(char out[IV_PROV_TSN_MAX]);

/* "1234FF" → 0x1234FF（大小写皆可；**必须恰 6 位 HEX**）。
 * 返回 IV_OK / IV_EINVAL。 */
int iv_prov_tid_to_u32(const char *tid, uint32_t *out);

/* "aa-bb-cc-dd-ee-ff" / "aa:bb:cc:dd:ee:ff" / "aabbccddeeff" → 6 字节。
 * 返回 IV_OK / IV_EINVAL。 */
int iv_prov_mac_parse(const char *s, uint8_t mac[6]);

/* 6 字节 → "aa:bb:cc:dd:ee:ff"。out 容量至少 IV_PROV_MAC_MAX。 */
void iv_prov_mac_format(const uint8_t mac[6], char out[IV_PROV_MAC_MAX]);

/* 当前时间：with_time=0 → "YYYYMMDD"；=1 → "YYYYMMDDHHMMSS"。out 容量至少
 * IV_PROV_DATE_MAX。取本地时间（板端 RTC/ntp 由 settime 通道负责）。 */
void iv_prov_date_now(char out[IV_PROV_DATE_MAX], int with_time);

/* --------------------------------------------------------------------------
 * 报文解析（容错扫描器；只用在这两条指令上）
 * -------------------------------------------------------------------------- */
typedef struct {
    char tid[IV_PROV_TID_MAX];   /* info.tid（原样，未做合法性判断） */
    char date[IV_PROV_DATE_MAX]; /* info.date */
    char types[IV_PROV_TYPE_MAX];/* data.types */
    char mac[IV_PROV_MAC_MAX];   /* data.mac（**原样**保存，由 iv_prov_decide 负责解析） */
    int  has_mac;                /* data.mac 是否存在（空串也算存在） */
} iv_prov_msg_t;

/* 解析一条报文。返回 IV_OK / IV_EINVAL（参数）/ IV_EPROTO（语法或结构不符：
 * 缺 info/data 对象、types 为空、字符串未闭合、长度超限等）。
 * 容忍：任意空白、键序、**尾逗号**、多余未知键。 */
int iv_prov_msg_parse(const char *json, size_t len, iv_prov_msg_t *out);

/* --------------------------------------------------------------------------
 * 决策（纯逻辑，不做任何 IO —— 单测据此覆盖全部分支）
 * -------------------------------------------------------------------------- */
typedef enum {
    IV_PROV_ACT_NONE = 0,   /* 不认识 / 不是给我的 / 未实现 ⇒ **不回复** */
    IV_PROV_ACT_SEARCH,     /* 回 search 应答 */
    IV_PROV_ACT_UPDATE_MAC  /* 写身份（mac_ok 为真时）＋回 update_mac 应答 */
} iv_prov_act_t;

typedef struct {
    iv_prov_act_t act;
    int           mac_ok;                    /* UPDATE_MAC：1=可写，0=报 fault 且不写 */
    char          new_tid[IV_PROV_TID_MAX];  /* 要写入的设备 ID（广播时＝本机 ID） */
    uint8_t       new_mac[6];                /* mac_ok 为真时有效 */
} iv_prov_plan_t;

/* 决定"这条报文该干什么"。own_tid 为本机当前设备 ID（6 位 HEX；可为 NULL/空，
 * 此时只认广播寻址）。返回 IV_OK（*out 已填，可能 act=NONE）/
 * IV_EINVAL（参数）/ IV_EPROTO（不是合法协议报文）。 */
int iv_prov_decide(const char *json, size_t len, const char *own_tid,
                   iv_prov_plan_t *out);

/* --------------------------------------------------------------------------
 * 组包（输出**严格 JSON**：无尾逗号；返回写入的字节数，失败返回 <0）
 * -------------------------------------------------------------------------- */
/* 工装侧：搜索查询（tid=000000）。 */
int iv_prov_build_search_query(char *out, size_t cap);

/* 工装侧：配置设备 ID、MAC（tid 即要写入的新 ID；mac 用 ':' 分隔）。 */
int iv_prov_build_update_mac_query(const char *tid, const char *mac,
                                   char *out, size_t cap);

/* 设备侧：搜索应答（含全部设备信息字段）。 */
int iv_prov_build_search_reply(const iv_prov_devinfo_t *self, char *out, size_t cap);

/* 设备侧：配置应答（ok=1 → "success"，0 → "fault"，后跟设备信息字段）。 */
int iv_prov_build_update_mac_reply(const iv_prov_devinfo_t *self, int ok,
                                   char *out, size_t cap);

/* --------------------------------------------------------------------------
 * 工厂标记
 * -------------------------------------------------------------------------- */

/* 标记文件存在 ⇒ 1；不存在 ⇒ 0（含参数为 NULL、stat 出错）。 */
int iv_prov_marker_present(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_PROVISION_H */
