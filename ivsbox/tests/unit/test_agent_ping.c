/*
 * 平台 0xE8「查询 PING 信息」的应答拼装单测  2026-10-29
 *
 * ============================ 它验的是什么 ============================
 * `iv_agent_build_ping_json()` 是**纯函数**：只读 agent 的身份（devtype/devid）
 * 与一份 `iv_probe_stat_t`，不碰 socket、不改状态。所以这里能直接把"已结算的
 * 一轮"喂进去，逐字节钉住平台要吃的那 8 个字段。
 *
 * 协议依据（`指令-通用版 20260623.xlsx` → 查询指令 → 查询PING信息）：
 *   请求 len=4（目标 IPv4 四字节）；应答
 *   `##{"code":0,"data":{…,"cmd":"E8","st1":"1","dt1":"20",…,"st4":"0","dt4":"0","crc":"…"}}##`
 *   `st[i]`＝1 成功 / 0 失败；`dt[i]`＝该包 RTT(ms)，**失败为 0**。
 *
 * ============================ 为什么这些断言值得写 ============================
 * ① "没数据" 必须输出成 **8 个 0**，不能省略字段、也不能留空串：平台侧按定长
 *    键取值，缺键会被当成"设备不支持"而不是"这轮没通"；
 * ② `dt` 在失败包上必须是 **0**，不能沿用上一包的 RTT —— 那会造出"通了但 0 ms"
 *    这种不存在的状态，现场会照着它去查根本不存在的网络问题；
 * ③ 缓冲不够必须返回 IV_ERANGE（**整条不发**），不许截断：半截 JSON 平台解析必失败，
 *    而且失败点会被误判成"设备坏了"；
 * ④ `crc` 必须与 E1/E3/E4 **同一套算法同一个函数**（`iv_report_query_crc()`）——
 *    抄一份写死的值迟早与 E3 漂移，平台判签名错，表现是"所有查询都失败"。
 *
 * ============================ 反向证伪 ============================
 *   F1 把 `st == NULL || round_seq == 0` 那一支改成"返回 IV_ESTATE"⇒ p02 红；
 *   F2 把失败包的 `dtv` 改成沿用 `st->round_dt[i]` ⇒ p03b 红；
 *   F3 去掉各 `snprintf` 的 ERANGE 判断（直接 n += k）⇒ p04 红；
 *   F4 把 crc 改成常量 "00" ⇒ p05 红。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_probe.h"
#include "ivsbox/iv_report.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 造一个"只有身份"的 agent 壳：本用例只用到 `rp.pf.id` 与 `ping_qn`，
 * 刻意不调 `iv_agent_open()`（那要串口 / netlink / raw socket）。 */
static void make_agent(iv_agent_t *a, uint16_t devtype, uint32_t devid)
{
    memset(a, 0, sizeof(*a));
    iv_agent_init(a);
    a->opened          = 1;
    a->rp.pf.id.devtype = devtype;
    a->rp.pf.id.devid   = devid;
}

static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* ---------------- p01 入参守卫 ---------------- */
static void p01_guards(void)
{
    iv_agent_t a;
    char       buf[256];

    make_agent(&a, 0x0400u, 0x101u);
    chk(iv_agent_build_ping_json(NULL, "1", NULL, buf, sizeof buf) == IV_EINVAL,
        "p01 NULL agent -> EINVAL");
    chk(iv_agent_build_ping_json(&a, "1", NULL, NULL, sizeof buf) == IV_EINVAL,
        "p01 NULL out -> EINVAL");
    chk(iv_agent_build_ping_json(&a, "1", NULL, buf, 8u) == IV_EINVAL,
        "p01 tiny cap -> EINVAL");
}

/* ---------------- p02 没有结算过任何一轮：8 个 0，但字段必须齐 ---------------- */
static void p02_no_round(void)
{
    iv_agent_t a;
    char       buf[512];
    char       expect[120];
    int        n;

    make_agent(&a, 0x0400u, 0x101u);
    n = iv_agent_build_ping_json(&a, "20210121143412008", NULL, buf, sizeof buf);
    chk(n > 0, "p02 build succeeded without round data");
    chk(has(buf, "\"cmd\":\"E8\""), "p02 cmd field present");
    chk(has(buf, "\"code\":0"), "p02 code=0");
    chk(has(buf, "\"qn\":\"20210121143412008\""), "p02 qn echoed verbatim");
    /* 逐字段钉死：缺任何一个平台都会当成"设备不支持这一项" */
    for (int i = 1; i <= 4; i++) {
        char key[24];

        (void)snprintf(key, sizeof key, "\"st%d\":\"0\"", i);
        chk(has(buf, key), "p02 all-fail st is 0");
        (void)snprintf(key, sizeof key, "\"dt%d\":\"0\"", i);
        chk(has(buf, key), "p02 all-fail dt is 0");
    }
    (void)snprintf(expect, sizeof expect, "\"ver\":\"%02x\"", (unsigned)IV_PROTO_VER);
    chk(has(buf, expect), "p02 ver matches the protocol version");
    /* devtype/tid 与 E3/E4 同口径：`%04x` / `%x` */
    chk(has(buf, "\"devtype\":\"0400\""), "p02 devtype is 4-hex");
    chk(has(buf, "\"tid\":\"101\""), "p02 tid is lower-case hex without padding");
    /* 一份完整 JSON 必须以 `}}` 收尾（外壳 data + 根），不能靠平台容忍 */
    chk(n >= 2 && buf[n - 2] == '}' && buf[n - 1] == '}', "p02 json closed twice");
}

