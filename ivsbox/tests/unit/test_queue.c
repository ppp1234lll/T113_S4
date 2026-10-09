/*
 * iv_queue 单测（libivmodules，功能开发计划 M3-S3.4）
 *
 * ============================ 测法 ============================
 * 队列是**落盘**结构，所以测试在 mkdtemp 出来的临时目录里真读真写（不是 mock）。
 * **每个用例用独立的 mkdtemp 子目录**（否则前一个用例遗留的段文件会被下一次
 * open 的 scan 恢复进来，污染计数 —— 这是第一版踩过的坑）。
 * 退出前把顶层临时目录整棵删掉（AGENTS.md 规则 6）。
 *
 * ============================ 反向证伪点（每条都对应一个真机制） ============================
 *   c03 open 恢复后 next_seq 必须续接（最大序号 +1）—— 置回 1 会复用/覆盖旧段文件；
 *   c04 ack 必须删盘上段文件（不是只删索引）—— 只删索引则盘空间永不释放；
 *   c05 满时丢"库内最低优先级"条目（同级丢最老）—— 选错方向会丢最关键的 CTL；
 *   c06 新条目比库内最低级还低时丢新条目（不是驱逐更重要的旧条目）；
 *   c07 字节上限与条目上限**都要**生效 —— 只查条目数则大条目能把盘撑爆。
 *
 * 临时产物：全部在 mkdtemp 目录内，退出前整棵删除（规则 6）。
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_queue.h"
#include "ivsbox/iv_ret.h"

static int  g_fail;
static char g_root[160]; /* 顶层 mkdtemp 目录 */
static char g_case[220]; /* 当前用例目录（g_root 下的 mkdtemp） */

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* out = a + "/" + b（手工拼装，避开 snprintf 的 -Wformat-truncation） */
static void join2(char *out, size_t cap, const char *a, const char *b)
{
    size_t al = strlen(a);
    size_t bl = strlen(b);

    if (al + 1u + bl + 1u > cap) {
        chk(0, "join2: buffer too small");
        out[0] = '\0';
        return;
    }
    memcpy(out, a, al);
    out[al] = '/';
    memcpy(out + al + 1u, b, bl + 1u);
}

/* 用例目录内的相对路径 */
static void case_path(char *out, size_t cap, const char *rel)
{
    join2(out, cap, g_case, rel);
}

/* 为当前用例开一个新目录（g_root/<tag>XXXXXX）并返回指向它的配置 */
static iv_queue_cfg_t new_cfg(const char *tag)
{
    iv_queue_cfg_t c;
    char           tmpl[260];
    size_t         l;

    join2(tmpl, sizeof(tmpl), g_root, tag);
    l = strlen(tmpl);
    if (l + 7u > sizeof(tmpl)) {
        chk(0, "new_cfg: path too long");
        tmpl[0] = '\0';
    } else {
        memcpy(tmpl + l, "XXXXXX", 7u);
    }
    if (mkdtemp(tmpl) == NULL) {
        chk(0, "new_cfg: mkdtemp failed");
        tmpl[0] = '\0';
    }
    if (strlen(tmpl) >= sizeof(g_case)) {
        chk(0, "new_cfg: case dir too long");
        g_case[0] = '\0';
    } else {
        memcpy(g_case, tmpl, strlen(tmpl) + 1u);
    }
    iv_queue_cfg_default(&c);
    c.dir = g_case;
    return c;
}

static int exists(const char *p)
{
    return access(p, F_OK) == 0;
}

/* 独立重写段文件命名（16 位十进制 + ".q"），不复用被测实现的拼名逻辑 */
static void mk_name(char *out, uint64_t seq)
{
    int i;

    for (i = 15; i >= 0; i--) {
        out[i] = (char)('0' + (int)(seq % 10u));
        seq /= 10u;
    }
    out[16] = '.';
    out[17] = 'q';
    out[18] = '\0';
}

static int item_exists(uint64_t seq)
{
    char nm[24];
    char p[300];

    mk_name(nm, seq);
    case_path(p, sizeof(p), nm);
    return exists(p);
}

static int write_raw(const char *path, const void *buf, size_t len)
{
    int    fd  = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    size_t put = 0u;

    if (fd < 0)
        return -1;
    while (put < len) {
        ssize_t n = write(fd, (const uint8_t *)buf + put, len - put);

        if (n <= 0) {
            (void)close(fd);
            return -1;
        }
        put += (size_t)n;
    }
    return close(fd);
}

