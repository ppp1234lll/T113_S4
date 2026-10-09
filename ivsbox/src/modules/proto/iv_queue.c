/*
 * 持久上报队列实现（M3-S3.4）
 *
 * 布局对照 iv_queue.h：
 *   - 段文件：`<dir>/<16位十进制序号>.q`，内容 = ivq_rec_t(24B) + 载荷
 *   - 内存索引：iv_queue_t.idx[] 仅元数据，按 seq 升序
 *   - 落盘：tmp → fsync(file) → rename → fsync(dir)（掉电只会留半截 .tmp，open 清理）
 *
 * 设计依据：include/ivsbox/iv_queue.h 文件头（契约）与架构 §7.3 / §10.3。
 * 零 malloc、单写者、不引 pthread/Reactor。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_queue.h"

/* ---------------------------------------------------------------------------
 * 段文件头 / 命名
 * ------------------------------------------------------------------------- */
#define IVQ_MAGIC 0x31515649u /* 'I''V''Q''1' 小端 */
#define IVQ_EXT   ".q"        /* 正式段文件后缀 */
#define IVQ_TMP   ".tmp"      /* 落盘中转后缀（open 期清理） */

#define IVQ_NAME_LEN 16u /* 序号十进制位数（定长，便于按名解析） */

typedef struct {
    uint32_t magic;
    uint32_t len;  /* 载荷字节数 */
    uint64_t seq;  /* 冗余存一份，便于校验文件名与内容一致 */
    uint8_t  prio;
    uint8_t  rsv[7];
} ivq_rec_t;

_Static_assert(sizeof(ivq_rec_t) == 24u, "ivq_rec_t 必须 24 字节");

/* ---------------------------------------------------------------------------
 * 基础 I/O
 * ------------------------------------------------------------------------- */
static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p   = (uint8_t *)buf;
    size_t   got = 0u;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1; /* 提前 EOF */
        got += (size_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p   = (const uint8_t *)buf;
    size_t         put = 0u;

    while (put < len) {
        ssize_t n = write(fd, p + put, len - put);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        put += (size_t)n;
    }
    return 0;
}

static void fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd >= 0) {
        (void)fsync(fd); /* 让 rename 后的目录项落地 */
        (void)close(fd);
    }
}

