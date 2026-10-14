/*
 * M7-S7.4 故障注入 —— 掉电（**写库中**）端到端：SQLite 半截事务在重启后不得被采纳
 *
 * ============================ 它验证什么 ============================
 * `tests/unit/test_db.c` 已经把 `iv_db` 的基本封装钉死了（open/tx/wal/损坏恢复/integrity）。
 * 它证明不了"**写库过程中掉电**"这一条 —— 那要求进程在**事务中途**被强行拿掉，
 * 而单测里的事务都是自己 begin/commit 收尾的。
 *
 * 本用例构造三种掉电形状，每一种都断言**重启后库是自洽的**：
 *
 *   t01 **提交点前后被打死**（反复多轮，同一路径累积）
 *       子进程：提交 N 批（每批 BATCH 行，批内一个 `BEGIN IMMEDIATE`…`COMMIT`）⇒
 *       落一个"已到中途"标记 ⇒ 再开一批**永不提交**的事务 ⇒ 挂住等死。
 *       父进程看到标记即 `SIGKILL`。断言：
 *         - 已提交的 N 批**一行不少**（提交即持久）；
 *         - 那一批未提交的**一行不多**（半截事务被回滚，不是"写进去一半"）；
 *         - `PRAGMA integrity_check` 通过；
 *         - 重开**没有**把库判成损坏（不得把好库当坏库改名 `*.corrupt`）；
 *         - 恢复后的库**还能继续写**，且 `-wal` 文件仍在（WAL 模式没被回退）。
 *       多轮累积 ⇒ 同时证明"反复掉电"不会让库越来越坏。
 *
 *   t02 **WAL 残留**：子进程停在中途事务里（被 `pause()` 静止，不再产生新写）时，
 *       把 `control.db` + `-wal` + `-shm` **整组**拷走一份，再杀掉子进程。
 *       断言：重放这份快照后 `integrity_check` 通过、行数**恰好等于已提交批**。
 *       ⇒ 这是"掉电留下半截 WAL，重启重放时**未提交的事务必须被丢弃**"的直接证据。
 *
 *   t03 **库文件被写成 0 字节**（掉电时文件已创建、内容全丢 —— 工程里踩过的同款形状）
 *       断言：`iv_db_open` 返回 OK、可用、表为空，且原文件被**改名保全**成
 *       `<path>.corrupt`（保留现场，不是静默覆盖）；对照：全新路径**不得**产生 `.corrupt`。
 *
 * ============================ 它**不**验证什么（如实登记） ============================
 * `SIGKILL` 只能近似"进程未及清理就消失"，**不能**模拟真实掉电：内核 page cache 里的
 * 数据在 SIGKILL 后仍然会被写回磁盘，而真掉电会丢掉它们 —— 那才是 SQLite 的
 * `synchronous`/fsync 真正要扛的场景。**真实拔电（升级中 / 录像中 / 写库中）只能在板端做**，
 * 属 M7 退出条件的现场部分，登记在 docs/修改记录.md，不在本用例里假装覆盖。
 *
 * ============================ 反向证伪锚点 ============================
 *   f1 子进程那批"永不提交"的事务改成提前 `COMMIT` ⇒ t01 行数多出 BATCH ⇒ FAIL
 *   f2 `iv_db_open` 的 header 校验放宽成"文件存在即有效" ⇒ t03 不再产生 `.corrupt` ⇒ FAIL
 *   f3 快照只拷主库、不拷 `-wal` ⇒ t02 行数少于已提交批 ⇒ FAIL
 *   变异必须**编译干净**（撞 `-Werror` 的 BUILD FAILED 不算证据）。
 *
 * ============================ 跑法与降级 ============================
 *   - 宿主机：`make itest`（本文件属 `ITEST_SRCS` 通配，自动纳入）。
 *   - 全部落在 `mkdtemp("/tmp/iv_dbpow_XXXXXX")` 的临时目录里，退出即删，不碰 eMMC。
 *   - 需要 fork/信号/临时目录，宿主机与板端都能跑（无 unshare 依赖）。
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ivsbox/iv_db.h"
#include "ivsbox/iv_ret.h"

/* 每批行数：够小以便快速跑完，够大以便"半截批"必然与整数批相区分。 */
#define BATCH 20

static int g_fail = 0;

static void chk(int ok, const char *what)
{
    if (ok)
        return;
    g_fail++;
    printf("FAIL: %s\n", what);
}

static void sleep_ms(int ms)
{
    (void)usleep((useconds_t)ms * 1000u);
}

/* 文件是否存在（区分"不存在"与"存在但取不到信息"：只要 stat 成功就算存在）。 */
static int exists(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0;
}

