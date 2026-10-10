/*
 * 上报装配层实现（M3-S3.6）。口径、字段表与已知取舍见 include/ivsbox/iv_report.h。
 *
 * 分层：只经各模块公共头 + 回调交互；不引 pthread、不自己建线程。
 * 时间：一律由调用方传入 now_ms（reactor 模式内部用 iv_clock_monotonic_ms()）。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_log.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_serial.h"
#include "ivsbox/iv_version.h"

#define REP_MOD "report"

static void on_serial_ev(int fd, uint32_t events, void *arg);

/* 有界追加：缓冲不足返回 IV_ERANGE（调用方据此丢弃整段，绝不发半截） */
#define APPEND(out, cap, n, ...)                                     \
    do {                                                             \
        int _w = snprintf((out) + (n), (cap) - (n), __VA_ARGS__);    \
        if (_w < 0 || (size_t)_w >= (cap) - (n))                     \
            return IV_ERANGE;                                        \
        (n) += (size_t)_w;                                           \
    } while (0)

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */
void iv_report_cfg_default(iv_report_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->serial_path  = IV_REPORT_SERIAL_DEFAULT;
    cfg->devtype      = IV_PROTO_DEVTYPE_DEFAULT;
    cfg->report_ms    = IV_REPORT_REPORT_MS;
    cfg->heartbeat_ms = IV_REPORT_HEARTBEAT_MS;
    cfg->poll_ms      = IV_REPORT_POLL_MS;
}

void iv_report_init(iv_report_t *rp)
{
    if (rp == NULL)
        return;
    memset(rp, 0, sizeof(*rp));
    rp->serial_fd = -1;
    rp->env.csq   = -1; /* 未知：默认不上传 CSQ */
    iv_status_init(&rp->st);
    iv_queue_init(&rp->q);
    iv_transport_init(&rp->tr);
}

/* ---------------------------------------------------------------------------
 * 串口（hal 薄包装）：抽干读 / 有界暂存写
 * ------------------------------------------------------------------------- */
static void serial_drop(iv_report_t *rp)
{
    if (rp->r != NULL && rp->serial_ev != NULL) {
        (void)iv_reactor_del(rp->r, rp->serial_ev);
        rp->serial_ev = NULL;
    }
    if (rp->serial_fd >= 0) {
        (void)iv_serial_close(rp->serial_fd);
        rp->serial_fd = -1;
    }
    rp->txlen = 0;
    iv_link_clear(&rp->lk, IV_ECONN);
    iv_link_reset(&rp->lk);
    iv_status_init(&rp->st);
    rp->next_serial_retry = rp->now_ms + IV_REPORT_SERIAL_RETRY_MS;
}

static int serial_tx_flush(iv_report_t *rp)
{
    while (rp->txlen > 0) {
        int w = iv_serial_write(rp->serial_fd, rp->txbuf, rp->txlen);

        if (w > 0) {
            rp->serial_tx += (uint64_t)w;
            if ((size_t)w < rp->txlen)
                memmove(rp->txbuf, rp->txbuf + w, rp->txlen - (size_t)w);
            rp->txlen -= (size_t)w;
            continue;
        }
        if (w == IV_EAGAIN)
            return IV_OK; /* 内核发送缓冲满：留到下次冲 */
        rp->serial_err++;
        serial_drop(rp);
        return (w < 0) ? w : IV_EIO;
    }
    return IV_OK;
}

static int on_link_tx(const uint8_t *bytes, size_t len, void *arg)
{
    iv_report_t *rp = arg;

    if (rp->serial_fd < 0)
        return IV_ECONN;
    if (len > sizeof(rp->txbuf) - rp->txlen) {
        rp->tx_drop++; /* 不覆盖未发完的字节：整帧丢弃并计数 */
        return IV_EFULL;
    }
    memcpy(rp->txbuf + rp->txlen, bytes, len);
    rp->txlen += len;
    return serial_tx_flush(rp);
}

