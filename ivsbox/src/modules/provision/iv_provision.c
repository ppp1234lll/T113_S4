/*
 * IVSBox 出厂身份与工装支撑 —— 实现（M5-S5.5）
 * 记录/报文格式与安全边界见 include/ivsbox/iv_provision.h 的文件头。
 *
 * 实现上的两条纪律：
 *   1. **字节序显式小端**：不靠 `memcpy` 到 uint32_t 的"本机恰好是小端"这个
 *      假设 —— 那样换到大端工具链会静默错位。
 *   2. **混淆的边界写清楚**：只混淆 56 字节的记录本身，4 KiB 块里剩余的字节
 *      填 0xFF（擦除态）。整块混淆没有任何额外收益，只会让"读回来先解混淆"
 *      的范围变得含糊。
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"

/* 固定 XOR key（见文件头：只防明文浏览，不是加密） */
static const uint8_t XOR_KEY[4] = { 0x5Au, 0xA5u, 0x3Cu, 0xC3u };

/* --------------------------------------------------------------------------
 * 基础工具
 * -------------------------------------------------------------------------- */
uint32_t iv_prov_checksum(const uint8_t *buf, size_t len)
{
    uint32_t s = 0u;
    size_t   i;

    if (buf == NULL)
        return 0u;
    for (i = 0u; i < len; i++)
        s = (s + (uint32_t)buf[i]) & 0xFFFFFFFFu;
    return s;
}

