============================================================
src/modules/link  ——  采集板 UART 链路：成帧 + 命令级可靠收发
============================================================

【目录作用】
  采集板（单片机）与主板间的 UART 链路处理，分两层：
  `iv_frame.c` 负责把字节流切成完整帧并把 cmd+data 编成帧（M2-S2.2）；
  `iv_link.c` 在其上做命令级可靠收发、应答超时重传、链路 UP/DOWN 判定（M2-S2.3）。
  属模块层（`libivmodules.a`），依赖 `libivcore`（iv_crc / iv_ret）。
  **不读也不写串口**：要发的字节经 `on_tx` 回调交出，收到的字节由调用方喂入。

【文件说明】
  iv_frame.c    成帧/编码：同步、半包、粘包、噪声重同步、长度上限、CRC8 校验、
                帧尾校验、帧超时复位；并把 cmd+data 编成完整帧字节
  iv_link.c     命令级可靠层：下发→同 cmd 应答配对、超时重传、无配对帧上交、
                链路状态维护（对接 S2.6 状态镜像的重建）
  iv_s2_uart.c  串口装配：Reactor 驱动收发、E1 查询、断线清镜像及重连；待 main.c 接入
  .gitkeep      目录占位

【实现状态】
  已实现（功能开发计划 M2-S2.2、M2-S2.3）。

【约定与注意事项】
  - 帧规格（架构 §18.2，2026-10-08 冻结，来源《单片机通信 20260427.xlsx》）：
    `head(2) | cmd(1) | len(2) | data(len) | crc8(1) | end(2)`；下行头 `0xF0F0`、
    上行头 `0x0F0F`、帧尾 `0xFFFF`；`crc8` = CRC-8/SMBUS（poly 0x07/init 0x00）
    覆盖 **cmd+len+data**；`len` 为 data 字节数，**小端**（2026-10-08 由现网源码更正）。
  - **零 malloc**：`iv_framer_t` / `iv_link_t` 由调用方持有（`iv_link_t` 约 5.5 KB）。
  - 帧视图 `iv_frame_t.data` 指向解析器内部缓冲，**只在回调内有效**，需留存须拷贝。
  - 超长帧与坏帧的长度域不可信 ⇒ 一律回到同步字搜索，不做精确跳过；
    `gap_ms = 0` 关闭帧超时机制。
  - **单写者、内部无锁**：全部接口只在装配层一个线程内调用（与 Reactor 一致）；
    `on_done` 在事务槽**摘除之后**调用，回调里再 `send()` / `clear()` 不会踩正在用
    的槽（与 iv_taskpool "on_done 锁外调用"同一纪律）。
  - 配对规则＝同 cmd 在飞事务唯一（同 cmd 第二笔返回 `IV_EBUSY`）；协议无请求号/
    ACK/事务号，D1/D2 控制动作不自动重传，仍需应用层确认结果；**不做内容去重**。
  - 返回码（iv_ret.h）：`iv_link_send` → `IV_OK`/`IV_EINVAL`/`IV_ERANGE`/`IV_EBUSY`/
    `IV_EFULL` 或发送回调错误；`on_done` 的 rc → `IV_OK`/`IV_ETIMEDOUT`（重传耗尽）/
    `IV_ECANCELED`/发送回调错误；
    `iv_frame_build` → `IV_OK`(>0 字节数)/`IV_EINVAL`/`IV_ERANGE`/`IV_ENOSPC`。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S2.2、§S2.3
  T113运维终端-系统架构设计.md：§18.2（采集板 UART 帧协议，已冻结）
