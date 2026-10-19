/*
 * iv_cellular 单测（libivmodules，4G 模组状态）
 *
 * ============================ 样本从哪来 ============================
 * **全部是 2026-10-19 板端 AT 口实测抓下来的原样响应**，不是照文档手编的：
 *   ATI      → "Quectel / EC200G / Revision: EC200GCNLER01A13M04"
 *   CPIN?    → "+CME ERROR: 10"          （SIM not inserted）
 *   QSIMSTAT → "+QSIMSTAT: 0,0"
 *   QCCID    → "+CME ERROR: 13"
 *   CSQ      → "+CSQ: 27,99"
 *   COPS?    → "+COPS: 0"                （未注册时只回这一个数）
 *   CEREG?   → "+CEREG: 0,2"             （搜网中）
 *   CGATT?   → "+CGATT: 0"
 *   QENG     → 17 个字段的 LTE servingcell，state=LIMSRV
 *   CGCONTRDP→ 仅 "OK"                   （无活动 PDP）
 * 用实测样本的意义：字段位置、前导空格、CME 错误码这些"文档里不写"的细节
 * 才是解析器真正会踩的地方。首版就是栽在"冒号后那个空格"上。
 *
 * 第二组样本是"卡正常"的形态（READY + CEREG 1 + CGATT 1 + 有 IP），
 * 用来覆盖 `usable = 1` 这一支 —— 现场这台板当前插不了 SIM，只能靠构造。
 * 构造值严格按 3GPP 27.007 / Quectel 的格式，不臆造字段顺序。
 *
 * ============================ 断言口径 ============================
 * 不做"跑一遍看看"式的检查：每条断言都钉一个**具体值**或**哨兵**。
 * 特别是"未知"必须是 IV_CELL_UNKNOWN 而不是 0 —— `0 dBm` 是合法信号值，
 * 用 0 当未知会让界面把"查不到"显示成"信号极差"。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_cellular.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 板端实测：模块正常、有信号，但**读不到 SIM 卡**
 * ------------------------------------------------------------------------- */
static const char *const k_live_nosim[] = {
    "Quectel\r\nEC200G\r\nRevision: EC200GCNLER01A13M04\r\n\r\nOK\r\n", /* ATI */
    "EC200GCNLER01A13M04\r\n\r\nOK\r\n",                               /* CGMR */
    "\r\n+CME ERROR: 10\r\n",                                          /* CPIN? */
    "\r\n+QSIMSTAT: 0,0\r\n\r\nOK\r\n",                                /* QSIMSTAT? */
    "\r\n+CME ERROR: 13\r\n",                                          /* QCCID */
    "\r\n+CME ERROR: 10\r\n",                                          /* CIMI */
    "\r\n+CSQ: 27,99\r\n\r\nOK\r\n",                                   /* CSQ */
    "\r\n+COPS: 0\r\n\r\nOK\r\n",                                      /* COPS? */
    "\r\n+CEREG: 0,2\r\n\r\nOK\r\n",                                   /* CEREG? */
    "\r\n+CGATT: 0\r\n\r\nOK\r\n",                                     /* CGATT? */
    "\r\n+QENG: \"servingcell\",\"LIMSRV\",\"LTE\",\"FDD\",460,01,9435290,"
    "394,300,1,5,5,5277,-85,-9,-57,69,30\r\n\r\nOK\r\n",               /* QENG */
    "\r\nOK\r\n"                                                       /* CGCONTRDP */
};

/* ---------------------------------------------------------------------------
 * 构造：卡正常、已注册、已附着、拿到 IP（覆盖 usable=1 那一支）
 * ------------------------------------------------------------------------- */
