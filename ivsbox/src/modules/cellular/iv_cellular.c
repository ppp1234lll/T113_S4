/*
 * IVSBox 4G 模组状态 —— 解析器与轮询状态机（实现）
 *
 * 设计口径见 iv_cellular.h 文件头。此处只补三条实现层面的约定：
 *
 * 1. **响应缓冲区 `resp` 累积的是"当前命令的所有完整行"，含终结行**
 *    （`OK` / `ERROR` / `+CME ERROR: n`）。不丢终结行，因为对 `AT+CPIN?` 来说
 *    `+CME ERROR: 10` **本身就是答案**（SIM not inserted），不是"命令失败了"。
 *    把它当失败丢掉，页面就只能显示"未知"，而现场恰恰需要这个"10"。
 *
 * 2. **每条命令单独超时，超时后继续下一条**，不走"整轮放弃"。
 *    共用 AT 口上偶尔丢一条是常态；整轮放弃会让一个字段的偶发失败
 *    拖垮另外十个本来能读到的字段。
 *
 * 3. **数值未知一律 IV_CELL_UNKNOWN（−32768），文本未知一律空串**。
 *    不用 0 当未知：`0 dBm`、`MCC 0` 都是"看起来合法的假值"，
 *    在界面上没法与真实值区分。
 */
#include "ivsbox/iv_cellular.h"

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_ret.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELL_MOD "cell"

/* 失败轮日志的打印间隔（轮数）。一轮默认 30 s，10 轮 ≈ 5 min：
 * 掉线 15 h 也不过 180 条，既能看出"一直在失败"，又不会把日志刷爆。
 * **改这里之前先想清楚**：本模块原先零日志，1795 轮失败一条记录都没有，
 * 现场只能靠页面上的计数倒推原因（2026-10-23）。 */
#define IV_CELL_LOG_EVERY 10u

/* --------------------------------------------------------------------------
 * 小工具
 * -------------------------------------------------------------------------- */

/* 复制到行尾（遇 \r / \n 截断）并裁掉尾部空格。dst 永远 NUL 结尾。 */
static void copy_line(char *dst, size_t cap, const char *src)
{
    size_t n = 0;

    if (dst == NULL || cap == 0)
        return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while (*src != '\0' && *src != '\r' && *src != '\n' && n + 1 < cap) {
        dst[n] = *src;
        n++;
        src++;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t'))
        n--;
    dst[n] = '\0';
}

/* 在响应里找以 prefix 开头的行，返回该行起始指针（不含行首空白）。 */
static const char *find_line(const char *resp, const char *prefix)
{
    size_t      pl;
    const char *p;

    if (resp == NULL || prefix == NULL)
        return NULL;
    pl = strlen(prefix);
    p  = resp;
    while (*p != '\0') {
        while (*p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        if (strncmp(p, prefix, pl) == 0)
            return p;
        while (*p != '\0' && *p != '\n')
            p++;
    }
    return NULL;
}

/* 取第 n 个非空行（n 从 0 起）；越界返回 NULL。 */
static const char *nth_line(const char *resp, int n)
{
    const char *p;
    int         i = 0;

    if (resp == NULL || n < 0)
        return NULL;
    p = resp;
    while (*p != '\0') {
        const char *start;

        while (*p == '\r' || *p == '\n')
            p++;
        if (*p == '\0')
            break;
        start = p;
        while (*p != '\0' && *p != '\n')
            p++;
        if (i == n)
            return start;
        i++;
    }
    return NULL;
}

/* 按逗号切字段，**识别双引号**（引号内的逗号不切），去掉包裹的引号。
 * 返回切出的字段数。QENG 与 CGCONTRDP 都踩这条：APN 与 DNS 里带逗号。 */
static int split_csv(const char *s, char f[][IV_CELL_TXT], int max)
{
    int         n = 0;
    const char *p = s;

    if (s == NULL || max <= 0)
        return 0;
    while (*p != '\0' && n < max) {
        char  *o = f[n];
        size_t k = 0;
        int    quoted = 0;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '"') {
            quoted = 1;
            p++;
        }
        while (*p != '\0') {
            if (quoted) {
                if (*p == '"') {
                    p++;
                    break;
                }
            } else if (*p == ',') {
                break;
            }
            if (k + 1 < IV_CELL_TXT)
                o[k++] = *p;
            p++;
        }
        o[k] = '\0';
        n++;
        if (*p == ',')
            p++;
        else if (*p != '\0' && !quoted)
            break;
        else if (*p == '\0')
            break;
    }
    return n;
}

/* 字符串转整数；空串 / 非数字返回 IV_CELL_UNKNOWN。
 * **必须跳过前导空格**：AT 的响应形如 `+CSQ: 27,99`、`+CGATT: 0`，
 * 冒号后一定有一个空格。不跳就会把每个值都判成"未知"——
 * 这个 bug 在首版验证里同时打掉了 csq 与 attached 两个字段。 */
static int to_int(const char *s)
{
    int         v   = 0;
    int         neg = 0;
    const char *p   = s;

    if (s == NULL)
        return IV_CELL_UNKNOWN;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '\0')
        return IV_CELL_UNKNOWN;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p < '0' || *p > '9')
        return IV_CELL_UNKNOWN;
    while (*p >= '0' && *p <= '9') {
        if (v > 100000000)
            return IV_CELL_UNKNOWN; /* 明显不是我们要的量 */
        v = v * 10 + (*p - '0');
        p++;
    }
    return neg ? -v : v;
}

