/*
 * 平台协议层（libivmodules，功能开发计划 M2-S2.4）
 *
 * ============================ 它是什么 ============================
 * 与云平台的私有协议（依据《指令-通用版 20260623.xlsx》＋参考实现
 * General_Version/main/APP/User/src/com.c，2026-10-09 冻结口径）：
 *
 *   ── 二进制控制/查询帧（ACK 型，双向）──
 *     下行(平台→设备): F0 F0 | 11 | TYPE:2 | ID:3 | CMD | QN1:4 | QN2:4 |
 *                      LEN:1 | DATA:N | CRC8 | FF FF
 *     上行(设备→平台): 0F 0F | 11 | TYPE:2 | ID:3 | CMD | QN1:4 | QN2:4 |
 *                      LEN:1 | DATA:N | CRC8 | FF FF
 *     - CRC8 = iv_crc8(version 起到 DATA 末尾，seed 0)，与 S3 同一实现；
 *     - QN 请求标识码：平台下发的 17~19 位十进制时间戳按十进制值拆成
 *       高/低两个 32 位字（QN1 高位字、QN2 低位字，大端字节序在线路上）；
 *     - 响应帧 CMD = 原命令，DATA[0] = 0x01 成功 / 0x70~0x7F 错误码。
 *
 *   ── ## 周期上报帧（设备→平台）──
 *     ## + 4 位 ASCII 十进制长度 + 数据段 + 2 位 ASCII 十六进制 CRC8 + ##
 *     - 长度＝**整段**字符数（4 位前导零，含 `QN=..;…;CP=` 前缀）；
 *     - CRC8 **只覆盖数据区**，即数据段内**第一个 `&&` 起到段尾**的那一段
 *       （`&&K=V;K=V;…&&`），与 MCU 的 calc_crc8 同参；**不含** `QN=..;CP=` 前缀。
 *       依据：指令表《正常上报》文案 + 参考实现 `com.c` 的
 *       `p = strstr(data,"&&"); crc = calc_crc8(p, strlen(p));`（2026-10-10 核对）。
 *       数据段内**必须**含 `&&`，否则本层返回 `IV_EINVAL`（缺数据区是畸形段，
 *       发出去平台必拒——宁可失败不发）。
 *     - 数据段形态：QN=0;TID=..;VER=..;DEVTYPE=..;CP=&&K=V;K=V;...&&
 *
 *   ── ## 查询响应帧（设备→平台，**与上报帧不同壳**）──
 *     ## + JSON + ##
 *     - **无 4 位长度域、无尾 2 位 CRC**（依据《指令-通用版》"查询指令" sheet
 *       的多个示例，如 `##{"code":0,"qn":"…","data":{…}}##`）；
 *     - 其内部 `crc` 字段是 **JSON 的一个键**，由装配层按业务公式生成，
 *       本层不解析内容、只负责套 `##` 壳。
 *
 * ============================ 职责边界（用户 2026-10-09 拍板） ============================
 * **只做协议，不做传输**：TCP 连接、断线重连、出口选择全部归 M3 网络管理。
 * 本层通过 `iv_proto_transport_t` 回调与传输层解耦：
 *   - on_tx: 本层要发出的字节（传输层负责写 socket）；
 *   - iv_proto_recv(): 传输层把收到的字节喂进来。
 * 单测用构造字节流驱动，零 socket。
 *
 * ============================ 并发与生命周期 ============================
 *   - 单写者：全部接口只在装配层（Reactor）单线程内调用，内部无锁；
 *   - 回调内 data 指向内部暂存缓冲，**回调返回后失效**，需留存须拷贝；
 *   - 零 malloc：iv_proto_t 由调用方持有（约 2 KB，主要为组包/暂存缓冲）。
 *
 * ============================ 返回码（iv_ret.h） ============================
 *   IV_OK / IV_EINVAL（参数）/ IV_ERANGE（缓冲不足）/ IV_EAGAIN（帧不完整，
 *   需要更多字节）/ IV_EPROTO（CRC 或格式错，字节已丢弃并计数）。
 */
#ifndef IVSBOX_IV_PROTO_H
#define IVSBOX_IV_PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 协议常量（《指令-通用版 20260623.xlsx》冻结口径）
 * ------------------------------------------------------------------------- */
#define IV_PROTO_VER          0x11u  /* 数据版本 */
#define IV_PROTO_HEAD_DOWN    0xF0F0u /* 平台→设备 帧头 */
#define IV_PROTO_HEAD_UP      0x0F0Fu /* 设备→平台 帧头 */
#define IV_PROTO_TAIL         0xFFFFu /* 帧尾 */

