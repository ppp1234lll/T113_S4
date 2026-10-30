/*
 * iv_tunnel 单测（libivmodules，功能开发计划 M8-S8.1~S8.3）
 *
 * ============================ 靶子是什么 ============================
 * 远程通道是"从公网进内网的口子"，它唯一的价值是**该拒的拒得干净**：
 *   - 服务名只认白名单（`web` / `camera:N` / `media:N`），**摄像机 IP 不许进配置**；
 *   - 钥匙派生必须与运维侧（BIRD 运维箱 `GenerateSTCPKey`）**字节级一致**，
 *     否则访问方拿着自己算的钥匙连不上，故障现象会非常像"网络问题"；
 *   - 通道默认关，开了必须能**自己到期关掉**。
 * 所以每组用例都问同一个问题：**这次开启该不该被拒、拒的理由对不对**
 * （架构 §16.2 场景 10：远程代理无法访问白名单之外的 IP 和端口）。
 *
 * ============================ 覆盖 ============================
 *   t01 服务名白名单：合法三类 + 16 类非法（**含 ip:port / 域名 / URL**）
 *   t02 服务名渲染往返
 *   t03 secretKey 黄金向量（对照 BIRD 公式，含"每服务独立派生"）
 *   t04 toml 渲染逐字钉：stcp / secretKey / localIP / localPort / 多项
 *   t05 非环回目标 ⇒ EHOST（想代理到网内别的机器，这是"禁 ip:port"的硬证据）
 *   t06 渲染拒绝面：身份缺 / 服务端缺 / resolver 缺或拒 / 数量越界 / 缓冲不足
 *   t07 开启：配置文件落盘且权限 0600、标记文件存在且**内容＝到期时刻**、重复开启 ⇒ ESTATE
 *   t08 关闭：标记消失、状态 OFF、会话留档（时长与原因）
 *   t09 到期：tick 自动关、expires 计数、remain_s 递减
 *   t10 有效期：越界 ⇒ EBADARG；0 ⇒ 取默认
 *   t11 能力位：缺任一项 ⇒ 0
 *   t12 invalidate：开启中作废 ⇒ STOP_RECONF
 *   t13 统计：opens / denies / deny_by / uptime_s
 *   t14 参数与 NULL 安全
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_tunnel.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* ---------------------------------------------------------------------------
 * 测试侧材料
 * ------------------------------------------------------------------------- */
#define SN    "A120F"
#define TOKEN "bird123"

/* 与 BIRD `utils.GenerateSTCPKey` 同源的黄金向量（由 Go 侧公式独立算出）：
 *   md5("A120F" + "bird123")       前 8 字节 = 1239036bac71d702（svc 为空 ⇒ BIRD 原公式）
 *   md5("A120F" + "bird123"+"web")          = 1058c8a5738276aa
 *   md5("A120F" + "bird123"+"camera:2")     = ebcba71b5463cb24
 *   md5("A120F" + "bird123"+"media:0")      = 451846dee3300a33
 *   md5("A120F" + "bird123"+"ftp")          = 2f8940c2063062ed
 *   md5("A120F" + "bird123"+"ftpdata")      = 86b14dcca51266fb */
#define SEC_PLAIN   "1239036bac71d702"
#define SEC_WEB     "1058c8a5738276aa"
#define SEC_CAM2    "ebcba71b5463cb24"
#define SEC_MEDIA0  "451846dee3300a33"
#define SEC_FTP     "2f8940c2063062ed"
#define SEC_FTPDATA "86b14dcca51266fb"

static char g_dir[64];   /* 临时目录 */
static char g_conf[128]; /* <dir>/frpc.toml */
static char g_flag[128]; /* <dir>/tunnel.enabled */

/* resolver：web→127.0.0.1:8080；camera:N→127.0.0.1:8081+N；media:N→127.0.0.1:8090 */
static int res_ok(const iv_tunnel_svc_t *svc, uint32_t *ip_be, uint16_t *port, void *arg)
{
    (void)arg;
    *ip_be = 0x7F000001u; /* 127.0.0.1（网络序） */
    if (svc->kind == IV_TUNNEL_SVC_WEB)        { *port = 8080u; return IV_OK; }
    if (svc->kind == IV_TUNNEL_SVC_CAMERA)     { *port = (uint16_t)(8081u + svc->index); return IV_OK; }
    if (svc->kind == IV_TUNNEL_SVC_MEDIA)      { *port = 8090u; return IV_OK; }
    if (svc->kind == IV_TUNNEL_SVC_FTP)        { *port = 21u;   return IV_OK; }
    return IV_ENOENT;
}

