/*
 * iv_report 端到端联动测试（功能开发计划 M3-S3.6「怎么验证」）
 *
 * 假采集板 = 一对 pty（master 由测试持有，slave 路径交给装配层开）；
 * 假平台   = 127.0.0.1 上临时端口的真 TCP listener（装配层用内置真 socket 连它）；
 * 队列     = 临时目录（不碰 /mnt/UDISK，VM 上也能跑）。
 * **零 mock、全真实 I/O**：串口走真正的 tty 字符设备，平台走真正的 TCP。
 *
 * 覆盖（对照架构 §16.2 场景 1/2）：
 *   c01 装配层起来即向采集板发 0xE1 轮询帧（字节级黄金校验）；
 *   c02 采集板 0xE1 应答 → 状态镜像被刷新（串口→帧→可靠层→镜像 全链路）；
 *   c03 平台链路建成 UP；
 *   c04 立即上报 → 经「队列 → 取件泵 → TCP」到平台，字段值/顺序/壳/CRC 正确；
 *   c05 平台断开 → 上报入队积压（不丢）；
 *   c06 平台恢复 → 欠包按序补发（CSQ 11/12/13 的到达顺序即序号顺序）、队列清空；
 *   c07 平台下发 E3 查询 → `##`+JSON+`##` 应答、**QN 按平台原值回填**、crc=06；
 *   c08 平台下发未注册命令 → 回 ACK(0x01)（协议层默认分支）；
 *   c09 关闭后资源收口（is_open=0）。
 *
 * 时序约束（重要）：0xE1 在飞事务的应答窗口是 2 s（`IV_LINK_CFG_DEFAULT`），
 * 且 `on_done` 是**逐笔**登记的——应答必须在窗口内喂进；故 c02 紧跟 c01、
 * 只推进 2 拍（400 ms）。
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_proto.h"
#include "ivsbox/iv_queue.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"

#define STEP_MS 200u

static int      g_fail;
static uint64_t g_now;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------------- */

static int pty_pair(char *slave_path, size_t cap)
{
    int m;

    m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0)
        return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) {
        (void)close(m);
        return -1;
    }
    if (ptsname_r(m, slave_path, cap) != 0) {
        (void)close(m);
        return -1;
    }
    if (fcntl(m, F_SETFL, O_NONBLOCK) != 0) {
        (void)close(m);
        return -1;
    }
    return m;
}

static int listen_loopback(uint16_t *port)
{
    int                ls;
    int                on = 1;
    struct sockaddr_in sa;
    socklen_t          sl = sizeof sa;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0)
        return -1;
    (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port        = 0;
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(ls, 4) != 0 ||
        getsockname(ls, (struct sockaddr *)&sa, &sl) != 0) {
        (void)close(ls);
        return -1;
    }
    *port = ntohs(sa.sin_port);
    return ls;
}

static int accept_ms(int ls, int ms)
{
    struct pollfd p;

    p.fd      = ls;
    p.events  = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, ms) <= 0)
        return -1;
    return accept(ls, NULL, NULL);
}

/* 读干 fd（pty 与 socket 通用）；ms 为首轮 poll 等待上限，之后一律不阻塞 */
static size_t drain(int fd, char *out, size_t cap, int ms)
{
    size_t n = 0;

    for (;;) {
        struct pollfd p;
        ssize_t       r;

        p.fd      = fd;
        p.events  = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (n == 0u) ? ms : 0) <= 0)
            break;
        r = read(fd, out + n, cap - n);
        if (r <= 0)
            break; /* EAGAIN / EOF / 错误：都当"没得读了" */
        n += (size_t)r;
        if (n >= cap)
            break;
    }
    return n;
}

static int mk_tmpdir(char *out, size_t cap)
{
    const char *base = getenv("TMPDIR");
    char        tpl[256];

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (snprintf(tpl, sizeof tpl, "%s/ivsbox-e2e-XXXXXX", base) >=
        (int)sizeof tpl)
        return -1;
    if (mkdtemp(tpl) == NULL)
        return -1;
    if (strlen(tpl) >= cap)
        return -1;
    (void)strcpy(out, tpl);
    return 0;
}

static void rm_tmpdir(const char *dir)
{
    DIR *d;

    d = opendir(dir);
    if (d == NULL)
        return;
    for (;;) {
        struct dirent *e = readdir(d);
        char           p[512];

        if (e == NULL)
            break;
        if (e->d_name[0] == '.')
            continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) < (int)sizeof p)
            (void)unlink(p);
    }
    (void)closedir(d);
    (void)rmdir(dir);
}