/* 二进制帧固定部分长度（头2+ver1+type2+id3+cmd1+qn8+len1+crc1+tail2） */
#define IV_PROTO_BIN_FIXED    21u
/* ## 上报帧固定部分长度（## + 4 位长度 + 2 位 CRC + ##） */
#define IV_PROTO_TXT_FIXED    8u
/* ## 查询响应帧固定部分长度（## + … + ##；**无**长度域与尾 CRC） */
#define IV_PROTO_JSON_FIXED   4u

/* 二进制帧 DATA 上限。LEN 字段 1 字节（0~255）；255 同时留足心跳/ACK 空间。 */
#define IV_PROTO_BIN_DATA_MAX 255u
/* ## 帧数据段上限：参考实现上报样例约 415 字符，取 1 KiB 整数上限。 */
#define IV_PROTO_TXT_DATA_MAX 1024u

/* 默认设备类型（参考 General_Version/main 的 DEVICE_TYPE；配置可覆盖） */
#define IV_PROTO_DEVTYPE_DEFAULT 0x0400u

/* ---------------------------------------------------------------------------
 * 命令字（《指令-通用版 20260623.xlsx》"指令和错误字典"）
 * ------------------------------------------------------------------------- */
/* 查询 */
#define IV_PROTO_CMD_QUERY_CFG      0xE1u /* 查询设备当前参数设置 */
#define IV_PROTO_CMD_QUERY_INFO     0xE2u /* 立即上报设备状态 */
#define IV_PROTO_CMD_QUERY_VERSION  0xE3u /* 查询设备软件版本号 */
#define IV_PROTO_CMD_QUERY_IPC_IP   0xE4u /* 搜索摄像机信息 */
#define IV_PROTO_CMD_QUERY_SNMP     0xE7u /* 查询 SNMP 信息 */
#define IV_PROTO_CMD_QUERY_PING     0xE8u /* 查询 PING 信息 */
#define IV_PROTO_CMD_QUERY_TIME     0xE9u /* 查询设备配时 */
#define IV_PROTO_CMD_QUERY_ELEC     0xEAu /* 查询设备电流 */
#define IV_PROTO_CMD_QUERY_DEV_CFG  0xEBu /* 查询设备配置 */
/* 配置 */
#define IV_PROTO_CMD_CFG_SERVER     0xF1u /* 配置服务器 IP/域名、端口 */
#define IV_PROTO_CMD_CFG_PING_INTV  0xF2u /* 配置 PING 间隔 */
#define IV_PROTO_CMD_CFG_MODE       0xF3u /* 配置传输模式 */
#define IV_PROTO_CMD_CFG_LOCAL_NET  0xF4u /* 配置设备 IP、网关 */
#define IV_PROTO_CMD_CFG_REPORT_INTV 0xF5u /* 配置定时上报间隔 */
#define IV_PROTO_CMD_CFG_FAN_TEMP   0xF6u /* 配置风扇温度阈值 */
#define IV_PROTO_CMD_CFG_CAMERA     0xF7u /* 配置摄像机 IP 及连接位 */
#define IV_PROTO_CMD_CFG_MAIN_NET   0xF8u /* 配置主网检测 IP */
#define IV_PROTO_CMD_CFG_HUMI       0xF9u /* 配置湿度阈值 */
#define IV_PROTO_CMD_CFG_TEMP       0xFAu /* 配置温度阈值 */
#define IV_PROTO_CMD_CFG_NET_DELAY  0xFEu /* 配置网络延时判断时间 */
#define IV_PROTO_CMD_CFG_ONVIF_ACC  0xA2u /* 配置摄像机 ONVIF 账号 */
#define IV_PROTO_CMD_CFG_DEV_NAME   0xA3u /* 设置设备名称 */
#define IV_PROTO_CMD_CFG_THRESHOLD  0xA4u /* 配置阈值 */
#define IV_PROTO_CMD_CFG_SEARCH     0xA5u /* 配置搜索方式 */
#define IV_PROTO_CMD_CFG_485_FMT    0xA6u /* 配置 485 数据格式 */
#define IV_PROTO_CMD_CFG_485_PASS   0xA7u /* 485 透传下发 */
#define IV_PROTO_CMD_CFG_ELEC_CH    0xA8u /* 配置电流通道 */
#define IV_PROTO_CMD_CFG_VOLT_CH    0xA9u /* 配置电压通道 */
#define IV_PROTO_CMD_CFG_SIG_TH     0xAAu /* 配置信号机电流阈值 */
#define IV_PROTO_CMD_CFG_DOOR_TIME  0xACu /* 配置箱门工作时段 */
#define IV_PROTO_CMD_CFG_NET_PWR    0xADu /* 配置重启网络传输设备电源 */
#define IV_PROTO_CMD_CFG_SNMP_OID   0xBEu /* 配置 SNMP 查询指令 */
/* 控制 */
#define IV_PROTO_CMD_CTL_REBOOT     0xD9u /* 设备重启 */
#define IV_PROTO_CMD_CTL_CLR_CFG    0xD2u /* 清除配置 */
#define IV_PROTO_CMD_CTL_FAN        0xC1u /* 控制风扇启停 */
#define IV_PROTO_CMD_CTL_OUT_PWR    0xC4u /* 控制输出电压 */
/* 更新与心跳 */
#define IV_PROTO_CMD_UPDATE         0xB3u /* 更新系统 */
#define IV_PROTO_CMD_UPLOAD_LOG     0xB2u /* 日志上报 */
#define IV_PROTO_CMD_HEARTBEAT      0xFFu /* 心跳包（设备→平台） */

