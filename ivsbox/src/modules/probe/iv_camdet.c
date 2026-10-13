/*
 * 摄像机网络状态探测实现。口径、五态编码与取舍见 include/ivsbox/iv_camdet.h。
 *
 * 三条路径：
 *   PING      —— 内嵌 iv_probe 实例（发探/收包/超时全由引擎做）→ 本文件只
 *                "采样新完成的包 → 压 4 包环 → 集满 4 个结算一轮 → 映射五态"。
 *   端口扫描  —— 本文件用非阻塞 connect ＋ tick 推进的串行状态机做。
 *   RTSP      —— 同上，多一步"发 OPTIONS 收响应"。
 * 三者互斥，切换时丢弃在飞资源。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_camdet.h"
#include "ivsbox/iv_clock.h"
#include "ivsbox/iv_log.h"

#define CD_MOD "camdet"

/* 常见摄像机端口表（现网 port_scan.c 的 camera_ports[] 逐字照抄） */
static const uint16_t s_ports[IV_CAMDET_PORT_TABLE_N] = {
    80u, 554u, 8000u, 37777u, 5000u, 8080u, 8899u
};

const uint16_t *iv_camdet_port_table(void)
{
    return s_ports;
}

/* ---------------------------------------------------------------------------
 * 默认注入点
 * ------------------------------------------------------------------------- */
static int cd_socket(int domain, int type, int proto)
{
    return socket(domain, type, proto);
}
static int cd_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    return connect(fd, addr, len);
}
static int cd_getsockopt(int fd, int level, int optname, void *optval,
                         socklen_t *optlen)
{
    return getsockopt(fd, level, optname, optval, optlen);
}
static ssize_t cd_send(int fd, const void *buf, size_t len, int flags)
{
    return send(fd, buf, len, flags);
}
static ssize_t cd_recv(int fd, void *buf, size_t len, int flags)
{
    return recv(fd, buf, len, flags);
}
static int cd_close(int fd)
{
    return close(fd);
}
static uint64_t cd_now_ms(void)
{
    return iv_clock_monotonic_ms();
}

static const iv_camdet_iops_t s_iops = {
    cd_socket, cd_connect, cd_getsockopt, cd_send, cd_recv, cd_close, cd_now_ms
};

const iv_camdet_iops_t *iv_camdet_default_iops(void)
{
    return &s_iops;
}

/* ---------------------------------------------------------------------------
 * 配置与枚举转名
 * ------------------------------------------------------------------------- */
void iv_camdet_cfg_default(iv_camdet_cfg_t *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode       = IV_CAMDET_MODE_PING;
    cfg->rtt_thr_ms = IV_CAMDET_RTT_THR_MS_DEFAULT;
    cfg->round_ms   = IV_CAMDET_ROUND_MS_DEFAULT;
}

const char *iv_camdet_status_name(iv_camdet_status_t s)
{
    switch (s) {
    case IV_CAMDET_NO_IP:  return "noip";
    case IV_CAMDET_NORMAL: return "normal";
    case IV_CAMDET_FAULT:  return "fault";
    case IV_CAMDET_DELAY:  return "delay";
    case IV_CAMDET_LOSS:   return "loss";
    default:               return "?";
    }
}

const char *iv_camdet_mode_name(int mode)
{
    switch (mode) {
    case IV_CAMDET_MODE_PING: return "ping";
    case IV_CAMDET_MODE_PORT: return "port";
    case IV_CAMDET_MODE_RTSP: return "rtsp";
    default:                  return "?";
    }
}

static int mode_valid(int mode)
{
    return (mode == IV_CAMDET_MODE_PING || mode == IV_CAMDET_MODE_PORT ||
            mode == IV_CAMDET_MODE_RTSP);
}

static uint32_t round_ms_of(const iv_camdet_t *d)
{
    return (d->cfg.round_ms != 0u) ? d->cfg.round_ms : IV_CAMDET_ROUND_MS_DEFAULT;
}

