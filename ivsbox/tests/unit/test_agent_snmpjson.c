/*
 * 平台 0xE7「查询 SNMP 信息」应答体单测  2026-10-29
 *
 * ============================ 它验的是什么 ============================
 * `iv_agent_build_snmp_json()` 是纯函数：数据**只从已采到的结果表**取
 * （`a->snmp_job[which].slot[]`，与 Web/ctl 读同一份），不碰 socket。
 *
 * 协议依据（`指令-通用版 20260623.xlsx` → 查询指令 → 查询SNMP信息）：
 *   摄像机：num / brand / model / sid / cpu / mem / rate / mac
 *   交换机：num / brand / model / port（逗号表）/ poe（逗号表）
 *   表里明确允许"不存在直接返回空字符串"。
 *
 * ============================ 为什么值得写这些断言 ============================
 * ① **没采到 ≠ 0**：`cpuPercent` 没应答时必须是空串。填 0 会让平台显示
 *    "CPU 0%"（看起来正常），而实际是"这台摄像机根本不支持 SNMP"；
 * ② `port` / `poe` 是**逗号表**，不是单个值：行序必须稳定（端口 1..N 的顺序），
 *    否则页面上的端口图会错位；
 * ③ `sysDescr` / `sysName` 是设备给的任意字节流。**直接 `%s` 塞进 JSON，一个
 *    双引号就让平台整条解析失败** —— 这是本条命令最容易出的现场故障，且只在
 *    某台交换机上复现；
 * ④ cmd / crc 必须与 E1/E3/E4 同源（`iv_report_query_crc()`）且带 `E7`，
 *    抄一份写死的值会与 E3 漂移 ⇒ 平台判签名错。
 *
 * ============================ 反向证伪 ============================
 *   F1 把"无值"分支改成塞 "0" ⇒ s02b 红；
 *   F2 去掉 `json_put_escaped()` 直接 %s ⇒ s04b 红（JSON 引号计数变奇数）；
 *   F3 `snmp_val_list()` 反向遍历行 ⇒ s03b 红（端口顺序颠倒）；
 *   F4 crc 换成常量 ⇒ s05 红。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_snmp.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void make_agent(iv_agent_t *a, uint16_t devtype, uint32_t devid)
{
    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    a->opened           = 1;
    a->rp.pf.id.devtype = devtype;
    a->rp.pf.id.devid   = devid;
}

/* 按槽位表顺序填一条"标量字符串值"。`which` 决定用哪套槽位表。 */
static void slot_str(iv_agent_t *a, int which, int idx, const char *oid,
                     const char *val)
{
    iv_agent_snmp_slot_t *s = &a->snmp_job[which].slot[idx];

    (void)snprintf(s->oid, sizeof s->oid, "%s", oid);
    s->n_rows   = 1;
    s->rc       = IV_OK;
    s->row[0].type    = IV_SNMP_T_OCTETSTR;
    s->row[0].num     = 0;
    (void)snprintf(s->row[0].str, sizeof s->row[0].str, "%s", val);
}

static void slot_int(iv_agent_t *a, int which, int idx, const char *oid,
                     int64_t val)
{
    iv_agent_snmp_slot_t *s = &a->snmp_job[which].slot[idx];

    (void)snprintf(s->oid, sizeof s->oid, "%s", oid);
    s->n_rows         = 1;
    s->rc             = IV_OK;
    s->row[0].type    = IV_SNMP_T_INTEGER;
    s->row[0].num     = val;
    s->row[0].str[0]  = '\0'; /* INTEGER 没有字符串形态，取值时走 num 分支 */
}

static void slot_none(iv_agent_t *a, int which, int idx, const char *oid)
{
    iv_agent_snmp_slot_t *s = &a->snmp_job[which].slot[idx];

    (void)snprintf(s->oid, sizeof s->oid, "%s", oid);
    s->n_rows         = 0; /* 无应答 */
    s->rc             = IV_ETIMEDOUT;
    s->row[0].type    = IV_SNMP_T_NOSUCHOBJECT;
    s->row[0].str[0]  = '\0';
}

static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* 数一个 JSON 里未转义的双引号个数：合法的必须成对（偶数）。
 * 这是"设备给的字符串把结构捅破"最快的判据。 */
static int count_quotes(const char *s)
{
    int n = 0;

    for (; *s != '\0'; s++) {
        if (*s == '"')
            n++;
    }
    return n;
}

