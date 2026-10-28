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
 *   c04b 继电器动作后：催 0xE1 → 镜像刷新 → **立刻补一帧上报**（不等周期）；
 *   c04c 采集板自己改继电器也要报；同值不重复报；
 *   c04d 采集板 0xC2 事件 → **同一拍**催 0xE1 → 全量回来后才组包，走正常上报报文；
 *   c04e 事件撞上"0xE1 还在飞" → 补报按 IV_REPORT_POLL_RETRY_MS 重试，不退回 30 s；
 *   c02c 采集板 0xC1 箱门开启 → 同上（顺带验门开钩子经 cfg 装配链挂上）；
 *   c05 平台断开 → 上报入队积压（不丢）；
 *   c06 平台恢复 → 欠包按序补发（CSQ 11/12/13 的到达顺序即序号顺序）、队列清空；
 *   c07 平台下发 E3 查询 → `##`+JSON+`##` 应答、**QN 按平台原值回填**、crc=06；
 *   c08 平台下发未注册命令 → 回 ACK(0x74)（明确失败；回 OK 等于对平台撒谎）；
 *   c09 串口断开清除旧镜像，重连后自动重新查询；
 *   c10 关闭后资源收口（is_open=0）。
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

/* S4.5 门开钩子记录器。**为什么要在集成里再测一遍**：单测是直接调
 * `iv_status_set_door_cb()` 挂的，覆盖不到"装配层到底有没有把钩子挂上去"——
 * 正是"回调没挂上测不出"的教训。这里钩子只经 `iv_report_cfg_t` 传入。 */
static int g_door_hits;
static int g_door_last;

