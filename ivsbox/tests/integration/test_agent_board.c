/*
 * iv_agent 装配层「控制板（采集板）通信参数」集成用例
 * （功能开发计划 M2-S2.6 的对外出口：board.status）
 *
 * ============================ 它验的是什么 ============================
 * `tests/unit/test_status.c` 证明的是 **镜像与解析**对不对（0xE1 应答怎么填镜像、
 * 0xC1/0xC2 怎么增量）。`tests/integration/test_e2e_report.c` 证明的是**整条上报链**
 * （poll → 应答 → 镜像 → 组报文 → 入队 → 发到假平台）。
 *
 * 本用例补的是中间那段**给现场看的出口**：
 *
 *   b01 没有数据时 `board.status` 长什么样 —— 串口已开、`frames=0`、
 *       `age_ms=null`、运行参数**全是 null**（不是 0）；
 *   b02 真喂一帧 0xE1 应答（经 pty 走真串口读路径）后，运行参数/阈值/三路继电器
 *       都被解出来，`frames>=1` 且 `age_ms` 是数字；
 *   b03 `0xC1` 门开事件把 `DS` 顶成 2（1=关 / 2=开）且 `door_events` 加一；
 *   b04 `0xC2` 增量事件只覆盖它带的那一个字段，别的字段保持原值；
 *   b05 守卫：没有 agent 时必须报错而不是崩，未知方法名必须被拒。
 *
 * ============================ 为什么必须用 pty ============================
 * `board.status` 的价值全在"链路统计"上（`frames`/`crc_err`/`age_ms`）。这些数字
 * 只有**字节真的从串口读进来**才会动；直接改 `rp->st` 只能测到 JSON 拼接，测不到
 * "串口 → 成帧 → 镜像"这段。故用 `posix_openpt()` 造一对 pty 当假采集板
 * （与 `test_e2e_report.c` 同一手法），走 `iv_report_step()` 的真读路径。
 *
 * ============================ 反向证伪点（每条对应一个真缺陷） ============================
 *   b01 未知量若写成 0 而不是 null ⇒ `"V":0` 会让现场以为"输入电压 0V"（本用例
 *       显式否定 `"V":0`，和 `"V":null` 的断言成对）；
 *   b02 阈值/三路继电器若漏接 ⇒ `limit.hv` / `relay` 的断言失败；
 *   b03 `DS` 若按参考项目做 1↔2 映射 ⇒ `DS:2` 断言失败（本项目口径 1=关/2=开）。
 */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_ctl.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