static const iv_camdet_iops_t *iops_of(const iv_camdet_t *d)
{
    return (d->cfg.iops != NULL) ? d->cfg.iops : iv_camdet_default_iops();
}

/* ---------------------------------------------------------------------------
 * 生命周期
 * ------------------------------------------------------------------------- */
void iv_camdet_init(iv_camdet_t *d, const iv_camdet_cfg_t *cfg)
{
    int ch;

    if (d == NULL)
        return;
    memset(d, 0, sizeof(*d));
    if (cfg != NULL)
        d->cfg = *cfg;
    else
        iv_camdet_cfg_default(&d->cfg);
    if (!mode_valid(d->cfg.mode))
        d->cfg.mode = IV_CAMDET_MODE_PING;
    if (d->cfg.rtt_thr_ms == 0u)
        d->cfg.rtt_thr_ms = IV_CAMDET_RTT_THR_MS_DEFAULT;
    if (d->cfg.round_ms == 0u)
        d->cfg.round_ms = IV_CAMDET_ROUND_MS_DEFAULT;

    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        d->ch_idx[ch]       = -1;
        d->ch_ip_be[ch]     = 0u;
        d->vd[ch]           = IV_CAMDET_NO_IP;
        d->st[ch]           = IV_CAMDET_NO_IP;
        d->ch_changed_ms[ch] = 0u;
    }
    d->fd      = -1;
    d->link_up = 1; /* 开机先假设网线在位，装配层随后用 set_link() 刷新 */
    /* 引擎本体也要置成“未 start”的合法初值（icmp_fd = -1）：
     * open() 之前不会驱动它，但 init 完后取 fd 计数必须不会把 fd 0 当成活动套接字。 */
    (void)iv_probe_init(&d->pb, NULL);
}

/* 用当前通道表重建 PING 引擎的项（引擎无删项接口，只能整份重建） */
static void pb_rebuild(iv_camdet_t *d)
{
    iv_probe_cfg_t pc;
    int            ch;

    if (d->pb.started)
        (void)iv_probe_stop(&d->pb);

    iv_probe_cfg_default(&pc);
    pc.interval_ms          = IV_CAMDET_PING_INTERVAL_MS;
    pc.timeout_ms           = IV_CAMDET_PING_TIMEOUT_MS;
    pc.fail_n               = 1u;
    pc.ok_n                 = 1u;
    pc.degrade_rtt_ms       = 0u;
    pc.degrade_loss_permille = 0u;
    pc.iops                 = d->cfg.pb_iops;
    (void)iv_probe_init(&d->pb, &pc);

    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        char name[IV_PROBE_NAME_LEN];
        int  idx;

        d->ch_idx[ch] = -1;
        if (d->ch_ip_be[ch] == 0u)
            continue;
        /* 通道名只用于诊断（"cam0"…"cam5"），手工拼，不引 snprintf */
        name[0] = 'c';
        name[1] = 'a';
        name[2] = 'm';
        name[3] = (char)('0' + ch);
        name[4] = '\0';
        idx = iv_probe_add_icmp(&d->pb, d->ch_ip_be[ch], 0, name);
        if (idx < 0) {
            /* 同一 IP 配了两路（IV_EEXIST）或项表满：该路无项，结论保持"无 IP" */
            d->dup_ip++;
            IV_LOG_W(CD_MOD, "channel %d probe item rejected (%d)", ch, idx);
            continue;
        }
        d->ch_idx[ch] = idx;
    }
    (void)iv_probe_start(&d->pb, d->now_ms);
    if (iv_probe_icmp_available(&d->pb) == 0)
        IV_LOG_W(CD_MOD, "raw ICMP unavailable, ping mode will not produce verdicts");
}