/* 两位数字补零（MNC 常见 01 / 03，直接 atoi 会掉前导零）。
 * 由调用方用 to_int 存数值、前端补零显示；此处不做字符串处理。 */

/* --------------------------------------------------------------------------
 * 命令表
 * -------------------------------------------------------------------------- */

static const char *const k_cmd_text[IV_CELL_CMD_LAST] = {
    "",
    "ATI",
    "AT+CGMR",
    "AT+CPIN?",
    "AT+QSIMSTAT?",
    "AT+QCCID",
    "AT+CIMI",
    "AT+CSQ",
    "AT+COPS?",
    "AT+CEREG?",
    "AT+CGATT?",
    "AT+QENG=\"servingcell\"",
    "AT+CGCONTRDP"
};

const char *iv_cell_cmd_text(iv_cell_cmd_id_t id)
{
    if ((int)id <= (int)IV_CELL_CMD_IDLE || (int)id >= (int)IV_CELL_CMD_LAST)
        return "";
    return k_cmd_text[(int)id];
}

/* --------------------------------------------------------------------------
 * 信号格换算
 * -------------------------------------------------------------------------- */

int iv_cell_level_from_csq(int csq)
{
    if (csq < 0 || csq > 31)
        return -1;          /* 99 = 未知，或越界 */
    if (csq <= 1)
        return 0;
    if (csq <= 5)
        return 1;
    if (csq <= 10)
        return 2;
    if (csq <= 14)
        return 3;
    if (csq <= 19)
        return 4;
    return 5;
}

/* --------------------------------------------------------------------------
 * 各命令的解析
 * -------------------------------------------------------------------------- */

static void parse_ati(iv_cell_t *c, const char *resp)
{
    /* 典型响应（实测）：
     *   Quectel
     *   EC200G
     *   Revision: EC200GCNLER01A13M04
     * 型号取 "Revision:" 之前最后一个非空行 —— 比"取第二行"稳，
     * 因为部分固件第一行会给完整商品名而不是厂商名。 */
    const char *model = NULL;
    int         i;

    for (i = 0; i < 6; i++) {
        const char *l = nth_line(resp, i);

        if (l == NULL)
            break;
        if (strncmp(l, "Revision:", 9) == 0) {
            const char *v = l + 9;

            while (*v == ' ')
                v++;
            copy_line(c->snap.firmware, sizeof c->snap.firmware, v);
            break;
        }
        if (strncmp(l, "OK", 2) != 0 && strncmp(l, "ATI", 3) != 0)
            model = l;
    }
    if (model != NULL)
        copy_line(c->snap.model, sizeof c->snap.model, model);
}

static void parse_cgmr(iv_cell_t *c, const char *resp)
{
    const char *l = nth_line(resp, 0);

    if (l != NULL && strncmp(l, "OK", 2) != 0)
        copy_line(c->snap.firmware, sizeof c->snap.firmware, l);
}

static void parse_cpin(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+CPIN:");

    if (l != NULL) {
        const char *v = l + 6;

        while (*v == ' ')
            v++;
        copy_line(c->snap.sim_state, sizeof c->snap.sim_state, v);
        /* 有 "+CPIN:" 行说明模块读到了卡；只有 READY 才算"完全就绪" */
        c->snap.sim_present = 1;
        return;
    }

    /* 没有 "+CPIN:" 行时，CME 错误码就是答案（实测本板回的就是这两条之一）：
     *   10 = SIM not inserted；13 = SIM failure。 */
    if (strstr(resp, "+CME ERROR: 10") != NULL) {
        c->snap.sim_present = 0;
        copy_line(c->snap.sim_state, sizeof c->snap.sim_state, "NOT INSERTED");
    } else if (strstr(resp, "+CME ERROR: 13") != NULL) {
        c->snap.sim_present = -1;   /* 有卡但模块读失败 —— 不能断言"没插" */
        copy_line(c->snap.sim_state, sizeof c->snap.sim_state, "SIM FAILURE");
    } else if (strstr(resp, "ERROR") != NULL) {
        c->snap.sim_present = -1;
        copy_line(c->snap.sim_state, sizeof c->snap.sim_state, "QUERY FAILED");
    }
}