void iv_prov_xor(uint8_t *buf, size_t len)
{
    size_t i;

    if (buf == NULL)
        return;
    for (i = 0u; i < len; i++)
        buf[i] ^= XOR_KEY[i & 3u];
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 把 server 字段规整进定长缓冲：复制并强制 NUL 结尾。 */
static void copy_server_from_bytes(char *dst, const uint8_t *src)
{
    memcpy(dst, src, IV_PROV_SERVER_MAX);
    dst[IV_PROV_SERVER_MAX - 1u] = '\0';
}

/* --------------------------------------------------------------------------
 * 记录编解码
 * -------------------------------------------------------------------------- */
int iv_prov_id_encode(const iv_prov_id_t *id, uint8_t out[IV_PROV_REC_LEN])
{
    if (id == NULL || out == NULL)
        return IV_EINVAL;

    memset(out, 0, IV_PROV_REC_LEN);
    put_u32le(out + 0, IV_PROV_REC_MAGIC);
    out[4] = (uint8_t)IV_PROV_REC_VER;
    out[5] = id->suite_id;
    /* 6..7 reserved */
    put_u32le(out + 8, id->devid);
    memcpy(out + 12, id->mac, 6u);
    /* 18..19 reserved */
    {
        char srv[IV_PROV_SERVER_MAX];

        memset(srv, 0, sizeof(srv));
        (void)memcpy(srv, id->server, sizeof(srv));
        srv[sizeof(srv) - 1u] = '\0';
        memcpy(out + 20, srv, IV_PROV_SERVER_MAX);
    }
    put_u32le(out + 52, iv_prov_checksum(out, 52u));
    return IV_OK;
}

int iv_prov_id_decode(const uint8_t in[IV_PROV_REC_LEN], iv_prov_id_t *out)
{
    if (in == NULL || out == NULL)
        return IV_EINVAL;
    if (get_u32le(in) != IV_PROV_REC_MAGIC)
        return IV_EPROTO;
    if (in[4] != (uint8_t)IV_PROV_REC_VER)
        return IV_EPROTO;
    if (get_u32le(in + 52) != iv_prov_checksum(in, 52u))
        return IV_EPROTO;

    memset(out, 0, sizeof(*out));
    out->suite_id = in[5];
    out->devid    = get_u32le(in + 8);
    memcpy(out->mac, in + 12, 6u);
    copy_server_from_bytes(out->server, in + 20);
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 组播报文
 * -------------------------------------------------------------------------- */
int iv_prov_pkt_parse(const uint8_t *pkt, size_t len, iv_prov_id_t *out)
{
    if (pkt == NULL || out == NULL)
        return IV_EINVAL;
    if (len != (size_t)IV_PROV_PKT_LEN)
        return IV_EPROTO;
    if (memcmp(pkt, IV_PROV_PKT_MAGIC, 4u) != 0)
        return IV_EPROTO;
    if (pkt[4] != (uint8_t)IV_PROV_PKT_VER)
        return IV_EPROTO;
    if (pkt[5] != (uint8_t)IV_PROV_PKT_TYPE_ID)
        return IV_EPROTO;
    if (get_u32le(pkt + 52) != iv_prov_checksum(pkt, 52u))
        return IV_EPROTO;

    memset(out, 0, sizeof(*out));
    out->devid    = get_u32le(pkt + 8);
    memcpy(out->mac, pkt + 12, 6u);
    out->suite_id = pkt[18];
    copy_server_from_bytes(out->server, pkt + 20);
    return IV_OK;
}

int iv_prov_pkt_build(const iv_prov_id_t *id, uint8_t out[IV_PROV_PKT_LEN])
{
    if (id == NULL || out == NULL)
        return IV_EINVAL;

    memset(out, 0, IV_PROV_PKT_LEN);
    memcpy(out, IV_PROV_PKT_MAGIC, 4u);
    out[4] = (uint8_t)IV_PROV_PKT_VER;
    out[5] = (uint8_t)IV_PROV_PKT_TYPE_ID;
    /* 6..7 reserved */
    put_u32le(out + 8, id->devid);
    memcpy(out + 12, id->mac, 6u);
    out[18] = id->suite_id;
    /* 19 reserved */
    {
        char srv[IV_PROV_SERVER_MAX];

        memset(srv, 0, sizeof(srv));
        (void)memcpy(srv, id->server, sizeof(srv));
        srv[sizeof(srv) - 1u] = '\0';
        memcpy(out + 20, srv, IV_PROV_SERVER_MAX);
    }
    put_u32le(out + 52, iv_prov_checksum(out, 52u));
    return IV_OK;
}

/* --------------------------------------------------------------------------
 * 裸分区读写
 * -------------------------------------------------------------------------- */
int iv_prov_private_write(const char *dev, const iv_prov_id_t *id)
{
    const char *path = (dev != NULL) ? dev : IV_PROV_PRIVATE_DEV_DEFAULT;
    uint8_t     blk[IV_PROV_SECTOR];
    int         fd;
    ssize_t     n;

    if (id == NULL)
        return IV_EINVAL;

    memset(blk, 0xFF, sizeof(blk)); /* 未被记录占用的字节保持擦除态 */
    if (iv_prov_id_encode(id, blk) != IV_OK)
        return IV_EINVAL;
    iv_prov_xor(blk, IV_PROV_REC_LEN);

    fd = open(path, O_WRONLY | O_SYNC);
    if (fd < 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;

    n = write(fd, blk, sizeof(blk));
    if (n != (ssize_t)sizeof(blk)) {
        (void)close(fd);
        return IV_EIO;
    }
    /* 裸分区没有文件系统替我们兜底，必须自己 fsync（计划 §S6 的纪律）。 */
    if (fsync(fd) != 0) {
        (void)close(fd);
        return IV_EIO;
    }
    (void)close(fd);
    return IV_OK;
}

int iv_prov_private_read(const char *dev, iv_prov_id_t *out)
{
    const char *path = (dev != NULL) ? dev : IV_PROV_PRIVATE_DEV_DEFAULT;
    uint8_t     blk[IV_PROV_SECTOR];
    int         fd;
    ssize_t     n;

    if (out == NULL)
        return IV_EINVAL;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return (errno == ENOENT) ? IV_ENOENT : IV_EIO;
    n = read(fd, blk, sizeof(blk));
    (void)close(fd);
    if (n < (ssize_t)IV_PROV_REC_LEN)
        return IV_EIO;

    iv_prov_xor(blk, IV_PROV_REC_LEN);
    /* 解出来不是合法记录 ⇒ 这块分区还没被工厂写过，按"不存在"上报。 */
    return (iv_prov_id_decode(blk, out) == IV_OK) ? IV_OK : IV_ENOENT;
}

/* --------------------------------------------------------------------------
 * 工厂标记
 * -------------------------------------------------------------------------- */
int iv_prov_marker_present(const char *path)
{
    const char  *p = (path != NULL) ? path : IV_PROV_MARKER_DEFAULT;
    struct stat  sb;

    return (stat(p, &sb) == 0) ? 1 : 0;
}