/* 想直接代理到摄像机 IP —— 必须被拒（EHOST） */
static int res_camera_ip(const iv_tunnel_svc_t *svc, uint32_t *ip_be, uint16_t *port, void *arg)
{
    (void)svc; (void)arg;
    *ip_be = 0xC0A8010Au; /* 192.168.1.10 */
    *port  = 80u;
    return IV_OK;
}

static int res_deny(const iv_tunnel_svc_t *svc, uint32_t *ip_be, uint16_t *port, void *arg)
{
    (void)svc; (void)ip_be; (void)port; (void)arg;
    return IV_ENOENT;
}

static const iv_tunnel_resolver_t RES_OK = { res_ok, NULL, 0u };
/* 自定义数据口：钉"配了就按配的来"，别退默认 50000 */
static const iv_tunnel_resolver_t RES_FTP6000 = { res_ok, NULL, 6000u };
/* 数据口＝控制口：一定是配错了（同一个口既当控制又当数据），必须拒 */
static const iv_tunnel_resolver_t RES_FTP21 = { res_ok, NULL, 21u };
static const iv_tunnel_resolver_t RES_IP = { res_camera_ip, NULL, 0u };
static const iv_tunnel_resolver_t RES_NO = { res_deny, NULL, 0u };

/* 装配好的句柄（临时目录 + 身份 + 服务端 + resolver） */
static void mk_rig(iv_tunnel_t *t)
{
    iv_tunnel_init(t);
    chk(iv_tunnel_set_identity(t, SN, TOKEN) == IV_OK, "rig: identity");
    chk(iv_tunnel_set_server(t, "frp.example.com", 7000u) == IV_OK, "rig: server");
    chk(iv_tunnel_set_resolver(t, &RES_OK) == IV_OK, "rig: resolver");
    chk(iv_tunnel_set_paths(t, g_conf, g_flag) == IV_OK, "rig: paths");
}

static int file_mode_is(const char *path, mode_t want)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & 0777u) == (want & 0777u);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* ---------------------------------------------------------------------------
 * t01 服务名白名单
 * ------------------------------------------------------------------------- */
static void t01_svc_white(void)
{
    static const char *const BAD[] = {
        "", "Web", "WEB", "web:80", "web/", "192.168.1.10", "1.2.3.4:80",
        "http://192.168.1.10", "camera:", "camera:-1", "camera:06", "camera:6",
        "camera:1:80", "media:8", "camera:99999", "ssh", "//web", "web ",
        "ftp:21", "FTP", "sftp", "ftp "
    };
    iv_tunnel_svc_t s;
    size_t          i;

    chk(iv_tunnel_svc_parse("web", 3, &s) == IV_OK && s.kind == IV_TUNNEL_SVC_WEB &&
        s.index == 0u, "t01: web");
    chk(iv_tunnel_svc_parse("camera:0", 8, &s) == IV_OK &&
        s.kind == IV_TUNNEL_SVC_CAMERA && s.index == 0u, "t01: camera:0");
    chk(iv_tunnel_svc_parse("camera:5", 8, &s) == IV_OK && s.index == 5u, "t01: camera:5");
    chk(iv_tunnel_svc_parse("media:7", 7, &s) == IV_OK &&
        s.kind == IV_TUNNEL_SVC_MEDIA && s.index == 7u, "t01: media:7");
    chk(iv_tunnel_svc_parse("ftp", 3, &s) == IV_OK && s.kind == IV_TUNNEL_SVC_FTP &&
        s.index == 0u, "t01: ftp");
    chk(iv_tunnel_svc_parse("camera:6", 8, &s) == IV_ERANGE, "t01: camera idx range");
    chk(iv_tunnel_svc_parse("media:8", 7, &s) == IV_ERANGE, "t01: media idx range");

    for (i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        char msg[128];
        snprintf(msg, sizeof msg, "t01: reject svc '%s'", BAD[i]);
        chk(iv_tunnel_svc_parse(BAD[i], strlen(BAD[i]), &s) != IV_OK, msg);
    }
    chk(iv_tunnel_svc_parse(NULL, 0, &s) == IV_EINVAL, "t01: NULL svc");
    chk(iv_tunnel_svc_parse("web", 3, NULL) == IV_EINVAL, "t01: NULL out");
}