/* 通用错误码（响应 DATA[0]；0x01=成功，其余见下） */
#define IV_PROTO_ACK_OK             0x01u
#define IV_PROTO_ERR_DEVID          0x70u /* 设备编号错误 */
#define IV_PROTO_ERR_CRC            0x71u /* 校验错误 */
#define IV_PROTO_ERR_HEAD           0x72u /* 数据头错误 */
#define IV_PROTO_ERR_TAIL           0x73u /* 数据尾错误 */
#define IV_PROTO_ERR_EXEC           0x74u /* 执行失败（含义随命令） */
#define IV_PROTO_ERR_CFG_MODE       0x75u /* 无法进入配置模式等 */
#define IV_PROTO_ERR_CFG_EXIT       0x76u /* 无法退出配置模式等 */
#define IV_PROTO_ERR_BUSY           0x77u /* 上一条指令执行中 */

/* ---------------------------------------------------------------------------
 * 帧（解析结果 / 组包输入）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t devtype;
    uint32_t devid;      /* 24 位有效 */
    uint8_t  cmd;
    uint32_t qn1;        /* QN 高 32 位（十进制时间戳的高位字） */
    uint32_t qn2;        /* QN 低 32 位 */
    uint16_t len;        /* DATA 长度 */
    const uint8_t *data; /* 指向解析器内部缓冲，回调返回后失效 */
} iv_proto_frame_t;

/* ---------------------------------------------------------------------------
 * 传输抽象（用户 2026-10-09 拍板：TCP 传输归 M3，本层只认回调）
 * ------------------------------------------------------------------------- */

/* 本层要发出的字节（传输层写 socket；返回后缓冲即可复用） */
typedef void (*iv_proto_tx_cb)(const uint8_t *bytes, size_t len, void *arg);

/* ---------------------------------------------------------------------------
 * ## 字符串帧组包辅助
 * ------------------------------------------------------------------------- */

/*
 * 把上报数据段打包成完整 ## 帧：
 *   ## + %04u(整段长度) + <seg> + %02x(crc8(`&&数据区&&`)) + ##
 * CRC8 **只覆盖段内第一个 "&&" 起到段尾**（`&&K=V;…&&`），不含 `QN=..;CP=` 前缀，
 * 与 MCU 的 calc_crc8 同参（seed 0、逐位 SMBUS）。**段内无 "&&" ⇒ IV_EINVAL**。
 * out_len 实际写入字节数（含 ##）。out 为 NULL 或 *out_len 为 0 时只做长度
 * 探测（*out_len 返回所需长度）；缓冲不足返回 IV_ERANGE 且不动 out 内容。
 * seg_len > IV_PROTO_TXT_DATA_MAX ⇒ IV_ERANGE。
 */
int iv_proto_text_frame(const char *seg, size_t seg_len,
                        uint8_t *out, size_t *out_len);

/*
 * 把查询响应 JSON 打包成完整 ## 帧：## + <json> + ##（**无长度域、无尾 CRC**）。
 * json_len > IV_PROTO_TXT_DATA_MAX ⇒ IV_ERANGE；NULL+0 非法组合 ⇒ IV_EINVAL。
 * out 为 NULL 或 *out_len 为 0 时只做长度探测；缓冲不足返回 IV_ERANGE。
 */
int iv_proto_json_frame(const char *json, size_t json_len,
                        uint8_t *out, size_t *out_len);

/* ---------------------------------------------------------------------------
 * 二进制帧组包
 * ------------------------------------------------------------------------- */

/*
 * 组上行二进制帧（设备→平台，帧头 0x0F0F）。
 * data 可为 NULL（len=0）；len > IV_PROTO_BIN_DATA_MAX 返回 IV_ERANGE。
 */
int iv_proto_bin_build(uint8_t *out, size_t out_cap, size_t *out_len,
                       uint16_t devtype, uint32_t devid, uint8_t cmd,
                       uint32_t qn1, uint32_t qn2,
                       const void *data, uint16_t len);

/* ---------------------------------------------------------------------------
 * 解析器（零 malloc：调用方持有，风格同 iv_framer_t）
 * ------------------------------------------------------------------------- */