static const char *const k_ok_sim[] = {
    "Quectel\r\nEC200G\r\nRevision: EC200GCNLER01A13M04\r\n\r\nOK\r\n",
    "EC200GCNLER01A13M04\r\n\r\nOK\r\n",
    "\r\n+CPIN: READY\r\n\r\nOK\r\n",                                  /* 卡就绪 */
    "\r\n+QSIMSTAT: 0,1\r\n\r\nOK\r\n",
    "\r\n+QCCID: 89860123456789012345\r\n\r\nOK\r\n",                  /* ICCID 20 位 */
    "\r\n460001234567890\r\n\r\nOK\r\n",                               /* IMSI 15 位 */
    "\r\n+CSQ: 24,99\r\n\r\nOK\r\n",
    "\r\n+COPS: 0,0,\"CHINA UNICOM\",7\r\n\r\nOK\r\n",                 /* 有运营商名 */
    "\r\n+CEREG: 0,1,\"03E2\",\"0B7B8A5D\",7\r\n\r\nOK\r\n",           /* 已注册 + TAC/CellID */
    "\r\n+CGATT: 1\r\n\r\nOK\r\n",
    "\r\n+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,01,9435290,"
    "394,300,1,5,5,5277,-85,-9,-57,20,30\r\n\r\nOK\r\n",
    "\r\n+CGCONTRDP: 1,5,\"cmnet\",\"10.158.0.108.255.255.255.0\",\"10.158.0.1\","
    "\"8.8.8.8,114.114.114.114\"\r\n\r\nOK\r\n"
};

static const int k_cmd_n = (int)(sizeof k_live_nosim / sizeof k_live_nosim[0]);

/* 驱动一轮：模拟 agent 的"取命令 → 写串口 → 读回响应 → 喂解析器"。
 * 返回实际发出去的条数，供断言"命令表有没有漏"。 */
static int run_round(iv_cell_t *c, const char *const *resps, int n, uint64_t *t)
{
    char buf[64];
    int  i    = 0;
    int  sent = 0;

    iv_cell_tick(c, *t);   /* 起新一轮（上一轮结束后 next_round_ms 才到期） */
    for (;;) {
        if (iv_cell_next_cmd(c, buf, sizeof buf, *t) <= 0)
            break;
        if (i < n)
            (void)iv_cell_feed(c, resps[i], strlen(resps[i]), *t + 10);
        i++;
        sent++;
        *t += 50;
        if (sent > 64)
            break;          /* 死循环保护：命令表跑不完就是 bug */
    }
    return sent;
}

/* ---------------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------------- */

/* 1) 初始化后必须全是"未知"，不是 0 */
static void c01_init_unknown(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;

    iv_cell_init(&c, 0);   /* 0 ⇒ 用默认周期 */
    iv_cell_snapshot(&c, &s);

    chk(c.period_ms == IV_CELL_PERIOD_MS, "c01 period falls back to default");
    chk(s.valid == 0, "c01 snapshot not valid before any round");
    chk(s.sim_present == -1, "c01 sim_present starts as unknown(-1)");
    chk(s.cereg_stat == -1, "c01 cereg starts as unknown(-1)");
    chk(s.attached == -1, "c01 attached starts as unknown(-1)");
    chk(s.csq == 99, "c01 csq starts at 99 (spec value for unknown)");
    chk(s.dbm == IV_CELL_UNKNOWN, "c01 dbm starts as sentinel, not 0");
    chk(s.rsrp == IV_CELL_UNKNOWN, "c01 rsrp starts as sentinel, not 0");
    chk(s.level == -1, "c01 level starts as -1");
    chk(s.model[0] == '\0', "c01 model empty");
    chk(s.usable == 0, "c01 not usable before query");
}