/* ---------------------------------------------------------------------------
 * t02 渲染往返
 * ------------------------------------------------------------------------- */
static void t02_svc_format(void)
{
    iv_tunnel_svc_t s;
    char            buf[IV_TUNNEL_SVC_MAX];

    s.kind = IV_TUNNEL_SVC_WEB; s.index = 0u;
    chk(iv_tunnel_svc_format(&s, buf, sizeof buf) == IV_OK && strcmp(buf, "web") == 0,
        "t02: web roundtrip");
    s.kind = IV_TUNNEL_SVC_CAMERA; s.index = 3u;
    chk(iv_tunnel_svc_format(&s, buf, sizeof buf) == IV_OK &&
        strcmp(buf, "camera:3") == 0, "t02: camera roundtrip");
    s.kind = IV_TUNNEL_SVC_MEDIA; s.index = 12u;
    chk(iv_tunnel_svc_format(&s, buf, sizeof buf) == IV_OK &&
        strcmp(buf, "media:12") == 0, "t02: media roundtrip");
    s.kind = IV_TUNNEL_SVC_FTP; s.index = 0u;
    chk(iv_tunnel_svc_format(&s, buf, sizeof buf) == IV_OK &&
        strcmp(buf, "ftp") == 0, "t02: ftp roundtrip");
    chk(iv_tunnel_svc_format(&s, buf, 3) != IV_OK && buf[0] == '\0', "t02: cap short");
}

/* ---------------------------------------------------------------------------
 * t03 secretKey 黄金向量
 * ------------------------------------------------------------------------- */
static void t03_secret(void)
{
    char out[IV_TUNNEL_SECRET_MAX];

    /* svc 为空 ⇒ BIRD 原公式（这是"与运维侧字节级一致"的钉子） */
    chk(iv_tunnel_secret(SN, TOKEN, NULL, out, sizeof out) == IV_OK &&
        strcmp(out, SEC_PLAIN) == 0, "t03: plain == BIRD formula");
    chk(iv_tunnel_secret(SN, TOKEN, "", out, sizeof out) == IV_OK &&
        strcmp(out, SEC_PLAIN) == 0, "t03: empty svc == BIRD formula");
    chk(iv_tunnel_secret(SN, TOKEN, "web", out, sizeof out) == IV_OK &&
        strcmp(out, SEC_WEB) == 0, "t03: web");
    chk(iv_tunnel_secret(SN, TOKEN, "camera:2", out, sizeof out) == IV_OK &&
        strcmp(out, SEC_CAM2) == 0, "t03: camera:2");
    chk(iv_tunnel_secret(SN, TOKEN, "media:0", out, sizeof out) == IV_OK &&
        strcmp(out, SEC_MEDIA0) == 0, "t03: media:0");
    /* 每服务独立：不同服务必须不同钥匙（否则"授权看 Web"＝授权全部） */
    chk(strcmp(SEC_WEB, SEC_CAM2) != 0 && strcmp(SEC_WEB, SEC_MEDIA0) != 0,
        "t03: per-service keys differ");
    chk(strlen(out) == IV_TUNNEL_SECRET_HEX, "t03: 16 hex chars");
    chk(iv_tunnel_secret(SN, TOKEN, "web", out, 8) == IV_ERANGE, "t03: cap short");
    chk(iv_tunnel_secret(NULL, TOKEN, "web", out, sizeof out) == IV_EINVAL, "t03: NULL sn");
}

/* ---------------------------------------------------------------------------
 * t04 toml 渲染逐字钉
 * ------------------------------------------------------------------------- */