int iv_camdet_open(iv_camdet_t *d, const iv_camdet_cfg_t *cfg, uint64_t now_ms)
{
    int ch;

    if (d == NULL)
        return IV_EINVAL;
    if (d->opened)
        return IV_ESTATE;

    if (cfg != NULL) {
        d->cfg = *cfg;
        if (!mode_valid(d->cfg.mode))
            d->cfg.mode = IV_CAMDET_MODE_PING;
        if (d->cfg.rtt_thr_ms == 0u)
            d->cfg.rtt_thr_ms = IV_CAMDET_RTT_THR_MS_DEFAULT;
        if (d->cfg.round_ms == 0u)
            d->cfg.round_ms = IV_CAMDET_ROUND_MS_DEFAULT;
    }
    d->now_ms = now_ms;
    d->fd     = -1;
    d->opened = 1;
    d->link_up = 1; /* 装配层 seed 前先假设在位，避免开机瞬间把全部通道判成故障 */
    d->sc_on   = 0;
    d->sc_ch   = 0;
    d->sc_step = 0;
    d->sc_port = 0;
    d->sc_bits = 0u;
    d->next_round_ms = now_ms;

    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        d->ch_idx[ch]  = -1;
        d->vd[ch]      = (d->ch_ip_be[ch] == 0u) ? IV_CAMDET_NO_IP : d->vd[ch];
        d->st[ch]      = d->vd[ch];
        d->ch_changed_ms[ch] = 0u;
        d->pk_pos[ch]  = 0u;
        d->pk_n[ch]    = 0u;
        d->pv_sent[ch] = 0u;
        d->pv_lost[ch] = 0u;
    }

    pb_rebuild(d);
    IV_LOG_I(CD_MOD, "open: mode=%s rtt_thr=%ums round=%ums",
             iv_camdet_mode_name(d->cfg.mode), (unsigned)d->cfg.rtt_thr_ms,
             (unsigned)round_ms_of(d));
    return IV_OK;
}

static void close_fd(iv_camdet_t *d)
{
    if (d->fd >= 0) {
        (void)iops_of(d)->close(d->fd);
        d->fd = -1;
    }
}

void iv_camdet_close(iv_camdet_t *d)
{
    if (d == NULL)
        return;
    close_fd(d);
    if (d->pb.started)
        (void)iv_probe_stop(&d->pb);
    d->opened = 0;
}

/* ---------------------------------------------------------------------------
 * 对外状态重算（含"网线断开"覆盖）与沿回调
 * ------------------------------------------------------------------------- */
static int refresh(iv_camdet_t *d, uint64_t now_ms)
{
    int changed = 0;
    int ch;

    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        iv_camdet_status_t ns;

        if (d->ch_ip_be[ch] == 0u)
            ns = IV_CAMDET_NO_IP;
        else if (!d->link_up)
            ns = IV_CAMDET_FAULT; /* 现网：网线断开 → 已配 IP 的摄像机一律 2 */
        else
            ns = d->vd[ch];

        if (ns != d->st[ch]) {
            iv_camdet_status_t old = d->st[ch];

            d->st[ch]           = ns;
            d->ch_changed_ms[ch] = now_ms;
            changed++;
            if (d->cfg.on_change != NULL)
                d->cfg.on_change(ch, old, ns, d->cfg.cb_arg);
            IV_LOG_I(CD_MOD, "ch%d %s -> %s", ch, iv_camdet_status_name(old),
                     iv_camdet_status_name(ns));
        }
    }
    return changed;
}

/* ---------------------------------------------------------------------------
 * 运行期设置
 * ------------------------------------------------------------------------- */
