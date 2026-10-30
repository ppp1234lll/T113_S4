/*
 * test_tunnel —— 受控远程通道（M8-S8.3）
 *
 * 2026-10-30 起本模块**按 BIRD 运维箱（bird_20260623）逐条对齐**：一条 stcp 代理、
 * 转发本机 22、密钥 md5(sn+token) 前 8 字节、/etc/frp/frpc.toml、/tmp/frp.enabled。
 * 所以本文件钉的是"与 BIRD 字节级一致"这件事，不再问"某个服务名该不该放行"：
 *
 *   t01 secretKey 黄金向量（与 GO 的 GenerateSTCPKey 同一个值）
 *   t02 frpc.toml 逐字钉（含空行位置，与 GO 的 fmt.Sprintf 产物逐字对比）
 *   t03 渲染拒绝路径（缺身份 / 缺服务端 / 缓冲不足）
 *   t04 enable：配置落盘 0600、标记落盘、状态与幂等
 *   t05 标记内容：ttl=0 写空文件（＝BIRD 的 touch）；ttl>0 写到期时刻
 *   t06 到期自动关（tick）与"不过期不会自己关"
 *   t07 身份/服务端变更 ⇒ invalidate（STOP_RECONF）
 *   t08 caps / stat / last_session
 *
 * 反向证伪：把 iv_tunnel.c 里对应实现改坏（localPort 改成 23、secretKey 少取一字节、
 * 标记不落盘、disable 不删标记），下面必有一条转红。
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ivsbox/iv_tunnel.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 测试侧材料
 * ------------------------------------------------------------------------- */
#define SN    "A120F"
#define TOKEN "bird123"

/* 与 BIRD `utils.GenerateSTCPKey(sn, token)` 同源：
 *   md5("A120F" + "bird123") 前 8 字节 = 1239036bac71d702
 * 访问端 connect.bat 用 certutil 算的是同一个值（前 16 位小写 hex）。 */
#define SEC_BIRD "1239036bac71d702"

static char g_dir[64];   /* 临时目录 */
static char g_conf[128]; /* <dir>/frpc.toml */
static char g_flag[128]; /* <dir>/frp.enabled */

static iv_tunnel_reason_t g_why; /* 用例里随手用的出参 */

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int file_mode_is(const char *path, mode_t want)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & 0777u) == (want & 0777u);
}

static long read_file(const char *path, char *buf, size_t cap)
{
    int  fd;
    long n;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = (long)read(fd, buf, cap - 1u);
    (void)close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/* 装配好的句柄（临时目录 + 身份 + 服务端） */
static void mk_rig(iv_tunnel_t *t)
{
    iv_tunnel_init(t);
    chk(iv_tunnel_set_identity(t, SN, TOKEN) == IV_OK, "rig: identity");
    chk(iv_tunnel_set_server(t, "frp.example.com", 7000u) == IV_OK, "rig: server");
    chk(iv_tunnel_set_paths(t, g_conf, g_flag) == IV_OK, "rig: paths");
}

/* ---------------------------------------------------------------------------
 * t01 secretKey 黄金向量
 * ------------------------------------------------------------------------- */
static void t01_secret(void)
{
    char out[IV_TUNNEL_SECRET_MAX];

    chk(iv_tunnel_secret(SN, TOKEN, out, sizeof out) == IV_OK &&
        strcmp(out, SEC_BIRD) == 0, "t01: secret == BIRD GenerateSTCPKey");
    chk(strlen(out) == IV_TUNNEL_SECRET_HEX, "t01: 16 hex chars");
    /* 换一个输入就必须换一把钥匙（否则"一把钥匙开所有设备"） */
    chk(iv_tunnel_secret(SN, "bird124", out, sizeof out) == IV_OK &&
        strcmp(out, SEC_BIRD) != 0, "t01: different token -> different key");
    chk(iv_tunnel_secret("A120G", TOKEN, out, sizeof out) == IV_OK &&
        strcmp(out, SEC_BIRD) != 0, "t01: different sn -> different key");
    chk(iv_tunnel_secret(SN, TOKEN, out, 8) == IV_ERANGE, "t01: cap short");
    chk(iv_tunnel_secret(NULL, TOKEN, out, sizeof out) == IV_EINVAL, "t01: NULL sn");
    chk(iv_tunnel_secret(SN, NULL, out, sizeof out) == IV_EINVAL, "t01: NULL token");
    chk(iv_tunnel_secret(SN, TOKEN, NULL, sizeof out) == IV_EINVAL, "t01: NULL out");
}

/* ---------------------------------------------------------------------------
 * t02 frpc.toml 逐字钉（与 GO writeFrpConfig 的产物逐字一致）
 * ------------------------------------------------------------------------- */
static void t02_render(void)
{
    static const char *const EXPECT =
        "serverAddr = \"frp.example.com\"\n"
        "serverPort = 7000\n"
        "auth.token = \"" TOKEN "\"\n"
        "\n"
        "[[proxies]]\n"
        "name = \"" SN "\"\n"
        "type = \"stcp\"\n"
        "secretKey = \"" SEC_BIRD "\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 22\n";
    iv_tunnel_reason_t why = IV_TUNNEL_ESTATE;
    char               buf[IV_TUNNEL_TXT_MAX];
    size_t             len = 0;

    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, buf, sizeof buf, &len,
                         &why) == IV_OK, "t02: render");
    chk(why == IV_TUNNEL_OK, "t02: reason ok");
    chk(strcmp(buf, EXPECT) == 0, "t02: exact text (BIRD-identical)");
    chk(len == strlen(EXPECT), "t02: out_len");
    /* 只有一条代理：多了就是又走回"按服务开代理"的老路 */
    {
        const char *q   = buf;
        int         cnt = 0;
        while ((q = strstr(q, "[[proxies]]")) != NULL) { cnt++; q++; }
        chk(cnt == 1, "t02: exactly one proxy");
    }
    /* 端口必须是 22（BIRD 硬编码），别被改成 web/ftp 的端口 */
    chk(strstr(buf, "localPort = 22\n") != NULL, "t02: forwards sshd 22");
    /* 代理名就是 SN 本身（BIRD: name = "%s", deviceSn） */
    chk(strstr(buf, "name = \"" SN "\"\n") != NULL, "t02: proxy name == SN");
}