static void parse_qsimstat(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+QSIMSTAT:");

    if (l == NULL)
        return;
    /* 格式 `+QSIMSTAT: <enable>,<inserted>`，取第二个数。 */
    {
        const char *comma = strchr(l + 10, ',');

        if (comma != NULL) {
            int ins = to_int(comma + 1);

            /* 只在 CPIN 没给出结论时采用：CPIN 是权威，QSIMSTAT 是旁证。 */
            if (ins == 0 && c->snap.sim_present < 0) {
                c->snap.sim_present = 0;
                copy_line(c->snap.sim_state, sizeof c->snap.sim_state, "NOT INSERTED");
            } else if (ins == 1 && c->snap.sim_present < 0) {
                c->snap.sim_present = 1;
            }
        }
    }
}

static void parse_iccid(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+QCCID:");

    if (l == NULL)
        l = find_line(resp, "+CCID:");
    if (l == NULL) {
        /* 少数固件直接回一行裸数字 */
        const char *n = nth_line(resp, 0);

        if (n != NULL && n[0] >= '0' && n[0] <= '9' && strncmp(n, "OK", 2) != 0) {
            copy_line(c->snap.iccid, sizeof c->snap.iccid, n);
            return;
        }
        return;
    }
    while (*l != '\0' && *l != ':')
        l++;
    if (*l == ':')
        l++;
    while (*l == ' ')
        l++;
    copy_line(c->snap.iccid, sizeof c->snap.iccid, l);
}

static void parse_cimi(iv_cell_t *c, const char *resp)
{
    const char *l = nth_line(resp, 0);

    if (l != NULL && l[0] >= '0' && l[0] <= '9')
        copy_line(c->snap.imsi, sizeof c->snap.imsi, l);
}

static void parse_csq(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+CSQ:");
    int         rssi;

    if (l == NULL) {
        c->snap.csq  = 99;
        c->snap.dbm  = IV_CELL_UNKNOWN;
        c->snap.level = -1;
        return;
    }
    rssi = to_int(l + 5);
    if (rssi == IV_CELL_UNKNOWN) {
        c->snap.csq = 99;
    } else {
        c->snap.csq = rssi;
    }
    /* 3GPP 27.007：rssi 0..31 → dBm = −113 + 2×rssi；99 = 未知。 */
    if (c->snap.csq >= 0 && c->snap.csq <= 31)
        c->snap.dbm = -113 + 2 * c->snap.csq;
    else
        c->snap.dbm = IV_CELL_UNKNOWN;
    c->snap.level = iv_cell_level_from_csq(c->snap.csq);
}

static void parse_cops(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+COPS:");
    char        f[8][IV_CELL_TXT];
    int         n;

    if (l == NULL)
        return;
    n = split_csv(l + 6, f, 8);
    /* 格式：+COPS: <mode>,<format>,"<oper>",<act>
     * 未注册时只回 "+COPS: 0"（实测本板就是这样），此时清空运营商名。 */
    if (n >= 3 && f[2][0] != '\0') {
        copy_line(c->snap.operator_name, sizeof c->snap.operator_name, f[2]);
    } else {
        c->snap.operator_name[0] = '\0';
    }
}

static void parse_cereg(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+CEREG:");
    char        f[8][IV_CELL_TXT];
    int         n;
    int         stat;

    if (l == NULL)
        return;
    n = split_csv(l + 7, f, 8);
    if (n < 2)
        return;
    stat = to_int(f[1]);
    if (stat == IV_CELL_UNKNOWN)
        return;
    c->snap.cereg_stat = stat;
    switch (stat) {
    case 0: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "未注册，未在搜网"); break;
    case 1: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "已注册（本地网）"); break;
    case 2: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "未注册，正在搜网"); break;
    case 3: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "注册被拒绝"); break;
    case 4: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "未知原因"); break;
    case 5: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "已注册（漫游）"); break;
    default: copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "未知状态"); break;
    }

    /* **TAC / CellID 只从 QENG 取，这里不解析**。
     * CEREG 在部分固件里也会带 `"<TAC>","<CellID>"`（HEX 字符串），但同一件事
     * 两个来源必然分叉：QENG 给十进制、CEREG 给 HEX，而 QENG 在本表里后发、
     * 会把 CEREG 的值覆盖掉 —— 那还不如从一开始就只认 QENG（它是"当前服务小区"，
     * 语义上也更准）。实测本板 CEREG 只回 `0,2`，本来就没有这两项。
     */
}

static void parse_cgatt(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+CGATT:");
    int         v;

    if (l == NULL)
        return;
    v = to_int(l + 7);
    if (v == 0 || v == 1)
        c->snap.attached = v;
}

