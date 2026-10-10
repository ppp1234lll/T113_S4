/*
 * iv_report 单测（开发计划 M3-S3.6）
 *
 * 覆盖（纯函数为主，零 socket、零串口）：
 *   1) iv_report_build_seg 黄金段：字段顺序/格式与架构 §18.1 字段表一致；
 *   2) 未刷新字段（valid=0）**不上传**（"未用到的字段无需上传"）；
 *   3) DS 原值直传（不做映射）、RELAY 三路、CSN/MN 定宽；
 *   4) LAT/LNG 仅在 have_loc 时上传、CSQ<0 不上传、ERR 空不上传；
 *   5) NULL 镜像/NULL 环境、dt14 非法、缓冲不足（IV_ERANGE）；
 *   6) iv_report_query_crc：与独立复算一致，黄金值按 cmd 区分
 *      （`110400101E3`→0x06、`110400101E1`→0x08；大写 cmd 有区分力）；
 *   7) iv_report_build_query_json：黄金 JSON（E3 应答，`crc` = 0x06）；
 *   8) 组出的数据段能被 iv_proto_text_frame 接受，且 CRC 只覆盖 &&数据区&&。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_crc.h"
#include "ivsbox/iv_proto.h"
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

/* 填一个"全字段已刷新"的镜像 */
static void fill_mirror(iv_status_t *st)
{
    iv_status_init(st);
    st->data.V = 234.17f;
    st->data.A = 0.00f;
    st->data.H = 50.01f;
    st->data.T = 31.33f;
    st->data.DS = 1;
    st->data.P = 0;
    strcpy(st->data.APOWER, "0.00");
    strcpy(st->data.AKW, "0.00");
    st->data.RELAY[0] = 1;
    st->data.RELAY[1] = 1;
    st->data.RELAY[2] = 1;
    st->data.CHV[0] = 234.17f;
    st->data.CHV[1] = 234.17f;
    st->data.CHV[2] = 234.17f;
    st->data.CHA[0] = 0.0f;
    st->data.CHA[1] = 0.0f;
    st->data.CHA[2] = 0.0f;
    st->data.POWER[0] = 0.0f;
    st->data.POWER[1] = 0.0f;
    st->data.POWER[2] = 0.0f;
    st->data.ELEC[0] = 0.0f;
    st->data.ELEC[1] = 0.0f;
    st->data.ELEC[2] = 0.0f;

    st->valid.V = 1;
    st->valid.A = 1;
    st->valid.H = 1;
    st->valid.T = 1;
    st->valid.DS = 1;
    st->valid.P = 1;
    st->valid.RELAY = 1;
    st->valid.CHV = 1;
    st->valid.CHA = 1;
    st->valid.POWER = 1;
    st->valid.ELEC = 1;
}

