/*
 * IVSBox 版本信息（开发计划 M1-S2 第 4 件）
 *
 * 语义版本号来自 ivsbox/ivsbox.h 的 IVSBOX_VERSION_*；
 * Git 短哈希由顶层 Makefile 在编译期注入 -DIV_GIT_VER="<short-sha>"。
 * 未注入时（例如手工 gcc 编译、或构建目录脱离 .git 的离线环境）退回
 * "unknown"，不会导致编译失败——降级由 iv_version_git_valid() 显式报告。
 */
#ifndef IVSBOX_IV_VERSION_H
#define IVSBOX_IV_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 完整版本串，形如 "1.0.0+g1e2d781"；未注入哈希时为 "1.0.0+unknown"。
 * 返回值指向进程内静态缓冲，生命周期同进程，调用方不得修改或释放。
 */
const char *iv_version_string(void);

/*
 * 编译期注入的 Git 短哈希**原值**：未注入时为实现里的默认值 "unknown"。
 * 这里刻意不做归一化——构建系统若注入了非法值（空串、非十六进制），原样返回，
 * 由 iv_version_git_valid() 报告可用性，iv_version_string() 负责降级显示。
 */
const char *iv_version_git(void);

/*
 * 注入的哈希是否可用：1 = 已注入合法短哈希，0 = 未注入或值非法。
 * 供上报 / OTA 等场景判断"该不该带上版本哈希"。
 */
int iv_version_git_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_VERSION_H */