static void parse_qeng(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+QENG:");
    char        f[20][IV_CELL_TXT];
    int         n;

    if (l == NULL)
        return;
    n = split_csv(l + 6, f, 20);
    /* EC200G 实测（17 个字段，与 Quectel 文档的 LTE servingcell 一致）：
     *   [0] "servingcell" [1] state [2] act [3] is_tdd
     *   [4] MCC [5] MNC [6] cellID [7] PCID [8] earfcn [9] band
     *   [10] UL_bw [11] DL_bw [12] TAC
     *   [13] RSRP [14] RSRQ [15] RSSI [16] SINR [17] srxlev
     * 少一个字段就整体不用 —— 位置一旦错开，报出来的值是"另一个量的值"，
     * 比报未知更危险。 */
    if (n < 18)
        return;
    copy_line(c->snap.act, sizeof c->snap.act, f[2]);
    c->snap.mcc = to_int(f[4]);
    c->snap.mnc = to_int(f[5]);

    c->snap.cell_id = (uint32_t)strtoul(f[6], NULL, 10);
    c->snap.pci     = to_int(f[7]);
    c->snap.earfcn  = to_int(f[8]);
    c->snap.band    = to_int(f[9]);
    c->snap.tac     = to_int(f[12]);
    c->snap.rsrp    = to_int(f[13]);
    c->snap.rsrq    = to_int(f[14]);
    c->snap.sinr    = to_int(f[16]);

    /* QENG 的 state 比 CEREG 更接近现场（LIMSRV / SEARCH / NOCONN）。
     * 只在 CEREG 没给出结论时用它兜底。 */
    if (c->snap.cereg_stat < 0) {
        if (strncmp(f[1], "LIMSRV", 6) == 0)
            copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "仅限紧急服务");
        else if (strncmp(f[1], "SEARCH", 6) == 0)
            copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "正在搜网");
        else if (strncmp(f[1], "NOCONN", 6) == 0)
            copy_line(c->snap.reg_text, sizeof c->snap.reg_text, "无连接");
    }
}

static void parse_cgcontrdp(iv_cell_t *c, const char *resp)
{
    const char *l = find_line(resp, "+CGCONTRDP:");
    char        f[12][IV_CELL_TXT];
    int         n;

    if (l == NULL) {
        /* 没有 `+CGCONTRDP:` 行有两种含义，**不能都记成"没有"**：
         * ① 模组明确回 `ERROR`：上下文确实不在了 ⇒ 0（明确否认）。
         * ② 空应答 / 命令不支持 / 超时：我们**没读到** ⇒ -1（未知）。
         * ECM（`cdc_ether`）模组走 ② 是常态；把它记成 0 会让上层把"读不到"
         * 当成"数据面断了" —— 自愈每小时空拨 5 次就出在这里。 */
        c->snap.pdp_active = (strstr(resp, "ERROR") != NULL) ? 0 : -1;
        return;
    }
    n = split_csv(l + 11, f, 12);
    /* 格式：<cid>,<bearer>,"<apn>","<local addr>.<mask>","<gw>","<dns1>,<dns2>"
     * 只取前四项与 DNS；地址形如 "10.158.0.108.255.255.255.0"，取前四段。 */
    if (n < 4)
        return;
    copy_line(c->snap.apn, sizeof c->snap.apn, f[2]);
    {
        const char *a = f[3];
        int         dots = 0;
        size_t      k = 0;

        while (*a != '\0' && k + 1 < sizeof c->snap.pdp_ip) {
            if (*a == '.') {
                dots++;
                if (dots == 4)
                    break;
            }
            c->snap.pdp_ip[k++] = *a;
            a++;
        }
        c->snap.pdp_ip[k] = '\0';
    }
    if (n >= 6) {
        /* DNS 是一整个引号字段，内部用逗号分隔两个地址 */
        const char *comma = strchr(f[5], ',');

        if (comma != NULL) {
            size_t k = (size_t)(comma - f[5]);

            if (k >= sizeof c->snap.dns1)
                k = sizeof c->snap.dns1 - 1;
            memcpy(c->snap.dns1, f[5], k);
            c->snap.dns1[k] = '\0';
            copy_line(c->snap.dns2, sizeof c->snap.dns2, comma + 1);
        } else {
            copy_line(c->snap.dns1, sizeof c->snap.dns1, f[5]);
        }
    }
    c->snap.pdp_active = (c->snap.pdp_ip[0] != '\0') ? 1 : 0;
}