/* 递归删除（顶层 + 各用例子目录），规则 6 */
static void purge(const char *dir)
{
    DIR           *d = opendir(dir);
    struct dirent *de;

    if (d == NULL)
        return;
    while ((de = readdir(d)) != NULL) {
        char        p[400];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        join2(p, sizeof(p), dir, de->d_name);
        if (p[0] == '\0')
            continue;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode))
            purge(p);
        else
            (void)unlink(p);
    }
    (void)closedir(d);
    (void)rmdir(dir);
}

/* ---------------------------------------------------------------------------
 * c01 参数与守卫
 * ------------------------------------------------------------------------- */
static void c01_args(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c01");
    uint64_t       seq = 0;
    static uint8_t big[IV_QUEUE_ITEM_MAX + 1u];

    iv_queue_init(&q);
    chk(iv_queue_open(NULL, NULL) == IV_EINVAL, "c01 open(NULL)");
    chk(iv_queue_push(NULL, 0, "x", 1, &seq) == IV_EINVAL, "c01 push(NULL q)");

    chk(iv_queue_open(&q, &c) == IV_OK, "c01 open");
    chk(iv_queue_push(&q, (uint8_t)IV_QUEUE_PRIO_N, "x", 1, &seq) == IV_EINVAL, "c01 prio oob");
    chk(iv_queue_push(&q, 0, NULL, 1, &seq) == IV_EINVAL, "c01 data NULL");
    chk(iv_queue_push(&q, 0, "x", 1, NULL) == IV_EINVAL, "c01 seq_out NULL");
    chk(iv_queue_push(&q, 0, "x", 0, &seq) == IV_ERANGE, "c01 len=0");
    chk(iv_queue_push(&q, 0, big, sizeof(big), &seq) == IV_ERANGE, "c01 len>ITEM_MAX");

    chk(iv_queue_peek(&q, NULL, NULL, NULL) == IV_EAGAIN, "c01 peek empty");
    chk(iv_queue_ack(&q, 12345u) == IV_ENOENT, "c01 ack missing");
    chk(iv_queue_count(&q) == 0 && iv_queue_bytes(&q) == 0, "c01 empty counts");
    chk(iv_queue_next_seq(&q) == 0, "c01 next_seq empty=0");

    chk(iv_queue_open(&q, &c) == IV_ESTATE, "c01 double open");
    chk(iv_queue_close(&q) == IV_OK, "c01 close");
    chk(iv_queue_close(&q) == IV_ESTATE, "c01 double close");
    chk(iv_queue_push(&q, 0, "x", 1, &seq) == IV_ESTATE, "c01 push after close");
    chk(iv_queue_count(&q) == 0 && iv_queue_next_seq(&q) == 0, "c01 closed queries 0");
}

/* ---------------------------------------------------------------------------
 * c02 基本写入 + 计数 + 落盘
 * ------------------------------------------------------------------------- */
static void c02_basic(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c02");
    uint64_t       s1 = 0, s2 = 0, s3 = 0;

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c02 open");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "AAA", 3, &s1) == IV_OK, "c02 push1");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_ALARM, "BBBB", 4, &s2) == IV_OK, "c02 push2");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "CCCCC", 5, &s3) == IV_OK, "c02 push3");
    chk(s1 == 1u && s2 == 2u && s3 == 3u, "c02 seq monotonic from 1");
    chk(iv_queue_count(&q) == 3u, "c02 count");
    chk(iv_queue_bytes(&q) == 12u, "c02 bytes");
    chk(iv_queue_next_seq(&q) == 1u, "c02 next = oldest");
    chk(iv_queue_pushes(&q) == 3u, "c02 pushes counted");
    chk(item_exists(1u) && item_exists(2u) && item_exists(3u), "c02 files on disk");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_MEDIA) == 0u, "c02 no drops");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c03 恢复：close→open 后索引/内容/next_seq
 * ------------------------------------------------------------------------- */