/* 手工组**下行**二进制帧（0xF0F0）。`iv_proto_bin_build` 只组上行帧，
 * 这里刻意独立拼一遍——顺带校验协议层解析器对"外部构造帧"的兼容。 */
static size_t build_down(uint8_t *out, uint16_t devtype, uint32_t devid,
                         uint8_t cmd, uint32_t qn1, uint32_t qn2,
                         const void *data, uint16_t len)
{
    size_t i;

    out[0] = 0xF0u;
    out[1] = 0xF0u;
    out[2] = (uint8_t)IV_PROTO_VER;
    out[3] = (uint8_t)(devtype >> 8);
    out[4] = (uint8_t)devtype;
    out[5] = (uint8_t)(devid >> 16);
    out[6] = (uint8_t)(devid >> 8);
    out[7] = (uint8_t)devid;
    out[8] = cmd;
    for (i = 0; i < 4u; i++) {
        out[9 + i]  = (uint8_t)(qn1 >> (24u - 8u * i));
        out[13 + i] = (uint8_t)(qn2 >> (24u - 8u * i));
    }
    out[17] = (uint8_t)len;
    if (len != 0u && data != NULL)
        memcpy(&out[18], data, len);
    /* CRC 覆盖 [ver, data 末尾) = 16 + len 字节 */
    out[18 + len] = iv_crc8(&out[2], 16u + (size_t)len, IV_CRC8_SEED_INIT);
    out[19 + len] = 0xFFu;
    out[20 + len] = 0xFFu;
    return 18u + (size_t)len + 3u;
}

/* 推进 n 拍；每拍前把 pty master 上的下行字节抽掉（防 pty 缓冲写满） */
static void pump(iv_report_t *rp, int master, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        char scratch[512];

        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(rp, g_now);
    }
}

/* 走一条 ## 上报帧，回填 seg 起点与长度；成功返回帧总字节数，否则 0 */
static size_t next_report_frame(const char *buf, size_t len, size_t off,
                                const char **seg, size_t *seg_len)
{
    size_t sl;

    if (off + 8u > len)
        return 0u;
    if (buf[off] != '#' || buf[off + 1u] != '#')
        return 0u;
    sl = (size_t)(buf[off + 2u] - '0') * 1000u +
         (size_t)(buf[off + 3u] - '0') * 100u +
         (size_t)(buf[off + 4u] - '0') * 10u + (size_t)(buf[off + 5u] - '0');
    if (off + 8u + sl > len)
        return 0u;
    *seg     = buf + off + 6u;
    *seg_len = sl;
    return 8u + sl;
}