/* ---------------------------------------------------------------------------
 * t03 渲染拒绝路径
 * ------------------------------------------------------------------------- */
static void t03_render_deny(void)
{
    iv_tunnel_reason_t why = IV_TUNNEL_OK;
    char               buf[IV_TUNNEL_TXT_MAX];
    char               tiny[16];
    size_t             len = 0;

    chk(iv_tunnel_render("", TOKEN, "frp.example.com", 7000u, buf, sizeof buf, &len,
                         &why) == IV_EPROTO, "t03: empty sn");
    chk(why == IV_TUNNEL_ENOIDENT, "t03: reason ENOIDENT");
    chk(iv_tunnel_render(SN, "", "frp.example.com", 7000u, buf, sizeof buf, &len,
                         &why) == IV_EPROTO, "t03: empty token");
    chk(iv_tunnel_render(SN, TOKEN, "", 7000u, buf, sizeof buf, &len,
                         &why) == IV_ENOENT, "t03: empty server");
    chk(why == IV_TUNNEL_ENOSERVER, "t03: reason ENOSERVER");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 0u, buf, sizeof buf, &len,
                         &why) == IV_ENOENT, "t03: zero port");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, tiny, sizeof tiny, &len,
                         &why) == IV_ERANGE, "t03: buffer too small");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, NULL, 0u, &len,
                         &why) == IV_EINVAL, "t03: NULL out");
}

/* ---------------------------------------------------------------------------
 * t04 enable：落盘与幂等
 * ------------------------------------------------------------------------- */