static void c03_recovery(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c03");
    uint64_t       s = 0, seq = 0;
    const void    *data = NULL;
    size_t         len = 0;
    int            k;

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c03 open");
    for (k = 0; k < 3; k++) {
        char buf[8];

        memset(buf, 'A' + k, sizeof(buf));
        chk(iv_queue_push(&q, IV_QUEUE_PRIO_STATE, buf, sizeof(buf), &s) == IV_OK, "c03 push");
    }
    chk(s == 3u, "c03 last seq");
    chk(iv_queue_close(&q) == IV_OK, "c03 close");

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c03 reopen");
    chk(iv_queue_count(&q) == 3u, "c03 recovered count");
    chk(iv_queue_bytes(&q) == 24u, "c03 recovered bytes");
    chk(iv_queue_next_seq(&q) == 1u, "c03 recovered next=oldest");
    for (k = 0; k < 3; k++) {
        chk(iv_queue_peek(&q, &seq, &data, &len) == IV_OK, "c03 peek");
        chk(seq == (uint64_t)(k + 1), "c03 seq ascending");
        chk(len == 8u, "c03 len");
        chk(((const char *)data)[0] == (char)('A' + k), "c03 payload preserved");
        chk(iv_queue_ack(&q, seq) == IV_OK, "c03 ack");
    }
    chk(iv_queue_count(&q) == 0u, "c03 drained");

    /* 恢复后 next_seq 必须续接（= 最大旧序号 + 1）：置回 1 会复用旧文件名 */
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "Z", 1, &s) == IV_OK, "c03 push after drain");
    chk(s == 4u, "c03 next_seq continues after recovery");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c04 按序补传 + 确认删除（ack 必须删盘）
 * ------------------------------------------------------------------------- */
static void c04_order_ack(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c04");
    uint64_t       seq = 0;
    const void    *data = NULL;
    size_t         len = 0;

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c04 open");
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m", 1, &seq);
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "c", 1, &seq);
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_ALARM, "a", 1, &seq);

    /* 补传顺序＝序号（先来先发），与优先级无关 */
    chk(iv_queue_peek(&q, &seq, &data, &len) == IV_OK, "c04 peek");
    chk(seq == 1u, "c04 peek returns oldest");
    chk(*(const char *)data == 'm', "c04 oldest is first-inserted (media)");

    chk(item_exists(1u), "c04 file 1 exists before ack");
    chk(iv_queue_ack(&q, 1u) == IV_OK, "c04 ack 1");
    chk(!item_exists(1u), "c04 ack removed the segment file"); /* 反证：只删索引会留文件 */
    chk(iv_queue_acks(&q) == 1u, "c04 acks counted");
    chk(iv_queue_count(&q) == 2u && iv_queue_bytes(&q) == 2u, "c04 remaining");
    chk(iv_queue_peek(&q, &seq, &data, &len) == IV_OK && seq == 2u, "c04 next=2");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c05 超限丢"库内最低优先级"（同级丢最老）
 * ------------------------------------------------------------------------- */
static void c05_evict_lowest(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c05");
    uint64_t       seq = 0;

    c.max_items = 3;
    c.max_bytes = 100000;
    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c05 open");
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "c1", 2, &seq);   /* seq=1 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_ALARM, "a1", 2, &seq); /* seq=2 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m1", 2, &seq); /* seq=3 */
    chk(iv_queue_count(&q) == 3u, "c05 full");

    /* 再入 STATE(2)：库内最低级是 MEDIA(3) ⇒ 驱逐 MEDIA(seq=3)，新条目留下(seq=4) */
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_STATE, "s1", 2, &seq) == IV_OK, "c05 push over cap");
    chk(seq == 4u, "c05 new item got next seq");
    chk(iv_queue_count(&q) == 3u, "c05 count stays at cap");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_MEDIA) == 1u, "c05 victim is lowest prio (MEDIA)");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_CTL) == 0u, "c05 CTL never dropped");
    chk(!item_exists(3u), "c05 victim segment file removed");
    chk(item_exists(1u) && item_exists(2u) && item_exists(4u), "c05 others intact");
    /* 剩余顺序应为 1(CTL)、2(ALARM)、4(STATE) —— 3 已消失 */
    chk(iv_queue_next_seq(&q) == 1u, "c05 head=1");
    chk(iv_queue_ack(&q, 1u) == IV_OK, "c05 ack1");
    chk(iv_queue_ack(&q, 2u) == IV_OK, "c05 ack2");
    chk(iv_queue_next_seq(&q) == 4u, "c05 head jumps to 4 (3 was evicted)");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c06 新条目比库内最低级还低 ⇒ 丢新条目
 * ------------------------------------------------------------------------- */
