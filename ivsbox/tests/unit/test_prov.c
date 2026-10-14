/*
 * iv_provision 单测（libivmodules，M6/S5.5 协议定稿：JSON over 组播）
 *
 * 覆盖八组：
 *   p01 tid / mac / date 三种字符串形式的**边界**（长度、大小写、分隔符、越界）；
 *   p02 报文解析的**容错**（文档原始样例带尾逗号、键序、空白）与**拒收**
 *       （缺 info/data、types 空、未闭合字符串、超长）；
 *   p03/p04 决策：search 的广播/定点寻址、update_mac 的 tid 双语义与 mac 非法；
 *   p05 其余 6 条指令**一律不回复**（登记为未实现）；
 *   p06 组包的**往返**（自己发的自己能解析回来）＋转义＋容量不足不产生半个包；
 *   p07 身份记录编解码（小端显式）、篡改四点拒收、裸分区真实文件往返；
 *   p08/p09 工厂标记与**协议常量钉住**（组播地址/端口/指令字面量）。
 *
 * 反向证伪锚点（变异后应抓出，见 2026-10-14 修改记录）：
 *   · 把 `tid_is_bcast` 改成恒 0            ⇒ p03a / p04b 变红；
 *   · 把 mac 解析的"恰 6 组"检查删掉        ⇒ p01c / p04d 变红；
 *   · 把 update_mac 的"tid 即新 ID"改成恒保留本机 ID ⇒ p04a 变红；
 *   · 解析器去掉"未知键跳过"或改成截断超长值 ⇒ p02d / p02c 变红；
 *   · 组包把 result 恒写 "success"          ⇒ p06d 变红；
 *   · 记录里 devid 改用 memcpy 本机序       ⇒ p07c 的小端断言变红。
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ivsbox/iv_log.h"
#include "ivsbox/iv_provision.h"
#include "ivsbox/iv_ret.h"

static int  g_fail;
static char g_dir[160];

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

static void noop_sink(int level, const char *module, const char *line, void *user)
{
    (void)level;
    (void)module;
    (void)line;
    (void)user;
}

static void path_of(char *out, size_t cap, const char *rel)
{
    size_t dl = strlen(g_dir);
    size_t rl = strlen(rel);

    if (dl + 1u + rl + 1u > cap) {
        chk(0, "path_of: buffer too small");
        out[0] = '\0';
        return;
    }
    memcpy(out, g_dir, dl);
    out[dl] = '/';
    memcpy(out + dl + 1u, rel, rl + 1u);
}

static void fill_id(iv_prov_id_t *id)
{
    memset(id, 0, sizeof(*id));
    id->devid    = 0x1234FFu;
    id->suite_id = 5u;
    id->mac[0] = 0x3Au; id->mac[1] = 0x82u; id->mac[2] = 0xB4u;
    id->mac[3] = 0x1Du; id->mac[4] = 0x3Cu; id->mac[5] = 0xBFu;
    (void)snprintf(id->server, sizeof(id->server), "10.20.30.40");
}

/* 判定"该报文是否被决策成某动作"。 */
static int decide_act(const char *json, const char *own_tid, iv_prov_plan_t *plan)
{
    int rc = iv_prov_decide(json, strlen(json), own_tid, plan);

    return (rc == IV_OK) ? (int)plan->act : -rc;
}

/* 解析一条**字符串常量**报文（长度由 strlen 得出，免得手写字节数数错）。 */
static int parse_str(const char *json, iv_prov_msg_t *m)
{
    return iv_prov_msg_parse(json, strlen(json), m);
}