static void t04_render(void)
{
    static const char *const EXPECT1 =
        "serverAddr = \"frp.example.com\"\n"
        "serverPort = 7000\n"
        "auth.token = \"bird123\"\n"
        "\n"
        "[[proxies]]\n"
        "name = \"A120F-web\"\n"
        "type = \"stcp\"\n"
        "secretKey = \"" SEC_WEB "\"\n"
        "localIP = \"127.0.0.1\"\n"
        "localPort = 8080\n";
    iv_tunnel_svc_t    svcs[2];
    iv_tunnel_reason_t why = IV_TUNNEL_ESTATE;
    char               buf[IV_TUNNEL_TXT_MAX];
    size_t             len = 0;

    chk(iv_tunnel_svc_parse("web", 3, &svcs[0]) == IV_OK, "t04: parse web");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, svcs, 1u,
                         buf, sizeof buf, &len, &why) == IV_OK, "t04: render one");
    chk(why == IV_TUNNEL_OK, "t04: reason ok");
    chk(strcmp(buf, EXPECT1) == 0, "t04: exact text (one proxy)");
    chk(len == strlen(EXPECT1), "t04: out_len");

    /* 两项：摄像机项必须出现自己的钥匙与本地代理端口 */
    chk(iv_tunnel_svc_parse("camera:2", 8, &svcs[1]) == IV_OK, "t04: parse camera:2");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, svcs, 2u,
                         buf, sizeof buf, &len, &why) == IV_OK, "t04: render two");
    chk(strstr(buf, "name = \"A120F-camera:2\"") != NULL, "t04: second proxy name");
    chk(strstr(buf, "secretKey = \"" SEC_CAM2 "\"") != NULL, "t04: second secret");
    chk(strstr(buf, "localPort = 8083") != NULL, "t04: second local port");
    chk(strstr(buf, "type = \"stcp\"") != NULL, "t04: stcp type");
    /* 摄像机 IP 绝不能出现在配置里 */
    chk(strstr(buf, "192.168.") == NULL, "t04: no camera ip in config");
}

/* ---------------------------------------------------------------------------
 * t04b `ftp`：一条服务名 → 两条代理（控制 + 被动数据）
 * 为什么要两条：FTP 的数据连接是另一条 TCP，frp 一条代理只映射一个端口。
 * 反向证伪：只渲染一条 / 数据口不兜默认 50000 / 两条钥匙相同 ⇒ 下面立刻红。
 * ------------------------------------------------------------------------- */
static void t04b_render_ftp(void)
{
    iv_tunnel_svc_t    svc;
    iv_tunnel_reason_t why = IV_TUNNEL_ESTATE;
    char               buf[IV_TUNNEL_TXT_MAX];
    size_t             len = 0;

    chk(iv_tunnel_svc_parse("ftp", 3, &svc) == IV_OK, "t04b: parse ftp");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, &svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_OK, "t04b: render ftp");
    chk(why == IV_TUNNEL_OK, "t04b: reason ok");
    /* 控制条 */
    chk(strstr(buf, "name = \"A120F-ftp\"") != NULL, "t04b: control proxy name");
    chk(strstr(buf, "secretKey = \"" SEC_FTP "\"") != NULL, "t04b: control secret");
    chk(strstr(buf, "localPort = 21\n") != NULL, "t04b: control port 21");
    /* 数据条：端口固定、钥匙独立 */
    chk(strstr(buf, "name = \"A120F-ftpdata\"") != NULL, "t04b: data proxy name");
    chk(strstr(buf, "secretKey = \"" SEC_FTPDATA "\"") != NULL, "t04b: data secret");
    chk(strstr(buf, "localPort = 50000\n") != NULL, "t04b: data port default 50000");
    {
        const char *q   = buf;
        int         cnt = 0;
        while ((q = strstr(q, "[[proxies]]")) != NULL) { cnt++; q++; }
        chk(cnt == 2, "t04b: exactly two proxy blocks");
    }
    /* 装配层配了数据口 ⇒ 按配的来（别硬编码 50000） */
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_FTP6000, &svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_OK, "t04b: render custom");
    chk(strstr(buf, "localPort = 6000\n") != NULL, "t04b: data port from resolver");
    /* 数据口撞控制口 ⇒ 拒 */
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_FTP21, &svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_EINVAL,
        "t04b: data port == control port rejected");
}