/* 2) 板端实测：无 SIM ⇒ usable=0，且原因指向 SIM */
static void c02_live_nosim(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;
    uint64_t       t = 1000;
    int            sent;

    iv_cell_init(&c, 30000);
    sent = run_round(&c, k_live_nosim, k_cmd_n, &t);
    iv_cell_snapshot(&c, &s);

    chk(sent == k_cmd_n, "c02 every command in the table gets sent once");
    chk(s.valid == 1, "c02 round marked valid (CSQ read)");
    chk(s.ok_rounds == 1 && s.fail_rounds == 0, "c02 round counters");

    chk(strcmp(s.model, "EC200G") == 0, "c02 model from ATI");
    chk(strcmp(s.firmware, "EC200GCNLER01A13M04") == 0, "c02 firmware from ATI/CGMR");

    chk(s.sim_present == 0, "c02 CME ERROR 10 => SIM not inserted");
    chk(strcmp(s.sim_state, "NOT INSERTED") == 0, "c02 sim_state text");
    chk(s.iccid[0] == '\0' && s.imsi[0] == '\0', "c02 iccid/imsi stay empty when unreadable");

    chk(s.csq == 27, "c02 csq raw 27");
    chk(s.dbm == -59, "c02 dbm = -113 + 2*27");
    chk(s.level == 5, "c02 level 5/5 for csq 27");

    chk(s.cereg_stat == 2, "c02 cereg stat 2 (searching)");
    chk(strstr(s.reg_text, "搜网") != NULL, "c02 reg_text explains stat 2");
    chk(s.attached == 0, "c02 CGATT 0 => not attached");
    chk(s.operator_name[0] == '\0', "c02 +COPS: 0 leaves operator empty");

    chk(s.mcc == 460 && s.mnc == 1, "c02 mcc/mnc from QENG");
    chk(s.cell_id == 9435290u && s.pci == 394, "c02 cell id / pci");
    chk(s.tac == 5277 && s.band == 1 && s.earfcn == 300, "c02 tac/band/earfcn");
    chk(s.rsrp == -85 && s.rsrq == -9 && s.sinr == 69, "c02 rsrp/rsrq/sinr raw");
    chk(strcmp(s.act, "LTE") == 0, "c02 act LTE");

    chk(s.pdp_active == 0 && s.pdp_ip[0] == '\0', "c02 bare OK => no PDP");
    chk(s.usable == 0, "c02 not usable with no SIM");
    chk(strstr(s.reason, "SIM") != NULL, "c02 reason points at SIM");
}

/* 3) 卡正常 ⇒ usable=1，IP/APN/DNS 都解析出来 */
static void c03_usable(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;
    uint64_t       t = 1000;

    iv_cell_init(&c, 30000);
    (void)run_round(&c, k_ok_sim, k_cmd_n, &t);
    iv_cell_snapshot(&c, &s);

    chk(s.sim_present == 1, "c03 +CPIN: READY => present");
    chk(strcmp(s.sim_state, "READY") == 0, "c03 sim_state READY");
    chk(strcmp(s.iccid, "89860123456789012345") == 0, "c03 iccid 20 digits");
    chk(strcmp(s.imsi, "460001234567890") == 0, "c03 imsi 15 digits");

    chk(s.cereg_stat == 1, "c03 registered (stat 1)");
    chk(s.attached == 1, "c03 attached");
    chk(strcmp(s.operator_name, "CHINA UNICOM") == 0, "c03 operator name");

    /* TAC / CellID 唯一来源是 QENG（十进制）。CEREG 那份 HEX 刻意不解析 ——
     * 同一件事两个来源必然分叉，而 QENG 后发会把它覆盖掉。 */
    chk(s.tac == 5277, "c03 TAC comes from QENG");
    chk(s.cell_id == 9435290u, "c03 cell id comes from QENG");

    chk(strcmp(s.pdp_ip, "10.158.0.108") == 0, "c03 PDP ip first four octets");
    chk(strcmp(s.apn, "cmnet") == 0, "c03 APN");
    chk(strcmp(s.dns1, "8.8.8.8") == 0, "c03 DNS1");
    chk(strcmp(s.dns2, "114.114.114.114") == 0, "c03 DNS2");
    chk(s.pdp_active == 1, "c03 pdp active");

    chk(s.usable == 1, "c03 usable when sim+reg+attach+pdp all pass");
    chk(s.reason[0] == '\0', "c03 no reason when usable");
}