static void c06_drop_new(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c06");
    uint64_t       seq = 0;

    c.max_items = 3;
    c.max_bytes = 100000;
    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c06 open");
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "c1", 2, &seq);   /* 1 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_ALARM, "a1", 2, &seq); /* 2 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_STATE, "s1", 2, &seq); /* 3 */
    chk(iv_queue_count(&q) == 3u, "c06 full");

    /* 新条目 MEDIA(3) 比库内最低级 STATE(2) 还低 ⇒ 丢新条目，旧条目原封不动 */
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m1", 2, &seq) == IV_EFULL, "c06 new dropped");
    chk(iv_queue_count(&q) == 3u, "c06 count unchanged");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_MEDIA) == 1u, "c06 drop counted on new prio");
    chk(!item_exists(4u), "c06 new segment not written");
    chk(item_exists(1u) && item_exists(2u) && item_exists(3u), "c06 originals intact");
    chk(iv_queue_next_seq(&q) == 1u, "c06 head still 1");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c07 字节上限（不只是条目上限）
 * ------------------------------------------------------------------------- */
static void c07_byte_limit(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c07");
    uint64_t       seq = 0;
    uint8_t        blob[1000];

    memset(blob, 0x5A, sizeof(blob));
    c.max_items = 64;    /* 条目位充裕，只让字节上限生效 */
    c.max_bytes = 2048u; /* 下限（open 会钳到 >= ITEM_MAX） */
    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c07 open");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_CTL, blob, sizeof(blob), &seq) == IV_OK, "c07 push 1");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_CTL, blob, sizeof(blob), &seq) == IV_OK, "c07 push 2");
    chk(iv_queue_bytes(&q) == 2000u, "c07 two items fit");
    /* 第 3 条：2000+1000 > 2048 ⇒ 驱逐同优先级最老(seq=1) */
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_CTL, blob, sizeof(blob), &seq) == IV_OK, "c07 push 3");
    chk(seq == 3u, "c07 seq");
    chk(iv_queue_count(&q) == 2u, "c07 count after byte eviction");
    chk(iv_queue_bytes(&q) == 2000u, "c07 bytes stays within cap");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_CTL) == 1u, "c07 byte-limit eviction counted");
    chk(!item_exists(1u), "c07 oldest evicted");
    chk(item_exists(2u) && item_exists(3u), "c07 newer kept");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c08 自愈：open 期坏文件/.tmp；peek 期段文件丢失
 * ------------------------------------------------------------------------- */
static void c08_selfheal(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c08");
    uint64_t       seq = 0;
    const void    *data = NULL;
    size_t         len = 0;
    char           nm[24];
    char           p[300];
    char           tmp[24];

    /* (a) 预置：坏 magic 的段文件、半截 .tmp、无关文件 */
    mk_name(nm, 7u);
    case_path(p, sizeof(p), nm);
    chk(write_raw(p, "GARBAGE-NOT-A-SEGMENT", 21u) == 0, "c08 seed bad segment");

    mk_name(tmp, 8u);      /* "000...0008.q" */
    tmp[16] = '.'; tmp[17] = 't'; tmp[18] = 'm'; tmp[19] = 'p'; tmp[20] = '\0';
    case_path(p, sizeof(p), tmp);
    chk(write_raw(p, "half", 4u) == 0, "c08 seed tmp leftover");

    case_path(p, sizeof(p), "not-ours.txt");
    chk(write_raw(p, "keep me", 7u) == 0, "c08 seed foreign file");

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c08 open");
    chk(iv_queue_count(&q) == 0u, "c08 bad segment not counted");
    chk(iv_queue_corrupt(&q) >= 1u, "c08 bad segment counted corrupt");
    chk(item_exists(7u) == 0, "c08 bad segment removed");
    case_path(p, sizeof(p), tmp);
    chk(!exists(p), "c08 .tmp leftover removed");
    case_path(p, sizeof(p), "not-ours.txt");
    chk(exists(p), "c08 foreign file untouched");

    /* (b) push 两条后手工删掉 seq=1 的段文件 ⇒ peek 自愈跳过 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_STATE, "one", 3, &seq); /* 1 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_STATE, "two", 3, &seq); /* 2 */
    mk_name(nm, 1u);
    case_path(p, sizeof(p), nm);
    chk(unlink(p) == 0, "c08 externally delete seq=1");
    chk(iv_queue_peek(&q, &seq, &data, &len) == IV_OK, "c08 peek self-heals");
    chk(seq == 2u, "c08 skipped the lost item");
    chk(len == 3u && memcmp(data, "two", 3u) == 0, "c08 next item readable");
    chk(iv_queue_count(&q) == 1u, "c08 lost item dropped from index");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_STATE) == 1u, "c08 self-heal counted");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c09 恢复时按新上限收敛（trim），丢最低优先级
 * ------------------------------------------------------------------------- */
