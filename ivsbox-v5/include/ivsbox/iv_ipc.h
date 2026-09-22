#ifndef IVS_IPC_H
#define IVS_IPC_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 进程间通信（架构文档 §5）
 *
 *  传输层     : Unix Domain SOCK_SEQPACKET（传输层保留消息边界，无需流式组帧）
 *  身份校验   : SO_PEERCRED + socket 权限 0660
 *  载荷       : 控制与查询用 JSON；媒体内部高频状态可用版本化定长结构
 *  长任务     : 立即返回 task_id，进度与结果经事件通知或查询接口获取
 *
 * 消息头与架构文档 §5.1 一致。
 *
 * 线格式注意（实现时必读）
 *   ivs_ipc_hdr_t 按自然对齐存在 4 字节空洞与尾部填充，实际 sizeof 为 32 而非 28：
 *     offset 0  magic        4B
 *     offset 4  version      2B
 *     offset 6  type         2B
 *     offset 8  flags        4B
 *     offset 12 (4B 空洞，因 request_id 需 8 字节对齐)
 *     offset 16 request_id   8B
 *     offset 24 payload_len  4B
 *     offset 28 reserved     4B
 *     sizeof = 32
 *   因此**不得对结构体直接整体 memcpy 到 socket**，须按字段显式序列化；
 *   若确需 28B 紧凑头，必须显式指定字节序与小端编码，不依赖编译器 packing。
 *   本头文件用 _Static_assert 把该布局钉死，任何平台上的偏移变化都会在编译期报错。
 */

#define IVS_IPC_MAGIC 0x42535649u /* 小端字节序下依次为 'I' 'V' 'S' 'B' */

#define IVS_IPC_TYPE_REQ    1u /* 请求 */
#define IVS_IPC_TYPE_RESP   2u /* 响应 */
#define IVS_IPC_TYPE_EVENT  3u /* 事件通知 */
#define IVS_IPC_TYPE_CANCEL 4u /* 取消（配合长任务取消标志） */

#define IVS_IPC_FLAG_MORE     0x0001u /* 分片：后续还有分片 */
#define IVS_IPC_FLAG_URGENT   0x0002u /* 控制类高优先级，不得与媒体共享队列 */

/* 单包最大长度上限；超限数据经文件句柄、任务 ID 或分片协议传递。
 * TODO(G4): Web 下载媒体文件走哪条路径（SCM_RIGHTS 传 fd / 只读白名单目录 /
 *           反代到 media 本地 HTTP）尚未定，定后在此补充相应接口。 */
#define IVS_IPC_MAX_PAYLOAD (64u * 1024u)

typedef struct {
    uint32_t magic;      /* 必须等于 IVS_IPC_MAGIC */
    uint16_t version;    /* 必须等于 IVS_IPC_PROTO_VERSION */
    uint16_t type;       /* IVS_IPC_TYPE_* */
    uint32_t flags;      /* IVS_IPC_FLAG_* */
    uint64_t request_id; /* 请求响应关联号；事件中可为 0 */
    uint32_t payload_len;
    uint32_t reserved;   /* 必须置 0 */
} ivs_ipc_hdr_t;

_Static_assert(sizeof(ivs_ipc_hdr_t) == 32, "ivs_ipc_hdr_t 布局变化，需重新冻结线格式");
_Static_assert(offsetof(ivs_ipc_hdr_t, version) == 4, "ivs_ipc_hdr_t.version 偏移变化");
_Static_assert(offsetof(ivs_ipc_hdr_t, flags) == 8, "ivs_ipc_hdr_t.flags 偏移变化");
_Static_assert(offsetof(ivs_ipc_hdr_t, request_id) == 16, "ivs_ipc_hdr_t.request_id 偏移变化");
_Static_assert(offsetof(ivs_ipc_hdr_t, payload_len) == 24, "ivs_ipc_hdr_t.payload_len 偏移变化");

/* ---------------------------------------------------------------------------
 * IPC 服务边界（架构文档 §5.2）
 *   ivsboxd     : system.snapshot / camera.list / camera.probe / lock.open /
 *                 config.apply / ota.begin
 *   ivsbox-media: record.start / record.stop / snapshot.take / media.list /
 *                 audio.play / audio.stop
 *   ivsbox-web  : 不向业务进程提供内部控制服务，只提供受鉴权的 HTTP 接口
 *
 * TODO(G12): 完整方法 ID 表与错误码字典在 M1 冻结后落到本文件。
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* IVS_IPC_H */