static void serial_drain(iv_report_t *rp, uint64_t now_ms)
{
    uint8_t buf[256];

    for (;;) {
        size_t got = 0;
        int    rc  = iv_serial_read(rp->serial_fd, buf, sizeof(buf), &got);

        if (rc == IV_EAGAIN)
            return;
        if (rc != IV_OK) {
            rp->serial_err++;
            serial_drop(rp);
            return;
        }
        if (got == 0)
            return;
        rp->serial_rx += (uint64_t)got;
        iv_link_recv(&rp->lk, buf, got, (uint32_t)now_ms);
        if (rp->serial_fd < 0)
            return;
    }
}

/* ---------------------------------------------------------------------------
 * 采集板链路回调
 * ------------------------------------------------------------------------- */
/* 0xE1 应答的事务完成回调（前向声明：done 回调由 rep_poll **逐笔登记**） */
static void on_link_done(int rc, uint8_t cmd, const uint8_t *data, uint16_t len,
                         void *arg);

static void rep_poll(iv_report_t *rp)
{
    static const uint8_t zero = 0x00;

    /* 0xE1 查询：请求 data = 0x00（架构 §18.2）。
     * 注意 on_done 是**逐笔**登记的（iv_link_init 不收它）——传 NULL 会让该
     * 槽的应答被静默丢弃、状态镜像永不刷新（iv_link.c: `if (done != NULL)`）。 */
    if (iv_link_send(&rp->lk, IV_FRAME_CMD_QUERY, &zero, 1u,
                     (uint32_t)rp->now_ms, on_link_done, rp) == IV_OK)
        rp->polls++;
}

static void on_link_done(int rc, uint8_t cmd, const uint8_t *data, uint16_t len,
                         void *arg)
{
    iv_report_t *rp = arg;

    /* 只消费 0xE1 应答 → 全量刷新镜像（0xF1/0xD1/0xD2 应答本步不用） */
    if (rc == IV_OK && cmd == IV_FRAME_CMD_QUERY)
        iv_status_handle_query(&rp->st, data, len);
}

static void on_link_upstream(uint8_t cmd, const uint8_t *data, uint16_t len,
                             void *arg)
{
    iv_report_t *rp = arg;

    iv_status_handle_upstream(cmd, data, len, &rp->st);
}

static void on_link_change(int up, void *arg)
{
    iv_report_t *rp = arg;

    if (!up)
        return;
    /* 链路恢复：立刻发 0xE1 全量查询，重建状态镜像 */
    rep_poll(rp);
}

/* ---------------------------------------------------------------------------
 * 平台链路回调
 * ------------------------------------------------------------------------- */
static int proto_on_tx(const uint8_t *bytes, size_t len, void *arg)
{
    iv_report_t *rp = arg;

    return iv_transport_send(&rp->tr, bytes, len);
}

static int pump_pull(void *arg, const uint8_t **bytes, size_t *len,
                     uint64_t *cookie)
{
    iv_report_t *rp = arg;
    uint64_t     seq  = 0;
    const void  *data = NULL;
    size_t       dlen = 0;
    int          rc   = iv_queue_peek(&rp->q, &seq, &data, &dlen);

    if (rc != IV_OK)
        return rc; /* IV_EAGAIN = 暂无可发；其它负码 = 本层跳过不 ack */
    *bytes  = (const uint8_t *)data;
    *len    = dlen;
    *cookie = seq;
    return IV_OK;
}

static void pump_ack(void *arg, uint64_t cookie)
{
    iv_report_t *rp = arg;

    (void)iv_queue_ack(&rp->q, cookie);
}

static void tr_on_rx(void *arg, const uint8_t *bytes, size_t len)
{
    iv_report_t *rp = arg;

    iv_proto_recv(&rp->pf, bytes, len); /* 半包/粘包由 iv_proto 缓冲 */
}

static void tr_on_state(void *arg, iv_transport_state_t st)
{
    iv_report_t *rp = arg;

    (void)rp;
    IV_LOG_I(REP_MOD, "transport state -> %d", (int)st);
}

/* ---------------------------------------------------------------------------
 * 平台查询处理
 * ------------------------------------------------------------------------- */
/* 平台 QN（十进制时间戳拆成两个 32 位字）→ 回填串；格式与参考实现
 * `sprintf("QN=%08d%09d", qn1, qn2)` 同构，保证回填值与平台原值逐字一致。 */
