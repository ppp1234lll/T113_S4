/*
 * iv_provision 单测（libivmodules，功能开发计划 M5-S5.5）
 *
 * 覆盖四组：
 *   ① 身份记录编解码的**往返**与**字段字节序**（小端显式，不靠本机字节序）；
 *   ② 校验的**反向证伪**：改 magic / 版本 / 累加和中的任意一个字节都必须被拒 ——
 *      只断言"正例能过"的用例挡不住"校验其实没做"；
 *   ③ 组播报文的解析边界（长度必须精确、四道校验）；
 *   ④ 裸分区读写的**真实文件往返**（用临时文件冒充分区设备；未写入过的块必须
 *      报 IV_ENOENT 而不是给出一堆零值身份）。
 *
 * 「整块 XOR 混淆」只验证"读回来能还原"，**不宣称它是加密**（见 iv_provision.h）。
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
    id->devid = 0x00A1B2C3u;
    id->suite_id = 5u;
    id->mac[0] = 0x3Au; id->mac[1] = 0x82u; id->mac[2] = 0xB4u;
    id->mac[3] = 0x1Du; id->mac[4] = 0x3Cu; id->mac[5] = 0xBFu;
    (void)snprintf(id->server, sizeof(id->server), "10.20.30.40");
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

    /* ---------------- p01 累加和 ---------------- */
    {
        const uint8_t a[] = { 'a', 'b', 'c' };

        chk(iv_prov_checksum(a, sizeof(a)) == (uint32_t)('a' + 'b' + 'c'),
            "p01: checksum is byte sum");
        chk(iv_prov_checksum(NULL, 10) == 0u, "p01b: checksum(NULL) is 0");
    }

    /* ---------------- p02 XOR 可逆 ---------------- */
    {
        uint8_t a[16], b[16];
        int     i;

        for (i = 0; i < 16; i++)
            a[i] = (uint8_t)(i * 7 + 1);
        memcpy(b, a, sizeof(a));
        iv_prov_xor(b, sizeof(b));
        chk(memcmp(a, b, sizeof(a)) != 0, "p02: xor actually changes bytes");
        iv_prov_xor(b, sizeof(b));
        chk(memcmp(a, b, sizeof(a)) == 0, "p02b: xor is its own inverse");
    }

    /* ---------------- p03 记录编解码往返 + 字节序 ---------------- */
    {
        iv_prov_id_t in, out;
        uint8_t      rec[IV_PROV_REC_LEN];

        fill_id(&in);
        chk(iv_prov_id_encode(&in, rec) == IV_OK, "p03: encode ok");
        /* devid 小端：最低字节在偏移 8 */
        chk(rec[8] == 0xC3u && rec[9] == 0xB2u && rec[10] == 0xA1u && rec[11] == 0x00u,
            "p03: devid is little-endian at offset 8");
        chk(iv_prov_id_decode(rec, &out) == IV_OK, "p03b: decode ok");
        chk(out.devid == in.devid, "p03c: devid round-trips");
        chk(out.suite_id == in.suite_id, "p03d: suite round-trips");
        chk(memcmp(out.mac, in.mac, 6u) == 0, "p03e: mac round-trips");
        chk(strcmp(out.server, in.server) == 0, "p03f: server round-trips");
    }

    /* ---------------- p04 反向证伪：三个校验点各改一字节都必须被拒 ---------------- */
    {
        iv_prov_id_t in, out;
        uint8_t      rec[IV_PROV_REC_LEN];

        fill_id(&in);
        chk(iv_prov_id_encode(&in, rec) == IV_OK, "p04: seed");

        {
            uint8_t bad[IV_PROV_REC_LEN];

            memcpy(bad, rec, sizeof(bad));
            bad[0] ^= 0x01u; /* magic 坏 */
            chk(iv_prov_id_decode(bad, &out) == IV_EPROTO, "p04a: bad magic rejected");

            memcpy(bad, rec, sizeof(bad));
            bad[4] = 0x02u; /* 记录版本坏 */
            chk(iv_prov_id_decode(bad, &out) == IV_EPROTO, "p04b: bad rec version rejected");

            memcpy(bad, rec, sizeof(bad));
            bad[12] ^= 0x80u; /* 改 MAC 但不重算 sum */
            chk(iv_prov_id_decode(bad, &out) == IV_EPROTO, "p04c: content tamper rejected");

            memcpy(bad, rec, sizeof(bad));
            bad[52] ^= 0x01u; /* 只改 sum */
            chk(iv_prov_id_decode(bad, &out) == IV_EPROTO, "p04d: bad checksum rejected");
        }
    }

    /* ---------------- p05 报文打包/解析往返 ---------------- */
    {
        iv_prov_id_t in, out;
        uint8_t      pkt[IV_PROV_PKT_LEN];

        fill_id(&in);
        chk(iv_prov_pkt_build(&in, pkt) == IV_OK, "p05: build ok");
        chk(memcmp(pkt, "IVPR", 4u) == 0, "p05b: packet magic");
        chk(pkt[4] == 1u && pkt[5] == 1u, "p05c: version and type");
        chk(iv_prov_pkt_parse(pkt, sizeof(pkt), &out) == IV_OK, "p05d: parse ok");
        chk(out.devid == in.devid && out.suite_id == in.suite_id &&
                memcmp(out.mac, in.mac, 6u) == 0 && strcmp(out.server, in.server) == 0,
            "p05e: packet round-trips every field");
    }

    /* ---------------- p06 报文解析的四道边界 ---------------- */
    {
        iv_prov_id_t in, out;
        uint8_t      pkt[IV_PROV_PKT_LEN];
        uint8_t      bad[IV_PROV_PKT_LEN + 4];

        fill_id(&in);
        chk(iv_prov_pkt_build(&in, pkt) == IV_OK, "p06: seed");

        chk(iv_prov_pkt_parse(pkt, IV_PROV_PKT_LEN - 1u, &out) == IV_EPROTO,
            "p06a: short packet rejected");
        memcpy(bad, pkt, sizeof(pkt));
        memset(bad + IV_PROV_PKT_LEN, 0, 4u);
        chk(iv_prov_pkt_parse(bad, sizeof(bad), &out) == IV_EPROTO,
            "p06b: over-long packet rejected (exact length required)");

        memcpy(bad, pkt, sizeof(pkt));
        bad[0] = 'X';
        chk(iv_prov_pkt_parse(bad, IV_PROV_PKT_LEN, &out) == IV_EPROTO,
            "p06c: bad packet magic rejected");

        memcpy(bad, pkt, sizeof(pkt));
        bad[5] = 9u; /* 未知类型 */
        chk(iv_prov_pkt_parse(bad, IV_PROV_PKT_LEN, &out) == IV_EPROTO,
            "p06d: unknown packet type rejected");

        memcpy(bad, pkt, sizeof(pkt));
        bad[20] ^= 0x11u; /* 改 server 不重算 checksum */
        chk(iv_prov_pkt_parse(bad, IV_PROV_PKT_LEN, &out) == IV_EPROTO,
            "p06e: tampered packet body rejected");
    }

    /* ---------------- p09 server 字段强制 NUL ---------------- */
    {
        iv_prov_id_t in, out;
        uint8_t      rec[IV_PROV_REC_LEN];
        int          i;

        fill_id(&in);
        for (i = 0; i < (int)IV_PROV_SERVER_MAX; i++)
            in.server[i] = (char)('A' + (i % 26)); /* 填满、不带 NUL */
        chk(iv_prov_id_encode(&in, rec) == IV_OK, "p09: encode with full server");
        chk(rec[20 + IV_PROV_SERVER_MAX - 1u] == 0u, "p09b: last server byte forced to NUL");
        chk(iv_prov_id_decode(rec, &out) == IV_OK, "p09c: decode ok");
        chk(strlen(out.server) <= IV_PROV_SERVER_MAX - 1u, "p09d: server stays in bounds");
    }

    /* ---------------- p07 裸分区读写往返（临时文件冒充设备） ---------------- */
    {
        iv_prov_id_t in, out;

        path_of(part, sizeof(part), "part.img");
        fill_id(&in);

        /* 先读一个不存在的设备：必须 IV_ENOENT（不是崩溃、也不是全零身份） */
        (void)unlink(part);
        chk(iv_prov_private_read(part, &out) == IV_ENOENT, "p07a: missing device -> ENOENT");

        /* 造一个"设备节点"再写。真实场景里 /dev/by-name/private 是**本来就存在**的
         * 块设备节点，工装的 open(path, O_WRONLY) 只负责打开它、**不负责创建分区设备**
         * —— 实现里刻意没有 O_CREAT：万一分区不在，root 下 O_CREAT 会在 /dev 里悄悄
         * 建出一个普通文件，身份写得"成功"却落在内存盘上，重启即丢，比直接报错危险得多。
         * 所以这里由用例补上"节点已存在"这个前提。 */
        {
            int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            chk(fd >= 0, "p07a2: stand-in device node exists");
            if (fd >= 0)
                (void)close(fd);
        }

        chk(iv_prov_private_write(part, &in) == IV_OK, "p07b: write ok");
        chk(iv_prov_private_read(part, &out) == IV_OK, "p07c: read back ok");
        chk(out.devid == in.devid && out.suite_id == in.suite_id &&
                memcmp(out.mac, in.mac, 6u) == 0 && strcmp(out.server, in.server) == 0,
            "p07d: identity round-trips through the partition file");

        /* 二次写入必须成功覆盖（计划 S5.5 的验证项） */
        in.devid = 0xDEADBEEFu;
        (void)snprintf(in.server, sizeof(in.server), "172.16.0.9");
        chk(iv_prov_private_write(part, &in) == IV_OK, "p07e: rewrite ok");
        chk(iv_prov_private_read(part, &out) == IV_OK && out.devid == 0xDEADBEEFu &&
                strcmp(out.server, "172.16.0.9") == 0,
            "p07f: rewrite overwrites the old identity");
    }

    /* ---------------- p10 出厂默认值（待用户冻结项的当前取值） ---------------- */
    chk(strcmp(IV_PROV_MCAST_IP_DEFAULT, "239.255.255.249") == 0 &&
            IV_PROV_MCAST_PORT_DEFAULT == 65000,
        "p10: placeholder multicast defaults unchanged (they are pending user input)");

    /* ---------------- p08 工厂标记 ---------------- */
    {
        char m[220];
        FILE *f;

        path_of(m, sizeof(m), "enable_provision");
        chk(iv_prov_marker_present(m) == 0, "p08: marker absent -> 0");
        f = fopen(m, "w");
        if (f != NULL) {
            (void)fputs("1\n", f);
            (void)fclose(f);
        }
        chk(iv_prov_marker_present(m) == 1, "p08b: marker present -> 1");
        (void)unlink(m);
        chk(iv_prov_marker_present(NULL) == 0, "p08c: default path likely absent -> 0");
    }

    /* ---------------- 收尾（规则 6） ---------------- */
    (void)unlink(part);
    (void)rmdir(g_dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_prov FAILED (%d checks)\n", g_fail);
        return 1;
    }
    printf("test_prov passed (record/packet round-trip, tamper rejection, partition rw)\n");
    return 0;
}