/* ---------------------------------------------------------------------------
 * t05 非环回目标 ⇒ EHOST
 * ------------------------------------------------------------------------- */
static void t05_host_gate(void)
{
    iv_tunnel_svc_t    svc;
    iv_tunnel_reason_t why = IV_TUNNEL_OK;
    char               buf[IV_TUNNEL_TXT_MAX];
    size_t             len = 0;

    chk(iv_tunnel_svc_parse("camera:2", 8, &svc) == IV_OK, "t05: parse");
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_IP, &svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_EPROTO &&
        why == IV_TUNNEL_EHOST, "t05: camera ip rejected (EHOST)");
    /* 环回但端口 0 也不行 */
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_NO, &svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_ENOENT &&
        why == IV_TUNNEL_EUNMAPPED, "t05: resolver deny");
}

/* ---------------------------------------------------------------------------
 * t06 渲染拒绝面
 * ------------------------------------------------------------------------- */
static void t06_render_deny(void)
{
    iv_tunnel_svc_t    svc[9];
    iv_tunnel_reason_t why = IV_TUNNEL_OK;
    char               buf[IV_TUNNEL_TXT_MAX];
    char               small[64];
    size_t             len = 0;
    unsigned           i;

    chk(iv_tunnel_svc_parse("web", 3, &svc[0]) == IV_OK, "t06: parse");
    for (i = 1; i < 9u; i++) svc[i] = svc[0];

    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render("", TOKEN, "frp.example.com", 7000u, &RES_OK, svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_EPROTO &&
        why == IV_TUNNEL_ENOIDENT, "t06: no sn");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, "", "frp.example.com", 7000u, &RES_OK, svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_EPROTO &&
        why == IV_TUNNEL_ENOIDENT, "t06: no token");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "", 7000u, &RES_OK, svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_ENOENT &&
        why == IV_TUNNEL_ENOSERVER, "t06: no server addr");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 0u, &RES_OK, svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_ENOENT &&
        why == IV_TUNNEL_ENOSERVER, "t06: no server port");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, NULL, svc, 1u,
                         buf, sizeof buf, &len, &why) == IV_ENOENT &&
        why == IV_TUNNEL_EUNMAPPED, "t06: no resolver");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, svc, 0u,
                         buf, sizeof buf, &len, &why) == IV_EINVAL &&
        why == IV_TUNNEL_EBADARG, "t06: n = 0");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, svc, 9u,
                         buf, sizeof buf, &len, &why) == IV_EINVAL &&
        why == IV_TUNNEL_EBADARG, "t06: n > max");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_render(SN, TOKEN, "frp.example.com", 7000u, &RES_OK, svc, 1u,
                         small, sizeof small, &len, &why) == IV_ERANGE &&
        why == IV_TUNNEL_EBADARG, "t06: buffer short");
    chk(iv_tunnel_render(NULL, TOKEN, "x", 7000u, &RES_OK, svc, 1u,
                         buf, sizeof buf, &len, NULL) == IV_EPROTO, "t06: NULL sn ok with NULL why");
}

/* ---------------------------------------------------------------------------
 * t07 开启
 * ------------------------------------------------------------------------- */