/* 按当前命令分派解析。`resp` 含终结行。 */
static void parse_current(iv_cell_t *c)
{
    switch (c->cur) {
    case IV_CELL_CMD_ATI:       parse_ati(c, c->resp); break;
    case IV_CELL_CMD_CGMR:      parse_cgmr(c, c->resp); break;
    case IV_CELL_CMD_CPIN:      parse_cpin(c, c->resp); break;
    case IV_CELL_CMD_QSIMSTAT:  parse_qsimstat(c, c->resp); break;
    case IV_CELL_CMD_QCCID:     parse_iccid(c, c->resp); break;
    case IV_CELL_CMD_CIMI:      parse_cimi(c, c->resp); break;
    case IV_CELL_CMD_CSQ:       parse_csq(c, c->resp); break;
    case IV_CELL_CMD_COPS:      parse_cops(c, c->resp); break;
    case IV_CELL_CMD_CEREG:     parse_cereg(c, c->resp); break;
    case IV_CELL_CMD_CGATT:     parse_cgatt(c, c->resp); break;
    case IV_CELL_CMD_QENG:      parse_qeng(c, c->resp); break;
    case IV_CELL_CMD_CGCONTRDP: parse_cgcontrdp(c, c->resp); break;
    default: break;
    }
}

/* --------------------------------------------------------------------------
 * 判定：四层与
 * -------------------------------------------------------------------------- */

static void compute_usable(iv_cell_snap_t *s)
{
    int sim_ok;
    int reg_ok;
    int att_ok;
    int pdp_ok;

    s->usable     = 0;
    s->reason[0]  = '\0';

    /* AT 层不通就别往下猜。
     * 早先这里直接落到 `sim_present < 0` 分支报 "SIM 卡状态未知" —— 而当时的
     * 真实情况是串口接在 diag 口上、一条 AT 都没回（`fail_rounds=1795`、
     * `cmd_timeouts=21540`）。把"串口/模块不回话"说成"卡状态不明"，现场会照着
     * SIM 卡去查，白费工夫。
     * 判据：本轮有 AT 层错误（超时/IO）**且** CSQ 无效（`csq` 有效说明 AT 是通的，
     * 此时 `99` 只是"信号未知"，属于模组正常答复）。 */
    if (s->at_err != 0 && (s->csq < 0 || s->csq > 31)) {
        (void)snprintf(s->reason, sizeof s->reason, "AT 无响应");
        return;
    }

    sim_ok = (s->sim_present == 1) &&
             (strncmp(s->sim_state, "READY", 5) == 0);
    reg_ok = (s->cereg_stat == 1 || s->cereg_stat == 5);
    att_ok = (s->attached == 1);
    pdp_ok = (s->pdp_active != 0) && (s->pdp_ip[0] != '\0');

    if (!sim_ok) {
        if (s->sim_present == 0)
            (void)snprintf(s->reason, sizeof s->reason, "SIM 卡未插入");
        else if (s->sim_present < 0)
            (void)snprintf(s->reason, sizeof s->reason, "SIM 卡状态未知");
        else if (s->sim_state[0] != '\0')
            (void)snprintf(s->reason, sizeof s->reason, "SIM 卡未就绪（%.32s）", s->sim_state);
        else
            (void)snprintf(s->reason, sizeof s->reason, "SIM 卡未就绪");
        return;
    }
    if (!reg_ok) {
        if (s->cereg_stat == 3)
            (void)snprintf(s->reason, sizeof s->reason, "网络注册被拒绝");
        else if (s->cereg_stat == 2)
            (void)snprintf(s->reason, sizeof s->reason, "正在搜网，尚未注册");
        else if (s->reg_text[0] != '\0')
            (void)snprintf(s->reason, sizeof s->reason, "未注册（%.32s）", s->reg_text);
        else
            (void)snprintf(s->reason, sizeof s->reason, "未注册到网络");
        return;
    }
    if (!att_ok) {
        (void)snprintf(s->reason, sizeof s->reason, "PS 域未附着");
        return;
    }
    if (!pdp_ok) {
        (void)snprintf(s->reason, sizeof s->reason, "未建立数据连接（无 PDP IP）");
        return;
    }
    s->usable    = 1;
    s->reason[0] = '\0';
}

/* --------------------------------------------------------------------------
 * 状态机
 * -------------------------------------------------------------------------- */

/* 每轮开始时把"会变化"的字段复位，避免上一轮的残留冒充本轮结果。
 * 不动的：model / firmware / iccid / imsi / sim_state（这些只在读到时才更新）。 */
