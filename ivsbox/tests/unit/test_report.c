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

/* 0xE1 `data` 段的假回调：只验外壳拼接，字段内容由装配层负责。
 * 契约＝写 `"k":"v",...` **不带尾逗号**，返回字节数。 */
static int fake_pairs(char *out, size_t cap, void *arg)
{
    int n;

    (void)arg;
    n = snprintf(out, cap, "\"tn\":\"\",\"tsn\":\"934072007C00\",\"rt\":\"180,0,0\"");
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return n;
}

static int bad_pairs(char *out, size_t cap, void *arg)
{
    (void)out;
    (void)cap;
    (void)arg;
    return -1;
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
            "V=234.17;A=0.00;APOWER=0.00;AKW=0.00;H=50.01;T=31.33;P=0;DS=1;"
            "RELAY=1,1,1;"
            "CHV=234.17,234.17,234.17;"
            "CHA=0.00,0.00,0.00;"
            "POWER=0.00,0.00,0.00;"
            "ELEC=0.00,0.00,0.00;"
            "CSQ=;MIU=;ERR=0;&&";
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

        /* 字段相对顺序：对齐参考实现 com.c（黄金串已整体锁死，这里给单点定位，
         * 顺序再被改动时报错信息能直接指出是哪一段错位） */
        {
            const char *ea = strstr(seg, ";A=0.00;");
            const char *ap = strstr(seg, "APOWER=");
            const char *eh = strstr(seg, ";H=50.01;");
            const char *pp = strstr(seg, "P=0;");
            const char *dd = strstr(seg, "DS=1;");

            chk(ea != NULL && ap != NULL && eh != NULL && pp != NULL && dd != NULL,
                "c01 order anchors present");
            if (ea != NULL && ap != NULL && eh != NULL)
                chk(ea < ap && ap < eh, "c01 APOWER/AKW sit right after A");
            if (pp != NULL && dd != NULL)
                chk(pp < dd, "c01 P before DS (com.c order)");
        }

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
            /* 固定开销 10（`##`2+长度4+CRC2+`##`2），字面量锁死 */
            chk(flen == 10u + (size_t)n, "c01 frame len");
            chk(frame[flen - 2u] == '#' && frame[flen - 1u] == '#',
                "c01 frame tail ## inside flen");
            chk(frame[6 + n] == (uint8_t)hex[0] &&
                frame[6 + n + 1] == (uint8_t)hex[1],
                "c01 frame crc covers &&data&& only");
        }
    }

    /* ---- 2) 未刷新字段：**字段名照发、值留空**（对齐参考实现的无条件拼接） ---- */
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
        chk(strstr(seg, "A=;") != NULL, "c02 A empty (not valid)");
        chk(strstr(seg, "H=;") != NULL, "c02 H empty (not valid)");
        chk(strstr(seg, "DS=;") != NULL, "c02 DS empty (not valid)");
        chk(strstr(seg, "RELAY=,,;") != NULL, "c02 RELAY keeps 3 empty slots");
        chk(strstr(seg, "APOWER=;") != NULL, "c02 APOWER empty");
        chk(strstr(seg, "CSQ=;") != NULL, "c02 CSQ empty (unknown)");
        chk(strstr(seg, "MIU=;") != NULL, "c02 MIU slot always present");
        chk(strstr(seg, "LAT=") == NULL, "c02 LAT omitted (no fix)");
        chk(strstr(seg, "ERR=0;") != NULL, "c02 ERR=0 when healthy");
        /* 空值 ≠ 0：不能把"没取到"写成 0 */
        chk(strstr(seg, "A=0.00;") == NULL, "c02 empty != 0");
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
        chk(strstr(seg, "CSQ=;") != NULL, "c05 NULL env -> empty CSQ slot");
        chk(strstr(seg, "MIU=;") != NULL && strstr(seg, "ERR=0;") != NULL,
            "c05 MIU/ERR slots always present");

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

    /* ---- 7) E3 应答 JSON 黄金串 ----
     * 字段名对齐参考实现 com_version_information()：devtype/sysver/tsn；
     * `type`/`sv` 是同一张 Excel 示例串里的旧名，作为别名保留（值必须与主名一致，
     * 否则平台读哪份实现都得不到同一个答案）。 */
    {
        const char *want =
            "{\"code\":0,\"qn\":\"20210121143412008\",\"data\":{"
            "\"ver\":\"11\",\"devtype\":\"0400\",\"type\":\"0400\",\"tid\":\"101\","
            "\"cmd\":\"E3\",\"mod\":\"FN_RZTY_PY\",\"sysver\":\"1.0.0.20261020\","
            "\"sv\":\"1.0.0.20261020\",\"tsn\":\"934072007C00\",\"crc\":\"06\"}}";
        char json[512];
        int  n = iv_report_build_query_json(0x0400u, 0x101u,
                                            "20210121143412008",
                                            "FN_RZTY_PY",
                                            "1.0.0.20261020",
                                            "934072007C00",
                                            json, sizeof(json));

        chk(n > 0 && (size_t)n == strlen(want), "c07 json length");
        chk(strcmp(json, want) == 0, "c07 json golden");
        if (strcmp(json, want) != 0)
            fprintf(stderr, "  got : %s\n  want: %s\n", json, want);

        /* 别名与主名同值：这条不变量比"两个键都在"更重要 */
        chk(strstr(json, "\"sysver\":\"1.0.0.20261020\"") != NULL &&
                strstr(json, "\"sv\":\"1.0.0.20261020\"") != NULL,
            "c07 sysver/sv aliases carry the same value");

        /* NULL qn/mod/sv/tsn 的降级 */
        n = iv_report_build_query_json(0x0400u, 0x101u, NULL, NULL, NULL, NULL,
                                       json, sizeof(json));
        chk(n > 0 && strstr(json, "\"qn\":\"0\"") != NULL, "c07 NULL qn -> 0");
        chk(strstr(json, "\"mod\":\"\"") != NULL, "c07 NULL mod -> empty");
        chk(strstr(json, "\"sysver\":\"\"") != NULL, "c07 NULL sv -> empty");
        chk(strstr(json, "\"tsn\":\"\"") != NULL, "c07 NULL tsn -> empty");

        /* 型号串按 IV_REPORT_MOD_MAX 截断：不截断会撑爆应答缓冲、退化成 ERANGE，
         * 症状是"点查询毫无反应"，比截断难查得多。
         * ⚠ 这里用**字面量 31**、不用宏：断言拿宏去比宏，等于宏填错两次也全绿
         * （IV_PROTO_TXT_FIXED 那次踩过，见 修改记录 2026-10-20 10:5x）。 */
        {
            char        sbuf[80];
            const int   lim = 31;

            memset(sbuf, 'X', sizeof(sbuf) - 1u);
            sbuf[sizeof(sbuf) - 1u] = '\0';
            n = iv_report_build_query_json(0x0400u, 0x101u, "1", sbuf, "s", "t",
                                           json, sizeof(json));
            chk(n > 0, "c07 long mod still builds");
            chk(strstr(json, "\"mod\":\"") != NULL, "c07 long mod key present");
            /* 截断后紧跟右引号：数一下 XXX 的个数 */
            {
                const char *p = strstr(json, "\"mod\":\"");
                int         cnt = 0;

                if (p != NULL) {
                    p += 7;
                    while (*p == 'X') {
                        cnt++;
                        p++;
                    }
                    chk(cnt == lim, "c07 long mod truncated to 31 chars");
                    chk(*p == '"', "c07 truncation stays inside the quotes");
                } else {
                    chk(0, "c07 long mod key present");
                }
            }
            /* 整条应答必须能装进装配层那块 384 B 缓冲（留一倍余量） */
            chk((size_t)n < 384u, "c07 worst-case reply fits the 384 B buffer");
        }

        chk(iv_report_build_query_json(0x0400u, 0x101u, "1", "m", "s", "t", json, 8u)
                == IV_ERANGE, "c07 small cap ERANGE");
    }

    /* ---- 8) 平台地址运行期改写（set_server）的边界（零副作用：不 open） ---- */
    {
        static iv_report_t rp;

        iv_report_init(&rp);
        chk(iv_report_set_server(NULL, "127.0.0.1", 1u) == IV_EINVAL, "c08 NULL rp");
        chk(iv_report_set_server(&rp, "127.0.0.1", 1u) == IV_EINVAL,
            "c08 not opened -> EINVAL");
        chk(iv_report_set_server(&rp, NULL, 0u) == IV_EINVAL,
            "c08 clear on closed -> EINVAL");
        chk(iv_report_reconnect(&rp, 0u) == IV_ESTATE, "c08 reconnect on closed -> ESTATE");
    }

    /* ---- 9) 出口网卡必须穿过 open 的"逐字段收口" ----
     * iv_report_open 把调用方配置逐字段收进局部 c 再往下传，漏一行就被默认值顶掉。
     * 第一版 bind_ifname 就漏在这里：agent 传了 usb0、传输层拿到空，
     * 现象是报文照旧从 eth0 出去，光看启动日志两边的值才分得清。 */
    {
        static iv_report_t rp;
        iv_report_cfg_t    cfg;

        iv_report_cfg_default(&cfg);
        cfg.server_host = "127.0.0.1"; /* 只建 socket，不 step，不产生流量 */
        cfg.server_port = 1u;
        cfg.serial_path = "/dev/null";
        cfg.bind_ifname = "lo";
        cfg.mod         = "M";
        cfg.sv          = "S";
        cfg.tsn         = "T";
        cfg.on_query_cfg     = fake_pairs;
        cfg.on_query_cfg_arg = NULL;

        chk(iv_report_open(&rp, &cfg, NULL, 0u) == IV_OK, "c09 open ok");
        chk(strcmp(iv_transport_bind_if(&rp.tr), "lo") == 0,
            "c09 bind_ifname survives open (field-by-field copy)");
        /* E3 三兄弟同样走"逐字段收口"：漏一行就整条查询应答变空串，
         * 而平台侧只表现为"点了没反应"，从日志完全看不出是装配层丢的。 */
        chk(rp.cfg.mod != NULL && strcmp(rp.cfg.mod, "M") == 0 &&
                rp.cfg.sv != NULL && strcmp(rp.cfg.sv, "S") == 0 &&
                rp.cfg.tsn != NULL && strcmp(rp.cfg.tsn, "T") == 0,
            "c09 mod/sv/tsn survive open (field-by-field copy)");
        /* 0xE1 的 data 段回调走同一条"逐字段收口"：漏一行 ⇒ 平台点「查询设备配置」
         * 永远收到 0x74，而设备侧看起来一切正常（route 注册了、日志也不报错）。 */
        chk(rp.cfg.on_query_cfg == fake_pairs && rp.cfg.on_query_cfg_arg == NULL,
            "c09 on_query_cfg survives open (0xE1 data provider)");

        /* 运行期换出口（netmgr 切换 WAN 走这条）：换完传输层要跟着变 */
        chk(iv_report_set_bind_if(&rp, "eth0") == IV_OK, "c09 set_bind_if ok");
        chk(iv_report_reconnect(&rp, 0u) == IV_OK, "c09 reconnect ok");
        chk(strcmp(iv_transport_bind_if(&rp.tr), "eth0") == 0,
            "c09 reconnect applies new bind_ifname");

        iv_report_close(&rp);
    }

    /* ---- 10) 运行期改址必须真落到传输层（2026-10-19 现场缺陷的库层回归） ----
     * iv_report 在 open 时把平台地址拷成**自己的副本**（srv_host/srv_port），
     * 传输层又再拷一次（tr.host/tr.port）。只改配置、不动这两处 ⇒ 报文照旧发往
     * 旧地址，现象是"Web 上改了端口、保存也成功，平台那边一直等不到设备"。
     * 装配层现在的动作＝set_server(新址) → set_bind_if → reconnect，本用例锁这条链：
     *   - set_server 只改本层副本，**不动**在用连接（否则一次设置就断流）；
     *   - 必须再走 reconnect，tr.host/tr.port 才会变成新址。 */
    {
        static iv_report_t rp;
        iv_report_cfg_t    cfg;

        iv_report_cfg_default(&cfg);
        cfg.server_host = "127.0.0.1";
        cfg.server_port = 1u;
        cfg.serial_path = "/dev/null";

        chk(iv_report_open(&rp, &cfg, NULL, 0u) == IV_OK, "c10 open ok");
        chk(strcmp(rp.tr.host, "127.0.0.1") == 0 && rp.tr.port == 1u,
            "c10 initial target comes from open");

        chk(iv_report_set_server(&rp, "127.0.0.2", 2u) == IV_OK, "c10 set_server ok");
        chk(strcmp(rp.tr.host, "127.0.0.2") != 0,
            "c10 set_server alone does NOT touch the live transport");

        chk(iv_report_reconnect(&rp, 0u) == IV_OK, "c10 reconnect ok");
        chk(strcmp(rp.tr.host, "127.0.0.2") == 0 && rp.tr.port == 2u,
            "c10 reconnect targets the NEW address");

        /* 清空：两路地址都没配时不建链（装配层把地址全清掉的语义） */
        chk(iv_report_set_server(&rp, "", 0u) == IV_OK, "c10 clear ok");
        chk(iv_report_reconnect(&rp, 0u) == IV_ESTATE, "c10 cleared -> no reconnect");

        iv_report_close(&rp);
    }

    /* ---- 11) 运行期改"周期上报间隔"（2026-10-20 现场缺陷的库层回归） ----
     * 原先 report_ms 只在 iv_report_open() 从配置取一次，网页上把
     * 「定时上报间隔」从 600 改 300，进程里跑的还是开进程那一刻的值 ——
     * 不重启等于没改。本用例锁三件事：改完立刻换节拍、只动节拍不动连接、
     * 0 回落默认。 */
    {
        static iv_report_t rp;
        iv_report_cfg_t    cfg;

        iv_report_cfg_default(&cfg);
        cfg.server_host = "127.0.0.1";
        cfg.server_port = 1u;
        cfg.serial_path = "/dev/null";
        cfg.report_ms   = 300000u;

        chk(iv_report_open(&rp, &cfg, NULL, 1000u) == IV_OK, "c11 open ok");
        chk(rp.next_report == 301000u, "c11 first report one period out");

        chk(iv_report_set_report_period(&rp, 60000u) == IV_OK, "c11 set 60 s ok");
        chk(rp.cfg.report_ms == 60000u, "c11 period applied");
        chk(rp.next_report == 61000u, "c11 pending deadline pulled in");

        /* 放长：已排得更早的那一拍不动（不能因为改大就推迟，更不能补发） */
        chk(iv_report_set_report_period(&rp, 900000u) == IV_OK, "c11 set 900 s ok");
        chk(rp.next_report == 61000u, "c11 longer period keeps pending deadline");

        chk(iv_report_set_report_period(&rp, 0u) == IV_OK, "c11 set 0 ok");
        chk(rp.cfg.report_ms == IV_REPORT_REPORT_MS, "c11 0 falls back to default");

        chk(iv_report_set_report_period(NULL, 1000u) == IV_EINVAL, "c11 null rejected");
        iv_report_close(&rp);
        chk(iv_report_set_report_period(&rp, 1000u) == IV_EINVAL,
            "c11 closed rejected");
    }

    /* ---- 12) 节拍是"自由运行整倍数"：唤醒延迟不得累加进下一拍 ----
     * 参考实现靠 1 ms 硬件计数器（`report_time++` 到点清零），延迟不进下一拍。
     * 本层原先写 `next = now_ms + period`，每次唤醒晚多少就往后推多少 ——
     * 300 s 上报实测 40 min 慢 1 s，平台上看着像"时间不固定"。
     * 这里故意让 step 迟到 137 ms / 900 ms：下一拍必须仍落在整倍数网格上。 */
    {
        static iv_report_t rp;
        iv_report_cfg_t    cfg;

        iv_report_cfg_default(&cfg);
        cfg.server_host = "127.0.0.1";
        cfg.server_port = 1u;
        cfg.serial_path = "/dev/null";
        cfg.report_ms   = 300000u;

        chk(iv_report_open(&rp, &cfg, NULL, 1000u) == IV_OK, "c12 open ok");
        chk(rp.next_report == 301000u, "c12 grid starts one period out");

        chk(iv_report_step(&rp, 301137u) == IV_OK, "c12 step ok (late by 137 ms)");
        chk(rp.next_report == 601000u, "c12 delay NOT accumulated into next beat");

        chk(iv_report_step(&rp, 601900u) == IV_OK, "c12 second step ok");
        chk(rp.next_report == 901000u, "c12 beat still on the grid");

        iv_report_close(&rp);
    }

    /* ---- 13) 平台 0xF5 载荷解析（纯函数）----
     * 参考实现：`sel = buff[0]; time = (buff[1]<<8 | buff[2]);`
     * —— **大端**，按小端读会把 600 读成 22528。短包 / NULL 一律拒。 */
    {
        static const uint8_t ok600[3] = { 0x01u, 0x02u, 0x58u }; /* sel=1, 600 s */
        static const uint8_t zero[3]  = { 0x01u, 0x00u, 0x00u }; /* 0 s */
        static const uint8_t sel2[3]  = { 0x02u, 0x00u, 0xB4u }; /* sel=2, 180 s */
        uint8_t              sel      = 0xFFu;
        uint16_t             sec      = 0xFFFFu;

        chk(iv_report_parse_cfg_intv(ok600, 3u, &sel, &sec) == IV_OK, "c13 parse ok");
        chk(sel == 1u && sec == 600u, "c13 sel=1 sec=600");

        chk(iv_report_parse_cfg_intv(zero, 3u, &sel, &sec) == IV_OK, "c13 zero ok");
        chk(sel == 1u && sec == 0u, "c13 0 s parsed as 0 (caller must reject)");

        chk(iv_report_parse_cfg_intv(sel2, 3u, &sel, &sec) == IV_OK, "c13 sel2 ok");
        chk(sel == 2u && sec == 180u, "c13 0x00B4 = 180 (big endian)");

        chk(iv_report_parse_cfg_intv(ok600, 2u, &sel, &sec) == IV_EINVAL,
            "c13 short payload rejected");
        chk(iv_report_parse_cfg_intv(NULL, 3u, &sel, &sec) == IV_EINVAL,
            "c13 null data rejected");
        chk(iv_report_parse_cfg_intv(ok600, 3u, NULL, &sec) == IV_EINVAL,
            "c13 null sel rejected");
        chk(iv_report_parse_cfg_intv(ok600, 3u, &sel, NULL) == IV_EINVAL,
            "c13 null sec rejected");
    }

    /* ---- 14) 平台 0xE1 应答 JSON 外壳（查询设备当前参数设置）----
     * 与 E3 的两处**刻意的不同**，这里逐条锁死（改"统一"就会让平台读不到）：
     *   ① `qn` 在 `data` 里面（E3 在顶层）；
     *   ② 没有 `type`/`sv` 别名（参考实现 E1 只发主名）。
     * 为什么值得单测：0xE1 长期未注册，平台点「查询设备配置」收 0x74 显示失败；
     * 补上后若把层级抄错，平台变成"收到了但解析不出 qn"，症状一样是失败。 */
    {
        const char *want =
            "{\"code\":0,\"data\":{"
            "\"ver\":\"11\",\"devtype\":\"0400\",\"qn\":\"20210121143412008\","
            "\"tid\":\"101\",\"cmd\":\"E1\","
            "\"tn\":\"\",\"tsn\":\"934072007C00\",\"rt\":\"180,0,0\","
            "\"crc\":\"08\"}}";
        char json[IV_REPORT_CFG_PAIRS_MAX + 256u];
        int  n;

        n = iv_report_build_cfg_json(0x0400u, 0x101u, "20210121143412008",
                                     fake_pairs, NULL, json, sizeof(json));
        chk(n > 0 && (size_t)n == strlen(want), "c14 json length");
        chk(strcmp(json, want) == 0, "c14 json golden");
        if (strcmp(json, want) != 0)
            fprintf(stderr, "  got : %s\n  want: %s\n", json, want);

        /* qn 必须在 data 里面：`"data":{...,"qn":` 出现，且顶层没有裸 `"qn":` */
        {
            const char *dq = strstr(json, "\"data\":{\"ver\"");

            chk(dq != NULL, "c14 data object first");
            chk(dq != NULL && strstr(dq, "\"qn\":") != NULL,
                "c14 qn lives inside data");
            chk(strstr(json, "\"type\":") == NULL, "c14 no type alias (unlike E3)");
            chk(strstr(json, "\"sv\":") == NULL, "c14 no sv alias (unlike E3)");
        }

        /* crc 必须按 E1 算，不能沿用 E3 的（两者只差末位的 cmd，最容易复制粘贴错） */
        chk(iv_report_query_crc(0x0400u, 0x101u, 0xE1u) !=
                iv_report_query_crc(0x0400u, 0x101u, 0xE3u),
            "c14 E1 crc differs from E3");

        /* 装配层没挂回调 ⇒ 负值（handler 据此回 0x74，而不是发一份半空的配置） */
        chk(iv_report_build_cfg_json(0x0400u, 0x101u, "1", NULL, NULL,
                                     json, sizeof(json)) < 0,
            "c14 NULL pairs -> negative (handler replies 0x74)");
        /* 回调失败 ⇒ 负值 */
        chk(iv_report_build_cfg_json(0x0400u, 0x101u, "1", bad_pairs, NULL,
                                     json, sizeof(json)) < 0,
            "c14 failing pairs -> negative");
        /* 缓冲不够 ⇒ IV_ERANGE（不能截断成一份能解析但少字段的 JSON） */
        chk(iv_report_build_cfg_json(0x0400u, 0x101u, "1", fake_pairs, NULL,
                                     json, 16u) == IV_ERANGE,
            "c14 small cap ERANGE");
        /* NULL qn ⇒ "0"（与 E3 同口径） */
        n = iv_report_build_cfg_json(0x0400u, 0x101u, NULL, fake_pairs, NULL,
                                     json, sizeof(json));
        chk(n > 0 && strstr(json, "\"qn\":\"0\"") != NULL, "c14 NULL qn -> 0");
    }

    if (g_fail == 0)
        printf("test_report passed (seg/frame/query-crc/json/env/set-server/bind-if/"
               "report-period/cfg-intv/cfg-json)\n");
    else
        printf("test_report FAILED (%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
