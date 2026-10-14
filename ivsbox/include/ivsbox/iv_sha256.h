/*
 * IVSBox SHA-256（libivcore，功能开发计划 M6-S6.4 的"校验值"）
 *
 * ============================ 为什么自己写 ============================
 * S6.4 的验证判据是「Range 请求 206 与全量 sha256 一致」⇒ 需要一个真 sha256。
 * 本工程一贯零第三方依赖（SNMP 客户端、HTTP 服务器都是自写的），而板端 sysroot
 * 里也没有现成可链的 crypto 库；sha256 是**纯计算**、无 IO、无平台差异，放 core
 * 层（只允许 libc）正合适，也顺带给单测一个标准向量可核实的靶子。
 *
 * 用途仅限"完整性校验值"，**不是加密** —— 不涉密钥、不涉 MAC，别往安全语义上靠。
 *
 * ============================ 用法 ============================
 *   - 流式：`init` → 多次 `update` → `final`（适合大文件/网络分片）；
 *   - 文件全量：`iv_sha256_file()`（内部 16 KiB 分块读，**不把整个文件读进内存**）；
 *   - 十六进制：`iv_sha256_hex()`（小写，长度恒 64）。
 *
 * 标准向量（单测钉住）：
 *   ""    → e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 *   "abc" → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 */
#ifndef IVSBOX_IV_SHA256_H
#define IVSBOX_IV_SHA256_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_ret.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IV_SHA256_DIGEST_LEN 32u
#define IV_SHA256_HEX_LEN    64u

typedef struct {
    uint32_t h[8];
    uint64_t total; /* 已喂入字节数 */
    uint8_t  buf[64];
    size_t   buflen;
} iv_sha256_t;

void iv_sha256_init(iv_sha256_t *c);
void iv_sha256_update(iv_sha256_t *c, const void *data, size_t len);
void iv_sha256_final(iv_sha256_t *c, uint8_t out[IV_SHA256_DIGEST_LEN]);

/* 文件全量摘要。返回 IV_OK / IV_EINVAL（参数）/ IV_EIO（打不开或读失败）。 */
int iv_sha256_file(const char *path, uint8_t out[IV_SHA256_DIGEST_LEN]);

/* 摘要 → 小写十六进制。cap 不足 65 时写出能装下的部分（永不越界）。 */
void iv_sha256_hex(const uint8_t digest[IV_SHA256_DIGEST_LEN], char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_SHA256_H */