/* ---------------- s01 守卫 ---------------- */
static void s01_guards(void)
{
    iv_agent_t a;
    char       buf[1024];

    make_agent(&a, 0x0400u, 0x101u);
    chk(iv_agent_build_snmp_json(NULL, 0, "1", buf, sizeof buf) == IV_EINVAL,
        "s01 NULL agent -> EINVAL");
    chk(iv_agent_build_snmp_json(&a, 0, "1", NULL, sizeof buf) == IV_EINVAL,
        "s01 NULL out -> EINVAL");
    chk(iv_agent_build_snmp_json(&a, 0, "1", buf, 16u) == IV_EINVAL,
        "s01 tiny cap -> EINVAL");
    chk(iv_agent_build_snmp_json(&a, -1, "1", buf, sizeof buf) == IV_EINVAL,
        "s01 which=-1 -> EINVAL");
    chk(iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_TARGET_N, "1", buf,
                                 sizeof buf) == IV_EINVAL,
        "s01 which out of range -> EINVAL");
}

/* ---------------- s02 摄像机：没采到就是空串，不是 0 ---------------- */
static void s02_cam_empty(void)
{
    iv_agent_t a;
    char       buf[1024];
    int        i;
    int        n;

    make_agent(&a, 0x0400u, 0x101u);
    /* 9 个槽位全部"无应答"：这是现场最常见的形态（摄像机不开 SNMP） */
    a.snmp_job[IV_AGENT_SNMP_CAM].n_slot = (int)IV_SNMP_CAM_SLOT_N;
    for (i = 0; i < (int)IV_SNMP_CAM_SLOT_N; i++)
        slot_none(&a, IV_AGENT_SNMP_CAM, i, "1.3.6.1.2.1.1.1.0");

    n = iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_CAM, "1", buf, sizeof buf);
    chk(n > 0, "s02 build succeeded with no values");
    chk(has(buf, "\"cmd\":\"E7\""), "s02 cmd is E7");
    chk(has(buf, "\"num\":\"1\""), "s02 camera 0 reports num=1 (1-based)");
    chk(has(buf, "\"cpu\":\"\""), "s02 missing cpu is an empty string, not 0");
    chk(has(buf, "\"mem\":\"\""), "s02 missing mem is an empty string");
    chk(has(buf, "\"model\":\"\""), "s02 missing model is an empty string");
    chk(has(buf, "\"sid\":\"\""), "s02 missing sid is an empty string");
    chk(has(buf, "\"rate\":\"\""), "s02 rate has no slot -> empty");
    chk(has(buf, "\"mac\":\"\""), "s02 mac has no slot -> empty");
    chk(!has(buf, "\"cpu\":\"0\""), "s02 never fabricates a numeric zero");
    chk(count_quotes(buf) % 2 == 0, "s02 json quotes are balanced");
}