static void reset_dynamic(iv_cell_snap_t *s)
{
    s->csq        = 99;
    s->dbm        = IV_CELL_UNKNOWN;
    s->level      = -1;
    s->rsrp       = IV_CELL_UNKNOWN;
    s->rsrq       = IV_CELL_UNKNOWN;
    s->sinr       = IV_CELL_UNKNOWN;
    s->cereg_stat = -1;
    s->reg_text[0] = '\0';
    s->attached   = -1;
    s->pdp_active = -1;   /* 本轮还没读到；只有 `ERROR` 或解析出结果才落 0/1 */
    s->pdp_ip[0]  = '\0';
    s->apn[0]     = '\0';
    s->dns1[0]    = '\0';
    s->dns2[0]    = '\0';
    s->act[0]     = '\0';
    s->mcc        = IV_CELL_UNKNOWN;
    s->mnc        = IV_CELL_UNKNOWN;
    s->tac        = IV_CELL_UNKNOWN;
    s->cell_id    = 0;
    s->pci        = IV_CELL_UNKNOWN;
    s->band       = IV_CELL_UNKNOWN;
    s->earfcn     = IV_CELL_UNKNOWN;
}

void iv_cell_init(iv_cell_t *c, uint32_t period_ms)
{
    if (c == NULL)
        return;
    memset(c, 0, sizeof *c);
    c->period_ms      = (period_ms != 0) ? period_ms : IV_CELL_PERIOD_MS;
    c->cur            = IV_CELL_CMD_IDLE;
    c->snap.csq       = 99;
    c->snap.dbm       = IV_CELL_UNKNOWN;
    c->snap.level     = -1;
    c->snap.sim_present = -1;
    c->snap.cereg_stat  = -1;
    c->snap.attached    = -1;
    c->snap.rsrp        = IV_CELL_UNKNOWN;
    c->snap.rsrq        = IV_CELL_UNKNOWN;
    c->snap.sinr        = IV_CELL_UNKNOWN;
    c->snap.mcc         = IV_CELL_UNKNOWN;
    c->snap.mnc         = IV_CELL_UNKNOWN;
    c->snap.tac         = IV_CELL_UNKNOWN;
    c->snap.pci         = IV_CELL_UNKNOWN;
    c->snap.band        = IV_CELL_UNKNOWN;
    c->snap.earfcn      = IV_CELL_UNKNOWN;
    /* 立刻起第一轮：页面刚打开不该等 30 s 才有数 */
    c->next_round_ms = 0;
}

void iv_cell_reset(iv_cell_t *c)
{
    if (c == NULL)
        return;
    /* 只丢状态机，**保留快照**（链路抖一下不该让界面上的信号值瞬间归零） */
    c->running   = 0;
    c->in_cmd    = 0;
    c->cur       = IV_CELL_CMD_IDLE;
    c->line_len  = 0;
    c->resp_len  = 0;
    c->line[0]   = '\0';
    c->resp[0]   = '\0';
    c->deadline_ms   = 0;
    c->next_round_ms = 0;
    /* 链路诊断计数跟着状态机一起重置：换了个口就重新开始数。
     * **`snap.fail_rounds` / `snap.ok_rounds` 不清** —— 它们是累计量，
     * 是"这块板到底坏过多久"的证据，重开串口不该把它抹掉。 */
    c->no_reply_rounds = 0;
    c->timeouts0       = c->snap.cmd_timeouts;
    c->fail_streak     = 0;
}

static void finish_round(iv_cell_t *c, uint64_t now_ms)
{
    uint32_t round_timeouts;
    int      ok;

    c->running = 0;
    c->in_cmd  = 0;
    c->cur     = IV_CELL_CMD_IDLE;
    c->next_round_ms = now_ms + c->period_ms;

    /* "本轮一条都没回"的判据：本轮 12 条命令**全部**超时。
     * 这个数才是"串口接错了/模块不回话"的可靠证据 —— 见 iv_cellular.h 里
     * `no_reply_rounds` 的说明。 */
    round_timeouts = c->snap.cmd_timeouts - c->timeouts0;
    if (round_timeouts >= (uint32_t)((int)IV_CELL_CMD_LAST - 1))
        c->no_reply_rounds++;
    else
        c->no_reply_rounds = 0;

    /* "本轮算成功"的判据：CSQ 读到了有效值。它是每轮必发的动态命令，
     * 能读到说明 AT 通道是通的；其它命令偶发失败不影响这一轮的结论。 */
    ok = (c->snap.csq >= 0 && c->snap.csq <= 31);
    if (ok) {
        c->snap.ok_rounds++;
        c->snap.last_ok_ms = now_ms;
        c->snap.valid      = 1;
        c->snap.at_err     = 0;
        if (c->fail_streak > 0u && c->snap.fail_rounds > 0u)
            IV_LOG_I(CELL_MOD, "AT polling recovered after %u failed rounds",
                     c->fail_streak);
        c->fail_streak = 0;
    } else {
        c->snap.fail_rounds++;
        c->fail_streak++;
        /* 按间隔打印，且**第一轮必打**：刚坏的时候最需要现场信息。 */
        if (c->fail_streak == 1u || (c->snap.fail_rounds % IV_CELL_LOG_EVERY) == 0u) {
            IV_LOG_W(CELL_MOD,
                     "round failed: streak=%u total=%u csq=%d at_err=%d "
                     "timeouts=%u no_reply=%u",
                     c->fail_streak, c->snap.fail_rounds, c->snap.csq,
                     c->snap.at_err, c->snap.cmd_timeouts, c->no_reply_rounds);
        }
    }
    compute_usable(&c->snap);
}