int iv_camdet_set_channels(iv_camdet_t *d, const uint32_t *ip_be, uint64_t now_ms)
{
    int ch;
    int dirty = 0;

    if (d == NULL)
        return IV_EINVAL;
    if (!d->opened)
        return IV_ESTATE;
    if (ip_be == NULL)
        return IV_EINVAL;

    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        if (d->ch_ip_be[ch] == ip_be[ch])
            continue;
        d->ch_ip_be[ch] = ip_be[ch];
        d->vd[ch]       = (ip_be[ch] == 0u) ? IV_CAMDET_NO_IP : d->vd[ch];
        d->pk_pos[ch]   = 0u;
        d->pk_n[ch]     = 0u;
        d->pv_sent[ch]  = 0u;
        d->pv_lost[ch]  = 0u;
        dirty++;
    }
    if (dirty == 0)
        return 0;

    d->now_ms = now_ms;
    if (d->cfg.mode == IV_CAMDET_MODE_PING) {
        pb_rebuild(d);
    } else {
        /* 新表：收掉在飞的旧轮（否则它会拿着旧通道序号继续跑）并下一 tick 开新轮 */
        close_fd(d);
        d->sc_on         = 0;
        d->sc_ch         = 0;
        d->sc_step       = 0;
        d->sc_port       = 0;
        d->sc_bits       = 0u;
        d->tx_len        = 0u;
        d->tx_off        = 0u;
        d->next_round_ms = now_ms;
    }
    return refresh(d, now_ms);
}

int iv_camdet_set_mode(iv_camdet_t *d, int mode, uint64_t now_ms)
{
    if (d == NULL)
        return IV_EINVAL;
    if (!d->opened)
        return IV_ESTATE;
    if (!mode_valid(mode))
        return IV_EINVAL;
    if (d->cfg.mode == mode)
        return 0;

    d->cfg.mode      = mode;
    d->now_ms        = now_ms;
    d->sc_on         = 0;
    d->sc_ch         = 0;
    d->sc_step       = 0;
    d->sc_port       = 0;
    d->sc_bits       = 0u;
    d->tx_len        = 0u;
    d->tx_off        = 0u;
    close_fd(d);
    d->next_round_ms = now_ms; /* 立即开新方式的第一轮 */

    if (mode == IV_CAMDET_MODE_PING) {
        pb_rebuild(d);
    } else if (d->pb.started) {
        (void)iv_probe_stop(&d->pb); /* 非 PING 方式不需要 ICMP 流量 */
    }
    IV_LOG_I(CD_MOD, "mode -> %s", iv_camdet_mode_name(mode));
    return 0;
}

int iv_camdet_set_link(iv_camdet_t *d, int up, uint64_t now_ms)
{
    if (d == NULL)
        return IV_EINVAL;

    d->link_up = (up != 0) ? 1 : 0;
    d->now_ms  = now_ms;
    return refresh(d, now_ms);
}

/* ---------------------------------------------------------------------------
 * PING：驱动引擎 + 采样 + 轮结算
 * ------------------------------------------------------------------------- */
static void probe_drive(iv_camdet_t *d, uint64_t now_ms)
{
    int n;
    int i;

    (void)iv_probe_tick(&d->pb, now_ms);
    n = iv_probe_fd_count(&d->pb);
    for (i = 0; i < n; i++) {
        int fd = iv_probe_fd_at(&d->pb, i);

        if (fd < 0)
            continue;
        (void)iv_probe_on_readable(&d->pb, fd, now_ms);
        (void)iv_probe_on_writable(&d->pb, fd, now_ms);
    }
}

static void ring_push(iv_camdet_t *d, int ch, int ok, uint32_t rtt)
{
    uint8_t pos = d->pk_pos[ch];

    d->pk_ok[ch][pos]  = (uint8_t)(ok ? 1 : 0);
    d->pk_rtt[ch][pos] = ok ? rtt : 0u;
    d->pk_pos[ch]      = (uint8_t)((pos + 1u) % IV_CAMDET_PING_SAMPLES);
    if (d->pk_n[ch] < IV_CAMDET_PING_SAMPLES)
        d->pk_n[ch]++;
}

