/*
 * iv_status 单测（开发计划 M2-S2.6）
 *
 * 覆盖：
 *   1) 0xE1 查询应答 → 全量刷新；valid 位逐位置 1；APOWER/AKW 用首字节判定
 *   2) 新一轮全量应答缺字段 → 旧字段失效；继电器组须三路齐全
 *   3) 字段解析鲁棒：多余空白 / 字段顺序乱 / 数字带小数
 *   4) 坏 JSON（缺引号、缺冒号）→ 全部 valid 不变（不静默归零）
 *   5) 0xC1 箱门（data=0x01）→ DS=2 / valid.DS=1
 *   6) 0xC2 事件上报：DS/P/SPD/T/H/PA/PV 各 TAG 正确覆盖；其它 TAG 走 no-op
 *   7) 0xC2 不带 "mcuEvent" → no-op
 *   8) snapshot JSON：仅写出 valid 字段、嵌套单层、未填字段省略、缓冲不够返 ERANGE
 *   9) format 摘要：键值对空格分隔，APOWER/AKW 走字符串
 *  10) iv_status_init 清零全部状态
 *  11) NULL 入参不炸
 *  12) on_done_query_only 适配：IV_OK + cmd=0xE1 → 全量刷新；其他 cmd → 忽略；IV_!=OK → 忽略
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_frame.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_status.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 单测直接拿适配好的回调（与 iv_link 的 4 参签名一致），单测可同时校验"装配层
 * 怎么把 iv_status_t* 塞进 arg"是否成立。 */
static void on_up(uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    iv_status_handle_upstream(cmd, data, len, arg);
}
static void on_done(int rc, uint8_t cmd, const uint8_t *data, uint16_t len, void *arg)
{
    /* 与装配层实际写法一致：仅 cmd=0xE1 + IV_OK 时全量刷新 */
    iv_status_t *st = (iv_status_t *)arg;
    if (st == NULL) return;
    if (rc == IV_OK && cmd == IV_FRAME_CMD_QUERY && data != NULL && len != 0)
        iv_status_handle_query(st, data, len);
}