static void c09_trim_on_open(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c09");
    uint64_t       seq = 0;

    c.max_items = 64;
    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c09 open");
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_CTL, "c", 1, &seq);   /* 1 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m", 1, &seq); /* 2 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m", 1, &seq); /* 3 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m", 1, &seq); /* 4 */
    (void)iv_queue_push(&q, IV_QUEUE_PRIO_MEDIA, "m", 1, &seq); /* 5 */
    chk(iv_queue_count(&q) == 5u, "c09 five queued");
    (void)iv_queue_close(&q);

    /* 以更小的条目上限重开：应裁到 2 条，且丢的是最低级(MEDIA)最老的 */
    c.max_items = 2;
    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c09 reopen smaller");
    chk(iv_queue_count(&q) == 2u, "c09 trimmed to new cap");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_MEDIA) == 3u, "c09 dropped 3 MEDIA");
    chk(iv_queue_dropped(&q, IV_QUEUE_PRIO_CTL) == 0u, "c09 kept CTL");
    chk(item_exists(1u) && item_exists(5u), "c09 kept CTL(1) and newest MEDIA(5)");
    chk(!item_exists(2u) && !item_exists(3u) && !item_exists(4u), "c09 old MEDIA gone");
    (void)iv_queue_close(&q);
}

/* ---------------------------------------------------------------------------
 * c10 最大条目 + 二进制载荷往返
 * ------------------------------------------------------------------------- */
static void c10_binary_max(void)
{
    iv_queue_t     q;
    iv_queue_cfg_t c = new_cfg("c10");
    uint64_t       seq = 0;
    static uint8_t blob[IV_QUEUE_ITEM_MAX];
    const void    *data = NULL;
    size_t         len = 0;
    size_t         i;
    int            same = 1;

    for (i = 0; i < sizeof(blob); i++)
        blob[i] = (uint8_t)(i & 0xffu); /* 含 0x00，验证按长度而非字符串处理 */

    iv_queue_init(&q);
    chk(iv_queue_open(&q, &c) == IV_OK, "c10 open");
    chk(iv_queue_push(&q, IV_QUEUE_PRIO_ALARM, blob, sizeof(blob), &seq) == IV_OK,
        "c10 max-size item accepted");
    chk(iv_queue_bytes(&q) == IV_QUEUE_ITEM_MAX, "c10 bytes = max item");
    chk(iv_queue_peek(&q, &seq, &data, &len) == IV_OK, "c10 peek");
    chk(len == sizeof(blob), "c10 length preserved");
    if (len == sizeof(blob))
        same = (memcmp(data, blob, len) == 0);
    chk(same, "c10 binary payload round-trips byte-exact");
    (void)iv_queue_close(&q);
}

int main(void)
{
    char tmpl[] = "/tmp/ivqXXXXXX";

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "cannot mkdtemp\n");
        return 1;
    }
    if (strlen(tmpl) >= sizeof(g_root)) {
        fprintf(stderr, "tmp dir path too long\n");
        return 1;
    }
    memcpy(g_root, tmpl, strlen(tmpl) + 1u);

    c01_args();
    c02_basic();
    c03_recovery();
    c04_order_ack();
    c05_evict_lowest();
    c06_drop_new();
    c07_byte_limit();
    c08_selfheal();
    c09_trim_on_open();
    c10_binary_max();

    purge(g_root);

    if (g_fail != 0) {
        fprintf(stderr, "test_queue FAILED (%d)\n", g_fail);
        return 1;
    }
    printf("test_queue passed (args, push/count, recovery, ordered-drain/ack, "
           "evict-lowest, drop-new, byte-limit, self-heal, trim-on-open, binary-max)\n");
    return 0;
}
