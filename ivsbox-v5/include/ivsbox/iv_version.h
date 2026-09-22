#ifndef IVS_VERSION_H
#define IVS_VERSION_H

/*
 * 版本与协议版本集中定义。
 *
 * 注意：架构文档 §18 要求编码前冻结五类版本（IPC / DB schema / 配置 schema /
 * 平台协议 / 采集板协议），并给出兼容矩阵 —— 见缺口清单 G11。
 * 本文件先给出其中三类，平台协议与采集板协议版本待接口冻结后补入。
 */

#define IVS_PRODUCT_NAME "Intelligent Video Surveillance Box"
#define IVS_PROJECT_CODE "IVSBox"

#define IVS_VERSION_MAJOR 0
#define IVS_VERSION_MINOR 1
#define IVS_VERSION_PATCH 0

/* 形如 "0.1.0"；构建脚本可注入 IVS_VERSION_BUILD */
#define IVS_STR2(x) #x
#define IVS_STR(x)  IVS_STR2(x)
#define IVS_VERSION_STRING \
    IVS_STR(IVS_VERSION_MAJOR) "." IVS_STR(IVS_VERSION_MINOR) "." IVS_STR(IVS_VERSION_PATCH)

/* IPC 协议版本：三个常驻进程必须一致，不匹配时拒绝连接并告警（不得静默降级） */
#define IVS_IPC_PROTO_VERSION 1u

/* 配置 schema 版本：升级时须支持读取上一版本 */
#define IVS_CONFIG_SCHEMA_VERSION 1u

/* 控制库 schema 版本 */
#define IVS_DB_SCHEMA_VERSION 1u

/* 采集板 UART 协议版本：0 表示仍按现网既有帧字节级兼容，尚未启用协商 */
#define IVS_UART_PROTO_VERSION 0u

#endif /* IVS_VERSION_H */