typedef struct {
    /* 内部组包缓冲（一条完整帧最多 IV_PROTO_BIN_FIXED-2+255 字节） */
    uint8_t  buf[IV_PROTO_BIN_FIXED + IV_PROTO_BIN_DATA_MAX];
    uint16_t pos;      /* 已收字节数 */
    uint16_t head;     /* 帧头滑动窗口 */
    uint16_t tail;     /* 帧尾滑动窗口 */
    uint8_t  want;     /* LEN 字段解析出的 DATA 期望长度 */
    uint8_t  got_head; /* 已锁定帧头 */
} iv_proto_parser_t;

/*
 * 喂收到的字节（来自平台方向的下行帧，帧头 0xF0F0）。
 * 每解出一条完整帧调用一次 on_frame；CRC 错的整帧丢弃并继续同步。
 * on_frame 可为 NULL（只做同步丢弃）；frame->data 指向内部缓冲，回调返回后失效。
 * 半包时字节留在内部缓冲，下次继续。
 */
void iv_proto_parser_init(iv_proto_parser_t *ps);
void iv_proto_parser_feed(iv_proto_parser_t *ps, const uint8_t *buf, size_t len,
                          void (*on_frame)(const iv_proto_frame_t *f, void *arg),
                          void *arg);

/* ---------------------------------------------------------------------------
 * 协议上下文（命令路由 + 心跳/ACK 组包）
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t devtype;
    uint32_t devid;
} iv_proto_id_t;

/* 平台命令处理器：data 指向解析器内部缓冲，回调返回后失效 */
typedef void (*iv_proto_cmd_cb)(const iv_proto_frame_t *f, void *arg);

typedef struct {
    uint8_t          cmd;
    iv_proto_cmd_cb  cb;
    void            *arg;
} iv_proto_route_t;

/* 路由表上限：协议命令字 ~40 个，留 48 余量 */
#define IV_PROTO_ROUTE_MAX 48

typedef struct {
    iv_proto_id_t      id;
    iv_proto_tx_cb     on_tx;   /* 发送出口（M3 传输层） */
    void              *tx_arg;
    iv_proto_parser_t  rx;      /* 内嵌下行解析器 */
    iv_proto_route_t   route[IV_PROTO_ROUTE_MAX];
    uint8_t            n_route;
    /* 最近一次收到的平台 QN（ACK / 查询响应回填用） */
    uint32_t           last_qn1;
    uint32_t           last_qn2;
    /* 统计 */
    uint32_t           rx_frames;
    uint32_t           rx_crc_err;
    uint32_t           tx_frames;
} iv_proto_t;

/* 初始化；on_tx 为 NULL 报 IV_EINVAL（没有出口的协议层无意义） */
int iv_proto_init(iv_proto_t *pf, const iv_proto_id_t *id,
                  iv_proto_tx_cb on_tx, void *tx_arg);

/* 注册命令处理器；cmd 已注册返回 IV_EEXIST；表满返回 IV_EFULL */
int iv_proto_route(iv_proto_t *pf, uint8_t cmd, iv_proto_cmd_cb cb, void *arg);

/*
 * 喂收到的平台字节：解出的每条帧先查路由表，
 *   命中   → 调 handler（handler 自己决定是否回 ACK/数据）；
 *   未命中 → 自动回 ACK：DATA[0]=IV_PROTO_ACK_OK（保守"已收到"语义，
 *            与 MCU 参考实现的 default 分支一致——未知命令也回 0x01）。
 * CRC 错帧静默丢弃并计数（不回错误帧——链路上可能有噪声，回错误帧会放大流量）。
 */
void iv_proto_recv(iv_proto_t *pf, const uint8_t *buf, size_t len);

/* 发一条 ACK（cmd=原命令、qn 取 last_qn1/qn2、code=IV_PROTO_ACK_OK 或错误码） */
int iv_proto_ack(iv_proto_t *pf, uint8_t cmd, uint8_t code);

/* 发心跳（CMD=0xFF、QN=0、DATA={0x01}） */
int iv_proto_heartbeat(iv_proto_t *pf);

/* 发原始二进制帧（组帧 + on_tx；qn 由调用方给，例如查询响应回填平台 QN） */
int iv_proto_send(iv_proto_t *pf, uint8_t cmd, uint32_t qn1, uint32_t qn2,
                  const void *data, uint16_t len);

/* 发 ## 上报帧（组帧 + on_tx；用于周期上报） */
int iv_proto_send_text(iv_proto_t *pf, const char *seg, size_t seg_len);

/* 发 ## 查询响应 JSON 帧（组帧 + on_tx） */
int iv_proto_send_json(iv_proto_t *pf, const char *json, size_t json_len);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_PROTO_H */