int main(void)
{
    char seg[IV_REPORT_SEG_MAX + 1u];

    /* ---- 1) 黄金段（数据区按指令表字段表顺序） ---- */
    {
        static iv_status_t      st;
        iv_report_env_t         env;
        const char *want =
            "QN=0;TID=5947;VER=11;DEVTYPE=0400;CP=&&"
            "DT=20250725171635;CNS=0,0,0,0,0,0;MN=3,0;"
            "V=234.17;A=0.00;H=50.01;T=31.33;DS=1;P=0;"
            "APOWER=0.00;AKW=0.00;"
            "RELAY=1,1,1;"
            "CHV=234.17,234.17,234.17;"
            "CHA=0.00,0.00,0.00;"
            "POWER=0.00,0.00,0.00;"
            "ELEC=0.00,0.00,0.00;&&";
        int n;

        fill_mirror(&st);
        memset(&env, 0, sizeof(env));
        env.csq = -1;
        env.mn[0] = 3;
        env.mn[1] = 0;

        n = iv_report_build_seg(&st, &env, 22855u, "20250725171635",
                                seg, sizeof(seg));
        chk(n > 0, "c01 seg built");
        chk((size_t)n == strlen(want), "c01 seg length");
        chk(strcmp(seg, want) == 0, "c01 seg golden");
        if (strcmp(seg, want) != 0)
            fprintf(stderr, "  got : %s\n  want: %s\n", seg, want);
        /* 数据段必含数据区标记（否则上报帧 CRC 无从算起） */
        chk(strstr(seg, "&&") != NULL, "c01 seg has &&");

        /* 该段能被 ## 上报帧打包，且 CRC 只覆盖 &&数据区&& */
        {
            uint8_t frame[IV_PROTO_TXT_FIXED + IV_REPORT_SEG_MAX];
            size_t  flen  = sizeof(frame);
            size_t  off   = (size_t)(strstr(seg, "&&") - seg);
            uint8_t crc   = iv_crc8((const uint8_t *)(seg + off),
                                    (size_t)n - off, IV_CRC8_SEED_INIT);
            char    hex[3];

            snprintf(hex, sizeof(hex), "%02x", (unsigned)crc);
            chk(iv_proto_text_frame(seg, (size_t)n, frame, &flen) == IV_OK,
                "c01 frame ok");
            chk(flen == 8u + (size_t)n, "c01 frame len");
            chk(frame[6 + n] == (uint8_t)hex[0] &&
                frame[6 + n + 1] == (uint8_t)hex[1],
                "c01 frame crc covers &&data&& only");
        }
    }

    /* ---- 2) 未刷新字段不上传 ---- */
    {
        static iv_status_t st;
        iv_report_env_t    env;

        iv_status_init(&st);
        st.valid.V = 1;
        st.data.V = 12.5f; /* 只有 V 被刷新过 */
        memset(&env, 0, sizeof(env));
        env.csq = -1;

        chk(iv_report_build_seg(&st, &env, 0x101u, "20250101000000",
                                seg, sizeof(seg)) > 0, "c02 built");
        chk(strstr(seg, "V=12.50;") != NULL, "c02 V present");
        chk(strstr(seg, "A=") == NULL, "c02 A omitted (not valid)");
        chk(strstr(seg, "H=") == NULL, "c02 H omitted (not valid)");
        chk(strstr(seg, "DS=") == NULL, "c02 DS omitted (not valid)");
        chk(strstr(seg, "RELAY=") == NULL, "c02 RELAY omitted");
        chk(strstr(seg, "APOWER=") == NULL, "c02 APOWER omitted (empty)");
        chk(strstr(seg, "CSQ=") == NULL, "c02 CSQ omitted (unknown)");
        chk(strstr(seg, "LAT=") == NULL, "c02 LAT omitted (no fix)");
        chk(strstr(seg, "ERR=") == NULL, "c02 ERR omitted (empty)");
    }

    /* ---- 3) DS 原值直传 + RELAY 三路 ---- */
    {
        static iv_status_t st;
        iv_report_env_t    env;

        fill_mirror(&st);
        st.data.DS = 2; /* 采集板：1=关/2=开。平台侧未定义，本层原值直传 */
        memset(&env, 0, sizeof(env));
        env.csq = -1;

        (void)iv_report_build_seg(&st, &env, 1u, "20250101000000",
                                  seg, sizeof(seg));
        chk(strstr(seg, "DS=2;") != NULL, "c03 DS passthrough (2)");
        chk(strstr(seg, "RELAY=1,1,1;") != NULL, "c03 RELAY 3 entries");
    }

    /* ---- 4) 环境字段：定位 / CSQ / 故障码 ---- */
    {
        static iv_status_t st;
        iv_report_env_t    env;

        iv_status_init(&st);
        memset(&env, 0, sizeof(env));
        env.cns[0] = 1; env.cns[2] = 2; env.cns[5] = 4;
        env.mn[0] = 1;  env.mn[1] = 2;
        env.csq = 21;
        env.have_loc = 1;
        env.lat = 36.123456;
        env.lng = 116.654321;
        env.err = "4,10100000";

        chk(iv_report_build_seg(&st, &env, 0x101u, "20250101000000",
                                seg, sizeof(seg)) > 0, "c04 built");
        chk(strstr(seg, "CNS=1,0,2,0,0,4;") != NULL, "c04 CNS 6 slots");
        chk(strstr(seg, "MN=1,2;") != NULL, "c04 MN 2 slots");
        chk(strstr(seg, "CSQ=21;") != NULL, "c04 CSQ present");
        chk(strstr(seg, "LAT=36.123456;LNG=116.654321;") != NULL,
            "c04 LAT/LNG 6 decimals");
        chk(strstr(seg, "ERR=4,10100000;") != NULL, "c04 ERR present");
    }

    /* ---- 5) NULL / 非法入参 / 缓冲不足 ---- */
    {
        static iv_status_t st;
        int                n;

        iv_status_init(&st);

        chk(iv_report_build_seg(NULL, NULL, 0x101u, "20250101000000",
                                seg, sizeof(seg)) > 0, "c05 NULL st/env ok");
        chk(strstr(seg, "QN=0;TID=101;VER=11;DEVTYPE=0400;CP=&&DT=") == seg,
            "c05 NULL st keeps header");
        chk(strstr(seg, "DT=") != NULL && strstr(seg, "CSQ=") == NULL,
            "c05 NULL env -> no CSQ");

        chk(iv_report_build_seg(&st, NULL, 1u, NULL, seg, sizeof(seg)) == IV_EINVAL,
            "c05 NULL dt EINVAL");
        chk(iv_report_build_seg(&st, NULL, 1u, "2025010100", seg, sizeof(seg)) == IV_EINVAL,
            "c05 short dt EINVAL");
        chk(iv_report_build_seg(&st, NULL, 1u, "20250101000000", NULL, 0u) == IV_EINVAL,
            "c05 NULL out EINVAL");

        n = iv_report_build_seg(&st, NULL, 1u, "20250101000000", seg, 10u);
        chk(n == IV_ERANGE, "c05 small cap ERANGE");
    }

    /* ---- 6) 查询响应 crc ---- */
    {
        uint8_t want = iv_crc8((const uint8_t *)"110400101E1", 11u,
                               IV_CRC8_SEED_INIT);

        chk(want == 0x08u, "c06 golden crc 0x08 (110400101E1)");
        chk(iv_report_query_crc(0x0400u, 0x101u, 0xE1u) == want,
            "c06 query_crc matches ver+devtype+tid+cmd");
        /* 与"整串大写/小写"确有区别：证明 cmd 大写是有意义的（不是巧合） */
        chk(want != iv_crc8((const uint8_t *)"110400101e1", 11u,
                            IV_CRC8_SEED_INIT),
            "c06 uppercase cmd matters");
        chk(iv_report_query_crc(0x0400u, 0x101u, 0x02u) != want,
            "c06 different cmd -> different crc");
    }

    /* ---- 7) E3 应答 JSON 黄金串 ---- */
    {
        const char *want =
            "{\"code\":0,\"qn\":\"20210121143412008\",\"data\":{"
            "\"ver\":\"11\",\"type\":\"0400\",\"tid\":\"101\",\"cmd\":\"E3\","
            "\"mod\":\"FN1110-G\",\"sv\":\"FN1110-G-1.0.0.200526\","
            "\"crc\":\"06\"}}";
        char json[512];
        int  n = iv_report_build_query_json(0x0400u, 0x101u,
                                            "20210121143412008",
                                            "FN1110-G",
                                            "FN1110-G-1.0.0.200526",
                                            json, sizeof(json));

        chk(n > 0 && (size_t)n == strlen(want), "c07 json length");
        chk(strcmp(json, want) == 0, "c07 json golden");
        if (strcmp(json, want) != 0)
            fprintf(stderr, "  got : %s\n  want: %s\n", json, want);

        /* NULL qn/mod/sv 的降级 */
        n = iv_report_build_query_json(0x0400u, 0x101u, NULL, NULL, NULL,
                                       json, sizeof(json));
        chk(n > 0 && strstr(json, "\"qn\":\"0\"") != NULL, "c07 NULL qn -> 0");
        chk(strstr(json, "\"mod\":\"\"") != NULL, "c07 NULL mod -> empty");

        chk(iv_report_build_query_json(0x0400u, 0x101u, "1", "m", "s", json, 8u)
                == IV_ERANGE, "c07 small cap ERANGE");
    }

    if (g_fail == 0)
        printf("test_report passed (seg/frame/query-crc/json/env)\n");
    else
        printf("test_report FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