static void ring_clear(iv_camdet_t *d, int ch)
{
    d->pk_pos[ch] = 0u;
    d->pk_n[ch]   = 0u;
}

/*
 * 本轮（4 个样本）→ 五态。与现网 lwip_ping_multi_result_to_status() 同判据：
 *   全应答 → 平均 RTT ≥ 阈值 ? 延时(4) : 正常(1)
 *   部分应答 → 丢包(5)
 *   全无应答 → 故障(2)
 */
static iv_camdet_status_t ping_verdict(const iv_camdet_t *d, int ch)
{
    uint32_t sum   = 0u;
    int      acked = 0;
    int      i;
    uint32_t avg;

    for (i = 0; i < (int)IV_CAMDET_PING_SAMPLES; i++) {
        if (d->pk_ok[ch][i] != 0u) {
            sum += d->pk_rtt[ch][i];
            acked++;
        }
    }
    if (acked == 0)
        return IV_CAMDET_FAULT;
    if (acked < (int)IV_CAMDET_PING_SAMPLES)
        return IV_CAMDET_LOSS;
    avg = sum / (uint32_t)acked;
    if (d->cfg.rtt_thr_ms != 0u && avg >= d->cfg.rtt_thr_ms)
        return IV_CAMDET_DELAY;
    return IV_CAMDET_NORMAL;
}

/*
 * 采样：每 tick 取一次引擎统计增量。包间隔（5 s）远大于 tick 周期（200 ms），
 * 故增量最多 1；`last_rtt_ms` 只在成功时更新，因此"刚完成的那个样本"的成败与
 * RTT 都能无歧义地取到。
 */
static int sample_ping(iv_camdet_t *d, uint64_t now_ms)
{
    int settled = 0;
    int ch;

    (void)now_ms;
    for (ch = 0; ch < IV_CAMDET_CH_N; ch++) {
        iv_probe_stat_t s;
        uint32_t        dsent;
        uint32_t        dlost;

        if (d->ch_idx[ch] < 0)
            continue;
        if (iv_probe_stat_get(&d->pb, d->ch_idx[ch], &s) != IV_OK)
            continue;

        dsent = s.sent - d->pv_sent[ch];
        dlost = s.lost - d->pv_lost[ch];
        if (dsent == 0u)
            continue;
        d->pv_sent[ch] = s.sent;
        d->pv_lost[ch] = s.lost;

        while (dsent > 0u) {
            int fail = 0;

            if (dlost > 0u) {
                fail = 1;
                dlost--;
            }
            ring_push(d, ch, fail ? 0 : 1, fail ? 0u : s.last_rtt_ms);
            dsent--;

            if (d->pk_n[ch] >= IV_CAMDET_PING_SAMPLES) {
                d->vd[ch] = ping_verdict(d, ch);
                ring_clear(d, ch);
                d->rounds++;
                settled++;
            }
        }
    }
    return settled;
}

/* ---------------------------------------------------------------------------
 * 端口扫描 / RTSP：共用的连接阶段
 * ------------------------------------------------------------------------- */
static uint16_t current_port(const iv_camdet_t *d)
{
    if (d->cfg.mode == IV_CAMDET_MODE_RTSP)
        return (uint16_t)IV_CAMDET_RTSP_PORT;
    if (d->sc_port < 0 || d->sc_port >= (int)IV_CAMDET_PORT_TABLE_N)
        return 0u;
    return s_ports[d->sc_port];
}

/* 发起一次非阻塞 connect。
 * 返回 0 = 已立刻成功；1 = 进行中（fd 存 d->fd）；-1 = 立刻失败。 */