/* 拼接 `<path><suffix>`（suffix 含前导 '-' 或 '.'）；buf 不够长返回 0。 */
static int path_suffix(char *buf, size_t cap, const char *path, const char *suffix)
{
    int n = snprintf(buf, cap, "%s%s", path, suffix);

    return n > 0 && (size_t)n < cap;
}

/* 用 open/write 落一个标记文件。**刻意不 fsync**：父进程与子进程在同一内核里，
 * 看见写入即可；这里模拟的是"进程被打死"，不是"掉电丢 cache"。 */
static int write_mark(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return -1;
    if (write(fd, "mid\n", 4) != 4) {
        (void)close(fd);
        return -1;
    }
    return close(fd);
}

/* ---------------------------------------------------------------------------
 * 子进程：open → 建表 → 提交 ncommit 批 → 落标记 → 开一批**不提交**的事务 → 挂住
 * 退出码 0 之外的任何值都表示子进程自己出错（父进程据此报 FAIL，而不是当成掉电）
 * ------------------------------------------------------------------------- */
static int child_write_then_hold(const char *db_path, const char *mark, int ncommit)
{
    iv_db_t *db = NULL;
    char     sql[128];
    int      i;
    int      j;

    if (iv_db_open(db_path, &db) != IV_OK)
        return 1;
    if (iv_db_exec(db, "CREATE TABLE IF NOT EXISTS rec(id INTEGER PRIMARY KEY, v INTEGER);")
        != IV_OK)
        return 2;

    for (i = 0; i < ncommit; i++) {
        if (iv_db_begin(db) != IV_OK)
            return 3;
        for (j = 0; j < BATCH; j++) {
            if (snprintf(sql, sizeof sql, "INSERT INTO rec(v) VALUES(%d);", i * 100 + j) <= 0)
                return 4;
            if (iv_db_exec(db, sql) != IV_OK)
                return 4;
        }
        if (iv_db_commit(db) != IV_OK)
            return 5;
    }

    /* 已提交 ncommit 批；以下这批**永不提交**，掉电后必须一行都不留。 */
    if (iv_db_begin(db) != IV_OK)
        return 6;
    for (j = 0; j < BATCH; j++) {
        if (snprintf(sql, sizeof sql, "INSERT INTO rec(v) VALUES(%d);", 9000 + j) <= 0)
            return 7;
        if (iv_db_exec(db, sql) != IV_OK)
            return 7;
    }
    if (write_mark(mark) != 0)
        return 8;

    for (;;)
        (void)pause(); /* 等父进程 SIGKILL；任何信号都会让它再进一次 pause */
}

/* 起子进程并等它到"中途"标记，返回 pid（失败返回 -1 并置 *why）。 */
static pid_t spawn_until_mark(const char *db_path, const char *mark, int ncommit, int *why)
{
    pid_t pid;
    int   t;

    (void)unlink(mark);
    pid = fork();
    if (pid < 0) {
        *why = -1;
        return -1;
    }
    if (pid == 0) {
        _exit(child_write_then_hold(db_path, mark, ncommit));
    }
    /* 最多等 20 s：SQLite 首次建库 + 几批 fsync 都不需要这么久。 */
    for (t = 0; t < 2000; t++) {
        if (exists(mark))
            return pid;
        sleep_ms(10);
    }
    *why = -2;
    return pid; /* 仍然返回 pid，交给调用方杀干净 */
}