/* ---------------- p03 有轮数据：逐包原样搬出来 ---------------- */
static void p03_round_data(void)
{
    iv_agent_t      a;
    iv_probe_stat_t st;
    char            buf[512];

    make_agent(&a, 0x0400u, 0x113u);
    memset(&st, 0, sizeof st);
    st.round_seq        = 3u;
    st.round_st[0]      = 1u;
    st.round_dt[0]      = 20u;
    st.round_st[1]      = 1u;
    st.round_dt[1]      = 30u;
    st.round_st[2]      = 1u;
    st.round_dt[2]      = 50u;
    st.round_st[3]      = 0u;   /* 第 4 个包失败 */
    st.round_dt[3]      = 0u;

    chk(iv_agent_build_ping_json(&a, "1", &st, buf, sizeof buf) > 0,
        "p03 build with round data");
    /* 指令表里的样例就是这个形状：1,20 / 1,30 / 1,50 / 0,0 */
    chk(has(buf, "\"st1\":\"1\",\"dt1\":\"20\""), "p03 packet 1 ok/20ms");
    chk(has(buf, "\"st2\":\"1\",\"dt2\":\"30\""), "p03 packet 2 ok/30ms");
    chk(has(buf, "\"st3\":\"1\",\"dt3\":\"50\""), "p03 packet 3 ok/50ms");
    chk(has(buf, "\"st4\":\"0\",\"dt4\":\"0\""), "p03 packet 4 lost -> 0/0");

    /* 失败包即使内存里被写进了 RTT，输出也必须是 0（不许把残留值发出去）：
     * 平台看到"通了但 0 ms"会去查一个根本不存在的网络问题。 */
    st.round_dt[3] = 999u;
    chk(iv_agent_build_ping_json(&a, "1", &st, buf, sizeof buf) > 0,
        "p03 rebuild with a dirty dt on the lost packet");
    chk(has(buf, "\"st4\":\"0\",\"dt4\":\"0\""),
        "p03 lost packet forces dt=0 even when the struct carries a stale RTT");
    chk(!has(buf, "999"), "p03 stale RTT never reaches the platform");
}

/* ---------------- p04 缓冲不够：整条不发 ---------------- */
static void p04_range(void)
{
    iv_agent_t      a;
    iv_probe_stat_t st;
    char            buf[512];
    int             need;

    make_agent(&a, 0x0400u, 0x113u);
    memset(&st, 0, sizeof st);
    st.round_seq = 1u;
    need = iv_agent_build_ping_json(&a, "1", &st, buf, sizeof buf);
    chk(need > 0, "p04 reference build succeeded");
    /* 返回长度**不含结尾 NUL**（与 snprintf 同语义）⇒ `need + 1` 刚好装下，
     * `need` 本身就要报错。这一字节的差别正是"截断半个 JSON"最容易溜过去的缝。 */
    chk(iv_agent_build_ping_json(&a, "1", &st, buf, (size_t)need + 1u) == need,
        "p04 exact-size buffer (len + NUL) still fits");
    chk(iv_agent_build_ping_json(&a, "1", &st, buf, (size_t)need) == IV_ERANGE,
        "p04 one byte short -> ERANGE");
}

/* ---------------- p05 crc 与 E1/E3/E4 同源 ---------------- */
static void p05_crc(void)
{
    iv_agent_t a;
    char       buf[512];
    char       want[8];
    char       needle[24];

    make_agent(&a, 0x0400u, 0x101u);
    chk(iv_agent_build_ping_json(&a, "1", NULL, buf, sizeof buf) > 0,
        "p05 build for crc check");
    (void)snprintf(want, sizeof want, "%02x",
                   (unsigned)iv_report_query_crc(0x0400u, 0x101u,
                                                 IV_PROTO_CMD_QUERY_PING));
    (void)snprintf(needle, sizeof needle, "\"crc\":\"%s\"", want);
    chk(has(buf, needle), "p05 crc uses iv_report_query_crc (same as E1/E3/E4)");
    /* 反过来钉一次：E8 的 crc 不能与 E4 相同（cmd 是签名的一部分） */
    {
        char other[8];
        char other_needle[24];

        (void)snprintf(other, sizeof other, "%02x",
                       (unsigned)iv_report_query_crc(0x0400u, 0x101u,
                                                     IV_PROTO_CMD_QUERY_IPC_IP));
        (void)snprintf(other_needle, sizeof other_needle, "\"crc\":\"%s\"", other);
        if (strcmp(want, other) != 0)
            chk(!has(buf, other_needle), "p05 cmd participates in the signature");
    }
}

int main(void)
{
    p01_guards();
    p02_no_round();
    p03_round_data();
    p04_range();
    p05_crc();

    if (g_fail != 0) {
        fprintf(stderr, "test_agent_ping FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_agent_ping passed (guards, all-fail shape, per-packet copy, "
           "range, crc)\n");
    return 0;
}