static int start_connect(iv_camdet_t *d, uint64_t now_ms)
{
    const iv_camdet_iops_t *io = iops_of(d);
    struct sockaddr_in      sa;
    int                     fd;
    int                     rc;

    fd = io->socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(current_port(d));
    sa.sin_addr.s_addr = d->ch_ip_be[d->sc_ch];

    rc = io->connect(fd, (const struct sockaddr *)&sa, (socklen_t)sizeof(sa));
    if (rc == 0) {
        (void)io->close(fd);
        return 0;
    }
    if (errno != EINPROGRESS && errno != EINTR) {
        (void)io->close(fd);
        return -1;
    }
    d->fd             = fd;
    d->fd_deadline_ms = now_ms + ((d->cfg.mode == IV_CAMDET_MODE_RTSP)
                                      ? IV_CAMDET_RTSP_TIMEOUT_MS
                                      : IV_CAMDET_PORT_TIMEOUT_MS);
    return 1;
}

/* 连接结果轮询；返回 0 = 成功；1 = 仍在进行；-1 = 失败 */
static int poll_connect(iv_camdet_t *d, uint64_t now_ms)
{
    const iv_camdet_iops_t *io = iops_of(d);
    int                     err = 0;
    socklen_t               el  = (socklen_t)sizeof(err);

    if (now_ms >= d->fd_deadline_ms) {
        close_fd(d);
        return -1;
    }
    if (io->getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0) {
        close_fd(d);
        return -1;
    }
    if (err == 0)
        return 0;
    if (err == EINPROGRESS || err == EALREADY)
        return 1;
    close_fd(d);
    return -1;
}

/* 一台做完：写结论 → 前进到下一台；全做完则收轮 */
static void next_channel(iv_camdet_t *d, uint64_t now_ms)
{
    d->sc_step = 0;
    d->sc_port = 0;
    d->sc_bits = 0u;
    d->tx_len  = 0u;
    d->tx_off  = 0u;
    close_fd(d);

    while (d->sc_ch < IV_CAMDET_CH_N && d->ch_ip_be[d->sc_ch] == 0u) {
        d->vd[d->sc_ch] = IV_CAMDET_NO_IP;
        d->ch_no_ip++;
        d->sc_ch++;
    }
    if (d->sc_ch >= IV_CAMDET_CH_N) {
        d->sc_on         = 0;
        d->next_round_ms = now_ms + round_ms_of(d);
        d->scan_rounds++;
    }
}

