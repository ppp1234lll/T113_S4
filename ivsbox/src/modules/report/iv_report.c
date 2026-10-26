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

/* ---- "字段名无条件发、取不到就留空" -----------------------------------------
 * 参考实现 `com.c:com_report_normally_function()` 对**每个**字段都无条件
 * `sprintf` 一次（读数拿不到时拼出来的就是 `V=;` 这种空值），所以它的上报
 * 字段表是**恒长**的。我们原先只在 `valid.X` 为真时才拼，字段个数随镜像刷新
 * 情况变化：同一台设备相邻两帧长度不同、字段表不同。
 * 空值 ≠ 0 —— 没取到就不填数，不伪造读数（见记忆"未 init 读数全零看着健康"）。
 * ------------------------------------------------------------------------- */
#define APPEND_F2(key, valid, v)                                             \
    do {                                                                     \
        int _w = (valid) ? snprintf(out + n, cap - n, "%s=%.2f;", (key),     \
                                    (double)(v))                             \
                         : snprintf(out + n, cap - n, "%s=;", (key));         \
        if (_w < 0 || (size_t)_w >= cap - n)                                 \
            return IV_ERANGE;                                                \
        n += (size_t)_w;                                                     \
    } while (0)

#define APPEND_I1(key, valid, v)                                             \
    do {                                                                     \
        int _w = (valid) ? snprintf(out + n, cap - n, "%s=%d;", (key), (v))  \
                         : snprintf(out + n, cap - n, "%s=;", (key));         \
        if (_w < 0 || (size_t)_w >= cap - n)                                 \
            return IV_ERANGE;                                                \
        n += (size_t)_w;                                                     \
    } while (0)

/* 字符串字段：空指针/空串都当"没取到"，只发键名 */
#define APPEND_S1(key, s)                                                    \
    do {                                                                     \
        const char *_s = (s);                                                \
        int _w = (_s != NULL && _s[0] != '\0')                               \
                     ? snprintf(out + n, cap - n, "%s=%s;", (key), _s)        \
                     : snprintf(out + n, cap - n, "%s=;", (key));             \
        if (_w < 0 || (size_t)_w >= cap - n)                                 \
            return IV_ERANGE;                                                \
        n += (size_t)_w;                                                     \
    } while (0)

/* 三路数组：没取到就发 `,,`（键名在、值位空），与参考实现 `%01d,%01d,%01d`
 * 的三槽布局保持一一对应，平台按槽取数不会错位。 */
#define APPEND_I3(key, valid, a, b, c)                                       \
    do {                                                                     \
        int _w = (valid) ? snprintf(out + n, cap - n, "%s=%d,%d,%d;", (key), \
                                    (int)(a), (int)(b), (int)(c))            \
                         : snprintf(out + n, cap - n, "%s=,,;", (key));       \
        if (_w < 0 || (size_t)_w >= cap - n)                                 \
            return IV_ERANGE;                                                \
        n += (size_t)_w;                                                     \
    } while (0)