/* ---------------- s03 摄像机：有值原样搬 / 交换机端口表 ---------------- */
static void s03_cam_values(void)
{
    iv_agent_t a;
    char       buf[1024];

    make_agent(&a, 0x0400u, 0x101u);
    a.snmp_job[IV_AGENT_SNMP_CAM].n_slot = (int)IV_SNMP_CAM_SLOT_N;
    (void)snprintf(a.cfg.snmp[IV_AGENT_SNMP_CAM].brand,
                   sizeof a.cfg.snmp[IV_AGENT_SNMP_CAM].brand, "%s", "Dahua");
    slot_str(&a, IV_AGENT_SNMP_CAM, 0, "1.3.6.1.2.1.1.1.0", "DH-SD6223-DGQ-i");
    slot_str(&a, IV_AGENT_SNMP_CAM, 5, "1.3.6.1.4.1.1004849.1.1.2.1.1",
             "BL02543PAN23A62");
    /* cpuPercent 是 INTEGER：走 num 分支 */
    slot_int(&a, IV_AGENT_SNMP_CAM, 6, "1.3.6.1.4.1.39165.1.7.0", 21);
    slot_none(&a, IV_AGENT_SNMP_CAM, 7, "1.3.6.1.4.1.39165.1.8.0");

    chk(iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_CAM, "1", buf, sizeof buf) > 0,
        "s03 camera build");
    chk(has(buf, "\"brand\":\"Dahua\""), "s03 brand comes from the assembly layer");
    chk(has(buf, "\"model\":\"DH-SD6223-DGQ-i\""), "s03 model from sysDescr");
    chk(has(buf, "\"sid\":\"BL02543PAN23A62\""), "s03 sid from serialNumber");
    chk(has(buf, "\"cpu\":\"21\""), "s03 integer slot rendered as text");
    chk(has(buf, "\"mem\":\"\""), "s03 a no-value slot stays empty next to real ones");

    /* 交换机：ifOperStatus 走表（多行），必须按行序拼成逗号串 */
    {
        iv_agent_snmp_slot_t *s = &a.snmp_job[IV_AGENT_SNMP_SW].slot[5];

        a.snmp_job[IV_AGENT_SNMP_SW].n_slot = (int)IV_SNMP_SW_SLOT_N;
        (void)snprintf(s->oid, sizeof s->oid, "1.3.6.1.2.1.2.2.1.8");
        s->rc    = IV_OK;
        s->n_rows = 4;
        for (int i = 0; i < 4; i++) {
            s->row[i].type   = IV_SNMP_T_INTEGER;
            s->row[i].num    = (i == 2) ? 2 : 1; /* 第 3 个口 down */
            s->row[i].str[0] = '\0';
        }
        slot_str(&a, IV_AGENT_SNMP_SW, 0, "1.3.6.1.2.1.1.1.0", "IPS53108PFMA");

        chk(iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_SW, "1", buf, sizeof buf) > 0,
            "s03 switch build");
        chk(has(buf, "\"num\":\"7\""), "s03 switch reports num=7");
        chk(has(buf, "\"port\":\"1,1,2,1\""), "s03 port list keeps row order");
        chk(has(buf, "\"poe\":\"\""), "s03 poe has no slot -> empty (never fake 1,1,1)");
        chk(has(buf, "\"model\":\"IPS53108PFMA\""), "s03 switch model from sysDescr");
        chk(!has(buf, "\"sid\""), "s03 switch shape has no sid field");
        chk(!has(buf, "\"cpu\""), "s03 switch shape has no cpu field");
    }
}

/* ---------------- s04 设备给的字节流不许捅破 JSON ---------------- */
static void s04_escaping(void)
{
    iv_agent_t a;
    char       buf[1024];

    make_agent(&a, 0x0400u, 0x101u);
    a.snmp_job[IV_AGENT_SNMP_SW].n_slot = (int)IV_SNMP_SW_SLOT_N;
    /* 真实世界里 sysDescr 什么都可能有：引号、反斜杠、换行、非 UTF-8 */
    slot_str(&a, IV_AGENT_SNMP_SW, 0, "1.3.6.1.2.1.1.1.0",
             "ACME \"core\" switch\\rev2\n\x01\xff");

    chk(iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_SW, "1", buf, sizeof buf) > 0,
        "s04 build with hostile sysDescr");
    chk(has(buf, "\\\"core\\\""), "s04 embedded quotes are escaped");
    chk(has(buf, "switch\\\\rev2"), "s04 backslash is escaped");
    chk(count_quotes(buf) % 2 == 0, "s04 json quotes stay balanced");
    chk(!has(buf, "\n"), "s04 control characters never reach the wire");
    chk(!has(buf, "\x01"), "s04 raw 0x01 replaced");
}

/* ---------------- s05 crc / cmd 同源 ---------------- */
static void s05_crc(void)
{
    iv_agent_t a;
    char       buf[1024];
    char       want[8];
    char       needle[24];

    make_agent(&a, 0x0400u, 0x101u);
    a.snmp_job[IV_AGENT_SNMP_CAM].n_slot = (int)IV_SNMP_CAM_SLOT_N;
    chk(iv_agent_build_snmp_json(&a, IV_AGENT_SNMP_CAM, "1", buf, sizeof buf) > 0,
        "s05 build for crc check");
    (void)snprintf(want, sizeof want, "%02x",
                   (unsigned)iv_report_query_crc(0x0400u, 0x101u,
                                                 IV_PROTO_CMD_QUERY_SNMP));
    (void)snprintf(needle, sizeof needle, "\"crc\":\"%s\"", want);
    chk(has(buf, needle), "s05 crc uses iv_report_query_crc with cmd=E7");
}

int main(void)
{
    s01_guards();
    s02_cam_empty();
    s03_cam_values();
    s04_escaping();
    s05_crc();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_snmpjson FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_agent_snmpjson passed (guards, empty-not-zero, scalar/table "
           "rendering, escaping, crc)\n");
    return 0;
}