/* 4) 单条超时：跳过它，后续命令照常解析（不能一条卡死整轮） */
static void c04_timeout_skip(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;
    uint64_t       t = 1000;
    char           buf[64];
    int            i   = 0;
    int            sent = 0;

    iv_cell_init(&c, 30000);
    iv_cell_tick(&c, t);
    for (;;) {
        if (iv_cell_next_cmd(&c, buf, sizeof buf, t) <= 0)
            break;
        if (i == 6) {                    /* 第 7 条（CSQ）不回 */
            t += IV_CELL_CMD_TIMEOUT_MS + 20;
            iv_cell_tick(&c, t);
        } else if (i < k_cmd_n) {
            (void)iv_cell_feed(&c, k_live_nosim[i], strlen(k_live_nosim[i]), t + 10);
        }
        i++;
        sent++;
        t += 50;
    }
    iv_cell_snapshot(&c, &s);

    chk(sent == k_cmd_n, "c04 timeout still sends the remaining commands");
    chk(s.cmd_timeouts == 1, "c04 exactly one command timed out");
    chk(s.at_err == IV_ETIMEDOUT, "c04 last AT error recorded as timeout");
    /* CSQ 没回来 ⇒ 本轮不算成功，但 QENG 的字段仍然要新鲜 */
    chk(s.fail_rounds == 1 && s.ok_rounds == 0, "c04 round counted as failed");
    chk(s.rsrp == -85, "c04 fields after the dropped command are still parsed");
    chk(s.sim_present == 0, "c04 SIM state from before the drop is kept");
}

/* 5) QENG 字段不足时**整体不采信**（位置错开会报出别的量） */
static void c05_qeng_truncated(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;
    uint64_t       t = 1000;

    iv_cell_init(&c, 30000);
    iv_cell_tick(&c, t);
    /* 只回一个残缺 QENG：直接喂，不跑整轮 */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);      /* ATI */
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* CGMR */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* CPIN */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* QSIMSTAT */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* QCCID */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* CIMI */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "+CSQ: 20,99\r\n\r\nOK\r\n", strlen("+CSQ: 20,99\r\n\r\nOK\r\n"), t);
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* COPS */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* CEREG */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c, "OK\r\n", 4, t);                /* CGATT */
    (void)iv_cell_next_cmd(&c, (char[64]){0}, 64, t);
    (void)iv_cell_feed(&c,
        "\r\n+QENG: \"servingcell\",\"LIMSRV\",\"LTE\"\r\n\r\nOK\r\n",
        strlen("\r\n+QENG: \"servingcell\",\"LIMSRV\",\"LTE\"\r\n\r\nOK\r\n"), t);

    iv_cell_snapshot(&c, &s);
    chk(s.rsrp == IV_CELL_UNKNOWN, "c05 truncated QENG leaves rsrp unknown");
    chk(s.cell_id == 0u, "c05 truncated QENG does not invent a cell id");
}