/* 把 db 连同 -wal/-shm 一组拷到 dst_base（dst_base 不含扩展名）。 */
static int copy_db_group(const char *src_base, const char *dst_base)
{
    static const char *sfx[] = { "", "-wal", "-shm" };
    size_t            k;

    for (k = 0; k < sizeof(sfx) / sizeof(sfx[0]); k++) {
        char sb[512];
        char db[512];
        int  in;
        int  out;
        char buf[8192];
        ssize_t n;

        if (!path_suffix(sb, sizeof sb, src_base, sfx[k]) ||
            !path_suffix(db, sizeof db, dst_base, sfx[k]))
            return -1;
        in = open(sb, O_RDONLY);
        if (in < 0) {
            if (k == 0)
                return -1;         /* 主库必须在 */
            continue;              /* -wal/-shm 可以不存在 */
        }
        out = open(db, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) {
            (void)close(in);
            return -1;
        }
        while ((n = read(in, buf, sizeof buf)) > 0) {
            ssize_t off = 0;

            while (off < n) {
                ssize_t w = write(out, buf + off, (size_t)(n - off));

                if (w <= 0) {
                    (void)close(in);
                    (void)close(out);
                    return -1;
                }
                off += w;
            }
        }
        (void)close(in);
        (void)close(out);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * t01：提交点前后被打死（反复多轮，同一路径累积）
 * ------------------------------------------------------------------------- */
static void t01_kill_around_commit(const char *root)
{
    char  db[512];
    char  mark[512];
    char  wal[512];
    char  corrupt[512];
    char  sql[128];
    int   committed = 0; /* 已提交批数，跨轮累积 */
    int   round;
    int   k;

    if (!path_suffix(db, sizeof db, root, "/control.db") ||
        !path_suffix(mark, sizeof mark, root, "/mid.mark"))
        return;

    for (round = 1; round <= 5; round++) {
        iv_db_t *h = NULL;
        pid_t    pid;
        int      why = 0;
        int      st = 0;
        int64_t  n = -1;
        int      want = committed + round;

        pid = spawn_until_mark(db, mark, round, &why);
        chk(pid > 0, "t01: fork the writer child");
        if (pid <= 0)
            return;
        chk(why == 0, "t01: child reached the mid-transaction marker");
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &st, 0);
        chk(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
            "t01: child died by SIGKILL (power-loss shape)");
        committed = want;

        /* 重启：重开、校验、数行 */
        chk(iv_db_open(db, &h) == IV_OK, "t01: reopen after the kill (db must open)");
        if (h == NULL)
            return;
        chk(iv_db_check_integrity(h) == IV_OK,
            "t01: PRAGMA integrity_check passes after the kill");
        chk(iv_db_scalar_int(h, "SELECT COUNT(*) FROM rec;", &n) == IV_OK,
            "t01: count(*) is readable after the kill");
        chk(n == (int64_t)committed * BATCH,
            "t01: exactly the committed batches survived (no half transaction)");

        /* 恢复后的库必须还能继续写（掉电不该把库变成只读/不可用）。
         * 刻意也走"整批提交"：这样「行数恒为 BATCH 的整数倍」就是贯穿全部轮次的不变式，
         * 下一轮的行数断言不必为杂散行留后门。 */
        chk(iv_db_begin(h) == IV_OK, "t01: begin a batch after recovery");
        for (k = 0; k < BATCH; k++) {
            if (snprintf(sql, sizeof sql, "INSERT INTO rec(v) VALUES(%d);", 8000 + k) <= 0)
                break;
            if (iv_db_exec(h, sql) != IV_OK)
                break;
        }
        chk(iv_db_commit(h) == IV_OK, "t01: commit a batch after recovery");
        committed += 1;
        {
            int64_t n2 = -1;

            chk(iv_db_scalar_int(h, "SELECT COUNT(*) FROM rec;", &n2) == IV_OK &&
                    n2 == (int64_t)committed * BATCH,
                "t01: the post-recovery batch is visible and whole");
        }
        /* `-wal` 的存在性必须在**连接还开着**时查：正常 close 会做 checkpoint 并把
         * `-wal` 删掉 ⇒ close 之后再断言，等于断言一个必然为假的事实（首轮就是这么
         * 踩的：5 轮全报 FAIL，而真正的问题是判据写错了位置，不是被测代码坏了）。 */
        chk(path_suffix(wal, sizeof wal, db, "-wal") && exists(wal),
            "t01: WAL mode is still in use after recovery");
        iv_db_close(h);

        chk(!(path_suffix(corrupt, sizeof corrupt, db, ".corrupt") && exists(corrupt)),
            "t01: a healthy db is NOT renamed to *.corrupt");
    }
}

/* ---------------------------------------------------------------------------
 * t02：WAL 残留 —— 中途静止时整组快照，重开后未提交事务必须被丢弃
 * ------------------------------------------------------------------------- */
static void t02_wal_snapshot(const char *root)
{
    char  db[512];
    char  snap[512];
    char  mark[512];
    const int ncommit = 4;
    pid_t pid;
    int   why = 0;
    int   st = 0;
    int   rc;
    iv_db_t *h = NULL;
    int64_t  n = -1;

    if (!path_suffix(db, sizeof db, root, "/wal.db") ||
        !path_suffix(snap, sizeof snap, root, "/walsnap.db") ||
        !path_suffix(mark, sizeof mark, root, "/wal.mid"))
        return;

    pid = spawn_until_mark(db, mark, ncommit, &why);
    chk(pid > 0 && why == 0, "t02: writer reached the mid-transaction marker");
    if (pid <= 0)
        return;

    /* 此刻子进程已落标记并在 pause() 里静止 ⇒ 文件组不再变化，拷贝是稳定快照。
     * 主库 + -wal + -shm 必须**整组**拷：只拷主库会丢掉已提交的事务（反向证伪锚点 f3）。 */
    chk(copy_db_group(db, snap) == 0, "t02: snapshot the db group (db + -wal + -shm)");

    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, &st, 0);
    chk(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "t02: writer died by SIGKILL");

    rc = iv_db_open(snap, &h);
    chk(rc == IV_OK, "t02: reopen the snapshot (WAL replay must succeed)");
    if (h == NULL)
        return;
    chk(iv_db_check_integrity(h) == IV_OK, "t02: integrity_check passes on the replayed WAL");
    chk(iv_db_scalar_int(h, "SELECT COUNT(*) FROM rec;", &n) == IV_OK,
        "t02: count(*) is readable on the replayed WAL");
    /* 关键断言：WAL 里那一批**未提交**的帧在重放时必须被丢弃。 */
    chk(n == (int64_t)ncommit * BATCH,
        "t02: WAL replay drops the uncommitted transaction (exactly the committed batches)");
    iv_db_close(h);
}