static void e2e_door_cb(int ds, void *arg)
{
    (void)arg;
    g_door_hits++;
    g_door_last = ds;
}

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

    /* 固定开销 10＝`##`2 + 长度域4 + CRC2 + `##`2。末尾 `##` **必须落在帧内**：
     * 以前按 8 算，帧尾被截掉也不报错，线上表现为平台隔一帧丢一帧。 */
    if (off + 10u > len)
        return 0u;
    if (buf[off] != '#' || buf[off + 1u] != '#')
        return 0u;
    sl = (size_t)(buf[off + 2u] - '0') * 1000u +
         (size_t)(buf[off + 3u] - '0') * 100u +
         (size_t)(buf[off + 4u] - '0') * 10u + (size_t)(buf[off + 5u] - '0');
    if (off + 10u + sl > len)
        return 0u;
    if (buf[off + 6u + sl + 2u] != '#' || buf[off + 6u + sl + 3u] != '#')
        return 0u;
    *seg     = buf + off + 6u;
    *seg_len = sl;
    return 10u + sl;
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
    static const char want_hdr[] = "QN=0;TID=101;VER=11;DEVTYPE=0a00;CP=&&";

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
    cfg.on_door      = e2e_door_cb; /* S4.5：端到端证明钩子经 cfg 挂进了 iv_report */
    cfg.on_door_arg  = NULL;

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

    /* ---- c02b M4-S4.4：0xD1 电源控制下行（**假采集板 = pty**，字节级黄金校验） ----
     * 覆盖 plan §S4.4「怎么验证」的集成项：动作帧真的按 §18.2 落到串口上；
     * 并端到端复现"同 cmd 在飞唯一" ⇒ 证明 S4.4 的待发队列不是过度设计。 */
    {
        static const char pl[]  = "{\"port\":3,\"status\":0}";
        static const char ack[] = "{\"result\":1}";
        uint8_t           want[IV_FRAME_MAX];
        uint8_t           up[IV_FRAME_MAX];
        char              got[64];
        size_t            wn;
        size_t            gn;
        uint16_t          pln = (uint16_t)(sizeof pl - 1u);
        int               un;

        chk(iv_report_send_cmd(NULL, IV_FRAME_CMD_POWER, pl, pln, g_now) == IV_EINVAL,
            "c02b NULL handle -> IV_EINVAL");

        chk(iv_report_send_cmd(&rp, IV_FRAME_CMD_POWER, pl, pln, g_now) == IV_OK,
            "c02b 0xD1 accepted");
        gn = drain(master, got, sizeof got, 500);
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_POWER, pl, pln,
                                    want, sizeof want);
        chk(wn > 0u, "c02b expected 0xD1 frame built");
        chk(gn == wn, "c02b 0xD1 frame length");
        chk(gn == wn && memcmp(got, want, wn) == 0, "c02b 0xD1 frame bytes golden");
        chk(gn >= 9u && (uint8_t)got[0] == 0xF0u && (uint8_t)got[1] == 0xF0u &&
            (uint8_t)got[2] == IV_FRAME_CMD_POWER &&
            /* §18.2：长度域**小端**（21 → 15 00） */
            (uint8_t)got[3] == (uint8_t)pln && (uint8_t)got[4] == 0x00u &&
            (uint8_t)got[wn - 2u] == 0xFFu && (uint8_t)got[wn - 1u] == 0xFFu,
            "c02b 0xD1 head/cmd/len(LE)/tail");

        /* 同 cmd 在飞唯一：第二笔必被拒 */
        chk(iv_report_send_cmd(&rp, IV_FRAME_CMD_POWER, pl, pln, g_now) == IV_EBUSY,
            "c02b second 0xD1 while in flight -> IV_EBUSY");

        /* 采集板回 0xD1 应答 ⇒ 事务完成、槽位释放、可再发 */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_POWER, ack,
                            (uint16_t)(sizeof ack - 1u), up, sizeof up);
        chk(un > 0, "c02b upstream 0xD1 ack built");
        chk(write(master, up, (size_t)un) == un, "c02b feed the 0xD1 ack");
        pump(&rp, master, 2);
        chk(iv_report_send_cmd(&rp, IV_FRAME_CMD_POWER, pl, pln, g_now) == IV_OK,
            "c02b after the ack the slot is reusable");

        /* 收干净：别把这一笔的字节留给 c03 */
        (void)drain(master, got, sizeof got, 500);
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
            /* 字段序对齐参考实现（2026-10-20 复核）：V A APOWER AKW H T P DS。
             * 两处容易写错的点：**APOWER/AKW 夹在 A 与 H 之间**，**P 在 DS 之前**
             * （原先的黄金是 `...H;T;DS;P;`，与参考实现相反，已按代码口径更正）。 */
            if (strstr(seg, "V=234.17;A=0.50;APOWER=0.00;AKW=0.00;H=50.01;"
                            "T=31.33;P=0;DS=1;") == NULL)
                fprintf(stderr, "  c04 DEBUG seg=%s\n", seg);
            chk(strstr(seg, "V=234.17;A=0.50;APOWER=0.00;AKW=0.00;H=50.01;"
                            "T=31.33;P=0;DS=1;") != NULL,
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

    /* ---- c02c M4-S4.5：0xC1 箱门开启 → DS=2 ＋ 门开钩子 ＋「立刻取全量再上报」 ----
     * 覆盖 plan §S4.5「怎么验证」单元项在**装配链**上的等价物：这里的钩子不是测试
     * 直接调 `iv_status` 挂的，而是经 `iv_report_cfg_t` 在 `iv_report_open()` 时落下的。
     * 帧字节也顺带验一次：0xC1 的 data = 0x01（**非 JSON**，架构 §18.2）。
     * 2026-10-28 追加：箱门/事件这类**采集板主动上报**到达后，本层现在会自动
     * 「①本拍催 0xE1 取全量 → ②镜像刷新完立刻补一帧正常上报」（用户原话见 c04d
     * 抬头），本块把这条链在装配层走通 —— 门开是安全事故，不能等 300 s 周期上报。
     * ⚠ 两点：①本块会把镜像 DS 从 1 改成 2；②本块产生的那帧上报必须从平台 socket
     * 里**收干净** —— c04b 断言"平台收到的第一帧是继电器补报"，留一帧在这里会让
     * 它错位。（不为了让新用例方便就去改旁边已有的黄金断言。） */
    {
        static const char json_ds2[] =
            "{\"V\":234.17,\"A\":0.50,\"H\":50.01,\"T\":31.33,\"DS\":2,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":1,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":234.17,\"CHV2\":234.17,\"CHV3\":234.17,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t     up[IV_FRAME_MAX];
        uint8_t     want[IV_FRAME_MAX];
        char        got[512];
        char        rx[4096];
        char        scratch[512];
        const char *seg  = NULL;
        size_t      sl   = 0u;
        size_t      rn   = 0u;
        size_t      used = 0u;
        size_t      wn;
        size_t      gn;
        uint64_t    reps0;
        int         un;
        int         j;

        g_door_hits = 0;
        g_door_last = -1;
        reps0 = iv_report_reports(&rp);
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR, "\x01", 1u,
                            up, sizeof up);
        chk(un > 0, "c02c upstream 0xC1 frame built");
        chk(write(master, up, (size_t)un) == un, "c02c feed 0xC1 door-open event");

        /* ① 事件到达的**这一拍**就得把 0xE1 发出去（不是等 30 s 轮询）。
         * 这里手动推进一格而不走 pump()：pump() 会先把 master 上的下行字节抽掉，
         * 那样就抓不到这一帧了。 */
        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        gn = drain(master, got, sizeof got, 200);
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY,
                                    "\x00", 1u, want, sizeof want);
        chk(wn > 0u, "c02c expected 0xE1 frame built");
        chk(gn == wn && memcmp(got, want, wn) == 0,
            "c02c door event -> immediate 0xE1 in the same step");
        chk(rp.st.valid.DS == 1u && rp.st.data.DS == 2, "c02c mirror DS=2 after 0xC1");
        chk(g_door_hits == 1, "c02c door hook fired once via cfg wiring");
        chk(g_door_last == 2, "c02c door hook got DS=2");
        chk(rp.want_report == 1u, "c02c report armed, waiting for the full mirror");
        chk(iv_report_reports(&rp) == reps0,
            "c02c nothing reported before the poll answer");

        /* ② 采集板回全量（门开 DS=2）→ 镜像刷新 → 立刻补一帧**正常上报** */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json_ds2,
                            (uint16_t)(sizeof json_ds2 - 1u), up, sizeof up);
        chk(un > 0, "c02c upstream 0xE1 (door open) built");
        chk(write(master, up, (size_t)un) == un, "c02c feed 0xE1 door-open answer");
        pump(&rp, master, 2);
        chk(rp.want_report == 0u, "c02c want_report consumed by the poll answer");
        chk(iv_report_reports(&rp) == reps0 + 1u,
            "c02c door event produced exactly one report");

        for (j = 0; j < 50 && iv_report_queue_count(&rp) > 0u; j++)
            pump(&rp, master, 1);
        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(used > 0u, "c02c platform got the door report");
        if (used > 0u) {
            chk(strncmp(seg, want_hdr, sizeof want_hdr - 1u) == 0,
                "c02c door report is the normal state frame");
            chk(strstr(seg, "DS=2;") != NULL, "c02c door report carries DS=2");
        }
    }

    /* ---- c04b 继电器动作后「催 0xE1 → 镜像刷新 → 立刻补报」 ----
     * 用户原话：「点击断开时，设备继电器断开，但是设备没有上报实时状态，每次
     * 继电器状态改变时，设备需要上报」。链路事实：`0xC4`（平台）与网页
     * `relay.set` 都只下发一条 `0xD1`，而 `0xD1` 的应答**不刷新镜像**
     * （见 `iv_report_send_cmd()`）⇒ 动作后必须再要一次 0xE1，拿到新状态后
     * 立刻补一帧上报，否则平台得等下一个 30 s 周期才看到继电器变了。
     * ⚠ 放在 c02c 之后：这里复用的镜像语义是「DS 已被 0xC1 改成 2」。 */
    {
        static const char json[] =
            "{\"V\":234.17,\"A\":0.50,\"H\":50.01,\"T\":31.33,\"DS\":2,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":0,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":234.17,\"CHV2\":234.17,\"CHV3\":234.17,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t  up[IV_FRAME_MAX];
        char     rx[4096];
        char     scratch[512];
        const char *seg  = NULL;
        size_t   sl   = 0u;
        size_t   rn   = 0u;
        size_t   used = 0u;
        int      un;
        int      j;
        uint64_t reps0;

        reps0 = iv_report_reports(&rp);

        /* 动作路径（装配层 `on_plat_relay()` 里的两行就是这两句） */
        iv_report_request_poll(&rp);
        iv_report_request_report_after_poll(&rp);
        chk(rp.want_report == 1u, "c04b want_report armed");

        /* 这一拍必须真的把 0xE1 发出去（request_poll 只拨时刻表，不直接发帧） */
        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        un = (int)drain(master, scratch, sizeof scratch, 200);
        chk(un > 0, "c04b 0xE1 re-issued after relay action");

        /* 采集板回一份「继电器 1 已断开」的全量状态 */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, json,
                            (uint16_t)(sizeof json - 1u), up, sizeof up);
        chk(un > 0, "c04b upstream 0xE1 (relay off) built");
        chk(write(master, up, (size_t)un) == un, "c04b feed 0xE1 relay-off");
        pump(&rp, master, 2);

        chk(rp.st.valid.RELAY == 1u && rp.st.data.RELAY[0] == 0,
            "c04b mirror RELAY1=0 after the poll");
        chk(rp.want_report == 0u, "c04b want_report consumed exactly once");
        chk(iv_report_reports(&rp) == reps0 + 1u,
            "c04b poll produced exactly one extra report");

        for (j = 0; j < 50 && iv_report_queue_count(&rp) > 0u; j++)
            pump(&rp, master, 1);
        chk(iv_report_queue_count(&rp) == 0u, "c04b forced report delivered");

        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(used > 0u, "c04b platform got the forced report");
        if (used > 0u)
            chk(strstr(seg, "RELAY=0,1,1;") != NULL,
                "c04b forced report carries the new relay state");
    }

    /* ---- c04c 采集板**自己**改了继电器 → 也要立刻上报；同值不重复报 ----
     * 2026-10-27 真机实证：平台点「通道3 重启」，采集板把 RELAY3 改成 2（断电），
     * 十几秒后自己又改回 1（上电）；回来这一下没有任何命令，`want_report` 早被
     * 消费掉 ⇒ 平台界面一直停在"断电"。所以判据必须是"镜像里的 RELAY 变了"，
     * 而不是"我们刚发过命令"。 */
    {
        static const char on[] =
            "{\"V\":234.17,\"A\":0.50,\"H\":50.01,\"T\":31.33,\"DS\":2,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":1,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":234.17,\"CHV2\":234.17,\"CHV3\":234.17,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t  up[IV_FRAME_MAX];
        char     rx[4096];
        char     scratch[512];
        const char *seg  = NULL;
        size_t   sl   = 0u;
        size_t   rn   = 0u;
        size_t   used = 0u;
        int      un;
        int      j;
        uint64_t reps0;

        reps0 = iv_report_reports(&rp);

        /* 主动喂一帧 0xE1（模拟轮询应答），继电器 1 由 0 回到 1 */
        iv_report_request_poll(&rp);
        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        (void)drain(master, scratch, sizeof scratch, 200);

        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, on,
                            (uint16_t)(sizeof on - 1u), up, sizeof up);
        chk(un > 0, "c04c upstream 0xE1 (relay back on) built");
        chk(write(master, up, (size_t)un) == un, "c04c feed 0xE1 relay-on");
        pump(&rp, master, 2);

        chk(rp.st.data.RELAY[0] == 1, "c04c mirror RELAY1 back to 1");
        chk(iv_report_reports(&rp) == reps0 + 1u,
            "c04c board-side relay change forced one report");

        for (j = 0; j < 50 && iv_report_queue_count(&rp) > 0u; j++)
            pump(&rp, master, 1);
        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(used > 0u, "c04c platform got the relay-change report");
        if (used > 0u)
            chk(strstr(seg, "RELAY=1,1,1;") != NULL,
                "c04c report carries RELAY=1,1,1");

        /* 同值再来一帧：状态没变就不许再报（否则每 30 s 一条，纯噪声） */
        reps0 = iv_report_reports(&rp);
        iv_report_request_poll(&rp);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        (void)drain(master, scratch, sizeof scratch, 200);
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, on,
                            (uint16_t)(sizeof on - 1u), up, sizeof up);
        chk(un > 0 && write(master, up, (size_t)un) == un,
            "c04c feed identical 0xE1 again");
        pump(&rp, master, 3);
        chk(iv_report_reports(&rp) == reps0, "c04c same state => no extra report");
    }

    /* ---- c04d 采集板 0xC2 事件（告警发生）→ 立刻取全量 → 组包走正常上报 ----
     * 用户原话（2026-10-28）：「参考采集板的事件上报，当接收到采集板事件后，主板
     * 需要立刻获取采集板信息，然后组包上报，走正常上报报文」。三点必须同时成立：
     *   ① 事件到达的**同一拍**就下发 0xE1（不是等 30 s 轮询）；
     *   ② 0xE1 应答回来**之前不许**上报 —— 事件帧只有一个 TAG，拿它组包就是半张
     *      状态（平台上表现为"告警响了，但电压电流还是上一条"）；
     *   ③ 上报走**正常上报报文**（`##` 全量状态帧），字段来自刚刷新完的镜像。
     * 另加一条用户 2026-10-28 的口径：**不解析具体事件**，检测到 0xC2 就动作
     * ⇒ 本块的事件载荷带一个不认识的 TAG（`zz`），镜像不许因此变化，但催查询与
     * 补报照样发生。③ 的判据是"上报里 T=49.75"（事件里根本没有 T），证明这一帧
     * 是**取完全量之后**组的，而不是拿事件增量凑的。 */
    {
        /* ⚠ 事件载荷故意用**不认识的 TAG**（`zz`）：判据只有命令字 0xC2，
         * 不解析事件内容（用户 2026-10-28 明确口径）。见下面那条"mirror 不变"断言。 */
        static const char ev[]   = "{\"key\":\"mcuEvent\",\"zz\":7}";
        static const char full[] =
            "{\"V\":240.10,\"A\":0.50,\"H\":50.01,\"T\":49.75,\"DS\":2,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":1,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":240.10,\"CHV2\":240.10,\"CHV3\":240.10,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t     up[IV_FRAME_MAX];
        uint8_t     want[IV_FRAME_MAX];
        char        got[512];
        char        rx[4096];
        char        scratch[512];
        const char *seg  = NULL;
        size_t      sl   = 0u;
        size_t      rn   = 0u;
        size_t      used = 0u;
        size_t      wn;
        size_t      gn;
        uint64_t    reps0;
        uint64_t    polls0;
        int         un;
        int         j;

        reps0  = iv_report_reports(&rp);
        polls0 = rp.polls;
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_EVENT, ev,
                            (uint16_t)(sizeof ev - 1u), up, sizeof up);
        chk(un > 0, "c04d upstream 0xC2 event built");
        chk(write(master, up, (size_t)un) == un, "c04d feed 0xC2 mcuEvent");

        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        gn = drain(master, got, sizeof got, 200);
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_QUERY,
                                    "\x00", 1u, want, sizeof want);
        chk(rp.polls == polls0 + 1u, "c04d event triggered one 0xE1 request");
        chk(wn > 0u && gn == wn && memcmp(got, want, wn) == 0,
            "c04d 0xE1 goes out in the same step as the event");
        chk(rp.st.data.T > 31.32f && rp.st.data.T < 31.34f,
            "c04d unknown event TAG ignored by the mirror (content not parsed)");
        chk(iv_report_reports(&rp) == reps0,
            "c04d no report until the full mirror is back");
        chk(rp.want_report == 1u, "c04d report pending the 0xE1 answer");

        /* 采集板回全量（T=49.75）→ 镜像刷新 → 一帧正常上报 */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, full,
                            (uint16_t)(sizeof full - 1u), up, sizeof up);
        chk(un > 0, "c04d upstream 0xE1 answer built");
        chk(write(master, up, (size_t)un) == un, "c04d feed the 0xE1 answer");
        pump(&rp, master, 2);
        chk(rp.want_report == 0u, "c04d want_report consumed");
        chk(iv_report_reports(&rp) == reps0 + 1u,
            "c04d event produced exactly one report");

        for (j = 0; j < 50 && iv_report_queue_count(&rp) > 0u; j++)
            pump(&rp, master, 1);
        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(used > 0u, "c04d platform got a normal ## report");
        if (used > 0u) {
            chk(strncmp(seg, want_hdr, sizeof want_hdr - 1u) == 0,
                "c04d the report is the standard state frame (no new format)");
            chk(strstr(seg, "T=49.75;") != NULL,
                "c04d report carries the post-poll value (not the event TAG)");
            chk(strstr(seg, "V=240.10;") != NULL,
                "c04d report carries the full snapshot");
        }
    }

    /* ---- c04e 事件撞上"0xE1 还在飞" → 催报不许退回 30 s 轮询 ----
     * 现场最容易踩的时序：轮询 30 s 一拍，而 `0xE1` 的在飞窗口是 2 s ×3 发
     * （最长 6 s）⇒ 事件有约 20% 的概率撞在"同 cmd 在飞"上（IV_EBUSY）。
     * 若这时按常规把 `next_poll` 推进 30 s，这帧补报就变成"有时立刻、有时半分钟"。
     * 本块钉住兜底：撞 IV_EBUSY 时 `next_poll` 只推到 `IV_REPORT_POLL_RETRY_MS`。 */
    {
        static const char ev[]   = "{\"key\":\"mcuEvent\",\"T\":50.5}";
        static const char full[] =
            "{\"V\":240.10,\"A\":0.50,\"H\":50.01,\"T\":51.25,\"DS\":2,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"0.00\",\"RELAY1\":1,\"RELAY2\":1,\"RELAY3\":1,"
            "\"CHV1\":240.10,\"CHV2\":240.10,\"CHV3\":240.10,"
            "\"CHA1\":0.0,\"CHA2\":0.0,\"CHA3\":0.0,"
            "\"POWER1\":0.0,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":0.0,\"ELEC2\":0.0,\"ELEC3\":0.0}";
        uint8_t     up[IV_FRAME_MAX];
        char        rx[4096];
        char        scratch[512];
        const char *seg  = NULL;
        size_t      sl   = 0u;
        size_t      rn   = 0u;
        size_t      used = 0u;
        uint64_t    reps0;
        uint64_t    polls0;
        int         un;
        int         j;

        /* 先欠一笔 0xE1 在飞（**不**喂应答） */
        iv_report_request_poll(&rp);
        (void)drain(master, scratch, sizeof scratch, 0);
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        (void)drain(master, scratch, sizeof scratch, 200);
        polls0 = rp.polls;
        reps0  = iv_report_reports(&rp);

        /* 事件到达：这一拍必然发不出 0xE1（同 cmd 在飞） */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_EVENT, ev,
                            (uint16_t)(sizeof ev - 1u), up, sizeof up);
        chk(un > 0 && write(master, up, (size_t)un) == un, "c04e feed 0xC2 while E1 in flight");
        g_now += STEP_MS;
        (void)iv_report_step(&rp, g_now);
        chk(rp.polls == polls0, "c04e collision: no new 0xE1 this step (IV_EBUSY)");
        chk(rp.want_report == 1u, "c04e report still pending after the collision");
        chk(rp.next_poll > g_now &&
            rp.next_poll - g_now <= (uint64_t)IV_REPORT_POLL_RETRY_MS,
            "c04e poll re-armed within IV_REPORT_POLL_RETRY_MS (not the 30 s period)");
        chk(iv_report_reports(&rp) == reps0, "c04e still no report (no fresh mirror)");

        /* 在飞那一笔的应答到达 → 镜像刷新 → 这一帧补报落地（只一帧） */
        un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_QUERY, full,
                            (uint16_t)(sizeof full - 1u), up, sizeof up);
        chk(un > 0 && write(master, up, (size_t)un) == un, "c04e feed the in-flight answer");
        pump(&rp, master, 2);
        chk(rp.want_report == 0u, "c04e want_report consumed by the in-flight answer");
        chk(iv_report_reports(&rp) == reps0 + 1u, "c04e exactly one report");

        for (j = 0; j < 50 && iv_report_queue_count(&rp) > 0u; j++)
            pump(&rp, master, 1);
        rn   = drain(cs, rx, sizeof rx, 1000);
        used = next_report_frame(rx, rn, 0u, &seg, &sl);
        chk(used > 0u, "c04e platform got the report after the collision");
        if (used > 0u)
            chk(strstr(seg, "T=51.25;") != NULL,
                "c04e report carries the post-poll value");
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
        /* 9：c04 1 + c02c 1（门开事件补报）+ c04b 1（动作后补报）
         *    + c04c 1（采集板自己改继电器）+ c04d 1（0xC2 事件补报）
         *    + c04e 1（撞在飞后仍补报）+ 这里 3 */
        chk(iv_report_reports(&rp) == 9u, "c05 total reports=9");
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
            chk(strstr(rx, "\"crc\":\"b2\"") != NULL,
                "c07 crc = CRC8(110a00101E3) = b2");
            chk(strstr(rx, "\"mod\":\"E2E-MOD\"") != NULL, "c07 mod");
            chk(strstr(rx, "\"sv\":\"E2E-SV\"") != NULL, "c07 sv");
            chk(strstr(rx, "\"type\":\"0a00\"") != NULL, "c07 devtype");
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
            /* 0x99 是**从没实现过**的命令 ⇒ 按 2026-10-20 定下的口径必须回
             * 0x74(IV_PROTO_ERR_EXEC)，不能回 0x01 —— 回 OK 等于对平台撒谎
             * （"设置成功"而设备一分没动），现场排查会一路怀疑网络。
             * 口径原文见 iv_proto.c 的 dispatch() 未注册分支。 */
            if (!((uint8_t)rx[17] == 0x01u && (uint8_t)rx[18] == IV_PROTO_ERR_EXEC))
                fprintf(stderr, "  c08 DEBUG len=%u data0=0x%02X\n",
                        (unsigned)(uint8_t)rx[17], (unsigned)(uint8_t)rx[18]);
            chk((uint8_t)rx[17] == 0x01u && (uint8_t)rx[18] == IV_PROTO_ERR_EXEC,
                "c08 ACK data[0]=0x74 (unimplemented cmd must not lie OK)");
            /* 22 字节帧：crc 在 [19]，帧尾在 [20][21] */
            chk((uint8_t)rx[20] == 0xFFu && (uint8_t)rx[21] == 0xFFu,
                "c08 frame tail");
        }
    }

    /* ---- c09 串口断开后镜像失效；新 pty 重连并重查 E1 ---- */
    (void)close(master);
    master = -1;
    g_now += cfg.poll_ms;
    (void)iv_report_step(&rp, g_now);
    chk(iv_report_serial_fd(&rp) < 0, "c09 serial detached");
    chk(rp.st.valid.T == 0u && rp.st.valid.V == 0u,
        "c09 stale mirror cleared");
    master = pty_pair(slave_path, sizeof slave_path);
    chk(master >= 0, "c09 replacement pty created");
    if (master >= 0) {
        char rx[64];
        size_t rn;
        g_now += IV_REPORT_SERIAL_RETRY_MS;
        (void)iv_report_step(&rp, g_now);
        rn = drain(master, rx, sizeof rx, 500);
        chk(iv_report_serial_fd(&rp) >= 0 && rn >= 8u,
            "c09 reconnect sent fresh E1");

        /* S4.5：串口断开走 `serial_drop()` → `iv_status_init()` 会清掉门开钩子
         * ⇒ 装配层必须重挂。这里重新喂一次 0xC1，钩子仍必须触发；
         * 否则现场表现就是"串口断一次以后，再开门就不抓拍了"。 */
        g_door_hits = 0;
        {
            uint8_t up[IV_FRAME_MAX];
            int     un = iv_frame_build(IV_FRAME_HEAD_UP, IV_FRAME_CMD_DOOR,
                                        "\x01", 1u, up, sizeof up);
            chk(un > 0, "c09 upstream 0xC1 frame built");
            chk(write(master, up, (size_t)un) == un, "c09 feed 0xC1 after reconnect");
            pump(&rp, master, 2);
        }
        chk(g_door_hits == 1, "c09 door hook re-armed after serial reconnect");
    }

    /* ---- c10 收口 ---- */
    iv_report_close(&rp);
    chk(iv_report_is_open(&rp) == 0, "c10 closed");

    if (cs >= 0)
        (void)close(cs);
    (void)close(ls);
    if (master >= 0)
        (void)close(master);
    rm_tmpdir(qdir);

    if (g_fail == 0)
        printf("test_e2e_report passed "
               "(poll/mirror/power-0xD1/door-event/report/queue/replay/query/ack/"
               "relay-forced-report/relay-change-report/board-event-forced-report/"
               "serial-reconnect)\n");
    else
        printf("test_e2e_report FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