static void t04_enable(void)
{
    iv_tunnel_t t;
    char        buf[IV_TUNNEL_TXT_MAX];

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    mk_rig(&t);

    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t04: starts OFF");
    chk(iv_tunnel_enable(&t, 0u, 1000u, &g_why) == IV_OK, "t04: enable (no ttl)");
    chk(g_why == IV_TUNNEL_OK, "t04: reason ok");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_ON, "t04: state ON");
    chk(file_exists(g_conf), "t04: frpc.toml written");
    chk(file_exists(g_flag), "t04: flag written");
    /* secretKey 在文件里 ⇒ 必须 0600（BIRD 同款） */
    chk(file_mode_is(g_conf, 0600), "t04: frpc.toml is 0600");
    chk(read_file(g_conf, buf, sizeof buf) > 0 && strstr(buf, SEC_BIRD) != NULL,
        "t04: file carries the key");
    /* 已开启时重复 enable ⇒ ESTATE（要改有效期先 disable） */
    chk(iv_tunnel_enable(&t, 0u, 1001u, &g_why) == IV_ESTATE, "t04: double enable");
    chk(g_why == IV_TUNNEL_ESTATE, "t04: reason ESTATE");
    chk(iv_tunnel_disable(&t, 1100u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t04: disable");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t04: state OFF");
    chk(!file_exists(g_flag), "t04: flag removed");
    chk(file_exists(g_conf), "t04: config kept for next time");
    chk(iv_tunnel_disable(&t, 1101u, IV_TUNNEL_STOP_LOCAL) == IV_ESTATE,
        "t04: double disable");
    /* 但"未开时 disable" 仍必须把残留标记清掉：内存态与标记可以不同步（进程刚重启、
     * restore 还没跑），此时平台发 disable 若连文件都不删，回"成功"却关不掉通道。 */
    {
        FILE *f = fopen(g_flag, "wb");

        chk(f != NULL, "t04: plant a stale flag");
        if (f != NULL) { fputs("9999999999\n", f); fclose(f); }
        chk(file_exists(g_flag), "t04: stale flag exists");
        chk(iv_tunnel_disable(&t, 1102u, IV_TUNNEL_STOP_LOCAL) == IV_ESTATE,
            "t04: disable while OFF still reports ESTATE");
        chk(!file_exists(g_flag), "t04: stale flag removed anyway (idempotent stop)");
    }
}

/* ---------------------------------------------------------------------------
 * t05 标记文件内容：ttl=0 空文件（BIRD 的 touch）；ttl>0 写到期时刻
 * ------------------------------------------------------------------------- */