/* ---------------------------------------------------------------------------
 * t03：库文件被写成 0 字节 —— 现场必须被改名保全，且新库可用
 * ------------------------------------------------------------------------- */
static void t03_zero_length_db(const char *root)
{
    char    db[512];
    char    corrupt[512];
    char    fresh[512];
    char    fresh_corrupt[512];
    int     fd;
    iv_db_t *h = NULL;
    int64_t n = -1;

    if (!path_suffix(db, sizeof db, root, "/zero.db") ||
        !path_suffix(fresh, sizeof fresh, root, "/fresh.db"))
        return;

    /* 先写点东西，再人为把它截成 0 字节（掉电把文件内容全丢的形状）。 */
    {
        iv_db_t *a = NULL;

        chk(iv_db_open(db, &a) == IV_OK, "t03: create a db to be zeroed");
        if (a != NULL) {
            chk(iv_db_exec(a, "CREATE TABLE t(x INTEGER);") == IV_OK, "t03: create table");
            chk(iv_db_exec(a, "INSERT INTO t VALUES(7);") == IV_OK, "t03: insert a row");
            iv_db_close(a);
        }
    }
    fd = open(db, O_WRONLY | O_TRUNC);
    chk(fd >= 0, "t03: truncate the db to zero length");
    if (fd >= 0)
        (void)close(fd);

    chk(iv_db_open(db, &h) == IV_OK, "t03: open a zero-length db (must recover, not fail)");
    if (h != NULL) {
        chk(iv_db_check_integrity(h) == IV_OK, "t03: the rebuilt db passes integrity_check");
        chk(iv_db_exec(h, "CREATE TABLE t2(x INTEGER);") == IV_OK,
            "t03: the rebuilt db is usable");
        chk(iv_db_scalar_int(h, "SELECT COUNT(*) FROM t2;", &n) == IV_OK && n == 0,
            "t03: the rebuilt db is empty");
        iv_db_close(h);
    }
    chk(path_suffix(corrupt, sizeof corrupt, db, ".corrupt") && exists(corrupt),
        "t03: the zero-length file is preserved as *.corrupt (site kept, not overwritten)");

    /* 对照：全新路径不该凭空产生 .corrupt（否则说明 header 校验把"不存在"也当损坏） */
    chk(iv_db_open(fresh, &h) == IV_OK, "t03: open a brand-new db path");
    if (h != NULL)
        iv_db_close(h);
    chk(!(path_suffix(fresh_corrupt, sizeof fresh_corrupt, fresh, ".corrupt") &&
          exists(fresh_corrupt)),
        "t03: a brand-new path does NOT produce a *.corrupt");
}

/* 递归不足以删干净（只删本用例自建的平铺文件），逐个 unlink 即可。 */
static void cleanup(const char *root)
{
    static const char *names[] = { "/control.db",     "/control.db-wal", "/control.db-shm",
                                   "/control.db.corrupt", "/mid.mark",   "/wal.db",
                                   "/wal.db-wal",     "/wal.db-shm",     "/walsnap.db",
                                   "/walsnap.db-wal", "/walsnap.db-shm", "/wal.mid",
                                   "/zero.db",        "/zero.db.corrupt", "/fresh.db",
                                   "/fresh.db-wal",   "/fresh.db-shm" };
    size_t            i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char p[512];

        if (path_suffix(p, sizeof p, root, names[i]))
            (void)unlink(p);
    }
    (void)rmdir(root);
}

int main(void)
{
    char root[] = "/tmp/iv_dbpow_XXXXXX";

    if (mkdtemp(root) == NULL) {
        printf("SKIP: mkdtemp failed (%s), cannot build a throwaway db dir\n", strerror(errno));
        return 0;
    }

    t01_kill_around_commit(root);
    t02_wal_snapshot(root);
    t03_zero_length_db(root);

    cleanup(root);

    if (g_fail == 0)
        printf("test_fault_db_power passed (committed data survives a mid-transaction kill, "
               "uncommitted data never does, WAL replay drops the half transaction, a "
               "zero-length db is preserved and rebuilt)\n");
    return g_fail == 0 ? 0 : 1;
}