static void qn_to_str(uint32_t qn1, uint32_t qn2, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%08u%09u", (unsigned)qn1, (unsigned)qn2);
}

/* E3 查询设备软硬件版本 → ## + JSON + ## */
static void h_query_version(const iv_proto_frame_t *f, void *arg)
{
    iv_report_t *rp = arg;
    char         qn[32];
    char         json[320];
    int          n;

    qn_to_str(f->qn1, f->qn2, qn, sizeof(qn));
    n = iv_report_build_query_json(rp->pf.id.devtype, rp->pf.id.devid,
                                   qn, rp->cfg.mod, rp->cfg.sv,
                                   json, sizeof(json));
    if (n <= 0) {
        IV_LOG_W(REP_MOD, "E3 json build failed (%d)", n);
        (void)iv_proto_ack(&rp->pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (iv_proto_send_json(&rp->pf, json, (size_t)n) == IV_OK)
        rp->query_resp++;
}

/* E2 立即上报设备状态 → 回 ACK + 立刻入一帧上报 */
static void h_query_report_now(const iv_proto_frame_t *f, void *arg)
{
    iv_report_t *rp = arg;

    (void)iv_proto_ack(&rp->pf, f->cmd, IV_PROTO_ACK_OK);
    (void)iv_report_trigger(rp, rp->now_ms);
}

/* ---------------------------------------------------------------------------
 * 上行报文生成
 * ------------------------------------------------------------------------- */
static void now_dt14(char *out, size_t cap)
{
    time_t    t = time(NULL);
    struct tm tmv;

    memset(&tmv, 0, sizeof(tmv));
    if (localtime_r(&t, &tmv) == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out, cap, "%04d%02d%02d%02d%02d%02d",
                   tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* 组一段上报数据段 → 打包成 ## 帧 → 入持久队列（优先级 STATE） */
static int rep_send_report(iv_report_t *rp)
{
    char    seg[IV_REPORT_SEG_MAX + 1u];
    char    dt[16];
    uint8_t frame[IV_PROTO_TXT_FIXED + IV_REPORT_SEG_MAX];
    size_t  flen = sizeof(frame);
    uint64_t seq = 0;
    int      n;
    int      rc;

    now_dt14(dt, sizeof(dt));
    n = iv_report_build_seg(&rp->st, &rp->env, rp->pf.id.devid, dt,
                            seg, sizeof(seg));
    if (n <= 0)
        return (n == 0) ? IV_EINVAL : n;

    rc = iv_proto_text_frame(seg, (size_t)n, frame, &flen);
    if (rc != IV_OK)
        return rc;

    rc = iv_queue_push(&rp->q, IV_QUEUE_PRIO_STATE, frame, flen, &seq);
    if (rc == IV_OK)
        rp->reports++;
    else if (rc == IV_EFULL)
        rp->queue_full++;
    return rc;
}

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */
void iv_report_on_serial(iv_report_t *rp, uint64_t now_ms)
{
    if (rp == NULL || !rp->opened)
        return;
    rp->now_ms = now_ms;
    if (rp->serial_fd >= 0) {
        serial_drain(rp, now_ms);
        if (rp->serial_fd >= 0)
            (void)serial_tx_flush(rp);
    }
}

int iv_report_step(iv_report_t *rp, uint64_t now_ms)
{
    if (rp == NULL)
        return IV_EINVAL;
    if (!rp->opened)
        return IV_ESTATE;
    rp->now_ms = now_ms;

    /* 采集板侧 */
    if (rp->serial_fd < 0 && now_ms >= rp->next_serial_retry) {
        int sf = iv_serial_open(rp->cfg.serial_path, NULL);
        if (sf >= 0) {
            rp->serial_fd = sf;
            if (rp->r != NULL) {
                rp->serial_ev = iv_reactor_add(rp->r, sf, IV_EV_READ,
                                               on_serial_ev, rp);
                if (rp->serial_ev == NULL)
                    serial_drop(rp);
            }
            if (rp->serial_fd >= 0)
                rp->next_poll = now_ms;
        } else {
            rp->next_serial_retry = now_ms + IV_REPORT_SERIAL_RETRY_MS;
        }
    }
    if (rp->serial_fd >= 0) {
        serial_drain(rp, now_ms);
        if (rp->serial_fd >= 0)
            (void)serial_tx_flush(rp);
    }
    iv_link_tick(&rp->lk, (uint32_t)now_ms);

    /* 平台侧（建连/退避/收发/取件全在 step 内推进） */
    if (rp->tr.opened)
        (void)iv_transport_step(&rp->tr, now_ms);

    /* 定时器（一次性触发后顺延；用 >= 以免欠跑时漏拍） */
    if (now_ms >= rp->next_poll) {
        rp->next_poll = now_ms + rp->cfg.poll_ms;
        rep_poll(rp);
    }
    if (now_ms >= rp->next_hb) {
        rp->next_hb = now_ms + rp->cfg.heartbeat_ms;
        if (iv_proto_heartbeat(&rp->pf) == IV_OK)
            rp->heartbeats++;
    }
    if (now_ms >= rp->next_report) {
        rp->next_report = now_ms + rp->cfg.report_ms;
        (void)rep_send_report(rp);
    }
    return IV_OK;
}

int iv_report_trigger(iv_report_t *rp, uint64_t now_ms)
{
    if (rp == NULL)
        return IV_EINVAL;
    if (!rp->opened)
        return IV_ESTATE;
    rp->now_ms = now_ms;
    return rep_send_report(rp);
}

/* ---------------------------------------------------------------------------
 * Reactor 回调（仅 reactor 模式使用）
 * ------------------------------------------------------------------------- */
static void on_serial_ev(int fd, uint32_t events, void *arg)
{
    iv_report_t *rp = arg;

    (void)fd;
    if (events & (IV_EV_ERR | IV_EV_HUP | IV_EV_RDHUP)) {
        serial_drop(rp);
        return;
    }
    iv_report_on_serial(rp, iv_clock_monotonic_ms());
}

static void on_tick(void *arg)
{
    iv_report_t *rp = arg;

    (void)iv_report_step(rp, iv_clock_monotonic_ms());
    if (rp->r != NULL)
        rp->tick_timer = iv_timer_add(rp->r, IV_REPORT_TICK_MS, on_tick, rp);
}

/* ---------------------------------------------------------------------------
 * open / close
 * ------------------------------------------------------------------------- */
int iv_report_open(iv_report_t *rp, const iv_report_cfg_t *cfg,
                   iv_reactor_t *r, uint64_t now_ms)
{
    iv_report_cfg_t c;
    iv_proto_id_t   id;
    iv_queue_cfg_t  qc;
    int             sf;
    int             rc;

    if (rp == NULL || cfg == NULL)
        return IV_EINVAL;
    if (rp->opened)
        return IV_ESTATE;

    iv_report_init(rp);

    iv_report_cfg_default(&c);
    if (cfg->serial_path != NULL)
        c.serial_path = cfg->serial_path;
    c.server_host  = cfg->server_host;
    c.server_port  = cfg->server_port;
    if (cfg->queue_dir != NULL)
        c.queue_dir = cfg->queue_dir;
    c.devid       = cfg->devid;
    c.devtype     = cfg->devtype ? cfg->devtype : IV_PROTO_DEVTYPE_DEFAULT;
    c.report_ms   = cfg->report_ms ? cfg->report_ms : IV_REPORT_REPORT_MS;
    c.heartbeat_ms = cfg->heartbeat_ms ? cfg->heartbeat_ms : IV_REPORT_HEARTBEAT_MS;
    c.poll_ms     = cfg->poll_ms ? cfg->poll_ms : IV_REPORT_POLL_MS;
    c.mod         = cfg->mod;
    c.sv          = cfg->sv;

    rp->cfg    = c;
    rp->r      = r;
    rp->now_ms = now_ms;
    rp->env.csq = -1;

    /* 1. 协议层（on_tx 直发到传输层）+ 查询路由 */
    id.devtype = c.devtype;
    id.devid   = c.devid;
    if (iv_proto_init(&rp->pf, &id, proto_on_tx, rp) != IV_OK)
        return IV_EINVAL;
    (void)iv_proto_route(&rp->pf, IV_PROTO_CMD_QUERY_VERSION, h_query_version, rp);
    (void)iv_proto_route(&rp->pf, IV_PROTO_CMD_QUERY_INFO, h_query_report_now, rp);

    /* 2. 采集板可靠层 */
    if (iv_link_init(&rp->lk, NULL, on_link_tx, on_link_upstream,
                     on_link_change, rp) != IV_OK)
        return IV_EINVAL;

    /* 3. 待发持久队列（打不开不致命：平台链路仍可跑，只是入不了队） */
    iv_queue_cfg_default(&qc);
    if (c.queue_dir != NULL)
        qc.dir = c.queue_dir;
    rc = iv_queue_open(&rp->q, &qc);
    if (rc != IV_OK)
        IV_LOG_W(REP_MOD, "queue open failed (%d): %s", rc,
                 c.queue_dir ? c.queue_dir : IV_QUEUE_DIR_DEFAULT);

    /* 4. 串口（打不开不致命：记录后继续，镜像停在未刷新态） */
    sf = iv_serial_open(c.serial_path, NULL);
    if (sf < 0) {
        rp->serial_fd = -1;
        rp->next_serial_retry = now_ms + IV_REPORT_SERIAL_RETRY_MS;
        IV_LOG_E(REP_MOD, "serial open failed (%d): %s", sf, c.serial_path);
    } else {
        rp->serial_fd = sf;
        if (r != NULL)
            rp->serial_ev = iv_reactor_add(r, sf, IV_EV_READ, on_serial_ev, rp);
        if (r != NULL && rp->serial_ev == NULL)
            serial_drop(rp);
    }

    /* 5. 平台传输（host 空则不启动） */
    if (c.server_host != NULL && c.server_host[0] != '\0' && c.server_port != 0u) {
        iv_transport_cfg_t tc;

        memset(&tc, 0, sizeof(tc));
        tc.host         = c.server_host;
        tc.port         = c.server_port;
        tc.io           = NULL; /* 内置真实 socket */
        rp->pump.pull   = pump_pull;
        rp->pump.ack    = pump_ack;
        rp->pump.arg    = rp;
        tc.pump         = &rp->pump; /* 传输层只存指针：本体在 rp 内，生命周期覆盖 */
        tc.on_rx        = tr_on_rx;
        tc.rx_arg       = rp;
        tc.on_state     = tr_on_state;
        tc.state_arg    = rp;

        rc = iv_transport_open(&rp->tr, &tc);
        if (rc != IV_OK)
            IV_LOG_W(REP_MOD, "transport open failed (%d): %s:%u", rc,
                     c.server_host, (unsigned)c.server_port);
    } else {
        IV_LOG_W(REP_MOD, "platform server not configured, transport idle");
    }

    rp->next_poll   = now_ms;              /* 立即向采集板要一次全量 */
    rp->next_hb     = now_ms + c.heartbeat_ms;
    rp->next_report = now_ms + c.report_ms;
    rp->opened      = 1;

    if (r != NULL)
        rp->tick_timer = iv_timer_add(r, IV_REPORT_TICK_MS, on_tick, rp);

    IV_LOG_I(REP_MOD,
             "report layer up: devid=%x devtype=%04x serial_fd=%d server=%s:%u",
             (unsigned)c.devid, (unsigned)c.devtype, rp->serial_fd,
             (c.server_host != NULL) ? c.server_host : "(none)",
             (unsigned)c.server_port);
    return IV_OK;
}

void iv_report_close(iv_report_t *rp)
{
    if (rp == NULL || !rp->opened)
        return;

    if (rp->r != NULL) {
        if (rp->tick_timer != NULL) {
            (void)iv_timer_cancel(rp->r, rp->tick_timer);
            rp->tick_timer = NULL;
        }
        if (rp->serial_ev != NULL) {
            (void)iv_reactor_del(rp->r, rp->serial_ev);
            rp->serial_ev = NULL;
        }
    }

    if (rp->tr.opened)
        (void)iv_transport_close(&rp->tr);
    iv_link_reset(&rp->lk);
    if (rp->q.opened)
        (void)iv_queue_close(&rp->q);

    if (rp->serial_fd >= 0) {
        (void)iv_serial_close(rp->serial_fd);
        rp->serial_fd = -1;
    }
    rp->txlen  = 0;
    rp->opened = 0;
    rp->r      = NULL;
}

/* ---------------------------------------------------------------------------
 * 纯函数：上报数据段 / 查询应答
 * ------------------------------------------------------------------------- */
int iv_report_build_seg(const iv_status_t *st, const iv_report_env_t *env,
                        uint32_t devid, const char *dt14,
                        char *out, size_t cap)
{
    const iv_status_data_t *d;
    iv_report_env_t         e0;
    const iv_report_env_t  *e = env;
    size_t                  n = 0;
    int                     i;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (dt14 == NULL || strlen(dt14) != 14u)
        return IV_EINVAL;
    if (e == NULL) {
        memset(&e0, 0, sizeof(e0));
        e0.csq = -1;
        e = &e0;
    }
    d = (st != NULL) ? &st->data : NULL;

    /* ---- 数据段头（字段表：QN/TID/VER/DEVTYPE/CP） ---- */
    APPEND(out, cap, n, "QN=0;");
    APPEND(out, cap, n, "TID=%x;", (unsigned)devid);
    APPEND(out, cap, n, "VER=%02x;", (unsigned)IV_PROTO_VER);
    APPEND(out, cap, n, "DEVTYPE=%04x;", (unsigned)IV_PROTO_DEVTYPE_DEFAULT);
    APPEND(out, cap, n, "CP=&&");

    /* ---- 数据区（按指令表字段表顺序；未取到值的不上传） ---- */
    APPEND(out, cap, n, "DT=%s;", dt14);

    APPEND(out, cap, n, "CNS=%d,%d,%d,%d,%d,%d;",
           e->cns[0], e->cns[1], e->cns[2], e->cns[3], e->cns[4], e->cns[5]);
    APPEND(out, cap, n, "MN=%d,%d;", e->mn[0], e->mn[1]);

    if (d != NULL && st != NULL) {
        if (st->valid.V)
            APPEND(out, cap, n, "V=%.2f;", (double)d->V);
        if (st->valid.A)
            APPEND(out, cap, n, "A=%.2f;", (double)d->A);
        if (st->valid.H)
            APPEND(out, cap, n, "H=%.2f;", (double)d->H);
        if (st->valid.T)
            APPEND(out, cap, n, "T=%.2f;", (double)d->T);
        /* DS：指令表数据区无此行，按示例串位置（T 与 P 之间）原值直传（见文件头 ⚠1） */
        if (st->valid.DS)
            APPEND(out, cap, n, "DS=%d;", (int)d->DS);
        if (st->valid.P)
            APPEND(out, cap, n, "P=%d;", (int)d->P);
        if (d->APOWER[0] != '\0')
            APPEND(out, cap, n, "APOWER=%s;", d->APOWER);
        if (d->AKW[0] != '\0')
            APPEND(out, cap, n, "AKW=%s;", d->AKW);

        if (st->valid.RELAY)
            APPEND(out, cap, n, "RELAY=%d,%d,%d;",
                   (int)d->RELAY[0], (int)d->RELAY[1], (int)d->RELAY[2]);
        if (st->valid.CHV)
            APPEND(out, cap, n, "CHV=%.2f,%.2f,%.2f;",
                   (double)d->CHV[0], (double)d->CHV[1], (double)d->CHV[2]);
        if (st->valid.CHA)
            APPEND(out, cap, n, "CHA=%.2f,%.2f,%.2f;",
                   (double)d->CHA[0], (double)d->CHA[1], (double)d->CHA[2]);
        if (st->valid.POWER)
            APPEND(out, cap, n, "POWER=%.2f,%.2f,%.2f;",
                   (double)d->POWER[0], (double)d->POWER[1], (double)d->POWER[2]);
        if (st->valid.ELEC)
            APPEND(out, cap, n, "ELEC=%.2f,%.2f,%.2f;",
                   (double)d->ELEC[0], (double)d->ELEC[1], (double)d->ELEC[2]);
    }

    if (e->have_loc)
        APPEND(out, cap, n, "LAT=%.6f;LNG=%.6f;", e->lat, e->lng);
    if (e->csq >= 0)
        APPEND(out, cap, n, "CSQ=%d;", e->csq);
    if (e->err != NULL && e->err[0] != '\0')
        APPEND(out, cap, n, "ERR=%s;", e->err);

    APPEND(out, cap, n, "&&");

    (void)i;
    return (int)n;
}

uint8_t iv_report_query_crc(uint16_t devtype, uint32_t devid, uint8_t cmd)
{
    char s[32];
    int  n;

    /* 与参考实现 sprintf(crc_buff,"%02x%04x%xE3",...) 同构：
     * ver 与 devtype/tid 用小写十六进制、cmd 用**大写**（`%02X`）。
     * cmd 取"被应答的命令号"：E3 → `110400101E3` → CRC8=0x06。 */
    n = snprintf(s, sizeof(s), "%02x%04x%x%02X",
                 (unsigned)IV_PROTO_VER, (unsigned)devtype,
                 (unsigned)devid, (unsigned)cmd);
    if (n <= 0)
        return 0;
    if ((size_t)n >= sizeof(s))
        n = (int)sizeof(s) - 1;
    return iv_crc8((const uint8_t *)s, (size_t)n, IV_CRC8_SEED_INIT);
}

int iv_report_build_query_json(uint16_t devtype, uint32_t devid,
                               const char *qn, const char *mod, const char *sv,
                               char *out, size_t cap)
{
    char    crc2[3];
    size_t  n = 0;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (mod == NULL)
        mod = "";
    if (sv == NULL)
        sv = "";
    (void)snprintf(crc2, sizeof(crc2), "%02x",
                   (unsigned)iv_report_query_crc(devtype, devid,
                                                 IV_PROTO_CMD_QUERY_VERSION));

    APPEND(out, cap, n, "{\"code\":0,\"qn\":\"%s\",\"data\":{",
           (qn != NULL && qn[0] != '\0') ? qn : "0");
    APPEND(out, cap, n, "\"ver\":\"%02x\",", (unsigned)IV_PROTO_VER);
    APPEND(out, cap, n, "\"type\":\"%04x\",", (unsigned)devtype);
    APPEND(out, cap, n, "\"tid\":\"%x\",", (unsigned)devid);
    APPEND(out, cap, n, "\"cmd\":\"%02X\",", (unsigned)IV_PROTO_CMD_QUERY_VERSION);
    APPEND(out, cap, n, "\"mod\":\"%s\",", mod);
    APPEND(out, cap, n, "\"sv\":\"%s\",", sv);
    APPEND(out, cap, n, "\"crc\":\"%s\"}}", crc2);
    return (int)n;
}

/* ---------------------------------------------------------------------------
 * 只读读数 / 测试辅助
 * ------------------------------------------------------------------------- */
int iv_report_is_open(const iv_report_t *rp)
{
    return (rp != NULL) ? (int)rp->opened : 0;
}

int iv_report_serial_fd(const iv_report_t *rp)
{
    return (rp != NULL) ? rp->serial_fd : -1;
}

uint64_t iv_report_reports(const iv_report_t *rp)
{
    return (rp != NULL) ? rp->reports : 0u;
}

uint64_t iv_report_heartbeats(const iv_report_t *rp)
{
    return (rp != NULL) ? rp->heartbeats : 0u;
}

uint64_t iv_report_query_responses(const iv_report_t *rp)
{
    return (rp != NULL) ? rp->query_resp : 0u;
}

size_t iv_report_queue_count(const iv_report_t *rp)
{
    return (rp != NULL) ? iv_queue_count(&rp->q) : 0u;
}

uint64_t iv_report_queue_full(const iv_report_t *rp)
{
    return (rp != NULL) ? rp->queue_full : 0u;
}

void iv_report_feed_platform(iv_report_t *rp, const uint8_t *buf, size_t len)
{
    if (rp == NULL)
        return;
    iv_proto_recv(&rp->pf, buf, len);
}

void iv_report_set_env(iv_report_t *rp, const iv_report_env_t *env)
{
    if (rp == NULL)
        return;
    if (env == NULL) {
        memset(&rp->env, 0, sizeof(rp->env));
        rp->env.csq = -1;
        return;
    }
    rp->env = *env;
}