/* 6) 第二轮开始要把动态字段复位（上一轮的值不能冒充本轮） */
static void c06_round_reset(void)
{
    iv_cell_t      c;
    iv_cell_snap_t s;
    uint64_t       t = 1000;
    uint64_t       t2;

    iv_cell_init(&c, 30000);
    (void)run_round(&c, k_ok_sim, k_cmd_n, &t);
    iv_cell_snapshot(&c, &s);
    chk(strcmp(s.pdp_ip, "10.158.0.108") == 0, "c06 first round has PDP ip");

    /* 第二轮：全部只回 OK（等于什么都查不到） */
    t2 = t + 30000;
    iv_cell_tick(&c, t2);          /* 起第二轮 */
    {
        char buf[64];
        int  guard = 0;

        while (iv_cell_next_cmd(&c, buf, sizeof buf, t2) > 0) {
            (void)iv_cell_feed(&c, "\r\nOK\r\n", 6, t2 + 10);
            t2 += 50;
            if (++guard > 64)
                break;
        }
    }
    iv_cell_snapshot(&c, &s);

    chk(s.pdp_ip[0] == '\0', "c06 stale PDP ip cleared at round start");
    chk(s.pdp_active == 0, "c06 stale pdp_active cleared");
    chk(s.rsrp == IV_CELL_UNKNOWN, "c06 stale rsrp cleared to sentinel");
    chk(s.usable == 0, "c06 unusable when nothing answers");
    /* 静态字段（型号/固件/SIM 状态）保留：它们不会每轮变，重读不到不该清空 */
    chk(strcmp(s.model, "EC200G") == 0, "c06 model survives an empty round");
    chk(s.sim_present == 1, "c06 sim presence survives an empty round");
}

/* 7) 信号格换算边界 */
static void c07_level(void)
{
    chk(iv_cell_level_from_csq(-1) == -1, "c07 negative csq => unknown");
    chk(iv_cell_level_from_csq(99) == -1, "c07 99 => unknown");
    chk(iv_cell_level_from_csq(0) == 0, "c07 0 => 0 bars");
    chk(iv_cell_level_from_csq(1) == 0, "c07 1 => 0 bars");
    chk(iv_cell_level_from_csq(2) == 1, "c07 2 => 1 bar");
    chk(iv_cell_level_from_csq(12) == 3, "c07 12 => 3 bars");
    chk(iv_cell_level_from_csq(31) == 5, "c07 31 => 5 bars");
}

/* 8) 命令表自洽：文本非空、最后一条是 PDP 查询 */
static void c08_cmd_table(void)
{
    int i;

    chk(strcmp(iv_cell_cmd_text(IV_CELL_CMD_IDLE), "") == 0, "c08 IDLE has no text");
    chk(strcmp(iv_cell_cmd_text(IV_CELL_CMD_CSQ), "AT+CSQ") == 0, "c08 CSQ text");
    chk(strcmp(iv_cell_cmd_text(IV_CELL_CMD_QENG), "AT+QENG=\"servingcell\"") == 0,
        "c08 QENG text");
    chk(strcmp(iv_cell_cmd_text(IV_CELL_CMD_CGCONTRDP), "AT+CGCONTRDP") == 0,
        "c08 last command is PDP query");
    for (i = 1; i < (int)IV_CELL_CMD_LAST; i++)
        chk(iv_cell_cmd_text((iv_cell_cmd_id_t)i)[0] != '\0', "c08 every command has text");
}

/* 9) 只读纪律：命令表里不得出现会改模块状态/断网的命令 */
static void c09_readonly(void)
{
    int i;

    for (i = 1; i < (int)IV_CELL_CMD_LAST; i++) {
        const char *t = iv_cell_cmd_text((iv_cell_cmd_id_t)i);

        chk(strstr(t, "CFUN") == NULL, "c09 no AT+CFUN in the table");
        chk(strstr(t, "QCFG") == NULL, "c09 no AT+QCFG in the table");
        chk(strstr(t, "QNETDEVCTL") == NULL, "c09 no dial command in the table");
        chk(strstr(t, "CPIN=") == NULL, "c09 no PIN write in the table");
    }
}

int main(void)
{
    c01_init_unknown();
    c02_live_nosim();
    c03_usable();
    c04_timeout_skip();
    c05_qeng_truncated();
    c06_round_reset();
    c07_level();
    c08_cmd_table();
    c09_readonly();

    if (g_fail != 0) {
        fprintf(stderr, "test_cellular: %d check(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_cellular passed (live no-sim sample, usable path, timeout skip, "
           "round reset, qeng guard, read-only discipline)\n");
    return 0;
}