/* 轮起点判定：到期（或从未跑过）则开一轮；返回 1 = 有一轮在进行 */
static int round_begin(iv_camdet_t *d, uint64_t now_ms)
{
    if (!d->sc_on) {
        if (d->next_round_ms != 0u && now_ms < d->next_round_ms)
            return 0;
        d->sc_on   = 1;
        d->sc_ch   = 0;
        d->sc_step = 0;
        d->sc_port = 0;
        d->sc_bits = 0u;
        d->tx_len  = 0u;
        d->tx_off  = 0u;
        while (d->sc_ch < IV_CAMDET_CH_N && d->ch_ip_be[d->sc_ch] == 0u) {
            d->vd[d->sc_ch] = IV_CAMDET_NO_IP;
            d->ch_no_ip++;
            d->sc_ch++;
        }
        if (d->sc_ch >= IV_CAMDET_CH_N) {
            d->sc_on         = 0;
            d->next_round_ms = now_ms + round_ms_of(d);
            d->scan_rounds++;
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 端口扫描：逐台逐端口，任一端口可连 → 正常，全部不可连 → 故障
 * ------------------------------------------------------------------------- */
static void port_tick(iv_camdet_t *d, uint64_t now_ms)
{
    int rc;
    int guard = 0;

    if (!round_begin(d, now_ms))
        return;

    /* 一次 tick 内尽量往前推进（每个端口至少占一个 tick，故 guard 只为防死循环） */
    while (d->sc_on && guard++ < 64) {
        if (d->sc_step == 0) {
            rc = start_connect(d, now_ms);
            if (rc == 0) {
                d->sc_bits |= (uint32_t)(1u << d->sc_port);
                d->sc_port++;
                if (d->sc_port >= (int)IV_CAMDET_PORT_TABLE_N) {
                    d->vd[d->sc_ch] = (d->sc_bits != 0u) ? IV_CAMDET_NORMAL
                                                         : IV_CAMDET_FAULT;
                    d->sc_ch++;
                    next_channel(d, now_ms);
                }
                continue;
            }
            if (rc < 0) {
                d->sc_port++;
                if (d->sc_port >= (int)IV_CAMDET_PORT_TABLE_N) {
                    d->vd[d->sc_ch] = (d->sc_bits != 0u) ? IV_CAMDET_NORMAL
                                                         : IV_CAMDET_FAULT;
                    d->sc_ch++;
                    next_channel(d, now_ms);
                }
                continue;
            }
            d->sc_step = 1; /* 进行中：等下一个 tick */
            return;
        }

        /* sc_step == 1：等连接结果 */
        rc = poll_connect(d, now_ms);
        if (rc == 1)
            return; /* 仍在进行 */
        if (rc == 0)
            d->sc_bits |= (uint32_t)(1u << d->sc_port);
        d->sc_port++;
        d->sc_step = 0;
        if (d->sc_port >= (int)IV_CAMDET_PORT_TABLE_N) {
            d->vd[d->sc_ch] =
                (d->sc_bits != 0u) ? IV_CAMDET_NORMAL : IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return; /* 换台后留到下一个 tick，避免 6 台在一拍里跑完 */
        }
    }
}

/* ---------------------------------------------------------------------------
 * RTSP：connect 554 → 发 OPTIONS → 收到任意字节即正常
 * ------------------------------------------------------------------------- */
static int rtsp_build(iv_camdet_t *d, int ch)
{
    char            ipstr[IV_CAMDET_IP_STR_MAX];
    struct in_addr  in;
    int             n;

    in.s_addr = d->ch_ip_be[ch];
    if (inet_ntop(AF_INET, &in, ipstr, sizeof(ipstr)) == NULL)
        return -1;
    n = snprintf(d->tx, sizeof(d->tx),
                 "OPTIONS rtsp://%s:%u/ RTSP/1.0\r\n"
                 "CSeq: 2\r\n"
                 "User-Agent: LibVLC/3.0.19 (LIVE555 Streaming Media v2016.11.28)"
                 "\r\n\r\n",
                 ipstr, (unsigned)IV_CAMDET_RTSP_PORT);
    if (n <= 0 || n >= (int)sizeof(d->tx))
        return -1;
    d->tx_len = (size_t)n;
    d->tx_off = 0u;
    return 0;
}

static void rtsp_tick(iv_camdet_t *d, uint64_t now_ms)
{
    const iv_camdet_iops_t *io = iops_of(d);

    if (!round_begin(d, now_ms))
        return;

    switch (d->sc_step) {
    case 0: {
        int rc = start_connect(d, now_ms);

        if (rc == 0) {
            if (rtsp_build(d, d->sc_ch) != 0) {
                d->vd[d->sc_ch] = IV_CAMDET_FAULT;
                d->sc_ch++;
                next_channel(d, now_ms);
                return;
            }
            d->sc_step = 2;
            return;
        }
        if (rc < 0) {
            d->vd[d->sc_ch] = IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        d->sc_step = 1;
        return;
    }
    case 1: {
        int rc = poll_connect(d, now_ms);

        if (rc == 1)
            return;
        if (rc < 0) {
            d->vd[d->sc_ch] = IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        if (rtsp_build(d, d->sc_ch) != 0) {
            d->vd[d->sc_ch] = IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        d->sc_step = 2;
        return;
    }
    case 2: { /* 发请求（可能分多次） */
        ssize_t n = io->send(d->fd, d->tx + d->tx_off, d->tx_len - d->tx_off, 0);

        if (n > 0) {
            d->tx_off += (size_t)n;
            if (d->tx_off >= d->tx_len) {
                d->sc_step        = 3;
                d->fd_deadline_ms = now_ms + IV_CAMDET_RTSP_TIMEOUT_MS;
            }
            return;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        d->vd[d->sc_ch] = IV_CAMDET_FAULT;
        d->sc_ch++;
        next_channel(d, now_ms);
        return;
    }
    case 3: { /* 等响应 */
        uint8_t buf[256];
        ssize_t n;

        if (now_ms >= d->fd_deadline_ms) {
            d->vd[d->sc_ch] = IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        n = io->recv(d->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            /* 现网口径：收到任意 >0 字节即算正常（不校验 200/内容） */
            d->vd[d->sc_ch] = IV_CAMDET_NORMAL;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        if (n == 0) { /* 对端关闭：判定失败 */
            d->vd[d->sc_ch] = IV_CAMDET_FAULT;
            d->sc_ch++;
            next_channel(d, now_ms);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        d->vd[d->sc_ch] = IV_CAMDET_FAULT;
        d->sc_ch++;
        next_channel(d, now_ms);
        return;
    }
    default:
        d->sc_step = 0;
        return;
    }
}

/* ---------------------------------------------------------------------------
 * 驱动
 * ------------------------------------------------------------------------- */
int iv_camdet_tick(iv_camdet_t *d, uint64_t now_ms)
{
    if (d == NULL)
        return IV_EINVAL;
    if (!d->opened)
        return IV_ESTATE;

    d->now_ms = now_ms;

    switch (d->cfg.mode) {
    case IV_CAMDET_MODE_PING:
        probe_drive(d, now_ms);
        (void)sample_ping(d, now_ms);
        break;
    case IV_CAMDET_MODE_PORT:
        port_tick(d, now_ms);
        break;
    case IV_CAMDET_MODE_RTSP:
        rtsp_tick(d, now_ms);
        break;
    default:
        break;
    }
    return refresh(d, now_ms);
}

/* ---------------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------------- */
iv_camdet_status_t iv_camdet_status(const iv_camdet_t *d, int ch)
{
    if (d == NULL || ch < 0 || ch >= IV_CAMDET_CH_N)
        return IV_CAMDET_NO_IP;
    return d->st[ch];
}

iv_camdet_status_t iv_camdet_verdict(const iv_camdet_t *d, int ch)
{
    if (d == NULL || ch < 0 || ch >= IV_CAMDET_CH_N)
        return IV_CAMDET_NO_IP;
    return d->vd[ch];
}

uint32_t iv_camdet_ip(const iv_camdet_t *d, int ch)
{
    if (d == NULL || ch < 0 || ch >= IV_CAMDET_CH_N)
        return 0u;
    return d->ch_ip_be[ch];
}

int iv_camdet_cns(const iv_camdet_t *d, int *out)
{
    int ch;

    if (d == NULL || out == NULL)
        return IV_EINVAL;
    for (ch = 0; ch < IV_CAMDET_CH_N; ch++)
        out[ch] = (int)d->st[ch];
    return IV_OK;
}

int iv_camdet_is_open(const iv_camdet_t *d)
{
    return (d != NULL) ? d->opened : 0;
}

int iv_camdet_mode(const iv_camdet_t *d)
{
    return (d != NULL && d->opened) ? d->cfg.mode : 0;
}

uint64_t iv_camdet_last_change_ms(const iv_camdet_t *d, int ch)
{
    if (d == NULL || ch < 0 || ch >= IV_CAMDET_CH_N)
        return 0u;
    return d->ch_changed_ms[ch];
}

int iv_camdet_samples(const iv_camdet_t *d, int ch)
{
    if (d == NULL || ch < 0 || ch >= IV_CAMDET_CH_N)
        return 0;
    return (int)d->pk_n[ch];
}

uint64_t iv_camdet_rounds(const iv_camdet_t *d)
{
    return (d != NULL) ? d->rounds : 0u;
}
