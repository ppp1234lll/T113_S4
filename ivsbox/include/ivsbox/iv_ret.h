/*
 * IVSBox 本地 API 返回码
 *
 * 与 `iv_err.h` 的跨端故障码**是两套东西，严禁混用**：
 *   - 本文件：**本地调用约定**。有符号负数，只在本进程内按返回值传递，不上报、不写库、
 *     不出网口、不进 UART 帧，因此不受"与单片机侧对齐"约束，可自由增删。
 *   - `iv_err.h`：跨端故障码。uint32，与单片机工程共用码空间，用于告警与上报。
 *
 * 分法照抄单片机工程：它同样把 `ELEC_*`（uint32 故障码）与 `UPLOAD_HTTP_ERR_*` /
 * `LOG_ERR_*`（int8 负值本地码）分成互不相干的两套。
 *
 * 编号约定：0 = 成功；负数 = 失败，按用途分段并各留空间，便于扩充而不动已有值。
 *   -1  ~ -19   通用（参数、状态、资源）
 *   -21 ~ -39   网络 / 协议
 *   -41 ~ -59   存储 / 文件
 * 新增码**只能在所属段末尾追加**，不得改动已发布的值。
 */
#ifndef IVSBOX_IV_RET_H
#define IVSBOX_IV_RET_H

/* 成功 */
#define IV_OK ((int)0)

/* 通用 -1 ~ -19 */
#define IV_EFAIL     ((int)-1)  /* 未分类失败 */
#define IV_EINVAL    ((int)-2)  /* 参数非法 */
#define IV_EBUSY     ((int)-3)  /* 资源忙 / 队列已满，调用方应稍后重试 */
#define IV_EAGAIN    ((int)-4)  /* 暂时无数据或需重试 */
#define IV_ETIMEDOUT ((int)-5)  /* 超时 */
#define IV_ENOMEM    ((int)-6)  /* 内存不足 */
#define IV_ESTATE    ((int)-7)  /* 状态机当前状态不允许该操作 */
#define IV_ENOTSUP   ((int)-8)  /* 未实现 / 不支持 */
#define IV_EEXIST    ((int)-9)  /* 已存在 */
#define IV_ENOENT    ((int)-10) /* 不存在 */
#define IV_ERANGE    ((int)-11) /* 取值越界 */
#define IV_ECANCELED ((int)-12) /* 已取消 */
#define IV_EFULL     ((int)-13) /* 容量已满，且不打算等待 */

/* 网络 / 协议 -21 ~ -39 */
#define IV_EPROTO ((int)-21) /* 报文格式 / 校验不符 */
#define IV_ECONN  ((int)-22) /* 连接失败或已断开 */
#define IV_EAUTH  ((int)-23) /* 鉴权失败 */

/* 存储 / 文件 -41 ~ -59 */
#define IV_ENOSPC   ((int)-41) /* 空间不足 */
#define IV_EIO      ((int)-42) /* 读写错误 */
#define IV_ECORRUPT ((int)-43) /* 内容损坏 */

#endif /* IVSBOX_IV_RET_H */