static void finish_cmd(iv_cell_t *c, uint64_t now_ms)
{
    parse_current(c);
    c->in_cmd   = 0;
    c->cur      = (iv_cell_cmd_id_t)((int)c->cur + 1);
    c->resp_len = 0;
    c->line_len = 0;
    c->resp[0]  = '\0';
    c->line[0]  = '\0';
    if ((int)c->cur >= (int)IV_CELL_CMD_LAST)
        finish_round(c, now_ms);
}

/* 一行到手：追加进 resp，再判是否终结行。 */
static void handle_line(iv_cell_t *c, const char *line, uint64_t now_ms)
{
    size_t n;

    if (line == NULL)
        return;

    /* 追加到 resp（带换行），溢出只标记不丢整份 */
    n = strlen(line);
    if (c->resp_len + n + 2 < sizeof c->resp) {
        memcpy(c->resp + c->resp_len, line, n);
        c->resp_len += n;
        c->resp[c->resp_len++] = '\n';
        c->resp[c->resp_len]   = '\0';
    } else {
        c->resp_trunc++;
    }

    if (strcmp(line, "OK") == 0) {
        finish_cmd(c, now_ms);
        return;
    }
    if (strncmp(line, "ERROR", 5) == 0 ||
        strncmp(line, "+CME ERROR:", 11) == 0 ||
        strncmp(line, "+CMS ERROR:", 11) == 0) {
        /* 错误行**已经进 resp**，解析器会从里面取它要的信息（如 CME 10）。 */
        finish_cmd(c, now_ms);
    }
}

int iv_cell_next_cmd(iv_cell_t *c, char *out, size_t cap, uint64_t now_ms)
{
    const char *t;
    int         n;

    if (c == NULL || out == NULL || cap < 8u)
        return IV_EINVAL;
    if (!c->running || c->in_cmd)
        return 0;
    if ((int)c->cur <= (int)IV_CELL_CMD_IDLE || (int)c->cur >= (int)IV_CELL_CMD_LAST)
        return 0;

    t = iv_cell_cmd_text(c->cur);
    if (t[0] == '\0')
        return 0;

    n = snprintf(out, cap, "%s\r", t);
    if (n < 0 || (size_t)n >= cap)
        return IV_ERANGE;

    c->in_cmd      = 1;
    c->deadline_ms = now_ms + IV_CELL_CMD_TIMEOUT_MS;
    c->resp_len    = 0;
    c->resp[0]     = '\0';
    c->line_len    = 0;
    c->line[0]     = '\0';
    return n;
}

int iv_cell_feed(iv_cell_t *c, const char *buf, size_t len, uint64_t now_ms)
{
    size_t i;

    if (c == NULL || buf == NULL)
        return IV_EINVAL;
    /* 没在等响应（例如模块主动上报的 URC）：丢弃，不污染快照 */
    if (!c->running || !c->in_cmd)
        return IV_OK;

    for (i = 0; i < len; i++) {
        char ch = buf[i];

        if (ch == '\r' || ch == '\n') {
            if (c->line_len == 0u)
                continue;
            c->line[c->line_len] = '\0';
            c->line_len = 0;
            handle_line(c, c->line, now_ms);
            if (!c->in_cmd)
                break;  /* 当前命令已结束：同一批里剩下的字节属于下一条，丢掉 */
            continue;
        }
        if (c->line_len + 1u < sizeof c->line)
            c->line[c->line_len++] = ch;
    }
    return IV_OK;
}

void iv_cell_tick(iv_cell_t *c, uint64_t now_ms)
{
    if (c == NULL)
        return;

    /* 起新一轮 */
    if (!c->running && now_ms >= c->next_round_ms) {
        c->running         = 1;
        c->in_cmd          = 0;
        c->cur             = IV_CELL_CMD_ATI;
        c->resp_len        = 0;
        c->line_len        = 0;
        c->resp[0]         = '\0';
        c->line[0]         = '\0';
        c->snap.last_try_ms = now_ms;
        c->timeouts0        = c->snap.cmd_timeouts;   /* 本轮超时数的基准 */
        reset_dynamic(&c->snap);
    }

    /* 单条命令超时：跳过它继续下一条（整轮放弃会让一条偶发失败拖垮其余字段） */
    if (c->running && c->in_cmd && now_ms >= c->deadline_ms) {
        c->snap.cmd_timeouts++;
        c->snap.at_err = IV_ETIMEDOUT;
        c->in_cmd      = 0;
        c->cur         = (iv_cell_cmd_id_t)((int)c->cur + 1);
        c->resp_len    = 0;
        c->line_len    = 0;
        c->resp[0]     = '\0';
        c->line[0]     = '\0';
        if ((int)c->cur >= (int)IV_CELL_CMD_LAST)
            finish_round(c, now_ms);
    }
}