int main(void)
{
    char tmpl[] = "/tmp/ivprov_XXXXXX";
    char part[220];

    iv_log_set_sink(noop_sink, NULL);

    if (mkdtemp(tmpl) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    (void)snprintf(g_dir, sizeof(g_dir), "%s", tmpl);

    /* ---------------- p01 tid / mac / date ---------------- */
    {
        char t[IV_PROV_TID_MAX];
        char m[IV_PROV_MAC_MAX];
        char d[IV_PROV_DATE_MAX];
        uint32_t v = 0u;
        uint8_t  b[6];
        size_t   i;

        iv_prov_tid_from_u32(0x1234FFu, t);
        chk(strcmp(t, "1234FF") == 0, "p01a: tid format 1234FF");
        iv_prov_tid_from_u32(0x0000FFu, t);
        chk(strcmp(t, "0000FF") == 0, "p01b: tid zero padded");
        iv_prov_tid_from_u32(0x12345678u, t);
        chk(strcmp(t, "345678") == 0, "p01c: tid masked to 24 bits");

        chk(iv_prov_tid_to_u32("1234FF", &v) == IV_OK && v == 0x1234FFu,
            "p01d: tid parse upper");
        chk(iv_prov_tid_to_u32("12deff", &v) == IV_OK && v == 0x12DEFFu,
            "p01e: tid parse lower");
        chk(iv_prov_tid_to_u32("1234F", &v) == IV_EINVAL, "p01f: 5 digits rejected");
        chk(iv_prov_tid_to_u32("1234FG", &v) == IV_EINVAL, "p01g: non-hex rejected");
        chk(iv_prov_tid_to_u32("1234567", &v) == IV_EINVAL, "p01h: 7 digits rejected");
        chk(iv_prov_tid_to_u32(NULL, &v) == IV_EINVAL, "p01i: NULL rejected");

        chk(iv_prov_mac_parse("3a:82:b4:1d:3c:bf", b) == IV_OK &&
            b[0] == 0x3Au && b[5] == 0xBFu, "p01j: mac colon form");
        memset(b, 0, sizeof(b));
        chk(iv_prov_mac_parse("02-00-00-44-6c-40", b) == IV_OK &&
            b[0] == 0x02u && b[1] == 0x00u && b[5] == 0x40u,
            "p01k: mac dash form (doc sample)");
        memset(b, 0, sizeof(b));
        chk(iv_prov_mac_parse("AA.BB.CC.DD.EE.FF", b) == IV_OK && b[2] == 0xCCu,
            "p01l: mac dotted upper");
        memset(b, 0, sizeof(b));
        chk(iv_prov_mac_parse("aabbccddeeff", b) == IV_OK && b[3] == 0xDDu,
            "p01m: mac bare hex");
        chk(iv_prov_mac_parse("3a:82:b4:1d:3c", b) == IV_EINVAL, "p01n: 5 bytes rejected");
        chk(iv_prov_mac_parse("3a:82:b4:1d:3c:bf:11", b) == IV_EINVAL, "p01o: 7 bytes rejected");
        chk(iv_prov_mac_parse("gg:82:b4:1d:3c:bf", b) == IV_EINVAL, "p01p: non-hex rejected");
        chk(iv_prov_mac_parse("3a-82-b4-1d-3c-b", b) == IV_EINVAL, "p01q: odd nibble rejected");

        b[0] = 0x3Au; b[1] = 0x82u; b[2] = 0xB4u; b[3] = 0x1Du; b[4] = 0x3Cu; b[5] = 0xBFu;
        iv_prov_mac_format(b, m);
        chk(strcmp(m, "3a:82:b4:1d:3c:bf") == 0, "p01r: mac format");

        iv_prov_date_now(d, 0);
        chk(strlen(d) == 8u, "p01s: date is YYYYMMDD");
        for (i = 0u; i < 8u; i++)
            chk(d[i] >= '0' && d[i] <= '9', "p01t: date digits only");
        iv_prov_date_now(d, 1);
        chk(strlen(d) == 14u, "p01u: date with time is YYYYMMDDHHMMSS");
    }

    /* ---------------- p02 解析：容错与拒收 ---------------- */
    {
        iv_prov_msg_t m;
        /* 文档原样样例：带尾逗号、无空格 */
        const char *q1 = "{\"info\":{\"tid\":\"000000\",\"date\":\"20230531\",}"
                         "\"data\":{\"types\":\"search\",}}";
        /* 键序颠倒 + 空白 + 换行 + 未知键 */
        const char *q2 = "{ \"data\" : {\n  \"other\" : \"x\" ,\n \"types\" : \"search\"\n} ,\n"
                         "  \"info\" : { \"date\" : \"20230531\" , \"tid\" : \"1234FF\" } }";
        const char *q3 = "{\"info\":{\"tid\":\"1234FF\"},\"data\":{\"types\":\"update_mac\","
                         "\"mac\":\"02-00-00-44-6c-40\"}}";

        chk(iv_prov_msg_parse(q1, strlen(q1), &m) == IV_OK &&
            strcmp(m.tid, "000000") == 0 && strcmp(m.types, "search") == 0 &&
            strcmp(m.date, "20230531") == 0 && m.has_mac == 0,
            "p02a: doc sample with trailing commas parses");
        chk(iv_prov_msg_parse(q2, strlen(q2), &m) == IV_OK &&
            strcmp(m.tid, "1234FF") == 0 && strcmp(m.types, "search") == 0,
            "p02b: key order / whitespace / unknown key tolerated");
        chk(iv_prov_msg_parse(q3, strlen(q3), &m) == IV_OK && m.has_mac == 1 &&
            strcmp(m.mac, "02-00-00-44-6c-40") == 0,
            "p02c: mac kept raw");

        chk(parse_str("{\"info\":{\"tid\":\"1\"}}", &m) == IV_EPROTO,
            "p02d: missing data object rejected");
        chk(parse_str("{\"data\":{\"types\":\"search\"}}", &m) == IV_EPROTO,
            "p02e: missing info object rejected");
        chk(parse_str("{\"info\":{},\"data\":{\"types\":\"search\"}}", &m) == IV_EPROTO,
            "p02f: missing info.tid rejected");
        chk(parse_str("{\"info\":{\"tid\":\"1\"},\"data\":{\"types\":\"\"}}", &m) == IV_EPROTO,
            "p02g: empty types rejected");
        chk(parse_str("{\"info\":{\"tid\":\"1\"},\"data\":{\"types\":\"search}", &m) == IV_EPROTO,
            "p02h: unterminated string rejected");
        chk(parse_str("", &m) == IV_EPROTO, "p02i: empty payload rejected");
        /* 注意：这里**不能**走 parse_str —— strlen(NULL) 是未定义行为
         * （host -O2 会被 -Werror=nonnull 拦下，-O0 则直接段错误）。 */
        chk(iv_prov_msg_parse(NULL, 4u, &m) == IV_EINVAL, "p02j: NULL rejected");
        {
            /* 超长报文（> IV_PROV_JSON_MAX）一律拒收 */
            static char big[IV_PROV_JSON_MAX + 64u];

            memset(big, 'x', sizeof(big));
            memcpy(big, "{\"info\":{\"tid\":\"1\"},\"data\":{\"types\":\"search\"}}", 42u);
            chk(iv_prov_msg_parse(big, sizeof(big), &m) == IV_EPROTO,
                "p02k: oversized payload rejected");
        }
    }

    /* ---------------- p03 决策：search ---------------- */
    {
        iv_prov_plan_t p;
        const char *bcast = "{\"info\":{\"tid\":\"000000\",\"date\":\"20230531\"},"
                            "\"data\":{\"types\":\"search\"}}";
        const char *mine = "{\"info\":{\"tid\":\"1234ff\"},"
                           "\"data\":{\"types\":\"search\"}}";
        const char *other = "{\"info\":{\"tid\":\"ABCDEF\"},"
                            "\"data\":{\"types\":\"search\"}}";

        chk(decide_act(bcast, "1234FF", &p) == (int)IV_PROV_ACT_SEARCH,
            "p03a: broadcast search answered");
        chk(decide_act(mine, "1234FF", &p) == (int)IV_PROV_ACT_SEARCH &&
            strcmp(p.new_tid, "1234FF") == 0,
            "p03b: addressed search answered (case-insensitive)");
        chk(decide_act(other, "1234FF", &p) == (int)IV_PROV_ACT_NONE,
            "p03c: search for another device ignored");
        chk(decide_act(bcast, NULL, &p) == (int)IV_PROV_ACT_SEARCH,
            "p03d: broadcast answered even without own tid");
        chk(decide_act(bcast, "", &p) == (int)IV_PROV_ACT_SEARCH,
            "p03e: broadcast answered with empty own tid");
    }

    /* ---------------- p04 决策：update_mac ---------------- */
    {
        iv_prov_plan_t p;
        const char *doc = "{\"info\":{\"tid\":\"12DEFF\",\"date\":\"20230531\"},"
                          "\"data\":{\"types\":\"update_mac\",\"mac\":\"02-00-00-44-6c-40\"}}";
        const char *bcast = "{\"info\":{\"tid\":\"000000\"},"
                            "\"data\":{\"types\":\"update_mac\",\"mac\":\"02:00:00:44:6c:40\"}}";
        const char *self = "{\"info\":{\"tid\":\"1234FF\"},"
                           "\"data\":{\"types\":\"update_mac\",\"mac\":\"02:00:00:44:6c:40\"}}";
        const char *badmac = "{\"info\":{\"tid\":\"1234FF\"},"
                             "\"data\":{\"types\":\"update_mac\",\"mac\":\"gg:00:00:44:6c:40\"}}";
        const char *nomac = "{\"info\":{\"tid\":\"1234FF\"},"
                            "\"data\":{\"types\":\"update_mac\"}}";
        const char *badtid = "{\"info\":{\"tid\":\"12345\"},"
                             "\"data\":{\"types\":\"update_mac\",\"mac\":\"02:00:00:44:6c:40\"}}";

        chk(decide_act(doc, "1234FF", &p) == (int)IV_PROV_ACT_UPDATE_MAC,
            "p04a: doc sample -> update_mac");
        chk(p.mac_ok == 1 && p.new_mac[0] == 0x02u && p.new_mac[5] == 0x40u,
            "p04b: doc sample mac parsed (dash form)");
        chk(strcmp(p.new_tid, "12DEFF") == 0,
            "p04c: tid is taken as the NEW device id (documented inference)");

        chk(decide_act(bcast, "1234FF", &p) == (int)IV_PROV_ACT_UPDATE_MAC &&
            p.mac_ok == 1 && strcmp(p.new_tid, "1234FF") == 0,
            "p04d: broadcast tid keeps own device id, mac only");
        chk(decide_act(self, "1234FF", &p) == (int)IV_PROV_ACT_UPDATE_MAC &&
            strcmp(p.new_tid, "1234FF") == 0,
            "p04e: tid == own id keeps own device id");

        chk(decide_act(badmac, "1234FF", &p) == (int)IV_PROV_ACT_UPDATE_MAC && p.mac_ok == 0,
            "p04f: bad mac -> act with mac_ok=0 (fault, no write)");
        chk(decide_act(nomac, "1234FF", &p) == (int)IV_PROV_ACT_UPDATE_MAC && p.mac_ok == 0,
            "p04g: missing mac -> mac_ok=0");
        chk(decide_act(badtid, "1234FF", &p) == (int)IV_PROV_ACT_NONE,
            "p04h: malformed tid -> ignored (not broadcast, not an id)");
    }

    /* ---------------- p05 其余指令未实现：一律静默 ---------------- */
    {
        static const char *types[] = {
            IV_PROV_TYPE_INQUIRY, IV_PROV_TYPE_UPDATE_RELAY, IV_PROV_TYPE_UPDATE_IP,
            IV_PROV_TYPE_UPDATE_SERVER, IV_PROV_TYPE_UPDATE_NP, IV_PROV_TYPE_UPDATE_RUNADDR
        };
        size_t i;

        for (i = 0u; i < sizeof(types) / sizeof(types[0]); i++) {
            char          buf[160];
            iv_prov_plan_t p;

            (void)snprintf(buf, sizeof(buf),
                           "{\"info\":{\"tid\":\"1234FF\"},\"data\":{\"types\":\"%s\"}}",
                           types[i]);
            chk(decide_act(buf, "1234FF", &p) == (int)IV_PROV_ACT_NONE,
                "p05: unimplemented command stays silent");
        }
        {
            iv_prov_plan_t p;
            const char    *q = "{\"info\":{\"tid\":\"1\"},\"data\":{\"types\":\"search\"}}";

            chk(iv_prov_decide("not json at all", strlen("not json at all"), "1234FF", &p) == IV_EPROTO,
                "p05b: garbage -> EPROTO");
            chk(iv_prov_decide(q, strlen(q), "1234FF", NULL) == IV_EINVAL,
                "p05c: NULL plan rejected");
        }
    }

    /* ---------------- p06 组包 ---------------- */
    {
        char              json[IV_PROV_JSON_MAX];
        char              json2[IV_PROV_JSON_MAX];
        int               n;
        iv_prov_msg_t     m;
        iv_prov_devinfo_t d;

        n = iv_prov_build_search_query(json, sizeof(json));
        chk(n > 0, "p06a: search query built");
        chk(iv_prov_msg_parse(json, (size_t)n, &m) == IV_OK &&
            strcmp(m.types, "search") == 0 && strcmp(m.tid, "000000") == 0,
            "p06b: own search query parses back (round-trip)");
        chk(strstr(json, "\"date\":\"") != NULL && strlen(m.date) == 8u,
            "p06c: search query carries 8-digit date");

        n = iv_prov_build_update_mac_query("12DEFF", "aa:bb:cc:dd:ee:ff",
                                           json, sizeof(json));
        chk(n > 0, "p06d: update_mac query built");
        chk(iv_prov_msg_parse(json, (size_t)n, &m) == IV_OK &&
            strcmp(m.types, "update_mac") == 0 && strcmp(m.tid, "12DEFF") == 0 &&
            m.has_mac == 1 && strcmp(m.mac, "aa:bb:cc:dd:ee:ff") == 0,
            "p06e: update_mac query round-trip");

        memset(&d, 0, sizeof(d));
        (void)snprintf(d.tid, sizeof(d.tid), "1234FF");
        (void)snprintf(d.tnam, sizeof(d.tnam), "FNVIMT_JY");
        (void)snprintf(d.mod, sizeof(d.mod), "FN-ZJST/4G/QG");
        (void)snprintf(d.mac, sizeof(d.mac), "3a:82:b4:1d:3c:bf");
        (void)snprintf(d.ip, sizeof(d.ip), "192.168.2.120");
        (void)snprintf(d.nm, sizeof(d.nm), "255.255.255.0");
        (void)snprintf(d.gw, sizeof(d.gw), "192.168.1.1");
        (void)snprintf(d.iccid, sizeof(d.iccid), "8986048415220000015");

        n = iv_prov_build_search_reply(&d, json, sizeof(json));
        chk(n > 0, "p06f: search reply built");
        chk(iv_prov_msg_parse(json, (size_t)n, &m) == IV_OK &&
            strcmp(m.types, "search") == 0 && strcmp(m.tid, "1234FF") == 0 &&
            m.has_mac == 1 && strcmp(m.mac, "3a:82:b4:1d:3c:bf") == 0,
            "p06g: search reply round-trip");
        chk(strstr(json, "\"tnam\":\"FNVIMT_JY\"") != NULL &&
            strstr(json, "\"mod\":\"FN-ZJST/4G/QG\"") != NULL &&
            strstr(json, "\"ip\":\"192.168.2.120\"") != NULL &&
            strstr(json, "\"iccid\":\"8986048415220000015\"") != NULL,
            "p06h: search reply carries the documented device-info fields");

        n = iv_prov_build_update_mac_reply(&d, 1, json, sizeof(json));
        chk(n > 0 && strstr(json, "\"result\":\"success\"") != NULL,
            "p06i: update_mac reply success");
        n = iv_prov_build_update_mac_reply(&d, 0, json2, sizeof(json2));
        chk(n > 0 && strstr(json2, "\"result\":\"fault\"") != NULL,
            "p06j: update_mac reply fault");
        chk(strcmp(json, json2) != 0, "p06k: success and fault differ");

        /* 转义：带引号与反斜杠的字段不能把 JSON 弄碎 */
        memset(&d, 0, sizeof(d));
        (void)snprintf(d.tid, sizeof(d.tid), "1234FF");
        (void)snprintf(d.tnam, sizeof(d.tnam), "a\"b\\c");
        n = iv_prov_build_search_reply(&d, json, sizeof(json));
        chk(n > 0 && strstr(json, "a\\\"b\\\\c") != NULL,
            "p06l: quotes/backslashes escaped");
        chk(iv_prov_msg_parse(json, (size_t)n, &m) == IV_OK,
            "p06m: escaped reply still parses");

        chk(iv_prov_build_search_reply(&d, json, 8u) < 0, "p06n: tiny buffer -> <0");
        chk(iv_prov_build_search_query(NULL, 16u) < 0, "p06o: NULL out -> <0");
        chk(iv_prov_build_update_mac_query("1", "x", json, 8u) < 0, "p06p: tiny buffer -> <0");
    }

    /* ---------------- p07 记录 / 裸分区 ---------------- */
    {
        iv_prov_id_t in;
        iv_prov_id_t out;
        uint8_t      rec[IV_PROV_REC_LEN];
        uint8_t      bad[IV_PROV_REC_LEN];
        uint8_t      x[4] = { 1u, 2u, 3u, 4u };
        int          i;

        chk(iv_prov_checksum(x, sizeof(x)) == 10u, "p07a: checksum sums bytes");
        chk(iv_prov_checksum(NULL, 10u) == 0u, "p07b: checksum(NULL) is 0");
        iv_prov_xor(x, sizeof(x));
        chk(x[0] != 1u && x[1] != 2u, "p07c: xor changes bytes");
        iv_prov_xor(x, sizeof(x));
        chk(x[0] == 1u && x[1] == 2u && x[2] == 3u && x[3] == 4u,
            "p07d: xor is its own inverse");

        fill_id(&in);
        chk(iv_prov_id_encode(&in, rec) == IV_OK, "p07e: encode ok");
        chk(iv_prov_id_decode(rec, &out) == IV_OK, "p07f: decode ok");
        chk(out.devid == in.devid && out.suite_id == in.suite_id &&
            memcmp(out.mac, in.mac, 6u) == 0 && strcmp(out.server, in.server) == 0,
            "p07g: record round-trip");

        /* 小端显式：devid 0x001234FF 在偏移 8..11 应是 FF 34 12 00 */
        chk(rec[8] == 0xFFu && rec[9] == 0x34u && rec[10] == 0x12u && rec[11] == 0x00u,
            "p07h: devid stored little-endian");

        for (i = 0; i < 4; i++) {
            memcpy(bad, rec, sizeof(bad));
            switch (i) {
            case 0: bad[0] ^= 0xFFu; break;        /* magic */
            case 1: bad[4] = 9u; break;            /* rec version */
            case 2: bad[20] = (uint8_t)(bad[20] ^ 0xFFu); break; /* 内容 */
            default: bad[54] ^= 0x01u; break;      /* 累加和 */
            }
            chk(iv_prov_id_decode(bad, &out) == IV_EPROTO,
                "p07i: tampered record rejected");
        }

        path_of(part, sizeof(part), "private.part");
        chk(iv_prov_private_read(part, &out) == IV_ENOENT, "p07j: missing -> ENOENT");
        {
            FILE *f = fopen(part, "wb");

            if (f != NULL) {
                memset(rec, 0xFF, sizeof(rec));
                (void)fwrite(rec, 1u, sizeof(rec), f);
                (void)fclose(f);
            }
        }
        chk(iv_prov_private_read(part, &out) == IV_ENOENT,
            "p07k: never-written partition -> ENOENT (not a zero identity)");

        chk(iv_prov_private_write(part, &in) == IV_OK, "p07l: partition write ok");
        chk(iv_prov_private_read(part, &out) == IV_OK && out.devid == in.devid &&
            memcmp(out.mac, in.mac, 6u) == 0, "p07m: partition read back");
        {
            iv_prov_id_t other;

            fill_id(&other);
            other.devid = 0x00ABCDEFu;
            other.mac[5] = 0x11u;
            chk(iv_prov_private_write(part, &other) == IV_OK, "p07n: rewrite ok");
            chk(iv_prov_private_read(part, &out) == IV_OK && out.devid == 0x00ABCDEFu &&
                out.mac[5] == 0x11u, "p07o: rewrite visible on read");
        }
        chk(iv_prov_private_write("/nonexistent/dir/private", &in) == IV_ENOENT,
            "p07p: missing device -> ENOENT");

        {
            iv_prov_devinfo_t d;

            iv_prov_devinfo_from_id(&in, &d);
            chk(strcmp(d.tid, "1234FF") == 0 && strcmp(d.mac, "3a:82:b4:1d:3c:bf") == 0,
                "p07q: devinfo filled from identity");
            chk(d.tnam[0] == '\0' && d.iccid[0] == '\0',
                "p07r: other devinfo fields left empty (documented)");
            iv_prov_devinfo_from_id(NULL, &d);
            chk(d.tid[0] == '\0', "p07s: NULL identity -> empty devinfo");
        }
    }

    /* ---------------- p08 工厂标记 ---------------- */
    {
        char  m[220];
        FILE *f;

        path_of(m, sizeof(m), "enable_provision");
        chk(iv_prov_marker_present(m) == 0, "p08a: marker absent -> 0");
        f = fopen(m, "w");
        if (f != NULL) {
            (void)fputs("1\n", f);
            (void)fclose(f);
        }
        chk(iv_prov_marker_present(m) == 1, "p08b: marker present -> 1");
        (void)unlink(m);
        chk(iv_prov_marker_present(NULL) == 0, "p08c: NULL path -> 0 (default absent)");
    }

    /* ---------------- p09 协议常量钉住（文档定稿，改动即需重新核对 xlsx） -------- */
    {
        chk(strcmp(IV_PROV_MCAST_ADDR, "239.255.255.252") == 0,
            "p09a: search/config group is 239.255.255.252");
        chk(IV_PROV_MCAST_PORT == 37200, "p09b: group port is 37200");
        chk(strcmp(IV_PROV_MCAST_ADDR_NET, "239.255.255.249") == 0 &&
            IV_PROV_MCAST_PORT_NET == 65000,
            "p09c: update_ip/server group kept as documented (unimplemented)");
        chk(strcmp(IV_PROV_TYPE_SEARCH, "search") == 0 &&
            strcmp(IV_PROV_TYPE_UPDATE_MAC, "update_mac") == 0,
            "p09d: implemented types are exactly search/update_mac");
        chk(strcmp(IV_PROV_TID_BROADCAST, "000000") == 0,
            "p09e: broadcast tid is 000000");
        chk(strcmp(IV_PROV_RESULT_OK, "success") == 0 &&
            strcmp(IV_PROV_RESULT_FAULT, "fault") == 0,
            "p09f: result strings are success/fault");
        chk(IV_PROV_TTL == 1, "p09g: multicast ttl is 1");
    }

    /* ---------------- 收尾（规则 6） ---------------- */
    (void)unlink(part);
    (void)rmdir(g_dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_prov FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_prov passed (json parse/decide/build, tid+mac, record rw, constants)\n");
    return 0;
}