static void t07_enable(void)
{
    iv_tunnel_t        t;
    iv_tunnel_svc_t    svc;
    iv_tunnel_reason_t why = IV_TUNNEL_OK;
    FILE              *fp;
    char               line[256];
    int                found = 0;

    mk_rig(&t);
    chk(iv_tunnel_svc_parse("web", 3, &svc) == IV_OK, "t07: parse");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t07: default off");

    chk(iv_tunnel_enable(&t, &svc, 1u, 600u, 1000u, &why) == IV_OK &&
        why == IV_TUNNEL_OK, "t07: enable ok");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_ON, "t07: state on");
    chk(file_exists(g_conf), "t07: conf written");
    chk(file_mode_is(g_conf, 0600u), "t07: conf mode 0600 (secretKey inside)");
    chk(file_exists(g_flag), "t07: flag file exists");
    chk(iv_tunnel_remain_s(&t, 1000u) == 600u, "t07: remain");

    /* 标记内容＝到期时刻（unix 秒）。这是"主控死了脚本也能判过期"的全部依据
     * （见 scripts/frp.sh 的 flag_expired）：enable(600, now=1000) ⇒ 内容 1600。 */
    fp = fopen(g_flag, "r");
    chk(fp != NULL, "t07: open flag");
    if (fp != NULL) {
        char fl[64];

        chk(fgets(fl, sizeof fl, fp) != NULL, "t07: flag has one line");
        chk(strncmp(fl, "1600\n", 5) == 0, "t07: flag carries the expire instant");
        fclose(fp);
    }

    /* 配置文件里必须有派生出的钥匙，且不含摄像机地址 */
    fp = fopen(g_conf, "r");
    chk(fp != NULL, "t07: open conf");
    if (fp != NULL) {
        while (fgets(line, sizeof line, fp) != NULL) {
            if (strstr(line, SEC_WEB) != NULL) found = 1;
            chk(strstr(line, "192.168.") == NULL, "t07: no camera ip on disk");
        }
        fclose(fp);
    }
    chk(found == 1, "t07: secretKey on disk");

    /* 重复开启：要换服务集合必须先关，免得悄悄换掉正在用的通道 */
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_enable(&t, &svc, 1u, 600u, 1001u, &why) == IV_ESTATE &&
        why == IV_TUNNEL_ESTATE, "t07: double enable -> ESTATE");

    /* 有效期越界 */
    chk(iv_tunnel_disable(&t, 1100u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t07: disable");
    why = IV_TUNNEL_OK;
    chk(iv_tunnel_enable(&t, &svc, 1u, IV_TUNNEL_TTL_MAX + 1u, 1200u, &why) == IV_EINVAL &&
        why == IV_TUNNEL_EBADARG, "t07: ttl over max");
    /* ttl=0 ⇒ 取默认 */
    chk(iv_tunnel_enable(&t, &svc, 1u, 0u, 1200u, &why) == IV_OK, "t07: ttl default");
    chk(iv_tunnel_remain_s(&t, 1200u) == IV_TUNNEL_TTL_DEF, "t07: ttl default applied");
    chk(iv_tunnel_disable(&t, 1201u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t07: disable again");
}

/* ---------------------------------------------------------------------------
 * t08 关闭与会话留档
 * ------------------------------------------------------------------------- */
static void t08_disable(void)
{
    iv_tunnel_t           t;
    iv_tunnel_svc_t       svc;
    iv_tunnel_session_t   s;
    iv_tunnel_reason_t    why = IV_TUNNEL_OK;

    mk_rig(&t);
    chk(iv_tunnel_svc_parse("camera:1", 8, &svc) == IV_OK, "t08: parse");
    chk(iv_tunnel_enable(&t, &svc, 1u, 300u, 2000u, &why) == IV_OK, "t08: enable");
    chk(iv_tunnel_disable(&t, 2300u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t08: disable ok");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t08: state off");
    chk(!file_exists(g_flag), "t08: flag removed");
    chk(file_exists(g_conf), "t08: conf kept (0600)");

    chk(iv_tunnel_last_session(&t, &s) == IV_OK, "t08: session kept");
    chk(s.open_s == 2000u && s.close_s == 2300u, "t08: session window");
    chk(s.ttl_s == 300u && s.n_svc == 1u, "t08: session meta");
    chk(s.svc[0].kind == IV_TUNNEL_SVC_CAMERA && s.svc[0].index == 1u, "t08: session svc");
    chk(s.stop_reason == IV_TUNNEL_STOP_LOCAL, "t08: stop reason");

    /* 未开启就关闭 ⇒ ESTATE */
    chk(iv_tunnel_disable(&t, 2400u, IV_TUNNEL_STOP_LOCAL) == IV_ESTATE, "t08: disable twice");
}

/* ---------------------------------------------------------------------------
 * t09 到期自动关
 * ------------------------------------------------------------------------- */
static void t09_expire(void)
{
    iv_tunnel_t        t;
    iv_tunnel_svc_t    svc;
    iv_tunnel_stat_t   st;
    iv_tunnel_reason_t why = IV_TUNNEL_OK;

    mk_rig(&t);
    chk(iv_tunnel_svc_parse("web", 3, &svc) == IV_OK, "t09: parse");
    chk(iv_tunnel_enable(&t, &svc, 1u, 100u, 3000u, &why) == IV_OK, "t09: enable");

    chk(iv_tunnel_tick(&t, 3050u) == 0, "t09: not expired yet");
    chk(iv_tunnel_remain_s(&t, 3050u) == 50u, "t09: remain 50");
    chk(iv_tunnel_tick(&t, 3099u) == 0, "t09: boundary not expired");
    chk(iv_tunnel_tick(&t, 3100u) == 1, "t09: expired at boundary");
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t09: off after expire");
    chk(!file_exists(g_flag), "t09: flag removed on expire");

    iv_tunnel_stat(&t, &st);
    chk(st.expires == 1u, "t09: expires counted");
    chk(st.uptime_s == 100u, "t09: uptime counted");
    chk(iv_tunnel_tick(&t, 3200u) == 0, "t09: tick when off is noop");
}

/* ---------------------------------------------------------------------------
 * t10 / t11 能力位与统计
 * ------------------------------------------------------------------------- */
static void t10_caps(void)
{
    iv_tunnel_t t;

    iv_tunnel_init(&t);
    chk(iv_tunnel_caps(&t) == 0u, "t10: bare handle has no caps");
    chk(iv_tunnel_set_identity(&t, SN, TOKEN) == IV_OK, "t10: identity");
    chk(iv_tunnel_caps(&t) == 0u, "t10: still no server");
    chk(iv_tunnel_set_server(&t, "frp.example.com", 7000u) == IV_OK, "t10: server");
    chk(iv_tunnel_caps(&t) == 0u, "t10: still no resolver");
    chk(iv_tunnel_set_resolver(&t, &RES_OK) == IV_OK, "t10: resolver");
    chk((iv_tunnel_caps(&t) & IV_TUNNEL_CAP_ENABLED) != 0u, "t10: enabled bit");
    chk((iv_tunnel_caps(&t) & IV_TUNNEL_CAP_WEB) != 0u &&
        (iv_tunnel_caps(&t) & IV_TUNNEL_CAP_CAMERA) != 0u &&
        (iv_tunnel_caps(&t) & IV_TUNNEL_CAP_MEDIA) != 0u &&
        (iv_tunnel_caps(&t) & IV_TUNNEL_CAP_FTP) != 0u, "t10: service bits");
    /* 解除映射 ⇒ 能力立刻消失（不许声称一个开不起来的能力） */
    chk(iv_tunnel_set_resolver(&t, NULL) == IV_EINVAL, "t10: resolver cleared");
    chk(iv_tunnel_caps(&t) == 0u, "t10: caps gone after resolver cleared");
    /* 身份非法 */
    chk(iv_tunnel_set_identity(&t, "", TOKEN) == IV_EINVAL, "t10: empty sn rejected");
    chk(iv_tunnel_set_server(&t, "", 0u) == IV_EINVAL, "t10: empty addr rejected");
}

static void t11_stat(void)
{
    iv_tunnel_t        t;
    iv_tunnel_svc_t    svc;
    iv_tunnel_stat_t   st;
    iv_tunnel_reason_t why = IV_TUNNEL_OK;

    mk_rig(&t);
    chk(iv_tunnel_svc_parse("web", 3, &svc) == IV_OK, "t11: parse");

    /* 一次成功 + 一次被拒（有效期越界） */
    chk(iv_tunnel_enable(&t, &svc, 1u, 60u, 4000u, &why) == IV_OK, "t11: enable");
    chk(iv_tunnel_disable(&t, 4030u, IV_TUNNEL_STOP_LOCAL) == IV_OK, "t11: disable");
    chk(iv_tunnel_enable(&t, &svc, 1u, IV_TUNNEL_TTL_MAX + 1u, 4040u, &why) == IV_EINVAL,
        "t11: denied");
    iv_tunnel_stat(&t, &st);
    chk(st.opens == 1u, "t11: opens");
    chk(st.denies == 1u, "t11: denies");
    chk(st.deny_by[IV_TUNNEL_EBADARG] == 1u, "t11: deny_by reason");
    chk(st.uptime_s == 30u, "t11: uptime");

    /* 缺身份 ⇒ ENOIDENT 计数 */
    {
        iv_tunnel_t t2;
        iv_tunnel_init(&t2);
        chk(iv_tunnel_set_paths(&t2, g_conf, g_flag) == IV_OK, "t11: paths");
        chk(iv_tunnel_enable(&t2, &svc, 1u, 60u, 4000u, &why) == IV_EPROTO &&
            why == IV_TUNNEL_ENOIDENT, "t11: no identity denied");
    }

    /* invalidate：开启中作废 ⇒ RECONF 留档 */
    chk(iv_tunnel_enable(&t, &svc, 1u, 60u, 5000u, &why) == IV_OK, "t11: enable again");
    chk(iv_tunnel_invalidate(&t) == IV_OK, "t11: invalidate");
    {
        iv_tunnel_session_t s;
        chk(iv_tunnel_last_session(&t, &s) == IV_OK, "t11: session");
        chk(s.stop_reason == IV_TUNNEL_STOP_RECONF, "t11: reconf reason");
    }
    chk(iv_tunnel_state(&t) == IV_TUNNEL_OFF, "t11: off after invalidate");
    chk(iv_tunnel_invalidate(&t) == IV_OK, "t11: invalidate when off is noop");
    chk(!file_exists(g_flag), "t11: flag gone");
}

/* ---------------------------------------------------------------------------
 * t12 参数与 NULL 安全
 * ------------------------------------------------------------------------- */
static void t12_null(void)
{
    iv_tunnel_t        t;
    iv_tunnel_svc_t    svc;
    iv_tunnel_reason_t why = IV_TUNNEL_EBADARG;
    iv_tunnel_stat_t   st;
    iv_tunnel_session_t s;

    chk(iv_tunnel_svc_parse("web", 3, &svc) == IV_OK, "t12: parse");
    mk_rig(&t);

    /* 参数非法时**不改** why（调用方的旧值不该被踩） */
    chk(iv_tunnel_enable(NULL, &svc, 1u, 60u, 1u, &why) == IV_EINVAL, "t12: NULL handle");
    chk(why == IV_TUNNEL_EBADARG, "t12: why untouched");

    chk(iv_tunnel_disable(NULL, 1u, IV_TUNNEL_STOP_LOCAL) == IV_EINVAL, "t12: disable NULL");
    chk(iv_tunnel_tick(NULL, 1u) == IV_EINVAL, "t12: tick NULL");
    chk(iv_tunnel_state(NULL) == IV_TUNNEL_OFF, "t12: state NULL");
    chk(iv_tunnel_remain_s(NULL, 1u) == 0u, "t12: remain NULL");
    chk(iv_tunnel_last_session(NULL, &s) == IV_EINVAL, "t12: session NULL");
    iv_tunnel_stat(NULL, &st);     /* 不崩 */
    iv_tunnel_init(NULL);          /* 不崩 */
    chk(iv_tunnel_last_session(&t, &s) == IV_ENOENT, "t12: no session yet");
    chk(strcmp(iv_tunnel_reason_name(IV_TUNNEL_EHOST), "EHOST") == 0, "t12: reason name");
    chk(strcmp(iv_tunnel_reason_name((iv_tunnel_reason_t)99), "UNKNOWN") == 0,
        "t12: unknown reason name");
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    /* 临时目录：所有落盘用例都在这里，跑完清掉（规则 6） */
    snprintf(g_dir, sizeof g_dir, "/tmp/ivtun_XXXXXX");
    if (mkdtemp(g_dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp\n");
        return 1;
    }
    snprintf(g_conf, sizeof g_conf, "%s/frpc.toml", g_dir);
    snprintf(g_flag, sizeof g_flag, "%s/tunnel.enabled", g_dir);

    t01_svc_white();
    t02_svc_format();
    t03_secret();
    t04_render();
    t04b_render_ftp();
    t05_host_gate();
    t06_render_deny();
    t07_enable();
    t08_disable();
    t09_expire();
    t10_caps();
    t11_stat();
    t12_null();

    (void)unlink(g_conf);
    (void)unlink(g_flag);
    (void)rmdir(g_dir);

    if (g_fail != 0) {
        fprintf(stderr, "test_tunnel: %d failures\n", g_fail);
        return 1;
    }
    printf("test_tunnel passed\n");
    return 0;
}
