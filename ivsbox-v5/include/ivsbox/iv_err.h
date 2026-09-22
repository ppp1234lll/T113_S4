#ifndef IVS_ERR_H
#define IVS_ERR_H

/*
 * 统一错误码骨架。
 *
 * 约定：0 = 成功，负值 = 失败。
 *
 * TODO(G12): 完整错误码字典（统一探活失败细分码 TIMEOUT/REFUSED/AUTH_FAIL/
 *            NO_MEDIA/UNREACH + 平台/业务错误码 + IPC 错误码）在 M1 冻结，
 *            并补入架构文档 §18 冻结项。届时本文件与文档需同步。
 */

#define IVS_OK               0
#define IVS_ERR_INVAL       (-1)  /* 参数非法 */
#define IVS_ERR_NOMEM       (-2)  /* 内存不足 */
#define IVS_ERR_TIMEOUT     (-3)  /* 超时（用于有明确截止时间的任务） */
#define IVS_ERR_BUSY        (-4)  /* 有界队列已满，调用方应退避后重试 */
#define IVS_ERR_AGAIN       (-5)  /* 暂时不可用，可立即重试 */
#define IVS_ERR_IO          (-6)
#define IVS_ERR_PROTO       (-7)  /* 协议解析或语义错误 */
#define IVS_ERR_NOTREADY    (-8)  /* 启动就绪分级尚未达成 */
#define IVS_ERR_UNSUPPORTED (-9)
#define IVS_ERR_NOSPC       (-10) /* 存储空间不足 */
#define IVS_ERR_PERM        (-11)
#define IVS_ERR_CANCELED    (-12) /* 任务被取消（慢任务池支持取消标志） */

/* 错误码文本；实现见 src/core/iv_err.c（返回的均为 ASCII 常量串） */
const char *ivs_strerror(int code);

#endif /* IVS_ERR_H */