/* iv_agent_t ≈ 35 KB ＋ ctl 响应缓冲 ⇒ 一律静态区 */
static iv_agent_t   g_agent;
static iv_ctl_ctx_t g_ctx;
static int          g_master = -1;
static int          g_fail;
static uint64_t     g_now = 1000u;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void log_discard(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

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

/* 跑一条 ctl 请求，返回响应文本（静态缓冲；单线程够用）。 */
static const char *ctl(const char *req)
{
    static char out[IV_CTL_RSP_MAX];
    size_t      n = 0;
    int         rc;

    out[0] = '\0';
    rc = iv_ctl_handle(&g_ctx, req, strlen(req), out, sizeof(out), &n);
    if (rc != IV_OK) {
        fprintf(stderr, "ctl(%s) rc=%d\n", req, rc);
        out[0] = '\0';
    }
    return out;
}

/* 推一拍（400 ms 的整倍，避免与 200 ms 总线节拍错位） */
static void step(uint32_t adv)
{
    g_now += adv;
    (void)iv_agent_step(&g_agent, g_now);
}

/* 假采集板 → 主板：造一帧上行（同步字 0x0F0F ＋ CRC ＋ 帧尾）写进 pty */
static void mcu_send(uint8_t cmd, const char *payload, uint16_t len)
{
    uint8_t up[IV_FRAME_MAX];
    int     un;

    un = iv_frame_build(IV_FRAME_HEAD_UP, cmd, payload, len, up, sizeof up);
    chk(un > 0, "frame built");
    if (un <= 0)
        return;
    chk(write(g_master, up, (size_t)un) == un, "frame written to fake mcu");
}

/* 从 pty 主端收字节（主板发出来的下行帧都落在这里）。返回收到的字节数。 */
static size_t drain(int fd, char *out, size_t cap, int ms)
{
    struct pollfd p;
    size_t        n = 0;
    int           rc;

    p.fd     = fd;
    p.events = POLLIN;
    for (;;) {
        rc = poll(&p, 1, ms);
        if (rc <= 0)
            break;
        {
            ssize_t got = read(fd, out + n, cap - n);

            if (got <= 0)
                break;
            n += (size_t)got;
            if (n >= cap)
                break;
        }
        ms = 20; /* 后续用短超时把同一拍里的剩余字节一起收掉 */
    }
    return n;
}

static int agent_up(const char *slave)
{
    iv_agent_cfg_t cfg;

    iv_agent_cfg_default(&cfg);
    cfg.gps_path     = "";      /* 本用例不碰 GPS 串口 */
    cfg.serial_path  = slave;   /* 采集板 = 假 pty 从端 */
    cfg.queue_dir    = "/tmp";
    cfg.poll_ms      = 100u;    /* 小周期：让 0xE1 轮询尽快下发 */
    cfg.report_ms    = 3600u * 1000u;
    cfg.heartbeat_ms = 3600u * 1000u;
    iv_agent_init(&g_agent);
    return iv_agent_open(&g_agent, &cfg, NULL, g_now);
}

int main(void)
{
    char slave[128];
    int  m;

    iv_log_set_sink(log_discard, NULL);
    memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.agent = &g_agent;

    m = pty_pair(slave, sizeof slave);
    if (m < 0) {
        printf("test_agent_board SKIP (no pty available)\n");
        return 0;
    }
    g_master = m;

    /* ---- b01 装配与"还没数据"的呈现 ---- */
    chk(agent_up(slave) == IV_OK, "b01 agent open");
    step(0u);
    {
        const char *js = ctl("{\"m\":\"board.status\"}");

        chk(js[0] != '\0', "b01 board.status answered");
        chk(strstr(js, "\"open\":true") != NULL, "b01 serial opened on pty");
        chk(strstr(js, "\"frames\":0") != NULL, "b01 no frames yet");
        chk(strstr(js, "\"age_ms\":null") != NULL, "b01 no bytes yet -> age_ms null");
        chk(strstr(js, "\"V\":null") != NULL, "b01 unknown voltage is JSON null");
        chk(strstr(js, "\"V\":0") == NULL, "b01 voltage must NOT be faked as 0");
        chk(strstr(js, "\"DS\":null") != NULL, "b01 unknown door state is JSON null");
        chk(strstr(js, "\"hv\":null") != NULL, "b01 unknown threshold is JSON null");
        chk(strstr(js, "NaN") == NULL, "b01 no NaN leaks into JSON");
    }

    /* ---- b02 真喂一帧 0xE1 应答：运行参数 / 阈值 / 三路继电器 ---- */
    {
        static const char json[] =
            "{\"V\":234.17,\"A\":0.50,\"H\":50.01,\"T\":31.33,\"DS\":1,"
            "\"P\":0,\"SPD\":1,\"PA\":1,\"PV\":1,\"APOWER\":\"0.00\","
            "\"AKW\":\"1.25\",\"hv\":280,\"lv\":180,\"ov\":10,\"tu\":70,"
            "\"tl\":-10,\"hu\":90,\"hl\":20,\"sl\":1,\"ld\":30,"
            "\"RELAY1\":1,\"RELAY2\":2,\"RELAY3\":1,"
            "\"CHV1\":234.17,\"CHV2\":0.00,\"CHV3\":233.90,"
            "\"CHA1\":0.10,\"CHA2\":0.00,\"CHA3\":0.00,"
            "\"POWER1\":23.4,\"POWER2\":0.0,\"POWER3\":0.0,"
            "\"ELEC1\":1.5,\"ELEC2\":0.0,\"ELEC3\":0.0}";

        /* 先推一拍让 0xE1 轮询先下发（frame 必须在**有在飞事务**时到达，
         * 否则应答会被当成"无配对上行"交给 upstream 而忽略 —— 这是 iv_link 的
         * 既有语义，不是缺陷）。再把应答写进 pty、推一拍读回来。 */
        step(400u);
        mcu_send(IV_FRAME_CMD_QUERY, json, (uint16_t)(sizeof json - 1u));
        step(400u); /* 读串口＋成帧＋事务完成 */
        step(400u); /* 留一拍给事件/统计落定 */
    }
    {
        const char *js = ctl("{\"m\":\"board.status\"}");

        chk(strstr(js, "\"V\":234.17") != NULL, "b02 voltage decoded");
        chk(strstr(js, "\"T\":31.33") != NULL, "b02 temperature decoded");
        chk(strstr(js, "\"DS\":1") != NULL, "b02 door closed = DS 1 (1=关)");
        chk(strstr(js, "\"APOWER\":\"0.00\"") != NULL, "b02 APOWER kept as string");
        chk(strstr(js, "\"AKW\":\"1.25\"") != NULL, "b02 AKW kept as string");
        chk(strstr(js, "\"hv\":280") != NULL, "b02 threshold hv decoded");
        chk(strstr(js, "\"tl\":-10") != NULL, "b02 negative low temperature kept");
        chk(strstr(js, "\"relay\":[") != NULL, "b02 relay array present");
        chk(strstr(js, "\"state\":2") != NULL, "b02 RELAY2 open/closed decoded");
        chk(strstr(js, "\"frames\":0") == NULL, "b02 frames counted after a real frame");
        chk(strstr(js, "\"age_ms\":null") == NULL, "b02 age_ms becomes numeric");
        chk(strstr(js, "NaN") == NULL, "b02 no NaN leaks into JSON");
    }

    /* ---- b03 0xC1 门开事件：DS → 2，door_events +1 ---- */
    {
        static const char d = 0x01;

        mcu_send(IV_FRAME_CMD_DOOR, &d, 1u);
        step(400u);
        step(400u);
    }
    {
        const char *js = ctl("{\"m\":\"board.status\"}");

        chk(strstr(js, "\"DS\":2") != NULL, "b03 door open event -> DS 2");
        chk(iv_agent_door_events(&g_agent) >= 1u, "b03 door event counted once");
        /* 与 C1 同时进来的一帧不应把别的字段清掉（镜像只做增量） */
        chk(strstr(js, "\"V\":234.17") != NULL, "b03 other fields survive an event");
    }

    /* ---- b04 0xC2 增量事件：只覆盖它带的字段 ---- */
    {
        static const char ev[] = "{\"key\":\"mcuEvent\",\"T\":26.5}";

        mcu_send(IV_FRAME_CMD_EVENT, ev, (uint16_t)(sizeof ev - 1u));
        step(400u);
        step(400u);
    }
    {
        const char *js = ctl("{\"m\":\"board.status\"}");

        chk(strstr(js, "\"T\":26.5") != NULL, "b04 incremental event overwrote T");
        chk(strstr(js, "\"V\":234.17") != NULL, "b04 V untouched by the event");
    }

    /* ---- b05 守卫 ----
     * 注意 `iv_ctl` 的错误是**正常响应**：`rsp_err()` 会写出 `{"ok":false,...}` 并
     * 返回 IV_OK；只有"方法名不认识"才返回负码（IV_ENOENT）。这两条别混着断言。 */
    {
        iv_ctl_ctx_t bare;
        char         out[256];
        size_t       n = 0;
        int          rc;

        memset(&bare, 0, sizeof bare);
        out[0] = '\0';
        rc = iv_ctl_handle(&bare, "{\"m\":\"board.status\"}", 22u, out, sizeof out, &n);
        chk(rc == IV_OK, "b05 no agent -> still a normal response (no crash)");
        chk(strstr(out, "\"ok\":false") != NULL, "b05 no agent -> ok:false");
        chk(strstr(out, "agent unavailable") != NULL, "b05 no agent -> readable reason");

        out[0] = '\0';
        rc = iv_ctl_handle(&g_ctx, "{\"m\":\"board.nope\"}", 20u, out, sizeof out, &n);
        chk(rc != IV_OK, "b05 unknown method rejected");
    }

    /* ---- b06 relay.set：继电器号 → 端口换算 ＋ 0xD1 下行字节黄金 ----
     * 这条是本用例里唯一会**往采集板发动作**的路径，所以判据不看返回值，看**线上字节**：
     * `{"port":N,"status":S}` 由 `iv_recovery_build_payload()` 逐字生成，与 S4.4 自愈
     * 用的是同一份实现（两处漂移会变成"人工能用、自愈不能用"）。 */
    {
        const char *js;
        static const char on1[]  = "{\"port\":0,\"status\":1}";
        static const char off3[] = "{\"port\":2,\"status\":0}";
        uint8_t           want[IV_FRAME_MAX];
        char              got[96];
        size_t            wn;
        size_t            gn;

        (void)drain(g_master, got, sizeof got, 20); /* 先把前面的 0xE1 轮询清掉 */

        js = ctl("{\"m\":\"relay.set\",\"p\":{\"ch\":1,\"on\":true}}");
        chk(strstr(js, "\"ok\":true") != NULL, "b06 relay.set accepted");
        chk(strstr(js, "\"port\":0") != NULL, "b06 relay 1 -> port 0");
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_POWER, on1,
                                    (uint16_t)(sizeof on1 - 1u), want, sizeof want);
        gn = drain(g_master, got, sizeof got, 300);
        chk(wn > 0u && gn == wn, "b06 0xD1 frame length for relay 1 ON");
        chk(gn == wn && memcmp(got, want, wn) == 0, "b06 0xD1 bytes: relay1 -> port 0 status 1");

        /* 第二条必须等第一条的 2 s 应答窗口过去：`iv_link` 同 cmd 只允许一笔在飞，
         * 这是 0xD1 的既有约束（S4.4 用待发 FIFO 串行化整排 7 条正是为此），
         * 不是新缺陷 —— 这里如实把它演一遍。 */
        (void)drain(g_master, got, sizeof got, 20);
        step(2500u); /* 推过 0xD1 的 2 s 超时窗口，在飞事务被清掉 */
        (void)drain(g_master, got, sizeof got, 20);
        js = ctl("{\"m\":\"relay.set\",\"p\":{\"ch\":3,\"on\":false}}");
        chk(strstr(js, "\"ok\":true") != NULL, "b06 relay.set off accepted");
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_POWER, off3,
                                    (uint16_t)(sizeof off3 - 1u), want, sizeof want);
        gn = drain(g_master, got, sizeof got, 300);
        chk(gn == wn && memcmp(got, want, wn) == 0, "b06 0xD1 bytes: relay3 -> port 2 status 0");

        /* 守卫：路号越界、缺 on、没有 agent —— 都必须明确拒绝，且**不往线上发字节** */
        /* 守卫段之前先把线放空。0xD1 还在 2 s 窗口里时，任何"该被拒"的请求都会
         * 先被 iv_link 的 IV_EBUSY 挡成 ok:false —— 那样"越界被拒"与"忙被拒"
         * 分不开，ch=4 这类断言就失去牙口（只证明了忙，没证明守卫）。 */
        step(2500u);
        (void)drain(g_master, got, sizeof got, 20);
        chk(strstr(ctl("{\"m\":\"relay.set\",\"p\":{\"ch\":0,\"on\":true}}"), "\"ok\":false") != NULL,
            "b06 ch=0 rejected");
        chk(strstr(ctl("{\"m\":\"relay.set\",\"p\":{\"ch\":4,\"on\":true}}"), "\"ok\":false") != NULL,
            "b06 ch=4 rejected");
        chk(strstr(ctl("{\"m\":\"relay.set\",\"p\":{\"ch\":2}}"), "\"ok\":false") != NULL,
            "b06 missing on rejected");
        gn = drain(g_master, got, sizeof got, 120);
        chk(gn == 0u, "b06 rejected requests send nothing on the wire");
    }

    /* ---- b06b 阈值下发（0xF1）：载荷是 JSON ＋ 帧字节黄金 ----
     * 判据同样看**线上字节**。载荷必须是 JSON —— Excel《配置、控制》把它写成
     * "9 参数 × 2B = 18B"二进制，而现网单片机按 JSON 解析，用户 2026-10-08 已裁定
     * 取 JSON、Excel 该行作废。照二进制发，帧长/CRC 全都自洽，现场只会表现为
     * "阈值怎么改都不生效"，所以这里钉死黄金串。 */
    {
        iv_thresh_t       t;
        uint8_t           want[IV_FRAME_MAX];
        char              got[256];
        static const char json[] = "{\"hv\":250,\"tu\":55}";
        size_t            wn;
        size_t            gn = 0u;
        int               i;

        memset(&t, 0, sizeof(t));
        t.hv = 250; /* elec.volt_high */
        t.tu = 55;  /* sensor.temp_high */

        step(2500u);                               /* 放过上一条 0xD1 的应答窗口 */
        (void)drain(g_master, got, sizeof got, 20); /* 清掉在飞的 0xE1 */

        chk(iv_agent_apply_thresh(&g_agent, &t, g_now) == IV_OK, "b06b thresh queued");

        /* 撞上在飞的 0xE1 时 `thresh_drive()` 会推后 1 s 重试，所以最多等 3 轮 ——
         * 这本身就是"改配置那一刻不一定发得出去"的既有事实，不该断言一次必成。 */
        wn = (size_t)iv_frame_build(IV_FRAME_HEAD_DOWN, IV_FRAME_CMD_THRESH, json,
                                    (uint16_t)(sizeof json - 1u), want, sizeof want);
        for (i = 0; i < 3 && gn == 0u; i++) {
            step(1100u);
            gn = drain(g_master, got, sizeof got, 300);
        }
        chk(wn > 0u && gn == wn, "b06b 0xF1 frame length");
        chk(gn == wn && memcmp(got, want, wn) == 0,
            "b06b 0xF1 bytes = JSON {\"hv\":250,\"tu\":55} (not 18-byte binary)");

        /* 同值再登记一次：早返回，**不得再发一帧 0xF1**（否则改别的键会重复下发）。
         * 判据只比对"这一帧是不是 0xF1"——线上随时可能插进一条 0xE1 轮询，
         * 断言"线上一个字节都没有"会变成对轮询节拍的断言，那是另一件事。 */
        (void)drain(g_master, got, sizeof got, 20);
        chk(iv_agent_apply_thresh(&g_agent, &t, g_now) == IV_OK, "b06b same value ok");
        step(1100u);
        gn = drain(g_master, got, sizeof got, 200);
        chk(!(gn == wn && memcmp(got, want, wn) == 0),
            "b06b same value does not resend 0xF1");

        /* 九项全 0：没有可下发的内容，明确回 IV_ESTATE（不是"发了"） */
        memset(&t, 0, sizeof(t));
        chk(iv_agent_apply_thresh(&g_agent, &t, g_now) == IV_ESTATE,
            "b06b all-zero returns ESTATE, not OK");
    }

    /* ---- b08 sys.overview 的 device 段（总览页只答"这台设备是谁"） ----
     * 总览页原先堆的是网络/摄像机/采集板三家的实时数据，现在归各自页面；总览只留
     * 身份与版本。这里钉住三件事：① device 段真的在；② 本机可得的（主机名/内核/
     * 墙钟）都有值；③ **本机不可得的分两种处置** —— 型号/SoC/身份 MAC 给 JSON null
     * 而不是空串（空串会被前端当成"读到了但值是空的"，与"没有身份"是两件事）；
     * 设备 SN 则取**默认值 "1"**，并把 `sn_source` 标成 `default`。 */
    {
        const char *js = ctl("{\"m\":\"sys.overview\"}");

        chk(strstr(js, "\"device\":{") != NULL, "b08 overview has device block");
        chk(strstr(js, "\"version\":\"") != NULL, "b08 device.version present");
        chk(strstr(js, "\"hostname\":\"") != NULL, "b08 device.hostname present");
        chk(strstr(js, "\"kernel\":\"") != NULL, "b08 device.kernel present");
        chk(strstr(js, "\"machine\":\"") != NULL, "b08 device.machine present");
        chk(strstr(js, "\"wall_sec\":") != NULL, "b08 device.wall_sec present");
        chk(strstr(js, "\"model\":null") != NULL, "b08 device.model is JSON null on host");
        chk(strstr(js, "\"soc\":null") != NULL, "b08 device.soc is JSON null on host");
        /* SN 有默认值（"1"，2026-10-17 用户定），**不是 null 也不是空串**；
         * 来源必须如实标成 default —— 否则默认值会被当成产线写入的真 SN。 */
        chk(strstr(js, "\"sn\":\"1\"") != NULL, "b08 device.sn falls back to default 1");
        chk(strstr(js, "\"sn_source\":\"default\"") != NULL, "b08 device.sn_source marks default");
        chk(strstr(js, "\"id_mac\":null") != NULL, "b08 device.id_mac is JSON null on host");

        /* net.wan：**当前通信网口**（2026-10-19 用户要求 Web 展示）。
         * 宿主机上没有 usb0/eth0 这两个接口，所以**不校验 bound 的值**
         * （会是空串＝"未绑定"，这是宿主机上的正确结果）；只钉三件能钉的：
         * 块在、网口名取自配置（不是硬编码）、默认判定口径是 wireless。 */
        chk(strstr(js, "\"wan\":{") != NULL, "b08 overview has net.wan block");
        chk(strstr(js, "\"ifname\":\"usb0\"") != NULL,
            "b08 net.wan.ifname comes from configured wan_name");
        chk(strstr(js, "\"kind\":\"wireless\"") != NULL,
            "b08 net.wan.kind starts as wireless (AUTO 无线优先)");
        chk(strstr(js, "\"bound_ok\":") != NULL, "b08 net.wan.bound_ok present");
        chk(strstr(js, "\"transport\":\"") != NULL, "b08 net.wan.transport present");
        chk(strstr(js, "\"queue\":") != NULL, "b08 net.wan.queue present");
        chk(strstr(js, "\"switches\":") != NULL, "b08 net.wan.switches present");
    }

    /* ---- b09 device.identity.set（Web 配置设备 ID / MAC，2026-10-17 用户要求）----
     * 这是**唯一会写块设备**的 ctl 路径，判据分三层：
     *   ① 参数校验必须在**写之前**拦住（格式错、缺字段、广播占位 000000）；
     *   ② 空身份分区上只给 MAC ⇒ 必须拒（否则会写进一条 devid=0 的"存在但无效"记录，
     *      比空分区更难查）；
     *   ③ 分区不存在时写**必须如实失败**，不许"存起来了回头生效"。
     * 宿主机没有 `/dev/by-name/private`，正好是 ③ 的样本；有该设备的机器上跳过 ③，
     * 免得测试真去改一台设备的身份。 */
    {
        const char *js;

        js = ctl("{\"m\":\"device.identity.set\",\"p\":{}}");
        chk(strstr(js, "\"ok\":false") != NULL, "b09 empty request rejected");

        js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"devid\":\"zz\"}}");
        chk(strstr(js, "devid must be 1..6 hex") != NULL, "b09 non-hex devid rejected");

        js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"devid\":\"1234567\"}}");
        chk(strstr(js, "devid must be 1..6 hex") != NULL, "b09 over-long devid rejected");

        js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"devid\":\"000000\"}}");
        chk(strstr(js, "broadcast placeholder") != NULL, "b09 broadcast devid rejected");

        js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"devid\":\"1\",\"mac\":\"zz:zz\"}}");
        chk(strstr(js, "bad mac") != NULL, "b09 bad mac rejected");

        if (access("/dev/by-name/private", F_OK) != 0) {
            js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"mac\":\"02:11:22:33:44:55\"}}");
            chk(strstr(js, "identity partition is blank") != NULL,
                "b09 mac-only on blank identity rejected");

            js = ctl("{\"m\":\"device.identity.set\",\"p\":{\"devid\":\"1\"}}");
            chk(strstr(js, "\"ok\":false") != NULL && strstr(js, "write failed") != NULL,
                "b09 write to missing partition fails closed");
        }
    }

    /* ---- b07 关闭 ---- */
    iv_agent_close(&g_agent);
    chk(iv_agent_is_open(&g_agent) == 0, "b07 closed");

    (void)close(g_master);
    if (g_fail == 0)
        printf("test_agent_board passed (null semantics, 0xE1 decode, relay, limits, "
               "0xC1/0xC2 events, relay.set 0xD1 bytes, thresh 0xF1 JSON bytes, guards, "
               "overview device block, overview wan block, device identity guards)\n");
    else
        printf("test_agent_board FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