void iv_cell_snapshot(const iv_cell_t *c, iv_cell_snap_t *out)
{
    if (out == NULL)
        return;
    if (c == NULL) {
        memset(out, 0, sizeof *out);
        out->csq    = 99;
        out->dbm    = IV_CELL_UNKNOWN;
        out->level  = -1;
        out->sim_present = -1;
        out->cereg_stat  = -1;
        out->attached    = -1;
        return;
    }
    *out = c->snap;
}

/* ===========================================================================
 * 自动重拨判定
 *
 * 为什么是纯函数：真正的"拨"要 fork 一个出厂脚本、要先让出 AT 口、要等它退，
 * 这些都没法在单测里跑；而"什么时候该拨"才是容易写错的部分（漏一个前置条件
 * 就会对着没插卡的模组每两分钟跑一次脚本）。所以把判定抽出来，用真机快照当输入。
 * =========================================================================== */

void iv_cell_redial_init(iv_cell_redial_t *r)
{
    if (r == NULL)
        return;
    r->last_try_ms     = 0;
    r->window_start_ms = 0;
    r->down_since_ms   = 0;
    r->tries           = 0;
}

int iv_cell_redial_due(const iv_cell_snap_t *s, iv_cell_redial_t *r,
                       int wan_has_ipv4, uint64_t now_ms)
{
    if (s == NULL || r == NULL)
        return 0;
    /* 一轮都没跑完 / 卡没插 / 没注册 / 没附着 ⇒ 不该拨。
     * **这里绝不能清 `down_since_ms`**：AT 轮询是分批发命令的，每轮开头
     * `reset_dynamic()` 会把 `cereg_stat`/`attached`/`pdp_active` 复位成 -1，
     * 一轮中段读到的快照就是"未知"。若在未知时清零，断线计时每 30 s 被打断一次，
     * 永远凑不满 `DOWN_HOLD_MS` ⇒ 自愈彻底失效（实测 `/api/cell` 的快照正是这个形态：
     * `attached=null`、`csq=null`，而 `age_ms≈30 s`）。
     * 计时只由"数据面恢复"清零，见下。 */
    if (!s->valid || s->sim_present != 1 || s->attached != 1 ||
        (s->cereg_stat != 1 && s->cereg_stat != 5))
        return 0;

    /* 数据面：内核侧有地址、且模组没有明确否认，就算通。
     * `pdp_active == -1`（没读到）**不否认** —— ECM 模式下这是常态。 */
    if (wan_has_ipv4 && s->pdp_active != 0) {
        r->down_since_ms = 0;
        return 0;
    }

    /* 走到这里 = 数据面不可用（内核没地址，或模组明确说上下文没了）。
     * 先记起点、这一拍不动手，等持续够久再说。 */
    if (r->down_since_ms == 0) {
        r->down_since_ms = now_ms;
        return 0;
    }
    /* 时钟回拨：按"刚发现"重算，否则永远凑不满持续时长。 */
    if (now_ms < r->down_since_ms) {
        r->down_since_ms = now_ms;
        return 0;
    }
    if (now_ms - r->down_since_ms < IV_CELL_REDIAL_DOWN_HOLD_MS)
        return 0;

    /* 节奏条件一：最小间隔。时钟回拨（now < last）按"没拨过"处理。 */
    if (r->last_try_ms != 0 && now_ms >= r->last_try_ms &&
        now_ms - r->last_try_ms < IV_CELL_REDIAL_GAP_MS)
        return 0;

    /* 节奏条件二：窗口内次数上限。窗口过期由 mark 重置，这里只管"还在窗口内"。 */
    if (r->window_start_ms != 0 && now_ms >= r->window_start_ms &&
        now_ms - r->window_start_ms < IV_CELL_REDIAL_WINDOW_MS &&
        r->tries >= IV_CELL_REDIAL_MAX_TRIES)
        return 0;

    return 1;
}

void iv_cell_redial_mark(iv_cell_redial_t *r, uint64_t now_ms)
{
    if (r == NULL)
        return;
    if (r->window_start_ms == 0 || now_ms < r->window_start_ms ||
        now_ms - r->window_start_ms >= IV_CELL_REDIAL_WINDOW_MS) {
        r->window_start_ms = now_ms;
        r->tries           = 0;
    }
    r->tries++;
    r->last_try_ms = now_ms;
}