/* 取 `CSQ=<n>;` 中的 n；没有则 -1 */
static int seg_csq(const char *seg, size_t seg_len)
{
    int i;

    for (i = 0; i < 100; i++) {
        char        pat[16];
        const char *p;

        (void)snprintf(pat, sizeof pat, "CSQ=%d;", i);
        p = strstr(seg, pat);
        if (p != NULL && (size_t)(p - seg) < seg_len)
            return i;
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * 主流程
 * ------------------------------------------------------------------------- */
int main(void)
{
    static const char want_hdr[] = "QN=0;TID=101;VER=11;DEVTYPE=0400;CP=&&";

    char            slave_path[128];
    char            qdir[256];
    int             master = -1;
    int             ls     = -1;
    int             cs     = -1;
    uint16_t        port   = 0;
    iv_report_t     rp;
    iv_report_cfg_t cfg;
    iv_report_env_t env;
    int             i;

    master = pty_pair(slave_path, sizeof slave_path);
    if (master < 0) {
        fprintf(stderr, "SKIP: cannot create pty pair\n");
        return 0;
    }
    ls = listen_loopback(&port);
    if (ls < 0 || mk_tmpdir(qdir, sizeof qdir) != 0) {
        fprintf(stderr, "SKIP: cannot set up loopback/tmpdir\n");
        return 0;
    }

    iv_report_cfg_default(&cfg);
    cfg.serial_path  = slave_path;
    cfg.queue_dir    = qdir;
    cfg.server_host  = "127.0.0.1";
    cfg.server_port  = port;
    cfg.devid        = 0x101u;
    cfg.devtype      = IV_PROTO_DEVTYPE_DEFAULT;
    cfg.mod          = "E2E-MOD";
    cfg.sv           = "E2E-SV";
    cfg.report_ms    = 3600u * 1000u; /* 关周期上报：用 trigger 精确催报 */
    cfg.heartbeat_ms = 3600u * 1000u; /* 关心跳：会污染收包断言 */
    cfg.poll_ms      = 3600u * 1000u; /* 关轮询：喂应答的时点由测试掌握 */

    iv_report_init(&rp);
    chk(iv_report_open(&rp, &cfg, NULL, 0u) == IV_OK, "c00 open");
    chk(iv_report_is_open(&rp) == 1, "c00 is_open");
    chk(iv_report_serial_fd(&rp) >= 0, "c00 serial opened");

    /* ---- c01 起来即向采集板发 0xE1 轮询帧 ---- */
    {
        uint8_t want[IV_FRAME_MAX];
        char    got[64];
        size_t  wn;
        size_t  gn;

        g_now = 0u;
        (void)iv_report_step(&rp, g_now);
        gn = drain(master, got, sizeof got, 200);
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY,
                                    "\x00", 1u, want, sizeof want);
        chk(wn > 0u, "c01 expected frame built");
        chk(gn == wn, "c01 0xE1 poll frame length");
        chk(gn == wn && memcmp(got, want, wn) == 0, "c01 0xE1 poll frame bytes");
        chk(gn >= 9u && (uint8_t)got[0] == 0xF0u && (uint8_t)got[1] == 0xF0u &&
            (uint8_t)got[2] == IV_FRAME_CMD_QUERY &&
            /* §18.2：长度域**小端**（len=1 → 01 00） */
            (uint8_t)got[3] == 0x01u && (uint8_t)got[4] == 0x00u &&
            (uint8_t)got[5] == 0x00u && (uint8_t)got[7] == 0xFFu &&
            (uint8_t)got[8] == 0xFFu, "c01 poll head/cmd/len/tail");
    }

    /* ---- c02 采集板 0xE1 应答 → 镜像刷新（必须在 2 s 应答窗口内） ---- */
    {
        static const char json[] =
            "{\"V\":234.17,\"A\":0.50,\"H\":50.01,\"T\":31.33,\"DS\":1,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":1,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":234.17,\"CHV2\":234.17,\"CHV3\":234.17,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t up[IV_FRAME_MAX];
        int     un;

        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json,
                            (uint16_t)(sizeof json - 1u), up, sizeof up);
        chk(un > 0, "c02 upstream frame built");
        chk(write(master, up, (size_t)un) == un, "c02 feed upstream to mcu link");

        pump(&rp, master, 2); /* 2 拍 = 400 ms */

        chk(rp.st.valid.T == 1u, "c02 mirror T valid");
        chk(rp.st.data.T > 31.32f && rp.st.data.T < 31.34f, "c02 mirror T value");
        chk(rp.st.valid.V == 1u && rp.st.data.V > 234.16f &&
            rp.st.data.V < 234.18f, "c02 mirror V value");
        chk(rp.st.valid.DS == 1u && rp.st.data.DS == 1, "c02 mirror DS=1");
        chk(rp.st.valid.RELAY == 1u, "c02 mirror RELAY valid");
        chk(strcmp(rp.st.data.APOWER, "0.00") == 0, "c02 mirror APOWER");
    }

    /* ---- c03 平台链路 UP ---- */
    {
        int j;

        for (j = 0; j < 10 && cs < 0; j++) {
            g_now += STEP_MS;
            (void)iv_report_step(&rp, g_now);
            cs = accept_ms(ls, 0);
        }
        if (cs < 0)
            cs = accept_ms(ls, 2000);
        chk(cs >= 0, "c03 platform accepted");
        for (j = 0; j < 20 && iv_transport_state(&rp.tr) != IV_TRANSPORT_UP; j++) {
            g_now += STEP_MS;
            (void)iv_report_step(&rp, g_now);
        }
        chk(iv_transport_state(&rp.tr) == IV_TRANSPORT_UP, "c03 transport UP");
        if (cs >= 0)
            (void)fcntl(cs, F_SETFL, O_NONBLOCK);
    }

    /* ---- c04 立即上报 → 平台收到字段正确、壳与 CRC 正确的 ## 帧 ---- */
    {
        char        rx[4096];
        size_t      rn   = 0u;
        const char *seg  = NULL;
        size_t      sl   = 0u;
        size_t      used = 0u;

        memset(&env, 0, sizeof env);
        env.csq = 9;
        iv_report_set_env(&rp, &env);

        chk(iv_report_trigger(&rp, g_now) == IV_OK, "c04 trigger ok");
        chk(iv_report_queue_count(&rp) == 1u, "c04 queued 1");

        for (i = 0; i < 50 && iv_report_queue_count(&rp) > 0u; i++)
            pump(&rp, master, 1);
        chk(iv_report_queue_count(&rp) == 0u, "c04 item acked out of queue");

        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(rn > 0u, "c04 platform received bytes");
        chk(used > 0u, "c04 well-formed ## frame");
        chk(rn == used, "c04 exactly one frame, no trailing junk");
        if (used > 0u) {
            chk(strncmp(seg, want_hdr, sizeof want_hdr - 1u) == 0,
                "c04 segment header (QN/TID/VER/DEVTYPE/CP)");
            chk(strstr(seg, "V=234.17;A=0.50;H=50.01;T=31.33;DS=1;P=0;") != NULL,
                "c04 mirror fields in field-table order");
            chk(strstr(seg, "RELAY=1,1,1;") != NULL, "c04 RELAY 3 entries");
            chk(strstr(seg, "CSQ=9;") != NULL, "c04 injected CSQ");
            chk(strstr(seg, "LAT=") == NULL, "c04 no fix => no LAT");
            chk(sl >= 2u && seg[sl - 2u] == '&' && seg[sl - 1u] == '&',
                "c04 segment closes with &&");
            {
                const char *amp = strstr(seg, "&&");
                uint8_t     crc;
                char        hex[3];

                chk(amp != NULL, "c04 has data-area marker");
                if (amp != NULL) {
                    crc = iv_crc8(amp, (size_t)(seg + sl - amp),
                                  IV_CRC8_SEED_INIT);
                    (void)snprintf(hex, sizeof hex, "%02x", (unsigned)crc);
                    chk(rx[6 + sl] == hex[0] && rx[7 + sl] == hex[1],
                        "c04 wire CRC covers &&data area&& only");
                }
            }
        }
    }

    /* ---- c05 平台断开 → 上报入队积压（不丢） ---- */
    {
        int j;

        if (cs >= 0) {
            (void)close(cs);
            cs = -1;
        }
        for (j = 0; j < 50 && iv_transport_state(&rp.tr) == IV_TRANSPORT_UP; j++) {
            g_now += STEP_MS;
            (void)iv_report_step(&rp, g_now);
        }
        chk(iv_transport_state(&rp.tr) != IV_TRANSPORT_UP,
            "c05 transport left UP after peer close");

        for (i = 11; i <= 13; i++) {
            memset(&env, 0, sizeof env);
            env.csq = i;
            iv_report_set_env(&rp, &env);
            chk(iv_report_trigger(&rp, g_now) == IV_OK, "c05 trigger while down");
            pump(&rp, master, 1);
        }
        chk(iv_report_queue_count(&rp) == 3u, "c05 3 items queued while down");
        chk(iv_report_reports(&rp) == 4u, "c05 total reports=4");
        chk(iv_report_queue_full(&rp) == 0u, "c05 nothing dropped");
    }

    /* ---- c06 平台恢复 → 按序补发、队列清空 ---- */
    {
        char   rx[8192];
        size_t rn   = 0u;
        int    seen[8];
        int    nseen = 0;
        size_t off   = 0u;
        int    j;

        for (j = 0; j < 80 && cs < 0; j++) {
            g_now += STEP_MS; /* 走完退避窗口 */
            (void)iv_report_step(&rp, g_now);
            cs = accept_ms(ls, 0);
        }
        if (cs < 0)
            cs = accept_ms(ls, 2000);
        chk(cs >= 0, "c06 platform reconnected");
        if (cs >= 0)
            (void)fcntl(cs, F_SETFL, O_NONBLOCK);

        for (j = 0; j < 80 && (iv_transport_state(&rp.tr) != IV_TRANSPORT_UP ||
                               iv_report_queue_count(&rp) > 0u); j++)
            pump(&rp, master, 1);

        chk(iv_transport_state(&rp.tr) == IV_TRANSPORT_UP, "c06 back to UP");
        chk(iv_report_queue_count(&rp) == 0u, "c06 queue drained (all acked)");

        rn = drain(cs, rx, sizeof rx, 1000);
        while (nseen < 8) {
            const char *seg = NULL;
            size_t      sl  = 0u;
            size_t      u   = next_report_frame(rx, rn, off, &seg, &sl);

            if (u == 0u)
                break;
            seen[nseen++] = seg_csq(seg, sl);
            off += u;
        }
        chk(nseen == 3, "c06 exactly 3 frames replayed (no dup, no loss)");
        chk(nseen == 3 && seen[0] == 11 && seen[1] == 12 && seen[2] == 13,
            "c06 replay order == queue order (11/12/13)");
        chk(off == rn, "c06 no trailing bytes after replayed frames");
    }

    /* ---- c07 平台查询 E3 → QN 回填 + ## 壳 ---- */
    {
        uint8_t q[64];
        char    rx[2048];
        size_t  qn;
        size_t  rn;

        qn = build_down(q, IV_PROTO_DEVTYPE_DEFAULT, 0x101u,
                        IV_PROTO_CMD_QUERY_VERSION, 20210121u, 143412008u,
                        NULL, 0u);
        chk(write(cs, q, qn) == (ssize_t)qn, "c07 send E3 query");
        pump(&rp, master, 2);
        rn = drain(cs, rx, sizeof rx, 500);
        chk(rn > 8u && rn < sizeof rx, "c07 got query response");
        if (rn > 8u && rn < sizeof rx) {
            rx[rn] = '\0';
            chk(rx[0] == '#' && rx[1] == '#', "c07 response opens with ##");
            chk(rx[rn - 1u] == '#' && rx[rn - 2u] == '#',
                "c07 response closes with ##");
            chk(strstr(rx, "\"qn\":\"20210121143412008\"") != NULL,
                "c07 QN echoed verbatim");
            chk(strstr(rx, "\"cmd\":\"E3\"") != NULL, "c07 cmd E3");
            chk(strstr(rx, "\"crc\":\"06\"") != NULL,
                "c07 crc = CRC8(110400101E3) = 06");
            chk(strstr(rx, "\"mod\":\"E2E-MOD\"") != NULL, "c07 mod");
            chk(strstr(rx, "\"sv\":\"E2E-SV\"") != NULL, "c07 sv");
            chk(strstr(rx, "\"type\":\"0400\"") != NULL, "c07 devtype");
            chk(iv_report_query_responses(&rp) == 1u, "c07 counter");
        }
    }

    /* ---- c08 未注册命令 → 默认分支回 ACK(0x01) ---- */
    {
        uint8_t q[64];
        char    rx[512];
        size_t  qn;
        size_t  rn;

        qn = build_down(q, IV_PROTO_DEVTYPE_DEFAULT, 0x101u, 0x99u, 7u, 8u,
                        NULL, 0u);
        chk(write(cs, q, qn) == (ssize_t)qn, "c08 send unknown cmd");
        pump(&rp, master, 2);
        rn = drain(cs, rx, sizeof rx, 500);
        chk(rn >= 22u, "c08 got a binary response");
        if (rn >= 22u) {
            chk((uint8_t)rx[0] == 0x0Fu && (uint8_t)rx[1] == 0x0Fu,
                "c08 up-frame head");
            chk((uint8_t)rx[2] == (uint8_t)IV_PROTO_VER, "c08 ver");
            chk((uint8_t)rx[8] == 0x99u, "c08 ACK echoes cmd");
            chk((uint8_t)rx[17] == 0x01u && (uint8_t)rx[18] == IV_PROTO_ACK_OK,
                "c08 ACK data[0]=01");
            /* 22 字节帧：crc 在 [19]，帧尾在 [20][21] */
            chk((uint8_t)rx[20] == 0xFFu && (uint8_t)rx[21] == 0xFFu,
                "c08 frame tail");
        }
    }

    /* ---- c09 收口 ---- */
    iv_report_close(&rp);
    chk(iv_report_is_open(&rp) == 0, "c09 closed");

    if (cs >= 0)
        (void)close(cs);
    (void)close(ls);
    (void)close(master);
    rm_tmpdir(qdir);

    if (g_fail == 0)
        printf("test_e2e_report passed "
               "(poll/mirror/report/queue/replay/query/ack)\n");
    else
        printf("test_e2e_report FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