/* 递归建目录（与 iv_config/iv_db 同语义：逐级 mkdir，已存在不算错） */
static int mkdir_p(const char *path)
{
    char   buf[IV_QUEUE_PATH_MAX];
    size_t len, i;

    len = strlen(path);
    if (len == 0u || len >= sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1u);
    if (buf[len - 1u] == '/') /* 去掉尾斜杠，便于统一处理 */
        buf[--len] = '\0';

    for (i = 1u; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* out = dir + "/" + name；容量不足则置空（调用方按空串处理）。 */
static void path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t dl = strlen(dir);
    size_t nl = strlen(name);

    if (dl + 1u + nl + 1u > cap) {
        out[0] = '\0';
        return;
    }
    memcpy(out, dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, name, nl + 1u);
}

/* 把 seq 写成定长 16 位十进制（name 至少 17 字节）。 */
static void seq_to_name(char *name, uint64_t seq)
{
    int i;

    for (i = (int)IVQ_NAME_LEN - 1; i >= 0; i--) {
        name[i] = (char)('0' + (int)(seq % 10u));
        seq /= 10u;
    }
    name[IVQ_NAME_LEN] = '\0';
}

/* 段文件全路径（正式名 + 后缀）。 */
static void item_path(char *out, size_t cap, const char *dir, uint64_t seq)
{
    char name[IVQ_NAME_LEN + sizeof(IVQ_EXT)]; /* 16 + 3 */

    seq_to_name(name, seq);
    memcpy(name + IVQ_NAME_LEN, IVQ_EXT, sizeof(IVQ_EXT)); /* 含 '\0' */
    path_join(out, cap, dir, name);
}

/* ".tmp" 残留全路径（正式名 + ".tmp"）。 */
static void tmp_path(char *out, size_t cap, const char *dir, const char *base)
{
    char name[IVQ_NAME_LEN + sizeof(IVQ_TMP)]; /* 16 + 5 */

    memcpy(name, base, IVQ_NAME_LEN);
    memcpy(name + IVQ_NAME_LEN, IVQ_TMP, sizeof(IVQ_TMP));
    path_join(out, cap, dir, name);
}

/* 解析 `<16位数字>.q`；命中返回 1 并回填 seq。 */
static int parse_item_name(const char *nm, uint64_t *seq)
{
    uint64_t v = 0u;
    size_t   i;

    if (strlen(nm) != IVQ_NAME_LEN + 2u)
        return 0;
    for (i = 0; i < IVQ_NAME_LEN; i++) {
        if (nm[i] < '0' || nm[i] > '9')
            return 0;
        v = v * 10u + (uint64_t)(nm[i] - '0');
    }
    if (nm[IVQ_NAME_LEN] != '.' || nm[IVQ_NAME_LEN + 1u] != 'q')
        return 0;
    *seq = v;
    return 1;
}

/* 判断 `<16位数字>.tmp`（落盘半截残留）。 */
static int is_tmp_name(const char *nm)
{
    size_t i;

    if (strlen(nm) != IVQ_NAME_LEN + 4u) /* 16 + ".tmp" */
        return 0;
    for (i = 0; i < IVQ_NAME_LEN; i++)
        if (nm[i] < '0' || nm[i] > '9')
            return 0;
    return memcmp(nm + IVQ_NAME_LEN, IVQ_TMP, 4u) == 0;
}

/* ---------------------------------------------------------------------------
 * 段文件读写
 * ------------------------------------------------------------------------- */

/* 读回一条：校验 magic/prio/长度，且文件尾不得有多余字节。 */
static int read_item(const char *path, ivq_rec_t *rec, uint8_t *payload,
                     size_t cap, size_t *out_len)
{
    int    fd;
    char   extra;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return IV_EIO;
    if (read_all(fd, rec, sizeof(*rec)) != 0) {
        (void)close(fd);
        return IV_ECORRUPT;
    }
    if (rec->magic != IVQ_MAGIC || rec->prio >= (uint8_t)IV_QUEUE_PRIO_N ||
        rec->len == 0u || (size_t)rec->len > cap) {
        (void)close(fd);
        return IV_ECORRUPT;
    }
    if (read_all(fd, payload, rec->len) != 0) {
        (void)close(fd);
        return IV_ECORRUPT;
    }
    if (read(fd, &extra, 1u) > 0) { /* 尾部有多余字节 ⇒ 文件被改写/损坏 */
        (void)close(fd);
        return IV_ECORRUPT;
    }
    (void)close(fd);
    if (out_len != NULL)
        *out_len = (size_t)rec->len;
    return IV_OK;
}

/* 原子落盘一条：tmp → write+fsync → rename → fsync(dir)。 */
static int write_item(const iv_queue_t *q, uint64_t seq, uint8_t prio,
                      const void *data, uint32_t len)
{
    ivq_rec_t rec;
    char      fin[IV_QUEUE_PATH_MAX + 32];
    char      tmp[IV_QUEUE_PATH_MAX + 32];
    int       fd;

    item_path(fin, sizeof(fin), q->dir, seq);
    if (fin[0] == '\0')
        return IV_EIO;
    {
        char base[IVQ_NAME_LEN + 1];
        seq_to_name(base, seq);
        tmp_path(tmp, sizeof(tmp), q->dir, base);
    }

    memset(&rec, 0, sizeof(rec));
    rec.magic = IVQ_MAGIC;
    rec.len   = len;
    rec.seq   = seq;
    rec.prio  = prio;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return IV_EIO;
    if (write_all(fd, &rec, sizeof(rec)) != 0 || write_all(fd, data, len) != 0 ||
        fsync(fd) != 0) {
        (void)close(fd);
        (void)unlink(tmp);
        return IV_EIO;
    }
    if (close(fd) != 0) {
        (void)unlink(tmp);
        return IV_EIO;
    }
    if (rename(tmp, fin) != 0) {
        (void)unlink(tmp);
        return IV_EIO;
    }
    fsync_dir(q->dir);
    return IV_OK;
}

static void item_unlink(const iv_queue_t *q, uint64_t seq)
{
    char p[IV_QUEUE_PATH_MAX + 32];

    item_path(p, sizeof(p), q->dir, seq);
    if (p[0] != '\0')
        (void)unlink(p);
}

/* ---------------------------------------------------------------------------
 * 内存索引（按 seq 升序）
 * ------------------------------------------------------------------------- */
static int idx_find(const iv_queue_t *q, uint64_t seq)
{
    int i;

    for (i = 0; i < (int)q->n; i++)
        if (q->idx[i].seq == seq)
            return i;
    return -1;
}

static int idx_insert(iv_queue_t *q, uint64_t seq, uint32_t len, uint8_t prio)
{
    int i;

    /* 这里只挡**内存硬上限**；条目上限（max_items）由 make_room/trim 按优先级裁，
     * 不能让 scan 的读目录顺序决定丢谁（重要条目会被随机删掉）。 */
    if (q->n >= (uint16_t)IV_QUEUE_ITEMS_MAX)
        return IV_EFULL;
    /* 运行期 push 的 seq 单调递增，通常零移动；open 恢复期才真正排序。 */
    i = (int)q->n;
    while (i > 0 && q->idx[i - 1].seq > seq) {
        q->idx[i] = q->idx[i - 1];
        i--;
    }
    q->idx[i].seq  = seq;
    q->idx[i].len  = len;
    q->idx[i].prio = prio;
    q->n++;
    q->bytes += len;
    return IV_OK;
}

static void idx_remove(iv_queue_t *q, int pos)
{
    q->bytes -= q->idx[pos].len;
    q->n--;
    if (pos < (int)q->n)
        memmove(&q->idx[pos], &q->idx[pos + 1],
                (size_t)(q->n - pos) * sizeof(q->idx[0]));
}

/* 最低优先级条目下标：优先级数值最大者；同级取最老（idx 升序 => 首个命中）。
 * 队列空返回 -1。 */
static int victim_pos(const iv_queue_t *q)
{
    int     i, best = -1;
    uint8_t bp = 0u;

    for (i = 0; i < (int)q->n; i++) {
        if (best < 0 || q->idx[i].prio > bp) {
            best = i;
            bp   = q->idx[i].prio;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * 容量管理
 * ------------------------------------------------------------------------- */

/* 为新条目腾位。返回 IV_OK（已腾好）/ IV_EFULL（库内最低级都比新条目重要）。 */
static int make_room(iv_queue_t *q, uint32_t add_len, uint8_t new_prio)
{
    for (;;) {
        int vp;

        if (q->n < q->max_items && q->bytes + add_len <= q->max_bytes)
            return IV_OK;
        vp = victim_pos(q);
        if (vp < 0)
            return IV_OK; /* 空队列仍"满"只可能是单条超限，push 入口已挡 */
        if (q->idx[vp].prio < new_prio)
            return IV_EFULL; /* 连最低级都比新条目重要 ⇒ 丢新条目 */
        item_unlink(q, q->idx[vp].seq);
        q->dropped[q->idx[vp].prio]++;
        idx_remove(q, vp);
    }
}

/* 恢复后按当前上限收敛（上限被调小时用），丢最低级。 */
static void trim(iv_queue_t *q)
{
    for (;;) {
        int vp;

        if (q->n <= q->max_items && q->bytes <= q->max_bytes)
            return;
        vp = victim_pos(q);
        if (vp < 0)
            return;
        item_unlink(q, q->idx[vp].seq);
        q->dropped[q->idx[vp].prio]++;
        idx_remove(q, vp);
    }
}

/* 扫描目录重建索引：删 .tmp 残留、丢损坏段文件并计数。 */
static void scan(iv_queue_t *q)
{
    DIR           *d = opendir(q->dir);
    struct dirent *de;

    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        uint64_t  seq;
        char      p[IV_QUEUE_PATH_MAX + 32];
        ivq_rec_t rec;
        size_t    len = 0u;

        if (is_tmp_name(de->d_name)) {
            char base[IVQ_NAME_LEN + 1];

            memcpy(base, de->d_name, IVQ_NAME_LEN);
            base[IVQ_NAME_LEN] = '\0';
            tmp_path(p, sizeof(p), q->dir, base);
            if (p[0] != '\0')
                (void)unlink(p); /* 半截落盘残留，删 */
            continue;
        }
        if (!parse_item_name(de->d_name, &seq))
            continue; /* 非本队列文件，不碰 */
        item_path(p, sizeof(p), q->dir, seq);
        if (p[0] == '\0')
            continue;
        if (read_item(p, &rec, q->rbuf, sizeof(q->rbuf), &len) != IV_OK ||
            rec.seq != seq) {
            (void)unlink(p);
            q->corrupt++;
            continue;
        }
        if (idx_insert(q, seq, (uint32_t)len, rec.prio) != IV_OK) {
            (void)unlink(p); /* 索引满：多余的历史条目删盘并计数 */
            q->corrupt++;
        }
    }
    (void)closedir(d);
}

/* ---------------------------------------------------------------------------
 * 公开接口
 * ------------------------------------------------------------------------- */
void iv_queue_cfg_default(iv_queue_cfg_t *c)
{
    if (c == NULL)
        return;
    c->dir       = IV_QUEUE_DIR_DEFAULT;
    c->max_bytes = IV_QUEUE_BYTES_DEFAULT;
    c->max_items = IV_QUEUE_ITEMS_DEFAULT;
}

void iv_queue_init(iv_queue_t *q)
{
    if (q == NULL)
        return;
    memset(q, 0, sizeof(*q));
    q->opened = 0u;
}

int iv_queue_open(iv_queue_t *q, const iv_queue_cfg_t *c)
{
    const char *dir;
    size_t      dl;

    if (q == NULL)
        return IV_EINVAL;
    if (q->opened)
        return IV_ESTATE; /* 未 close 不得重开 */

    dir = (c != NULL && c->dir != NULL) ? c->dir : IV_QUEUE_DIR_DEFAULT;
    dl  = strlen(dir);
    if (dl == 0u || dl >= sizeof(q->dir))
        return IV_ERANGE;

    iv_queue_init(q); /* 清盘上无关状态 */
    memcpy(q->dir, dir, dl + 1u);

    q->max_bytes = (c != NULL && c->max_bytes != 0u) ? c->max_bytes
                                                     : (uint32_t)IV_QUEUE_BYTES_DEFAULT;
    q->max_items = (c != NULL && c->max_items != 0u) ? c->max_items
                                                     : (uint16_t)IV_QUEUE_ITEMS_DEFAULT;
    if (q->max_items > (uint16_t)IV_QUEUE_ITEMS_MAX)
        q->max_items = (uint16_t)IV_QUEUE_ITEMS_MAX;
    if (q->max_bytes < (uint32_t)IV_QUEUE_ITEM_MAX)
        q->max_bytes = (uint32_t)IV_QUEUE_ITEM_MAX; /* 至少容得下一条最大条目 */

    if (mkdir_p(q->dir) != 0)
        return IV_EIO;

    scan(q);
    trim(q);

    q->next_seq = (q->n > 0u) ? (q->idx[q->n - 1u].seq + 1u) : 1u;
    q->opened   = 1u;
    return IV_OK;
}

int iv_queue_close(iv_queue_t *q)
{
    if (q == NULL || !q->opened)
        return IV_ESTATE;
    q->opened = 0u;
    q->n      = 0u;
    q->bytes  = 0u;
    return IV_OK;
}

int iv_queue_push(iv_queue_t *q, uint8_t prio, const void *data, size_t len,
                  uint64_t *seq_out)
{
    uint64_t seq;
    int      rc;

    if (q == NULL || data == NULL || seq_out == NULL)
        return IV_EINVAL;
    if (prio >= (uint8_t)IV_QUEUE_PRIO_N)
        return IV_EINVAL;
    if (len == 0u || len > (size_t)IV_QUEUE_ITEM_MAX)
        return IV_ERANGE;
    if (!q->opened)
        return IV_ESTATE;
    if ((uint64_t)len > (uint64_t)q->max_bytes)
        return IV_ERANGE; /* 单条就超总容量，永远放不下 */

    rc = make_room(q, (uint32_t)len, prio);
    if (rc != IV_OK) {
        q->dropped[prio]++; /* 丢的是新条目 */
        return rc;          /* IV_EFULL */
    }

    seq = q->next_seq;
    rc  = write_item(q, seq, prio, data, len);
    if (rc != IV_OK)
        return rc; /* IV_EIO */

    rc = idx_insert(q, seq, (uint32_t)len, prio);
    if (rc != IV_OK) {
        item_unlink(q, seq); /* 理论到不了：make_room 已保证有条目位 */
        return rc;
    }
    q->next_seq = seq + 1u;
    q->pushes++;
    *seq_out = seq;
    return IV_OK;
}

int iv_queue_peek(iv_queue_t *q, uint64_t *seq, const void **data, size_t *len)
{
    if (q == NULL || !q->opened)
        return IV_ESTATE;

    for (;;) {
        ivq_rec_t rec;
        char      p[IV_QUEUE_PATH_MAX + 32];
        size_t    got = 0u;

        if (q->n == 0u)
            return IV_EAGAIN;
        item_path(p, sizeof(p), q->dir, q->idx[0].seq);
        if (p[0] != '\0' &&
            read_item(p, &rec, q->rbuf, sizeof(q->rbuf), &got) == IV_OK &&
            rec.seq == q->idx[0].seq) {
            if (seq != NULL)
                *seq = q->idx[0].seq;
            if (len != NULL)
                *len = got;
            if (data != NULL)
                *data = q->rbuf;
            return IV_OK;
        }
        /* 段文件被外部删除/损坏：自愈丢弃，继续取下一个（避免整队列卡死）。 */
        (void)unlink(p);
        q->dropped[q->idx[0].prio]++;
        idx_remove(q, 0);
    }
}

int iv_queue_ack(iv_queue_t *q, uint64_t seq)
{
    int pos;

    if (q == NULL || !q->opened)
        return IV_ESTATE;
    pos = idx_find(q, seq);
    if (pos < 0)
        return IV_ENOENT;
    item_unlink(q, seq);
    idx_remove(q, pos);
    q->acks++;
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
size_t iv_queue_count(const iv_queue_t *q)
{
    return (q != NULL && q->opened) ? (size_t)q->n : 0u;
}

uint64_t iv_queue_bytes(const iv_queue_t *q)
{
    return (q != NULL && q->opened) ? q->bytes : 0u;
}

uint64_t iv_queue_next_seq(const iv_queue_t *q)
{
    if (q == NULL || !q->opened || q->n == 0u)
        return 0u;
    return q->idx[0].seq;
}

uint64_t iv_queue_dropped(const iv_queue_t *q, uint8_t prio)
{
    if (q == NULL || prio >= (uint8_t)IV_QUEUE_PRIO_N)
        return 0u;
    return q->dropped[prio];
}

uint64_t iv_queue_corrupt(const iv_queue_t *q)
{
    return (q != NULL) ? q->corrupt : 0u;
}

uint64_t iv_queue_pushes(const iv_queue_t *q)
{
    return (q != NULL) ? q->pushes : 0u;
}

uint64_t iv_queue_acks(const iv_queue_t *q)
{
    return (q != NULL) ? q->acks : 0u;
}