int main(void)
{
    static iv_status_t st;
    char buf[512];
    int n;

    iv_status_init(&st);

    /* ---- 1) 0xE1 全量应答：覆盖所有键 ---- */
    {
        const char *json =
            "{\"V\":220.5,\"A\":0.123,\"H\":60.0,\"T\":25.5,"
            "\"DS\":1,\"P\":2,\"SPD\":1,\"PA\":1,\"PV\":1,"
            "\"APOWER\":\"12.34\",\"AKW\":\"567.89\","
            "\"hv\":280,\"lv\":100,\"ov\":400,\"tu\":50,\"tl\":-2,\"hu\":80,\"hl\":20,\"sl\":20,\"ld\":25,"
            "\"RELAY1\":1,\"RELAY2\":2,\"RELAY3\":1,"
            "\"CHV1\":220.5,\"CHV2\":221.0,\"CHV3\":219.8,"
            "\"CHA1\":0.5,\"CHA2\":0.0,\"CHA3\":0.3,"
            "\"POWER1\":110.0,\"POWER2\":0.0,\"POWER3\":66.0,"
            "\"ELEC1\":1.5,\"ELEC2\":2.0,\"ELEC3\":3.5}";
        iv_status_handle_query(&st, (const uint8_t *)json, (uint16_t)strlen(json));
        chk(st.valid.V && st.data.V == 220.5f, "V filled");
        chk(st.valid.A && st.data.A > 0.122f && st.data.A < 0.124f, "A filled");
        chk(st.valid.T && st.data.T == 25.5f, "T filled");
        chk(st.valid.H && st.data.H == 60.0f, "H filled");
        chk(st.valid.DS && st.data.DS == 1, "DS filled");
        chk(st.valid.P && st.data.P == 2, "P filled");
        chk(st.valid.SPD && st.data.SPD == 1, "SPD filled");
        chk(st.valid.PA && st.data.PA == 1, "PA filled");
        chk(st.valid.PV && st.data.PV == 1, "PV filled");
        chk(st.data.APOWER[0] && strcmp(st.data.APOWER, "12.34") == 0, "APOWER string");
        chk(st.data.AKW[0] && strcmp(st.data.AKW, "567.89") == 0, "AKW string");
        chk(st.valid.hv && st.data.hv == 280, "hv");
        chk(st.valid.lv && st.data.lv == 100, "lv");
        chk(st.valid.ov && st.data.ov == 400, "ov");
        chk(st.valid.tu && st.data.tu == 50, "tu");
        chk(st.valid.tl && st.data.tl == -2, "tl negative");
        chk(st.valid.hu && st.data.hu == 80, "hu");
        chk(st.valid.hl && st.data.hl == 20, "hl");
        chk(st.valid.sl && st.data.sl == 20, "sl");
        chk(st.valid.ld && st.data.ld == 25, "ld");
        chk(st.valid.RELAY && st.data.RELAY[0] == 1 && st.data.RELAY[1] == 2
            && st.data.RELAY[2] == 1, "RELAY 3 channels");
        chk(st.valid.CHV && st.data.CHV[0] == 220.5f
            && st.data.CHV[2] == 219.8f, "CHV 3 channels");
        chk(st.valid.CHA && st.data.CHA[1] == 0.0f, "CHA[1]==0");
        chk(st.valid.POWER && st.data.POWER[2] == 66.0f, "POWER[2]");
        chk(st.valid.ELEC && st.data.ELEC[2] == 3.5f, "ELEC[2]");
    }

    /* ---- 2) 全量同步：缺字段清旧值；三路组不完整不可标有效 ---- */
    {
        const char *json = "{\"V\":110.0,\"hv\":200,\"RELAY1\":1}";
        iv_status_handle_query(&st, (const uint8_t *)json, (uint16_t)strlen(json));
        chk(st.valid.V && st.data.V == 110.0f, "partial V");
        chk(st.valid.hv && st.data.hv == 200, "partial hv");
        chk(!st.valid.A && !st.valid.T && !st.valid.DS, "old fields invalidated");
        chk(st.data.A == 0.0f && st.data.DS == 0, "old values cleared");
        chk(!st.valid.RELAY && !st.valid.CHV, "incomplete groups invalid");
        chk(st.data.APOWER[0] == '\0', "old string cleared");
    }

    /* ---- 3) 解析鲁棒：多余空白 / 字段顺序乱 / 数字带小数 ---- */
    {
        iv_status_init(&st);
        const char *json = "  {  \"T\"  :  31.25 ,   \"V\"  :219.0  } ";
        iv_status_handle_query(&st, (const uint8_t *)json, (uint16_t)strlen(json));
        chk(st.valid.T && st.data.T == 31.25f, "T with whitespace");
        chk(st.valid.V && st.data.V == 219.0f, "V with whitespace");
    }

    /* ---- 4) 坏 JSON（缺引号、缺冒号）→ valid 全 0 ---- */
    {
        st.data.V = 123.0f;
        st.valid.V = 1;
        const char *json1 = "{V:220.0}";           /* 缺引号 */
        const char *json2 = "{\"V\" 220.0}";       /* 缺冒号 */
        const char *json3 = "not even json";
        const char *json4 = "{\"V\":9.0,broken}"; /* 前缀合法，后缀损坏 */
        iv_status_handle_query(&st, (const uint8_t *)json1, (uint16_t)strlen(json1));
        chk(st.valid.V && st.data.V == 123.0f, "bad json preserves old V");
        iv_status_handle_query(&st, (const uint8_t *)json2, (uint16_t)strlen(json2));
        chk(st.valid.V && st.data.V == 123.0f, "bad colon preserves old V");
        iv_status_handle_query(&st, (const uint8_t *)json3, (uint16_t)strlen(json3));
        chk(st.valid.V && st.data.V == 123.0f, "not-json preserves old V");
        iv_status_handle_query(&st, (const uint8_t *)json4, (uint16_t)strlen(json4));
        chk(st.valid.V && st.data.V == 123.0f, "malformed suffix preserves old V");
    }

    /* ---- 5) 0xC1 箱门 → DS=2 ---- */
    {
        iv_status_init(&st);
        uint8_t one = 0x01;
        on_up(IV_FRAME_CMD_DOOR, &one, 1, &st);
        chk(st.valid.DS && st.data.DS == 2, "door report -> DS=2");
    }

    /* ---- 6) 0xC2 事件：DS/P/SPD/T/H/PA/PV 各 TAG 覆盖 ---- */
    {
        iv_status_init(&st);
        const char *ev =
            "{\"key\":\"mcuEvent\",\"T\":30.0,\"P\":3}";
        on_up(IV_FRAME_CMD_EVENT, (const uint8_t *)ev, (uint16_t)strlen(ev), &st);
        chk(st.valid.T && st.data.T == 30.0f, "event T=30");
        chk(st.valid.P && st.data.P == 3, "event P=3");
        chk(!st.valid.V, "V still empty after T/P event");

        const char *ev2 = "{\"key\":\"mcuEvent\",\"DS\":2,\"H\":65.5,\"PA\":0}";
        on_up(IV_FRAME_CMD_EVENT, (const uint8_t *)ev2, (uint16_t)strlen(ev2), &st);
        chk(st.valid.DS && st.data.DS == 2, "event DS=2");
        chk(st.valid.H && st.data.H == 65.5f, "event H=65.5");
        chk(st.valid.PA && st.data.PA == 0, "event PA=0 (genuine zero) preserved");
    }

    /* ---- 7) 0xC2 不带 mcuEvent → no-op ---- */
    {
        iv_status_init(&st);
        const char *bad = "{\"foo\":1}";
        on_up(IV_FRAME_CMD_EVENT, (const uint8_t *)bad, (uint16_t)strlen(bad), &st);
        chk(!st.valid.T && !st.valid.P, "event without mcuEvent -> no-op");
    }

    /* ---- 8) snapshot JSON：仅 valid 字段、缓冲不够返 ERANGE ---- */
    {
        iv_status_init(&st);
        const char *json = "{\"V\":220.0,\"hv\":280,\"RELAY1\":1,\"RELAY2\":2,\"RELAY3\":1,\"CHV1\":220.0}";
        iv_status_handle_query(&st, (const uint8_t *)json, (uint16_t)strlen(json));
        n = iv_status_snapshot_json(&st, buf, sizeof(buf));
        chk(n > 0, "snapshot ok");
        chk(strstr(buf, "\"V\":220.0") != NULL, "snapshot contains V");
        chk(strstr(buf, "\"hv\":280") != NULL, "snapshot contains hv");
        chk(strstr(buf, "\"RELAY1\":1") != NULL, "snapshot contains RELAY1");
        chk(strstr(buf, "\"RELAY3\":1") != NULL, "snapshot contains RELAY3");
        chk(strstr(buf, "\"T\":") == NULL, "snapshot omits unfilled T");
        chk(strstr(buf, "\"A\":") == NULL, "snapshot omits unfilled A");
        chk(strstr(buf, "\"H\":") == NULL, "snapshot omits unfilled H");
        chk(buf[0] == '{' && buf[n - 1] == '}', "snapshot is wrapped in {}");

        /* 长度探测：本实现要求 buf 非 NULL（不另开 NULL 探测路径） */
        n = iv_status_snapshot_json(&st, buf, 0);
        chk(n == IV_ERANGE, "zero-size buf -> ERANGE");
    }

    /* ---- 9) format 摘要 ---- */
    {
        iv_status_init(&st);
        const char *json = "{\"V\":220.0,\"T\":25.0,\"APOWER\":\"1.23\"}";
        iv_status_handle_query(&st, (const uint8_t *)json, (uint16_t)strlen(json));
        n = iv_status_format(&st, buf, sizeof(buf));
        chk(n > 0, "format ok");
        chk(strstr(buf, "V=220.0") != NULL, "format V");
        chk(strstr(buf, "T=25.0") != NULL, "format T");
        chk(strstr(buf, "APOWER=1.23") != NULL, "format APOWER string");
    }

    /* ---- 10) init 清零 ---- */
    {
        iv_status_init(&st);
        st.data.V = 1.0f; st.valid.V = 1;
        iv_status_init(&st);
        chk(st.data.V == 0.0f && !st.valid.V, "init clears");
    }

    /* ---- 11) NULL 入参不炸 ---- */
    {
        iv_status_init(NULL); /* 静默 return */
        iv_status_handle_query(NULL, (const uint8_t *)"x", 1);
        iv_status_handle_upstream(IV_FRAME_CMD_DOOR, (const uint8_t *)"x", 1, NULL);
        on_up(0, (const uint8_t *)"x", 1, NULL);
        chk(iv_status_snapshot_json(NULL, buf, sizeof(buf)) == IV_EINVAL, "snapshot NULL -> EINVAL");
        chk(iv_status_format(NULL, buf, sizeof(buf)) == IV_EINVAL, "format NULL -> EINVAL");
    }

    /* ---- 12) on_done_query_only 适配：仅 0xE1 + IV_OK 触发 ---- */
    {
        iv_status_init(&st);
        const char *json = "{\"V\":222.0}";
        on_done(IV_OK, IV_FRAME_CMD_QUERY, (const uint8_t *)json, (uint16_t)strlen(json), &st);
        chk(st.valid.V && st.data.V == 222.0f, "on_done query -> V filled");

        iv_status_init(&st);
        on_done(IV_OK, IV_FRAME_CMD_POWER, (const uint8_t *)json, (uint16_t)strlen(json), &st);
        chk(!st.valid.V, "on_done non-query cmd ignored");

        iv_status_init(&st);
        on_done(IV_ETIMEDOUT, IV_FRAME_CMD_QUERY, (const uint8_t *)json, (uint16_t)strlen(json), &st);
        chk(!st.valid.V, "on_done failed rc ignored");
    }

    if (g_fail == 0)
        printf("test_status passed (query/partial/robust/badjson/door/event/nomcuEvent/"
               "snapshot/format/init/null/done_adapter)\n");
    return g_fail == 0 ? 0 : 1;
}