#define APPEND_F3(key, valid, a, b, c)                                       \
    do {                                                                     \
        int _w = (valid) ? snprintf(out + n, cap - n, "%s=%.2f,%.2f,%.2f;",  \
                                    (key), (double)(a), (double)(b),         \
                                    (double)(c))                             \
                         : snprintf(out + n, cap - n, "%s=,,;", (key));       \
        if (_w < 0 || (size_t)_w >= cap - n)                                 \
            return IV_ERANGE;                                                \
        n += (size_t)_w;                                                     \
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

/* iv_status_init() 会清零镜像上的钩子 ⇒ 每处 init 之后都从本层自持的
 * cfg 副本重挂一遍（init / serial_drop / open 三处）。
 * init 时 rp->cfg 尚为零（memset）⇒ 这里就是"挂 NULL"，真值在 open 里落。 */
static void status_apply_hooks(iv_report_t *rp)
{
    iv_status_set_door_cb(&rp->st, rp->cfg.on_door, rp->cfg.on_door_arg);
}

void iv_report_init(iv_report_t *rp)
{
    if (rp == NULL)
        return;
    memset(rp, 0, sizeof(*rp));
    rp->serial_fd = -1;
    rp->env.csq   = -1; /* 未知：默认不上传 CSQ */
    iv_status_init(&rp->st);
    status_apply_hooks(rp);
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
    status_apply_hooks(rp); /* 串口重连后把门开钩子重挂回去 */
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
        rp->last_rx_ms = now_ms;   /* 最近一次"真的收到字节"的时刻（Web 用它算数据年龄） */
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

int iv_report_send_cmd(iv_report_t *rp, uint8_t cmd, const void *data,
                       uint16_t len, uint64_t now_ms)
{
    if (rp == NULL)
        return IV_EINVAL;
    if (!rp->opened)
        return IV_ESTATE;
    /* 控制命令不登记 on_done：它的应答语义归应用层（本层不消费）。
     * 传 NULL 的代价是"应答被静默丢弃"，对 0xD1 无害——见 iv_link.h。 */
    return iv_link_send(&rp->lk, cmd, data, len, (uint32_t)now_ms, NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * 阈值下发（0xF1）
 * ------------------------------------------------------------------------- */
/* 纯函数：只拼 JSON，不碰链路。键序与单片机源码的解析顺序一致；值为 0 的项
 * **不出现**在 JSON 里（"0＝该项不设置"的口径见 iv_report.h）。 */
int iv_report_build_thresh(const iv_thresh_t *t, char *out, size_t cap, size_t *out_len)
{
    struct {
        const char *k;
        int32_t     v;
    } kv[9];
    size_t used = 0u;
    int    n;
    int    i;
    int    first = 1;

    if (t == NULL || out == NULL || out_len == NULL || cap == 0u)
        return IV_EINVAL;

    kv[0].k = "hv"; kv[0].v = t->hv;
    kv[1].k = "lv"; kv[1].v = t->lv;
    kv[2].k = "ov"; kv[2].v = t->ov;
    kv[3].k = "ld"; kv[3].v = t->ld;
    kv[4].k = "sl"; kv[4].v = t->sl;
    kv[5].k = "tu"; kv[5].v = t->tu;
    kv[6].k = "tl"; kv[6].v = t->tl;
    kv[7].k = "hu"; kv[7].v = t->hu;
    kv[8].k = "hl"; kv[8].v = t->hl;

    if (cap < 3u)
        return IV_ERANGE;
    out[used++] = '{';
    for (i = 0; i < 9; i++) {
        if (kv[i].v == 0)
            continue; /* 0 ＝ 不设置：不下发该键 */
        n = snprintf(out + used, cap - used, "%s\"%s\":%ld",
                     first ? "" : ",", kv[i].k, (long)kv[i].v);
        if (n < 0 || (size_t)n >= cap - used)
            return IV_ERANGE;
        used += (size_t)n;
        first = 0;
    }
    if (used + 2u > cap)
        return IV_ERANGE;
    out[used++] = '}';
    out[used]   = '\0';
    if (first)
        return IV_ESTATE; /* 九项全 0：没有可下发的内容 */
    *out_len = used;
    return IV_OK;
}

int iv_report_send_thresh(iv_report_t *rp, const char *json, size_t len, uint64_t now_ms)
{
    if (json == NULL || len == 0u)
        return IV_EINVAL;
    if (len > (size_t)IV_FRAME_DATA_MAX)
        return IV_ERANGE;
    return iv_report_send_cmd(rp, IV_FRAME_CMD_THRESH, json, (uint16_t)len, now_ms);
}

void iv_report_request_poll(iv_report_t *rp)
{
    if (rp == NULL || !rp->opened)
        return;
    /* 只拨时刻表，不在这里发帧：下发仍走 step() 的既有节拍。
     * 刻意**不**判断"有没有 0xE1 在飞"—— `rep_poll()` 那条路径遇到 IV_EBUSY
     * 只是本拍不发，下一拍会再试，不会丢状态。 */
    rp->next_poll = rp->now_ms;
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

    /* 走到这里 ⇔ 传输层把这**一整件写进了 socket**（`tx_consume()` 写尽才 ack）。
     * 与 `rep_send_report()` 的 `report #N` 配对看：两条计数应一一对应。
     * 对不上 ⇒ 丢在取件/写出这一段（我们的问题）；对得上 ⇒ 帧确实交付给了内核，
     * 之后再少就是链路或对端的事 —— 这个争议一次测完，别再靠代理抓包来回猜。 */
    IV_LOG_I(REP_MOD, "queue item written: seq=%llu left=%u", (unsigned long long)cookie,
             (unsigned)iv_queue_count(&rp->q));
    (void)iv_queue_ack(&rp->q, cookie);
}

static void tr_on_rx(void *arg, const uint8_t *bytes, size_t len)
{
    iv_report_t *rp = arg;
    size_t       i;
    size_t       k = 0u;
    char         hex[3u * 32u + 1u];

    /* 平台→设备方向取证：下行解析器对"帧头对不上"的字节是**静默丢弃**的，
     * 所以没有这一行，"平台一条没下发"和"下发了但被我们丢了"在日志里长得
     * 一模一样。平台这一路正常情况下是安静的（心跳 0xFF 除外），量很小。 */
    for (i = 0u; i < len && i < 32u; i++) {
        (void)snprintf(&hex[k], sizeof(hex) - k, "%02x", (unsigned)bytes[i]);
        k += 2u;
    }
    hex[k] = '\0';
    IV_LOG_I(REP_MOD, "rx %u bytes: %s%s", (unsigned)len, hex,
             (len > 32u) ? "..." : "");

    iv_proto_recv(&rp->pf, bytes, len); /* 半包/粘包由 iv_proto 缓冲 */
}

static void tr_on_state(void *arg, iv_transport_state_t st)
{
    iv_report_t *rp = arg;

    (void)rp;
    IV_LOG_I(REP_MOD, "transport state -> %d", (int)st);
    /* 出口绑定失败：连接照样会建（走内核默认路由），但报文可能从另一条 WAN 出去。
     * 这种情况光看 "transport UP" 完全看不出来，所以每次状态变化都把它带出来。 */
    if (iv_transport_bind_fail(&rp->tr) != 0u) {
        IV_LOG_W(REP_MOD, "bind to %s failed x%llu (errno=%d), falling back to default route",
                 iv_transport_bind_if(&rp->tr),
                 (unsigned long long)iv_transport_bind_fail(&rp->tr),
                 iv_transport_bind_errno(&rp->tr));
    }
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
    /* 缓冲按**最坏情况**算：qn 17 + mod 31 + sysver/sv 各 ~25 + tsn 12 + 其余键，
     * 实测约 210 B；留一倍余量，免得别人再加字段时悄悄撞 IV_ERANGE
     * （撞了的表现是"点了查询毫无反应"，比截断更难查）。 */
    char         json[384];
    int          n;

    qn_to_str(f->qn1, f->qn2, qn, sizeof(qn));
    n = iv_report_build_query_json(rp->pf.id.devtype, rp->pf.id.devid,
                                   qn, rp->cfg.mod, rp->cfg.sv, rp->cfg.tsn,
                                   json, sizeof(json));
    if (n <= 0) {
        IV_LOG_W(REP_MOD, "E3 json build failed (%d)", n);
        (void)iv_proto_ack(&rp->pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (iv_proto_send_json(&rp->pf, json, (size_t)n) == IV_OK) {
        rp->query_resp++;
        /* 原样回显一份：平台"点了没反应"时，光有 query_rsp 计数证明不了
         * 回的内容对不对（字段名写错也照样计数）。E3 是人工点击触发、频率极低，
         * 一条日志换"不用抓包就能对账"。 */
        IV_LOG_I(REP_MOD, "E3 reply: %s", json);
    }
}

/* E2 立即上报设备状态 → 回 ACK + 立刻入一帧上报 */
static void h_query_report_now(const iv_proto_frame_t *f, void *arg)
{
    iv_report_t *rp = arg;

    (void)iv_proto_ack(&rp->pf, f->cmd, IV_PROTO_ACK_OK);
    (void)iv_report_trigger(rp, rp->now_ms);
}

/* E1 查询设备当前参数设置 → 回 ##{...}## JSON（不是 ACK）。
 * 2026-10-21 之前这里是**未注册**的，平台点「查询设备配置」收到 0x74 就显示失败；
 * 板端日志 `downlink cmd 0xE1 not implemented (1 bytes), ack 0x74` 即此。 */
static void h_query_cfg(const iv_proto_frame_t *f, void *arg)
{
    iv_report_t *rp = arg;
    char         qn[32];
    /* 外壳约 150 B ＋ 装配层给的 data 段（≤768 B），再留余量。 */
    char         json[IV_REPORT_CFG_PAIRS_MAX + 256u];
    int          n;

    qn_to_str(f->qn1, f->qn2, qn, sizeof(qn));
    n = iv_report_build_cfg_json(rp->pf.id.devtype, rp->pf.id.devid, qn,
                                 rp->cfg.on_query_cfg, rp->cfg.on_query_cfg_arg,
                                 json, sizeof(json));
    if (n <= 0) {
        IV_LOG_W(REP_MOD, "E1 json build failed (%d)", n);
        (void)iv_proto_ack(&rp->pf, f->cmd, IV_PROTO_ERR_EXEC);
        return;
    }
    if (iv_proto_send_json(&rp->pf, json, (size_t)n) == IV_OK) {
        rp->query_resp++;
        /* 同 E3：把整条应答打进日志，"回了"和"回对了"是两件事。 */
        IV_LOG_I(REP_MOD, "E1 reply: %s", json);
    }
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
    n = iv_report_build_seg(&rp->st, &rp->env, rp->pf.id.devtype,
                            rp->pf.id.devid, dt, seg, sizeof(seg));
    if (n <= 0)
        return (n == 0) ? IV_EINVAL : n;

    rc = iv_proto_text_frame(seg, (size_t)n, frame, &flen);
    if (rc != IV_OK)
        return rc;

    rc = iv_queue_push(&rp->q, IV_QUEUE_PRIO_STATE, frame, flen, &seq);
    if (rc == IV_OK) {
        /* 内容指纹：**故意从 `CNS=` 起算，把 `DT` 排除在外** —— 这样两帧指纹相同
         * 就等价于"测量值一模一样，只有时间戳不同"。平台若按"重复值不再存"处理，
         * 这种帧会被吞掉，外部表现就是"报文少了一半、间隔翻倍"。
         * （采集板镜像是 0xE1 轮询刷新的，`iv_link_send` 遇 IV_EBUSY 会整拍跳过。） */
        const char *tail = strstr(seg, "CNS=");
        uint32_t    h    = 2166136261u;
        size_t      k;

        for (k = 0u; tail != NULL && tail[k] != '\0'; k++) {
            h ^= (uint32_t)(uint8_t)tail[k];
            h *= 16777619u;
        }
        rp->reports++;
        /* 每条周期上报留一行 DT —— 这是"设备到底按没按时发"的**唯一现场证据**。
         * 300 s 一条＝12 行/小时，代价可忽略；与平台「历史数据」对账时逐条比。 */
        IV_LOG_I(REP_MOD, "report #%llu DT=%s seg=%d frame=%u m=%08x end=%.4s",
                 (unsigned long long)rp->reports, dt, n, (unsigned)flen, (unsigned)h,
                 (flen >= 4u) ? (const char *)&frame[flen - 4u] : "");
    } else if (rc == IV_EFULL) {
        rp->queue_full++;
    }
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

    /* 定时器：**自由运行的整倍数节拍**（对齐参考实现 —— 那边是 1 ms 硬件计数器
     * 累加到设定值就发，唤醒延迟不进入下一拍）。
     * 原先写 `next = now_ms + period`：每次唤醒的延迟（tick 200 ms + 调度抖动）
     * 都会累加进下一拍，300 s 上报会持续后移 —— 实测 40 min 慢 1 s、一天 30 s 以上，
     * 平台上表现为"时间不固定"。改成 `next += period` 后节拍只由开启时刻决定；
     * 兜底：落后超过一整周期（长时间挂起/时钟回退）则从 now 重新对齐。 */
    if (now_ms >= rp->next_poll) {
        rp->next_poll += rp->cfg.poll_ms;
        if (rp->next_poll <= now_ms)
            rp->next_poll = now_ms + rp->cfg.poll_ms;
        rep_poll(rp);
    }
    if (now_ms >= rp->next_hb) {
        rp->next_hb += rp->cfg.heartbeat_ms;
        if (rp->next_hb <= now_ms)
            rp->next_hb = now_ms + rp->cfg.heartbeat_ms;
        if (iv_proto_heartbeat(&rp->pf) == IV_OK)
            rp->heartbeats++;
    }
    if (now_ms >= rp->next_report) {
        rp->next_report += rp->cfg.report_ms;
        if (rp->next_report <= now_ms)
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

/*
 * 重连平台传输（S3.3 切换事务的 close_old_tcp + reconnect 合体）：只动传输层，
 * 串口/队列/协议层原样保留 —— 队列里未确认的数据在连接重建后由取件泵补传。
 */
int iv_report_reconnect(iv_report_t *rp, uint64_t now_ms)
{
    iv_transport_cfg_t tc;
    int                rc;

    if (rp == NULL)
        return IV_EINVAL;
    if (!rp->opened)
        return IV_ESTATE;
    if (rp->srv_host[0] == '\0' || rp->srv_port == 0u) {
        /* 目标地址未配置/已清空 ⇒ **这里就是"断开在用连接"的那一步**。
         *
         * 病根（R-01，2026-10-22 板端实测）：原先这行直接 `return IV_ESTATE`
         * 就走，而装配层清空平台地址时正是走这条路径 ——
         * `iv_agent_reload_server()` 拿到 `iv_agent_pick_host()==NULL`，
         * 只把 `srv_host`/`srv_port` 副本清掉就调本函数，本函数又"无可重连"直接
         * 返回，于是**旧 TCP 一直 ESTAB、`transport` 恒 up、上报照旧发往旧平台**。
         * 现场把网页上的平台地址清空后平台仍照收数据，两个地址之间也切不开。
         *
         * 语义定为「重连到一个空地址 ＝ 断开」：只关传输层，串口/队列/协议层
         * 原样保留（与正常重连同口径），队列里未确认的数据等地址回来后再补传。 */
        (void)iv_transport_close(&rp->tr);
        rp->now_ms = now_ms;
        IV_LOG_I(REP_MOD, "server addr cleared, transport closed (if any)");
        return IV_ESTATE;
    }

    (void)iv_transport_close(&rp->tr);

    memset(&tc, 0, sizeof(tc));
    tc.host       = rp->srv_host; /* 自持副本；open 内部会再拷一次 */
    tc.port       = rp->srv_port;
    tc.io         = NULL; /* 内置真实 socket */
    tc.bind_ifname = (rp->bind_if[0] != '\0') ? rp->bind_if : NULL; /* 出口网卡 */
    rp->pump.pull = pump_pull;
    rp->pump.ack  = pump_ack;
    rp->pump.arg  = rp;
    tc.pump       = &rp->pump;
    tc.on_rx      = tr_on_rx;
    tc.rx_arg     = rp;
    tc.on_state   = tr_on_state;
    tc.state_arg  = rp;

    rc = iv_transport_open(&rp->tr, &tc);
    rp->now_ms = now_ms;
    if (rc == IV_OK)
        IV_LOG_I(REP_MOD, "transport reconnect on route switch: %s:%u",
                 rp->srv_host, (unsigned)rp->srv_port);
    else
        IV_LOG_W(REP_MOD, "transport reconnect failed (%d): %s:%u", rc,
                 rp->srv_host, (unsigned)rp->srv_port);
    return rc;
}

int iv_report_set_server(iv_report_t *rp, const char *host, uint16_t port)
{
    size_t n;

    if (rp == NULL || !rp->opened)
        return IV_EINVAL;

    if (host == NULL || host[0] == '\0' || port == 0u) {
        /* 清空：下一次重连不再建链（在用连接不动，是否断由调用方决定） */
        memset(rp->srv_host, 0, sizeof(rp->srv_host));
        rp->srv_port = 0u;
        return IV_OK;
    }

    n = strlen(host);
    if (n >= sizeof(rp->srv_host))
        return IV_ERANGE;

    memcpy(rp->srv_host, host, n + 1u);
    rp->srv_port = port;
    return IV_OK;
}

int iv_report_set_bind_if(iv_report_t *rp, const char *ifname)
{
    size_t n;

    if (rp == NULL)
        return IV_EINVAL;

    if (ifname == NULL || ifname[0] == '\0') {
        /* 解除绑定：下一次重连走内核默认路由（与加这功能之前的行为一致） */
        rp->bind_if[0] = '\0';
        return IV_OK;
    }

    n = strlen(ifname);
    if (n >= sizeof(rp->bind_if))
        return IV_ERANGE;

    memcpy(rp->bind_if, ifname, n + 1u);
    return IV_OK;
}

/*
 * 运行期改"周期上报间隔"。
 *
 * 病根：`report_ms` 原先只在 `iv_report_open()` 里从配置取一次，之后再无写入口 ——
 * 用户在网页上把「定时上报间隔」从 600 改成 300（或反过来），进程里跑的还是**开进程
 * 那一刻**的值，必须重启才生效。现场表现正是"页面上写着 5 分钟，平台上却十分钟一条"。
 *
 * 语义：只改周期，不动在用连接、不动队列；当前这一拍已排好的截止时刻按新周期就地
 * 前移（新周期更短时），使新值立即生效而不必等旧的那一拍到点。
 */
int iv_report_set_report_period(iv_report_t *rp, uint32_t report_ms)
{
    if (rp == NULL || !rp->opened)
        return IV_EINVAL;
    if (report_ms == 0u)
        report_ms = IV_REPORT_REPORT_MS;
    if (report_ms == rp->cfg.report_ms)
        return IV_OK;

    IV_LOG_I(REP_MOD, "report period %u -> %u ms", (unsigned)rp->cfg.report_ms,
             (unsigned)report_ms);
    rp->cfg.report_ms = report_ms;
    if (rp->next_report > rp->now_ms + report_ms)
        rp->next_report = rp->now_ms + report_ms;
    return IV_OK;
}

/*
 * 采集周期（0xE1 轮询间隔）热生效 —— `sys.sample.period_ms` 的落点。
 * 与上报周期同一套口径：只改节拍、不动连接与队列；改得更短时把已排好的
 * 下一次轮询往前拉，改得更长时**不推迟**已经排得更早的那一拍（不能因为我改大
 * 就把该发的那次吞掉）。
 */
int iv_report_set_poll_period(iv_report_t *rp, uint32_t poll_ms)
{
    if (rp == NULL || !rp->opened)
        return IV_EINVAL;
    if (poll_ms == 0u)
        poll_ms = IV_REPORT_POLL_MS;
    if (poll_ms == rp->cfg.poll_ms)
        return IV_OK;

    IV_LOG_I(REP_MOD, "poll period %u -> %u ms", (unsigned)rp->cfg.poll_ms,
             (unsigned)poll_ms);
    rp->cfg.poll_ms = poll_ms;
    if (rp->next_poll > rp->now_ms + poll_ms)
        rp->next_poll = rp->now_ms + poll_ms;
    return IV_OK;
}

/*
 * 平台下行 0xF5「配置定时上报间隔」载荷解析（纯函数）。
 * 口径见 include/ivsbox/iv_report.h（与参考实现 `com_set_next_report_time()` 同构）。
 * 这里只拆字段 —— 采不采纳、落不落盘、回 0x01 还是 0x74 都归装配层：
 * 本层不认配置键，也不该在协议回调里做文件 I/O。
 */
int iv_report_parse_cfg_intv(const uint8_t *data, uint16_t len,
                             uint8_t *sel, uint16_t *sec)
{
    if (data == NULL || sel == NULL || sec == NULL)
        return IV_EINVAL;
    if (len < 3u)
        return IV_EINVAL;
    *sel = data[0];
    /* 参考实现：`(buff[1]<<8 | buff[2])` —— **大端**，别按小端读 */
    *sec = (uint16_t)(((uint16_t)data[1] << 8) | (uint16_t)data[2]);
    return IV_OK;
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
    c.tsn         = cfg->tsn;
    c.on_door     = cfg->on_door;
    c.on_door_arg = cfg->on_door_arg;
    /* 0xE1 的 data 段回调（装配层拼配置/4G/网卡事实）。漏这行 ⇒ 平台点
     * 「查询设备配置」永远收到 0x74，设备侧看起来一切正常（路由注册了、
     * 日志也不报错）。c09 用例就是钉这一行的。 */
    c.on_query_cfg     = cfg->on_query_cfg;
    c.on_query_cfg_arg = cfg->on_query_cfg_arg;
    /* 这里是把调用方配置**逐字段**收进局部 c，漏一行就等于该项被默认值顶掉。
     * bind_ifname 第一版就漏在这里：agent 传了 "usb0"，传输层拿到的却是空，
     * 表现是报文仍从 eth0 出去，而启动日志里 bind=usb0/- 的右半边一直是 "-"。 */
    c.bind_ifname = cfg->bind_ifname;

    rp->cfg    = c;
    rp->r      = r;
    rp->now_ms = now_ms;
    rp->env.csq = -1;
    status_apply_hooks(rp); /* cfg 已就位，把箱门事件钩子挂上 */

    /* 平台目标地址：拷进本层自持副本（运行期可经 iv_report_set_server 改写） */
    if (c.server_host != NULL)
        (void)snprintf(rp->srv_host, sizeof(rp->srv_host), "%.127s", c.server_host);
    rp->srv_port = c.server_port;
    /* 出口网卡：与地址成对——地址决定连哪儿，这个决定从哪儿出 */
    rp->bind_if[0] = '\0';
    if (c.bind_ifname != NULL && c.bind_ifname[0] != '\0')
        (void)snprintf(rp->bind_if, sizeof(rp->bind_if), "%s", c.bind_ifname);

    /* 1. 协议层（on_tx 直发到传输层）+ 查询路由 */
    id.devtype = c.devtype;
    id.devid   = c.devid;
    if (iv_proto_init(&rp->pf, &id, proto_on_tx, rp) != IV_OK)
        return IV_EINVAL;
    (void)iv_proto_route(&rp->pf, IV_PROTO_CMD_QUERY_VERSION, h_query_version, rp);
    (void)iv_proto_route(&rp->pf, IV_PROTO_CMD_QUERY_INFO, h_query_report_now, rp);
    (void)iv_proto_route(&rp->pf, IV_PROTO_CMD_QUERY_CFG, h_query_cfg, rp);

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
    if (rp->srv_host[0] != '\0' && rp->srv_port != 0u) {
        iv_transport_cfg_t tc;

        memset(&tc, 0, sizeof(tc));
        tc.host         = rp->srv_host; /* open 内部会再拷一次，副本生命周期足够 */
        tc.port         = rp->srv_port;
        tc.io           = NULL; /* 内置真实 socket */
        tc.bind_ifname  = (rp->bind_if[0] != '\0') ? rp->bind_if : NULL; /* 出口网卡 */
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
                     rp->srv_host, (unsigned)rp->srv_port);
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
             (rp->srv_host[0] != '\0') ? rp->srv_host : "(none)",
             (unsigned)rp->srv_port);
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
                        uint16_t devtype, uint32_t devid, const char *dt14,
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

    /* ---- 数据段头（字段表：QN/TID/VER/DEVTYPE/CP） ----
     * `DEVTYPE` **必须与查询应答同源**：早先这里直接写 `IV_PROTO_DEVTYPE_DEFAULT`
     * 常量，于是 `cfg.devtype` 改了以后"查询应答里的 devtype 变了、上报帧的没变"，
     * 平台按上报帧认设备类型就永远看不到新值。现在由调用方把
     * `cfg.devtype`（0 → 默认）传进来。 */
    APPEND(out, cap, n, "QN=0;");
    APPEND(out, cap, n, "TID=%x;", (unsigned)devid);
    APPEND(out, cap, n, "VER=%02x;", (unsigned)IV_PROTO_VER);
    APPEND(out, cap, n, "DEVTYPE=%04x;",
           (unsigned)((devtype != 0u) ? devtype : IV_PROTO_DEVTYPE_DEFAULT));
    APPEND(out, cap, n, "CP=&&");

    /* ---- 数据区：**逐字段对齐参考实现 `com.c:com_report_normally_function()`**
     * 的拼接顺序与"无条件拼接"语义（2026-10-20 复核）：DT CNS MN V A APOWER
     * AKW H T P DS RELAY CHV CHA POWER ELEC CSQ MIU (LAT/LNG) ERR。
     * 字段名一律发：取到值就填数、取不到留空（`V=;`）—— 参考实现对每个字段都
     * `sprintf` 一次，读不到就拼出空值，因此它的字段表**恒长**；我们照此办理。 */
    APPEND(out, cap, n, "DT=%s;", dt14);

    APPEND(out, cap, n, "CNS=%d,%d,%d,%d,%d,%d;",
           e->cns[0], e->cns[1], e->cns[2], e->cns[3], e->cns[4], e->cns[5]);
    APPEND(out, cap, n, "MN=%d,%d;", e->mn[0], e->mn[1]);

    APPEND_F2("V", d != NULL && st->valid.V, d != NULL ? d->V : 0.0);
    APPEND_F2("A", d != NULL && st->valid.A, d != NULL ? d->A : 0.0);
    /* 总功率/总用电量：参考实现紧跟 V/A 之后（`APOWER`/`AKW`），字符串直传 */
    APPEND_S1("APOWER", d != NULL ? d->APOWER : NULL);
    APPEND_S1("AKW",    d != NULL ? d->AKW : NULL);
    APPEND_F2("H", d != NULL && st->valid.H, d != NULL ? d->H : 0.0);
    APPEND_F2("T", d != NULL && st->valid.T, d != NULL ? d->T : 0.0);
    /* 箱体姿态 P → 箱门 DS（参考实现先 P 后 DS） */
    APPEND_I1("P",  d != NULL && st->valid.P,  d != NULL ? (int)d->P : 0);
    APPEND_I1("DS", d != NULL && st->valid.DS, d != NULL ? (int)d->DS : 0);

    APPEND_I3("RELAY", d != NULL && st->valid.RELAY,
              d != NULL ? (int)d->RELAY[0] : 0,
              d != NULL ? (int)d->RELAY[1] : 0,
              d != NULL ? (int)d->RELAY[2] : 0);
    APPEND_F3("CHV", d != NULL && st->valid.CHV,
              d != NULL ? d->CHV[0] : 0.0,
              d != NULL ? d->CHV[1] : 0.0,
              d != NULL ? d->CHV[2] : 0.0);
    APPEND_F3("CHA", d != NULL && st->valid.CHA,
              d != NULL ? d->CHA[0] : 0.0,
              d != NULL ? d->CHA[1] : 0.0,
              d != NULL ? d->CHA[2] : 0.0);
    APPEND_F3("POWER", d != NULL && st->valid.POWER,
              d != NULL ? d->POWER[0] : 0.0,
              d != NULL ? d->POWER[1] : 0.0,
              d != NULL ? d->POWER[2] : 0.0);
    APPEND_F3("ELEC", d != NULL && st->valid.ELEC,
              d != NULL ? d->ELEC[0] : 0.0,
              d != NULL ? d->ELEC[1] : 0.0,
              d != NULL ? d->ELEC[2] : 0.0);

    /* CSQ 紧接 ELEC（参考实现位置）；没取到信号强度就发空值，不发 -1 */
    APPEND_I1("CSQ", e->csq >= 0, e->csq);
    /* MIU（漏电）：参考实现在 CSQ 之后**无条件**发 `MIU=%d;`。我们没有取值
     * 来源（见遗留项"MIU 取值来源"），所以只发键名占位，绝不编一个 0 出去。 */
    APPEND(out, cap, n, "MIU=;");

    if (e->have_loc)
        APPEND(out, cap, n, "LAT=%.6f;LNG=%.6f;", e->lat, e->lng);

    /* 错误码：参考实现 `Error_Get_Codesbuf()` **无条件**拼
     * `ERR=<数量>[,<故障码>...];`，健康时就是 `ERR=0;`。原先 err 为空就整段
     * 不发 ⇒ 帧尾缺字段，字段表随之变短。 */
    APPEND(out, cap, n, "ERR=%s;",
           (e->err != NULL && e->err[0] != '\0') ? e->err : "0");

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
     * cmd 取"被应答的命令号"：E3 → `110a00101E3` → CRC8=0xb2。 */
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
                               const char *tsn, char *out, size_t cap)
{
    char    crc2[3];
    size_t  n = 0;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (mod == NULL)
        mod = "";
    if (sv == NULL)
        sv = "";
    if (tsn == NULL)
        tsn = "";
    (void)snprintf(crc2, sizeof(crc2), "%02x",
                   (unsigned)iv_report_query_crc(devtype, devid,
                                                 IV_PROTO_CMD_QUERY_VERSION));

    /* 字段顺序照抄参考实现 `com_version_information()`：
     * ver → devtype → tid → cmd → mod → sysver → tsn → crc；
     * `type` / `sv` 紧跟各自的主名之后（见头文件里的别名说明），不改变主序。 */
    APPEND(out, cap, n, "{\"code\":0,\"qn\":\"%s\",\"data\":{",
           (qn != NULL && qn[0] != '\0') ? qn : "0");
    APPEND(out, cap, n, "\"ver\":\"%02x\",", (unsigned)IV_PROTO_VER);
    APPEND(out, cap, n, "\"devtype\":\"%04x\",", (unsigned)devtype);
    APPEND(out, cap, n, "\"type\":\"%04x\",", (unsigned)devtype);
    APPEND(out, cap, n, "\"tid\":\"%x\",", (unsigned)devid);
    APPEND(out, cap, n, "\"cmd\":\"%02X\",", (unsigned)IV_PROTO_CMD_QUERY_VERSION);
    APPEND(out, cap, n, "\"mod\":\"%.*s\",", (int)IV_REPORT_MOD_MAX, mod);
    APPEND(out, cap, n, "\"sysver\":\"%s\",", sv);
    APPEND(out, cap, n, "\"sv\":\"%s\",", sv);
    APPEND(out, cap, n, "\"tsn\":\"%s\",", tsn);
    APPEND(out, cap, n, "\"crc\":\"%s\"}}", crc2);
    return (int)n;
}

int iv_report_build_cfg_json(uint16_t devtype, uint32_t devid, const char *qn,
                             int (*pairs)(char *out, size_t cap, void *arg),
                             void *pairs_arg, char *out, size_t cap)
{
    char   crc2[3];
    char   mid[IV_REPORT_CFG_PAIRS_MAX + 1u];
    size_t n = 0;
    int    m;

    if (out == NULL || cap == 0u)
        return IV_EINVAL;
    if (pairs == NULL)
        return IV_ENOENT;   /* 装配层没挂 ⇒ 让 handler 回 0x74，不发半份配置 */
    m = pairs(mid, sizeof(mid), pairs_arg);
    if (m < 0)
        return IV_ERANGE;
    if ((size_t)m > IV_REPORT_CFG_PAIRS_MAX)
        return IV_ERANGE;
    mid[(size_t)m] = '\0';

    (void)snprintf(crc2, sizeof(crc2), "%02x",
                   (unsigned)iv_report_query_crc(devtype, devid,
                                                 IV_PROTO_CMD_QUERY_CFG));

    /* 层级照抄 `com_query_configuration_function()`：`code` 在顶层，
     * `qn` 在 `data` **里面**（与 E3 相反），`crc` 是 data 的最后一个键。 */
    APPEND(out, cap, n, "{\"code\":0,\"data\":{");
    APPEND(out, cap, n, "\"ver\":\"%02x\",", (unsigned)IV_PROTO_VER);
    APPEND(out, cap, n, "\"devtype\":\"%04x\",", (unsigned)devtype);
    APPEND(out, cap, n, "\"qn\":\"%s\",", (qn != NULL && qn[0] != '\0') ? qn : "0");
    APPEND(out, cap, n, "\"tid\":\"%x\",", (unsigned)devid);
    APPEND(out, cap, n, "\"cmd\":\"%02X\",", (unsigned)IV_PROTO_CMD_QUERY_CFG);
    if (m > 0)
        APPEND(out, cap, n, "%s,", mid);
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