static void t05_flag_content(void)
{
    iv_tunnel_t t;
    char        buf[64];
    long        n;

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    mk_rig(&t);

    /* BIRD 语义：没有有效期，标记是空文件，脚本侧判"不过期" */
    chk(iv_tunnel_enable(&t, 0u, 2000u, &g_why) == IV_OK, "t05: enable no ttl");
    n = read_file(g_flag, buf, sizeof buf);
    chk(n == 0, "t05: empty flag == BIRD touch");
    chk(iv_tunnel_remain_s(&t, 2000u) == 0u, "t05: no remain when no ttl");
    chk(iv_tunnel_tick(&t, 2000u + 86400u) == 0, "t05: never expires without ttl");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_ON, "t05: still ON");
    chk(iv_tunnel_disable(&t, 2001u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t05: disable");

    /* 本工程扩展：显式 TTL ⇒ 标记里写到期时刻，主控崩了脚本侧也能判 */
    chk(iv_tunnel_enable(&t, 600u, 3000u, &g_why) == IV_OK, "t05: enable ttl");
    n = read_file(g_flag, buf, sizeof buf);
    chk(n > 0 && (unsigned long long)atoll(buf) == (unsigned long long)(3000u + 600u),
        "t05: flag carries expiry epoch");
    chk(iv_tunnel_remain_s(&t, 3000u) == 600u, "t05: remain_s");
    chk(iv_tunnel_remain_s(&t, 3599u) == 1u, "t05: remain_s near expiry");
    chk(iv_tunnel_disable(&t, 3001u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t05: disable 2");

    /* 超上限一律拒（BIRD 没有 TTL，这层是本工程的护栏） */
    chk(iv_tunnel_enable(&t, IV_TUNNEL_TTL_MAX + 1u, 3000u, &g_why) == IV_EINVAL,
        "t05: ttl over max");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t05: rejected enable leaves state OFF");
    chk(!file_exists(g_flag), "t05: no flag written on rejection");
}

/* ---------------------------------------------------------------------------
 * t06 到期自动关
 * ------------------------------------------------------------------------- */
static void t06_expire(void)
{
    iv_tunnel_t      t;
    iv_tunnel_stat_t st;

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    mk_rig(&t);

    chk(iv_tunnel_enable(&t, 10u, 5000u, &g_why) == IV_OK, "t06: enable 10s");
    chk(iv_tunnel_tick(&t, 5009u) == 0, "t06: not yet");
    chk(iv_tunnel_tick(&t, 5010u) == 1, "t06: expires exactly at deadline");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t06: state OFF after expiry");
    chk(!file_exists(g_flag), "t06: flag cleared on expiry");
    iv_tunnel_stat(&t, &st);
    chk(st.expires == 1u, "t06: expires counter");
    {
        iv_tunnel_session_t last;
        chk(iv_tunnel_last_session(&t, &last) == IV_OK, "t06: last session recorded");
        chk(last.stop_reason == IV_TUNNEL_STOP_EXPIRED, "t06: stop reason expired");
        chk(last.open_s == 5000u && last.close_s == 5010u, "t06: session window");
        chk(last.ttl_s == 10u, "t06: session ttl");
    }
    /* 关掉后句柄上不该再残留有效期（否则 status 会显示"已关、ttl=10s"，读的人会
     * 以为还剩 10 秒），但**留档**里的 ttl 必须保住（上面那条）。 */
    chk(t.ttl_s == 0u && t.expire_s == 0u, "t06: no stale ttl on the handle");
}

/* ---------------------------------------------------------------------------
 * t07 身份/服务端变更 ⇒ 作废在开的会话
 * ------------------------------------------------------------------------- */
static void t07_invalidate(void)
{
    iv_tunnel_t t;

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    mk_rig(&t);

    chk(iv_tunnel_invalidate(&t) == IV_OK, "t07: invalidate while OFF is a no-op");
    chk(iv_tunnel_enable(&t, 0u, 7000u, &g_why) == IV_OK, "t07: enable");
    chk(iv_tunnel_invalidate(&t) == IV_OK, "t07: invalidate");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t07: closed");
    chk(!file_exists(g_flag), "t07: flag cleared so monitor stops frpc");
    {
        iv_tunnel_session_t last;
        chk(iv_tunnel_last_session(&t, &last) == IV_OK &&
            last.stop_reason == IV_TUNNEL_STOP_RECONF, "t07: stop reason reconf");
    }
}

/* ---------------------------------------------------------------------------
 * t08 caps / stat / 拒绝计数
 * ------------------------------------------------------------------------- */
static void t08_caps_stat(void)
{
    iv_tunnel_t         t;
    iv_tunnel_stat_t    st;
    iv_tunnel_session_t last;
    iv_tunnel_reason_t  why = IV_TUNNEL_OK;

    (void)unlink(g_conf);
    (void)unlink(g_flag);

    /* 缺身份 ⇒ 能力位为 0（fail-closed） */
    iv_tunnel_init(&t);
    chk(iv_tunnel_caps(&t) == 0u, "t08: no identity -> no caps");
    chk(iv_tunnel_set_server(&t, "frp.example.com", 7000u) == IV_OK, "t08: server");
    chk(iv_tunnel_caps(&t) == 0u, "t08: still no caps without identity");
    chk(iv_tunnel_enable(&t, 0u, 9000u, &why) == IV_EPROTO, "t08: enable denied");
    chk(why == IV_TUNNEL_ENOIDENT, "t08: reason ENOIDENT");
    iv_tunnel_stat(&t, &st);
    chk(st.denies == 1u && st.deny_by[IV_TUNNEL_ENOIDENT] == 1u, "t08: deny counted");
    chk(iv_tunnel_last_session(&t, &last) == IV_ENOENT, "t08: no session yet");

    mk_rig(&t);
    chk((iv_tunnel_caps(&t) & IV_TUNNEL_CAP_ENABLED) != 0u, "t08: enabled bit");
    chk((iv_tunnel_caps(&t) & IV_TUNNEL_CAP_SSH) != 0u, "t08: ssh bit");
    chk(iv_tunnel_enable(&t, 0u, 9001u, &why) == IV_OK, "t08: enable");
    chk((iv_tunnel_caps(&t) & IV_TUNNEL_CAP_ENABLED) != 0u, "t08: enabled while ON");
    chk(iv_tunnel_disable(&t, 9500u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t08: disable");
    iv_tunnel_stat(&t, &st);
    chk(st.opens == 1u && st.uptime_s == 499u, "t08: opens / uptime");
    chk(iv_tunnel_last_session(&t, &last) == IV_OK &&
        last.stop_reason == IV_TUNNEL_STOP_LOCAL, "t08: last session local");
    /* 未开启时 remain 恒 0（前端据此显示"关"） */
    chk(iv_tunnel_remain_s(&t, 9500u) == 0u, "t08: remain 0 when OFF");
    chk(iv_tunnel_state(NULL) == IV_TUNNEL_OFF, "t08: NULL handle is OFF");
    chk(iv_tunnel_caps(NULL) == 0u, "t08: NULL handle has no caps");
}

/* ---------------------------------------------------------------------------
 * t09 认领已开的通道（进程重启后 state 归零，但标记与 frpc 还在）
 *
 * BIRD 里通道独立于 Go 进程：重启 Go 服务不断线。本工程的 state 是内存态，重启后
 * 必须由 restore() 从标记文件认领回来，否则 status 会谎报 on=false、tick 也不判过期。
 * ------------------------------------------------------------------------- */
static void t09_restore(void)
{
    iv_tunnel_t t;
    FILE       *f;

    (void)unlink(g_conf);
    (void)unlink(g_flag);

    /* ① 没有标记 ⇒ 认领失败，保持 OFF（开机正常路径） */
    mk_rig(&t);
    chk(iv_tunnel_restore(&t, 10000u) == IV_ENOENT, "t09: nothing to adopt");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t09: still OFF");

    /* ② 标记是空文件（BIRD 的 touch）⇒ 认领为 ON 且不过期 */
    f = fopen(g_flag, "wb");
    chk(f != NULL, "t09: plant empty flag");
    if (f != NULL) fclose(f);
    mk_rig(&t);
    chk(iv_tunnel_restore(&t, 10000u) == IV_OK, "t09: adopt empty flag");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_ON, "t09: state ON after adopt");
    chk(t.ttl_s == 0u && t.expire_s == 0u, "t09: adopted as no-expiry");
    chk(t.local_port == 22u, "t09: adopted channel is ssh");
    chk(iv_tunnel_remain_s(&t, 99999u) == 0u, "t09: no remain when no expiry");
    chk(iv_tunnel_tick(&t, 99999u) == 0, "t09: adopted channel never expires");
    /* 已 ON 时重复认领 ⇒ ESTATE（不重复计 opens） */
    chk(iv_tunnel_restore(&t, 10001u) == IV_ESTATE, "t09: double adopt is ESTATE");

    /* ③ 标记里是未到的到期时刻 ⇒ 认领并带上剩余时间 */
    (void)iv_tunnel_disable(&t, 10002u, IV_TUNNEL_STOP_LOCAL);
    f = fopen(g_flag, "wb");
    chk(f != NULL, "t09: plant expiry flag");
    if (f != NULL) { fputs("10600\n", f); fclose(f); }
    mk_rig(&t);
    chk(iv_tunnel_restore(&t, 10000u) == IV_OK, "t09: adopt with expiry");
    chk(t.expire_s == 10600u, "t09: expiry adopted");
    chk(t.ttl_s == 600u, "t09: ttl derived from expiry");
    chk(iv_tunnel_remain_s(&t, 10300u) == 300u, "t09: remain counts down");
    chk(iv_tunnel_tick(&t, 10600u) == 1, "t09: adopted channel expires on time");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t09: OFF after expiry");
    chk(!file_exists(g_flag), "t09: expired flag cleared");

    /* ④ 标记里是已过的到期时刻 ⇒ 不认领，顺手清标记 */
    f = fopen(g_flag, "wb");
    chk(f != NULL, "t09: plant stale expiry");
    if (f != NULL) { fputs("5000\n", f); fclose(f); }
    mk_rig(&t);
    chk(iv_tunnel_restore(&t, 10000u) == IV_ESTATE, "t09: stale expiry rejected");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t09: stays OFF");
    chk(!file_exists(g_flag), "t09: stale flag cleaned up");
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    /* 临时目录：所有落盘用例都在这里，跑完清掉（规则 6） */
    snprintf(g_dir, sizeof g_dir, "/tmp/ivtun_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }
    snprintf(g_conf, sizeof g_conf, "%s/frpc.toml", g_dir);
    snprintf(g_flag, sizeof g_flag, "%s/frp.enabled", g_dir);

    t01_secret();
    t02_render();
    t03_render_deny();
    t04_enable();
    t05_flag_content();
    t06_expire();
    t07_invalidate();
    t08_caps_stat();
    t09_restore();

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    (void)rmdir(g_dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_tunnel: %d failures\n", g_fail);
        return 1;
    }
    printf("test_tunnel passed (BIRD secret golden, toml byte-exact, deny paths, "
           "0600 atomic write/idempotent, flag empty-when-no-ttl, ttl expire, "
           "invalidate, caps/stat, adopt-on-restart)\n");
    return 0;
}
