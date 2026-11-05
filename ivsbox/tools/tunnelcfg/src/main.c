/*
 * 运维箱通道配置（单文件 exe）1.1
 * ---------------------------------------------------------------------------
 * 与 tunnelcfg.ps1 行为对齐，但整条链路自己走，**不调用任何 .ps1/.cmd/PowerShell**：
 *
 *   · 设备侧：内置 HTTP 客户端（WinHTTP）直连 /api/login、/api/tunnel、
 *             /api/tunnel/config、/api/tunnel/enable|disable，会话 cookie 自己拿
 *   · 服务器侧：调 plink.exe -m <临时脚本>（唯一的外部程序；没有 plink 就只把
 *             命令复制到剪贴板让人手贴，绝不因此假装成功）
 *   · 本机侧：直接读写 remote.ini 里 token= 那一行（只改值，别的字节不动）
 *   · 令牌随机数来自 BCryptGenRandom（不自己写 PRNG，也不从 time() 派生）
 *
 * 界面：760x640，四段分组 + 只读日志框；非可缩放窗口（布局一次算准，省得
 * 每种 DPI 都要重排）。高 DPI 下所有坐标经 S() 缩放，字号按 pt 换算。
 *
 * 线程：动作都在 worker 线程里跑（设备 30 s 超时、轮换要等 6 s 复查、plink 可能
 * 卡十几秒），worker **不碰控件**，只用 PostMessage(WM_LOG) 把字符串交给 UI 线程；
 * 这样"点下去界面就假死"的老毛病不存在。
 *
 * 编译：见 build.sh（mingw64 gcc + windres）。字符一律走 W API，源码 UTF-8。
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#define APP_TITLE    L"运维箱通道配置"
#define APP_VER      L"1.1"
#define LOCAL_SSH   16000                 /* 运维箱远程访问.exe 的本地 stcp 监听端口 */
#define ECS_HOST_DEF L"47.95.22.96"
#define ECS_USER     L"root"
#define ECS_HOSTKEY  L"ssh-ed25519 255 SHA256:Uy99qmPWoPgHefIFj1Am23sr0QVoC8RnpS43+ENUID4"
#define PLINK_PATH   L"C:\\Program Files\\PuTTY\\plink.exe"

/* 控件 ID */
#define ID_HOST     1001
#define ID_PORT     1002
#define ID_TOK      1003
#define ID_SSHPW    1004
#define ID_IP       1005
#define ID_USER     1006
#define ID_DEVPW    1007
#define ID_INI      1008
#define ID_LOG      1009
#define ID_B_GEN    1010
#define ID_B_COPY   1011
#define ID_B_CHECK  1012
#define ID_B_DEVTOK 1013
#define ID_B_INITOK 1014
#define ID_B_ON     1015
#define ID_B_OFF    1016
#define ID_B_ROT    1017
#define ID_B_SAVE   1018
#define ID_B_CLR    1019

/* 自定义消息：worker -> UI */
#define WM_LOG      (WM_APP + 1)
#define WM_DONE     (WM_APP + 2)

#define NBUTTON 10

enum { ACT_CHECK = 1, ACT_DEVTOK, ACT_INITOK, ACT_ON, ACT_OFF, ACT_ROT, ACT_READ };

/* 动作参数快照：点按钮那一刻从界面抄一份，worker 只读它 */
typedef struct {
    int      action;
    wchar_t  devip[64];
    wchar_t  devuser[64];
    wchar_t  devpass[128];
    wchar_t  ini[MAX_PATH];
    wchar_t  ecshost[128];
    wchar_t  ecspass[128];
    wchar_t  token[128];
} job_t;

static HINSTANCE g_hi;
static HWND   g_hMain, g_hLog;
static HWND   g_hHost, g_hPort, g_hTok, g_hSshPw, g_hIp, g_hUser, g_hDevPw, g_hIni;
static HWND   g_btn[NBUTTON];
static HFONT  g_fontUi, g_fontMono;
static HBRUSH g_brLog;
static int    g_dpi = 96;
static int    g_fail;             /* 只由 worker / CLI 写，UI 在 WM_DONE 里读 */
static BOOL   g_busy;
static BOOL   g_cli;              /* CLI 模式：日志写文件而不是控件 */
static FILE  *g_cliLog;
static wchar_t g_shot[MAX_PATH];
static BOOL   g_shotCheck;        /* --shot 的第二段：自动点一次「一致性检查」 */
static volatile LONG g_done;      /* worker 已回 WM_DONE（只在 --shot 里等它） */

/* ---------------------------------------------------------------- 小工具 */

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void to_u8(const wchar_t *w, char *o, int cap)
{
    if (!o || cap <= 0) return;
    if (!w) { o[0] = 0; return; }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, o, cap, NULL, NULL) <= 0) o[0] = 0;
}

static void to_w(const char *s, wchar_t *o, int cap)
{
    if (!o || cap <= 0) return;
    if (!s) { o[0] = 0; return; }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, o, cap) <= 0) o[0] = 0;
}

/* 去掉首尾空白（界面里手输 IP 常带空格） */
static void trim(wchar_t *s)
{
    if (!s) return;
    wchar_t *p = s;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' ||
                     s[n - 1] == L'\n'))
        s[--n] = 0;
}

static wchar_t *wdump(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *p = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (p) memcpy(p, s, n * sizeof(wchar_t));
    return p;
}

/* ---------------------------------------------------------------- 日志 */

/* 统一入口：GUI 追加到只读框，CLI 追加到 UTF-8 文件。worker 绝不直接调它，
 * 只 PostMessage —— 控件不是线程安全的。 */
static void log_line(const wchar_t *s)
{
    if (g_cli) {
        if (g_cliLog) {
            char u8[4096];
            to_u8(s, u8, sizeof u8);
            fwrite(u8, 1, strlen(u8), g_cliLog);
            fwrite("\r\n", 1, 2, g_cliLog);
            fflush(g_cliLog);
        }
        return;
    }
    if (!g_hLog || !s) return;
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void wlog_line(const wchar_t *s)
{
    if (g_cli) { log_line(s); return; }
    wchar_t *p = wdump(s);
    if (p && !PostMessageW(g_hMain, WM_LOG, 0, (LPARAM)p)) free(p);
}

static void vlogf(const wchar_t *prefix, const wchar_t *fmt, va_list ap)
{
    wchar_t body[1800];
    wchar_t line[2048];
    if (vswprintf(body, 1800, fmt, ap) < 0) body[1799] = 0;
    swprintf(line, 2048, L"%s%s", prefix, body);
    wlog_line(line);
}

static void plainf(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlogf(L"", fmt, ap);
    va_end(ap);
}
static void okf(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlogf(L"[OK]   ", fmt, ap);
    va_end(ap);
}
static void warnf(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vlogf(L"[WARN] ", fmt, ap);
    va_end(ap);
}
static void badf(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    g_fail++;
    vlogf(L"[X]    ", fmt, ap);
    va_end(ap);
}
static void stepf(const wchar_t *fmt, ...)
{
    va_list ap;
    wchar_t body[512];
    wchar_t line[600];
    va_start(ap, fmt);
    if (vswprintf(body, 512, fmt, ap) < 0) body[511] = 0;
    va_end(ap);
    plainf(L"");
    swprintf(line, 600, L"== %s ==", body);
    wlog_line(line);
}

/* ---------------------------------------------------------------- 令牌 */

static BOOL tok_ok(const wchar_t *t)
{
    size_t n, i;
    if (!t) return FALSE;
    n = wcslen(t);
    if (n < 1 || n > 63) return FALSE;
    /* 与 frp 的 toml 字符串/命令行都能安全承载的字符集：不放空格、引号、反斜杠 */
    for (i = 0; i < n; i++) {
        wchar_t c = t[i];
        BOOL good = (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
                    (c >= L'a' && c <= L'z') || c == L'-' || c == L'_' ||
                    c == L'.' || c == L'~' || c == L'/' || c == L'+' ||
                    c == L'@' || c == L'=';
        if (!good) return FALSE;
    }
    return TRUE;
}

/* 16 字节 → 32 个 hex。系统 CSPRNG，无种子/无状态，不用操心 fork 之类 */
static void tok_new(wchar_t *out, int cap)
{
    unsigned char b[16];
    int i;
    if (BCryptGenRandom(NULL, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        memset(b, 0, sizeof b);       /* 极不可能；退化成全 0 会立刻被长度检查放过，
                                       * 所以下面再用 tick 混一点，避免"静默出个弱值" */
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) {
        DWORD t = GetTickCount();
        memcpy(b, &t, 4);
    }
    if (cap < 33) { if (cap > 0) out[0] = 0; return; }
    for (i = 0; i < 16; i++) swprintf(out + i * 2, (size_t)(cap - i * 2), L"%02x", b[i]);
}

/* 只露头尾，日志里不留完整令牌 */
static void mask_tok(const wchar_t *t, wchar_t *o, int cap)
{
    size_t n = t ? wcslen(t) : 0;
    if (n == 0) { swprintf(o, (size_t)cap, L"(空)"); return; }
    if (n <= 6) { swprintf(o, (size_t)cap, L"%s", t); return; }
    swprintf(o, (size_t)cap, L"%.4s…%s (len=%d)", t, t + n - 2, (int)n);
}

/* ---------------------------------------------------------------- ini */

static void ini_default(wchar_t *p, int cap)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
        GetTempPathW(MAX_PATH, base);
    swprintf(p, (size_t)cap, L"%s\\ivsbox-remote\\remote.ini", base);
}

/* 整个文件读进内存（这些 ini 只有几百字节，比按行流式简单且不怕 CRLF/LF 混用） */
static char *file_read(const wchar_t *path, DWORD *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    DWORD n, got = 0;
    char *b;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(h, NULL);
    if (n == INVALID_FILE_SIZE || n > 1024 * 1024) { CloseHandle(h); return NULL; }
    b = (char *)malloc(n + 2);
    if (!b) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, b, n, &got, NULL)) { CloseHandle(h); free(b); return NULL; }
    CloseHandle(h);
    b[got] = 0;
    b[got + 1] = 0;
    if (len) *len = got;
    return b;
}

/* 找 "key=" 那一行（行首匹配，不受 [section] 影响 —— 与 ps1 的 Get-IniLine 同口径） */
static const char *ini_find(const char *buf, const wchar_t *key, size_t *vlen)
{
    char k[64];
    size_t kl;
    to_u8(key, k, sizeof k);
    kl = strlen(k);
    if (!buf || !kl) return NULL;
    {
        const char *p = buf;
        while (*p) {
            if ((p == buf || p[-1] == '\n') && strncmp(p, k, kl) == 0 && p[kl] == '=') {
                const char *v = p + kl + 1;
                const char *e = v;
                while (*e && *e != '\r' && *e != '\n') e++;
                if (vlen) *vlen = (size_t)(e - v);
                return v;
            }
            p++;
        }
    }
    return NULL;
}

static BOOL ini_get(const wchar_t *path, const wchar_t *key, wchar_t *out, int cap)
{
    char *buf = file_read(path, NULL);
    const char *v;
    size_t n = 0;
    if (out && cap > 0) out[0] = 0;
    if (!buf) return FALSE;
    v = ini_find(buf, key, &n);
    if (!v) { free(buf); return FALSE; }
    if ((int)n >= cap) n = (size_t)cap - 1;
    if (out) {
        int i;
        for (i = 0; i < (int)n; i++) out[i] = (wchar_t)(unsigned char)v[i];
        out[n] = 0;                    /* host/port/token 全是 ASCII，逐字节升宽即可 */
    }
    free(buf);
    return TRUE;
}

/* 只换值，行结束符原样保留；没有这一行就报错（不新增行 —— ps1 也是这个行为） */
static BOOL ini_set(const wchar_t *path, const wchar_t *key, const wchar_t *val)
{
    DWORD len = 0;
    char *buf = file_read(path, &len);
    const char *v;
    size_t vlen = 0, klen;
    char k[64], val8[256];
    char *out;
    size_t pre, tail, total;
    HANDLE h;
    DWORD wr = 0;
    BOOL ok;

    if (!buf) return FALSE;
    v = ini_find(buf, key, &vlen);
    if (!v) { free(buf); return FALSE; }

    to_u8(key, k, sizeof k);
    to_u8(val, val8, sizeof val8);
    klen = strlen(k);

    pre  = (size_t)(v - buf);
    tail = len - pre - vlen;
    total = pre + strlen(val8) + tail + 1;
    out = (char *)malloc(total);
    if (!out) { free(buf); return FALSE; }
    memcpy(out, buf, pre);
    memcpy(out + pre, val8, strlen(val8));
    memcpy(out + pre + strlen(val8), v + vlen, tail);
    out[total - 1] = 0;
    (void)klen;

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    ok = (h != INVALID_HANDLE_VALUE) &&
         WriteFile(h, out, (DWORD)(total - 1), &wr, NULL) &&
         wr == (DWORD)(total - 1);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    free(out);
    free(buf);
    return ok;
}

/* ---------------------------------------------------------------- HTTP */

/* 返回 TRUE 表示"拿到了 HTTP 响应"；业务成败看 JSON 里的 ok */
static BOOL http_req(const wchar_t *host, const wchar_t *method, const wchar_t *path,
                     const char *body, const wchar_t *cookie,
                     char *out, int cap, int *code, wchar_t *err, int errcap)
{
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    wchar_t hdr[640];
    wchar_t hbuf[128];
    unsigned short port = 80;
    BOOL got = FALSE;
    DWORD clen = 0, status = 0, slen = sizeof(status);

    if (out && cap > 0) out[0] = 0;
    if (code) *code = 0;
    if (err && errcap > 0) err[0] = 0;

    /* 允许在「设备 IP」里写 `host:port`：现场排障时常常要对着另一台机器上
     * 用临时口令文件起的实例验一遍（`ivsbox-web --port 18081 --pass-file …`），
     * 那种实例不在 80 上，而界面没有单独的端口框。IPv4 才这么写，别塞 IPv6。 */
    {
        const wchar_t *colon = NULL, *q;
        for (q = host; q && *q; q++) if (*q == L':') colon = q;
        if (colon) {
            size_t n = (size_t)(colon - host);
            unsigned long pv;
            if (n > 127) n = 127;
            memcpy(hbuf, host, n * sizeof(wchar_t));
            hbuf[n] = 0;
            pv = wcstoul(colon + 1, NULL, 10);
            if (pv > 0 && pv <= 65535) port = (unsigned short)pv;
        } else {
            swprintf(hbuf, 128, L"%s", host ? host : L"");
        }
    }

    hs = WinHttpOpen(L"ivsbox-tunnelcfg/1.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) {
        if (err) swprintf(err, (size_t)errcap, L"WinHttpOpen 失败（%lu）", GetLastError());
        return FALSE;
    }
    /* 设备在局域网，绝不走代理；连接/发送 6 s，接收 20 s（写令牌要重写配置+拉起 frpc） */
    WinHttpSetTimeouts(hs, 6000, 6000, 15000, 20000);

    hc = WinHttpConnect(hs, hbuf, (INTERNET_PORT)port, 0);
    if (!hc) {
        if (err) swprintf(err, (size_t)errcap, L"连不上 %s:%u（%lu）", hbuf, port, GetLastError());
        goto done;
    }
    hr = WinHttpOpenRequest(hc, method, path, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hr) {
        if (err) swprintf(err, (size_t)errcap, L"建请求失败（%lu）", GetLastError());
        goto done;
    }

    hdr[0] = 0;
    if (cookie && cookie[0])
        swprintf(hdr, 640, L"Cookie: ivs_sid=%s\r\n", cookie);
    if (body) wcscat(hdr, L"Content-Type: application/json\r\n");

    if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L,
                            (LPVOID)body, body ? (DWORD)strlen(body) : 0,
                            body ? (DWORD)strlen(body) : 0, 0)) {
        if (err) swprintf(err, (size_t)errcap, L"发请求失败（%lu）—— IP 打不开？", GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(hr, NULL)) {
        if (err) swprintf(err, (size_t)errcap, L"等响应超时（%lu）", GetLastError());
        goto done;
    }
    WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen,
                        WINHTTP_NO_HEADER_INDEX);
    if (code) *code = (int)status;

    for (;;) {
        DWORD avail = 0, got2 = 0;
        if (!WinHttpQueryDataAvailable(hr, &avail)) break;
        if (avail == 0) break;
        if ((int)avail > 4096) avail = 4096;
        if ((int)(clen + avail) > cap - 1) avail = (DWORD)(cap - 1 - (int)clen);
        if ((int)avail <= 0) break;                     /* 响应过大：只留前面这段 */
        if (!WinHttpReadData(hr, out + clen, avail, &got2)) break;
        if (got2 == 0) break;
        clen += got2;
        out[clen] = 0;
    }
    got = TRUE;

done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    return got;
}

/* ------------------------------------------------------------ 迷你 JSON */

/* 找 "key" 后紧跟（可空白）冒号的位置。要求引号紧贴冒号，
 * 所以 "server" 不会误命中 "server_port" —— 这是这套手搓解析唯一要守的规矩。 */
static const char *jpos(const char *js, const char *key)
{
    char pat[64];
    const char *p;
    if (!js || !key) return NULL;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = js;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q == ':') {
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
            return q;
        }
        p += strlen(pat);
    }
    return NULL;
}

static BOOL json_bool(const char *js, const char *key, BOOL dflt)
{
    const char *v = jpos(js, key);
    if (!v) return dflt;
    if (strncmp(v, "true", 4) == 0) return TRUE;
    if (strncmp(v, "false", 5) == 0) return FALSE;
    return dflt;
}

static int json_int(const char *js, const char *key, int dflt)
{
    const char *v = jpos(js, key);
    if (!v) return dflt;
    return atoi(v);
}

static BOOL json_str(const char *js, const char *key, wchar_t *out, int cap)
{
    const char *v = jpos(js, key);
    char tmp[512];
    int n = 0;
    if (out && cap > 0) out[0] = 0;
    if (!v || *v != '"') return FALSE;
    v++;
    while (*v && *v != '"' && n < (int)sizeof tmp - 1) {
        if (*v == '\\' && v[1]) {              /* 我们自己的字段里不会有转义，防御性处理 */
            v++;
            switch (*v) {
            case 'n': tmp[n++] = '\n'; break;
            case 't': tmp[n++] = '\t'; break;
            case 'r': tmp[n++] = '\r'; break;
            case 'u': tmp[n++] = '?'; v += 4; break;
            default:  tmp[n++] = *v; break;
            }
            v++;
            continue;
        }
        tmp[n++] = *v++;
    }
    tmp[n] = 0;
    to_w(tmp, out, cap);
    return TRUE;
}

/* JSON 字符串转义（口令里可能有引号/反斜杠，不转义就把整个 body 弄坏了） */
static void json_esc(const char *in, char *out, int cap)
{
    int n = 0;
    const char *p = in;
    if (!out || cap <= 0) return;
    while (p && *p && n < cap - 2) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) { if (n < cap - 6) n += sprintf(out + n, "\\u%04x", c); }
        else out[n++] = (char)c;
        p++;
    }
    out[n] = 0;
}

/* ---------------------------------------------------------------- 设备 */

static BOOL dev_login(const job_t *j, wchar_t *sid, int sidcap, wchar_t *err, int errcap)
{
    char u[192], p[384], body[1280], js[2048];
    char ue[384], pe[768];
    int code = 0;

    to_u8(j->devuser, u, sizeof u);
    to_u8(j->devpass, p, sizeof p);
    json_esc(u, ue, sizeof ue);
    json_esc(p, pe, sizeof pe);
    snprintf(body, sizeof body, "{\"user\":\"%s\",\"pass\":\"%s\"}", ue, pe);

    if (!http_req(j->devip, L"POST", L"/api/login", body, NULL,
                  js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code != 200 || !json_bool(js, "ok", FALSE)) {
        int e = json_int(js, "e", 0);
        /* e=-23 就是 IV_EAUTH：设备认的这个 (账号,口令) 不成立。
         * 注意"体没解析成"与"口令错"在设备侧是同一句（见 api_login），单看这条区分不了，
         * 所以话术只报事实，并给出下一步——比猜"是不是网络不通"有用。 */
        swprintf(err, (size_t)errcap,
                 L"设备登录被拒（HTTP %d, e=%d）—— 账号或口令不对。"
                 L"出厂默认 admin/88888888 只在新机器上有效，这台可能已改过口令", code, e);
        return FALSE;
    }
    if (!json_str(js, "token", sid, sidcap)) {
        swprintf(err, (size_t)errcap, L"登录成功但响应里没有 token（Web 后端太旧？）");
        return FALSE;
    }
    return TRUE;
}

static BOOL dev_status(const job_t *j, const wchar_t *sid, char *js, int cap,
                       wchar_t *err, int errcap)
{
    int code = 0;
    if (!http_req(j->devip, L"GET", L"/api/tunnel", NULL, sid,
                  js, cap, &code, err, errcap))
        return FALSE;
    if (code != 200 || !json_bool(js, "ok", FALSE)) {
        swprintf(err, (size_t)errcap, L"读状态被拒（HTTP %d）", code);
        return FALSE;
    }
    return TRUE;
}

static BOOL dev_set_token(const job_t *j, const wchar_t *sid, const wchar_t *tok,
                          char *js, int cap, wchar_t *err, int errcap)
{
    char t[256], body[512];
    int code = 0;
    to_u8(tok, t, sizeof t);
    snprintf(body, sizeof body, "{\"token\":\"%s\",\"ttl\":0,\"enable\":true}", t);
    if (!http_req(j->devip, L"POST", L"/api/tunnel/config", body, sid,
                  js, cap, &code, err, errcap))
        return FALSE;
    if (code != 200) {
        swprintf(err, (size_t)errcap, L"下发失败（HTTP %d）", code);
        return FALSE;
    }
    return TRUE;
}

static BOOL dev_switch(const job_t *j, const wchar_t *sid, BOOL on,
                       char *js, int cap, wchar_t *err, int errcap)
{
    int code = 0;
    if (!http_req(j->devip, L"POST",
                  on ? L"/api/tunnel/enable" : L"/api/tunnel/disable",
                  "{}", sid, js, cap, &code, err, errcap))
        return FALSE;
    if (code != 200) {
        swprintf(err, (size_t)errcap, L"开关失败（HTTP %d）", code);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------- 端口 */

static BOOL port_open(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    BOOL ok;
    if (s == INVALID_SOCKET) return FALSE;
    ZeroMemory(&a, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    ok = (connect(s, (struct sockaddr *)&a, sizeof a) == 0);
    closesocket(s);
    return ok;
}

/* ------------------------------------------------------------ 进程捕获 */

/* 跑一条命令行，抓 stdout+stderr；返回退出码，-1=起不来，-2=超时 */
static int run_capture(const wchar_t *cmdline, char *out, int cap, DWORD timeout_ms)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL, nul;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *cmd;
    int used = 0;
    char buf[4096];
    DWORD got = 0, t0;

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof si);
    ZeroMemory(&pi, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nul;

    cmd = wdump(cmdline);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        free(cmd);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        CloseHandle(rd);
        return -1;
    }
    free(cmd);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (out && cap > 0) out[0] = 0;
    t0 = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
                while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0) {
                    if (out && used < cap - 1) {
                        int cp = (int)got;
                        if (used + cp > cap - 1) cp = cap - 1 - used;
                        memcpy(out + used, buf, cp);
                        used += cp;
                        out[used] = 0;
                    }
                }
                break;
            }
            if (GetTickCount() - t0 > timeout_ms) {
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                CloseHandle(rd);
                return -2;
            }
            continue;
        }
        if (!ReadFile(rd, buf, sizeof buf, &got, NULL) || got == 0) break;
        if (out && used < cap - 1) {
            int cp = (int)got;
            if (used + cp > cap - 1) cp = cap - 1 - used;
            memcpy(out + used, buf, cp);
            used += cp;
            out[used] = 0;
        }
    }
    WaitForSingleObject(pi.hProcess, 500);
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        return (int)code;
    }
}

/* ---------------------------------------------------------------- 服务器 */

/* 与 ps1 完全一致：备份 → 改 auth.token/webServer.password → 重启 → 自证 7000 在听 */
static void srv_script(const wchar_t *tok, wchar_t *o, int cap)
{
    o[0] = 0;
    wcscat(o, L"set -e\n");
    wcscat(o, L"TS=$(date +%Y%m%d-%H%M%S)\n");
    wcscat(o, L"for f in /etc/frp/frps.toml /etc/frp/visitor.toml; do\n");
    wcscat(o, L"  [ -f \"$f\" ] && cp -a \"$f\" \"$f.bak-$TS\"\n");
    wcscat(o, L"done\n");
    wcscat(o, L"sed -i 's|^auth\\.token = \".*\"|auth.token = \"");
    wcscat(o, tok);
    wcscat(o, L"\"|' /etc/frp/frps.toml /etc/frp/visitor.toml\n");
    wcscat(o, L"sed -i 's|^webServer\\.password = \".*\"|webServer.password = \"");
    wcscat(o, tok);
    wcscat(o, L"\"|' /etc/frp/frps.toml\n");
    wcscat(o, L"echo \"--- after edit ---\"\n");
    wcscat(o, L"grep -h -E '^(auth\\.token|webServer\\.password)' "
              L"/etc/frp/frps.toml /etc/frp/visitor.toml\n");
    wcscat(o, L"systemctl restart frps\n");
    wcscat(o, L"systemctl restart frpv 2>/dev/null || true\n");
    wcscat(o, L"sleep 2\n");
    wcscat(o, L"echo \"--- services ---\"\n");
    wcscat(o, L"systemctl is-active frps\n");
    wcscat(o, L"systemctl is-active frpv 2>/dev/null || true\n");
    wcscat(o, L"ss -lntp 2>/dev/null | grep -E ':(7000)[[:space:]]' || "
              L"echo \"WARN: 7000 not listening\"\n");
    o[cap - 1] = 0;
}

/* plink 只在默认路径和 PATH 里找；找不到就如实报错（绝不假装改成功） */
static BOOL plink_find(wchar_t *o, int cap)
{
    wchar_t p[MAX_PATH];
    if (GetFileAttributesW(PLINK_PATH) != INVALID_FILE_ATTRIBUTES) {
        swprintf(o, (size_t)cap, L"%s", PLINK_PATH);
        return TRUE;
    }
    if (SearchPathW(NULL, L"plink.exe", NULL, MAX_PATH, p, NULL) > 0) {
        swprintf(o, (size_t)cap, L"%s", p);
        return TRUE;
    }
    return FALSE;
}

/* 写临时脚本 → plink -m 跑 → 删脚本。用 -m 而不是把脚本塞进命令行，
 * 是为了彻底躲开「引号/换行/感叹号在 Windows 命令行里怎么转义」这一摊。 */
static BOOL ssh_apply(const job_t *j, const wchar_t *tok, wchar_t *err, int errcap)
{
    wchar_t plink[MAX_PATH], script[4096], tmpdir[MAX_PATH], tmpf[MAX_PATH];
    wchar_t cmdline[1600];
    char *u8;
    int len, rc;
    static char out[32 * 1024];
    HANDLE h;
    DWORD wr = 0;
    BOOL ok = FALSE;

    if (!plink_find(plink, MAX_PATH)) {
        swprintf(err, (size_t)errcap,
                 L"没找到 plink.exe（%s），无法自动改服务器；请用「复制服务器命令」手工执行",
                 PLINK_PATH);
        return FALSE;
    }
    srv_script(tok, script, 4096);
    u8 = (char *)malloc(16384);
    if (!u8) { swprintf(err, (size_t)errcap, L"内存不足"); return FALSE; }
    to_u8(script, u8, 16384);
    len = (int)strlen(u8);

    GetTempPathW(MAX_PATH, tmpdir);
    swprintf(tmpf, MAX_PATH, L"%stunnelcfg-%lu.sh", tmpdir, GetCurrentProcessId());
    h = CreateFileW(tmpf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(u8);
        swprintf(err, (size_t)errcap, L"写不了临时脚本 %s", tmpf);
        return FALSE;
    }
    /* 写成 LF：远端 sh 不认 CR，带 \r 会报 "$'\r': command not found" */
    {
        char *lf = (char *)malloc((size_t)len + 1);
        int i, n = 0;
        if (!lf) { CloseHandle(h); free(u8); return FALSE; }
        for (i = 0; i < len; i++) if (u8[i] != '\r') lf[n++] = u8[i];
        WriteFile(h, lf, (DWORD)n, &wr, NULL);
        free(lf);
    }
    CloseHandle(h);
    free(u8);

    swprintf(cmdline, 1600,
             L"\"%s\" -ssh -batch -pw \"%s\" -hostkey \"%s\" -m \"%s\" %s@%s",
             plink, j->ecspass, ECS_HOSTKEY, tmpf, ECS_USER, j->ecshost);

    rc = run_capture(cmdline, out, sizeof out, 90000);
    DeleteFileW(tmpf);

    if (rc == -1) {
        swprintf(err, (size_t)errcap, L"plink 起不来（%s）", plink);
    } else if (rc == -2) {
        swprintf(err, (size_t)errcap, L"plink 超过 90 s 没结束，已强杀");
    } else {
        /* 逐行回显（远端是 UTF-8/ASCII 混合，全按 UTF-8 解） */
        char *p = out;
        while (p && *p) {
            char *e = strchr(p, '\n');
            wchar_t line[1600];
            if (e) *e = 0;
            {
                size_t ln = strlen(p);
                while (ln > 0 && (p[ln - 1] == '\r' || p[ln - 1] == ' ')) p[--ln] = 0;
            }
            if (*p) {
                to_w(p, line, 1600);
                plainf(L"    %s", line);
            }
            if (!e) break;
            p = e + 1;
        }
        if (rc != 0) {
            swprintf(err, (size_t)errcap,
                     L"服务器侧命令失败（rc=%d）—— 令牌没改成，服务未重启", rc);
        } else if (strstr(out, "WARN: 7000 not listening")) {
            swprintf(err, (size_t)errcap, L"frps 重启后 7000 没监听");
        } else {
            ok = TRUE;
        }
    }
    return ok;
}

/* ---------------------------------------------------------------- 动作 */

static void act_read(const job_t *j)
{
    wchar_t it[128] = L"", ih[64] = L"", ip[32] = L"", m[160];
    stepf(L"读取现状");
    ini_get(j->ini, L"token", it, 128);
    ini_get(j->ini, L"host", ih, 64);
    ini_get(j->ini, L"port", ip, 32);
    if (GetFileAttributesW(j->ini) != INVALID_FILE_ATTRIBUTES) {
        okf(L"本机 exe 配置：%s", j->ini);
        mask_tok(it, m, 160);
        plainf(L"       服务器 %s:%s   令牌 %s", ih, ip, m);
    } else {
        warnf(L"本机 exe 配置不存在：%s（exe 首次运行时才生成）", j->ini);
    }
    plainf(L"       本机 ssh 端口 %d ：%s", LOCAL_SSH,
           port_open(LOCAL_SSH) ? L"在监听（通道已通）" : L"没监听（通道未通）");
}

static void act_check(const job_t *j)
{
    wchar_t it[128] = L"", ih[64] = L"", m[160], sid[160], err[512];
    char js[8192];
    BOOL have = FALSE;
    int itlen;

    stepf(L"三处一致性检查");
    ini_get(j->ini, L"token", it, 128);
    ini_get(j->ini, L"host", ih, 64);

    if (dev_login(j, sid, 160, err, 512)) {
        if (dev_status(j, sid, js, sizeof js, err, 512)) have = TRUE;
        else badf(L"设备 %s 读不到：%s", j->devip, err);
    } else {
        badf(L"设备 %s 读不到：%s", j->devip, err);
    }

    itlen = (int)wcslen(it);
    if (itlen) {
        mask_tok(it, m, 160);
        okf(L"本机令牌：%s", m);
    } else {
        warnf(L"本机令牌：空（exe 会用内置默认值）");
    }

    if (have) {
        BOOL tset = json_bool(js, "token_set", FALSE);
        int tlen = json_int(js, "token_len", 0);
        int sport = json_int(js, "server_port", 0);
        wchar_t srv[128] = L"";
        json_str(js, "server", srv, 128);

        if (tset) okf(L"设备令牌：已设置，长度 %d", tlen);
        else warnf(L"设备令牌：未设置");
        if (tlen == itlen) okf(L"本机与设备令牌长度一致（%d）", itlen);
        else badf(L"本机与设备令牌长度不一致：本机 %d / 设备 %d ⇒ 两者不是同一个值",
                  itlen, tlen);
        if (ih[0] && srv[0] && wcscmp(ih, srv) != 0)
            badf(L"本机服务器 %s 与设备 %s 不是同一台", ih, srv);
        else if (ih[0])
            okf(L"服务器地址一致：%s:%d", ih, sport);
        if (!json_bool(js, "on", FALSE))
            warnf(L"设备通道处于关闭状态（enabled=%s）",
                  json_bool(js, "enabled", FALSE) ? L"true" : L"false");
    }

    if (port_open(LOCAL_SSH))
        okf(L"通道实测：本机 %d 在监听（frpc 已登录成功）", LOCAL_SSH);
    else
        warnf(L"通道实测：本机 %d 没监听 —— 要么 exe 没开，要么 frpc 登录失败（看 exe 日志）",
              LOCAL_SSH);

    /* 结论只写"本工具确实能看到什么"，别把条件说成结论：
     * 令牌是"只进不出"的（设备只回 token_set / token_len，值永远读不回来），
     * 所以本机这一侧**永远**证明不了"值相同"；能当证据的只有登录结果。
     * 而登录结果只有两种可见形态：本机 16000 在监听（＝本机与 frps 同值），
     * 以及设备通道打开后设备侧 frpc 能否在 frps 上线（本机看不到这一步）。 */
    plainf(L"");
    plainf(L"结论：'长度/地址一致'只是必要条件 —— 令牌只进不出，设备不把值回给任何人，"
           L"所以本机这边证明不了'值相同'。");
    plainf(L"      能当证据的只有登录结果：本机 %d 在监听 ⇒ 本机与 frps 用的是同一串；"
           L"设备侧要看它自己的 frpc 有没有在 frps 上线（本机看不到那一步）。", LOCAL_SSH);
    if (have && !json_bool(js, "on", FALSE))
        plainf(L"      设备通道现在是关着的（on=false）⇒ 设备 frpc 根本没启动，"
               L"先点「开通道」，才有设备侧的登录结果可看。");
}

static BOOL act_devtoken(const job_t *j, const wchar_t *tok)
{
    wchar_t sid[160], err[512];
    char js[8192];
    int before = g_fail;

    if (!tok_ok(tok)) {
        badf(L"令牌含非法字符或长度越界（1~63，允许 0-9 A-Z a-z - _ . ~ + / = @）");
        return FALSE;
    }
    stepf(L"把令牌下发到设备（%s）", j->devip);
    if (!dev_login(j, sid, 160, err, 512)) { badf(L"%s", err); return FALSE; }
    okf(L"设备登录成功");

    if (!dev_set_token(j, sid, tok, js, sizeof js, err, 512)) { badf(L"%s", err); return FALSE; }
    if (!json_bool(js, "ok", FALSE)) {
        badf(L"下发失败：e=%d", json_int(js, "e", 0));
        return FALSE;
    }
    okf(L"设备已接受：令牌长度 %d，通道 on=%s",
        json_int(js, "token_len", 0),
        json_bool(js, "on", FALSE) ? L"true" : L"false");
    plainf(L"       设备立刻重写 secure/tunnel.token 与 frpc.toml，monitor.sh 拉起 frpc 重连；secretKey 自动重算");

    Sleep(6000);                        /* 给 frpc 重连留时间，再复查一次 */
    if (dev_status(j, sid, js, sizeof js, err, 512) && json_bool(js, "token_set", FALSE))
        okf(L"复查：token_set=true，通道 %s",
            json_bool(js, "on", FALSE) ? L"已开启" : L"未开启");
    else
        badf(L"复查失败：设备没报 token_set");

    return g_fail == before;
}

static BOOL act_initoken(const job_t *j, const wchar_t *tok)
{
    stepf(L"写入本机 exe 配置");
    if (!tok_ok(tok)) { badf(L"令牌含非法字符或长度越界"); return FALSE; }
    if (GetFileAttributesW(j->ini) == INVALID_FILE_ATTRIBUTES) {
        badf(L"配置文件不存在：%s（先在设备上跑一次 exe，让它生成）", j->ini);
        return FALSE;
    }
    if (ini_set(j->ini, L"token", tok)) okf(L"已写入 token=（长度 %d）", (int)wcslen(tok));
    else { badf(L"这个 ini 里没有 token= 这一行，请手工检查：%s", j->ini); return FALSE; }

    plainf(L"       生效方式：关掉「运维箱远程访问.exe」再打开（程序启动时读 ini）");
    return TRUE;
}

static BOOL act_switch(const job_t *j, BOOL on)
{
    wchar_t sid[160], err[512];
    char js[4096];

    stepf(on ? L"把设备通道打开" : L"把设备通道关闭");
    if (!dev_login(j, sid, 160, err, 512)) { badf(L"%s", err); return FALSE; }
    if (!dev_switch(j, sid, on, js, sizeof js, err, 512)) { badf(L"%s", err); return FALSE; }
    if (!json_bool(js, "ok", FALSE)) {
        badf(L"失败：e=%d", json_int(js, "e", 0));
        return FALSE;
    }
    okf(L"已%s：on=%s", on ? L"开启" : L"关闭",
        json_bool(js, "on", FALSE) ? L"true" : L"false");
    return TRUE;
}

static void act_rotate(const job_t *j, const wchar_t *tok_in)
{
    wchar_t tok[128], err[512];
    int before;

    stepf(L"一键轮换令牌（顺序：服务器 → 设备 → 本机，任一步失败即停）");
    if (tok_in && tok_in[0]) swprintf(tok, 128, L"%s", tok_in);
    else {
        tok_new(tok, 128);
        plainf(L"    生成新令牌：%s", tok);
    }
    if (!tok_ok(tok)) { badf(L"令牌非法"); return; }

    plainf(L"");
    plainf(L"将依次改动：");
    plainf(L"  1) %s：/etc/frp/frps.toml(auth.token/webServer.password) + "
           L"visitor.toml(auth.token)，重启 frps/frpv", j->ecshost);
    plainf(L"  2) 设备 %s：secure/tunnel.token + frpc.toml，frpc 重连", j->devip);
    plainf(L"  3) 本机 %s：token=", j->ini);
    plainf(L"影响：该 frps 上【所有】设备都会在 1) 之后断线，直到各自执行 2)。");
    plainf(L"");

    stepf(L"1/3 服务器");
    if (!ssh_apply(j, tok, err, 512)) {
        badf(L"服务器步骤失败：%s", err);
        plainf(L"        已中止：设备和本机都没动，三处仍是旧值，服务可用");
        return;
    }
    okf(L"服务器已切换并重启，7000 在监听");

    before = g_fail;
    if (!act_devtoken(j, tok)) {
        if (g_fail > before)
            plainf(L"        设备步骤已失败：本机配置先不改，避免出现'两边不同值'");
        return;
    }
    if (!act_initoken(j, tok)) return;

    stepf(L"收尾");
    okf(L"三处已同值。现在：关掉并重开「运维箱远程访问.exe」，再点刷新。");
    plainf(L"       别的设备（同一台 frps 上的其他 SN）也要各自执行一次'下发令牌到设备'。");
}

/* worker：跑一个动作，完了通知 UI */
static DWORD WINAPI worker_proc(LPVOID p)
{
    job_t *j = (job_t *)p;
    switch (j->action) {
    case ACT_READ:   act_read(j); break;
    case ACT_CHECK:  act_check(j); break;
    case ACT_DEVTOK: act_devtoken(j, j->token); break;
    case ACT_INITOK: act_initoken(j, j->token); break;
    case ACT_ON:     act_switch(j, TRUE); break;
    case ACT_OFF:    act_switch(j, FALSE); break;
    case ACT_ROT:    act_rotate(j, j->token); break;
    default: break;
    }
    PostMessageW(g_hMain, WM_DONE, (WPARAM)g_fail, 0);
    free(j);
    return 0;
}

/* ---------------------------------------------------------------- 界面 */

static HFONT make_font(const wchar_t *name, int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, name);
}

static HWND mk_group(HWND p, const wchar_t *t, int x, int y, int w, int h)
{
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             S(x), S(y), S(w), S(h), p, NULL, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    return c;
}

/* 控件一律建在**主窗口**上，坐标加进分组框的位置 —— 不能挂在分组框下面：
 * 分组框是 STATIC 类控件，按钮被点出来的 BN_CLICKED 的 WM_COMMAND 是发给
 * **父窗口**的，而 DefWindowProc(STATIC) 直接把它丢掉 ⇒ 主窗口永远收不到点击，
 * 表现就是"点按钮没反应、日志一个字都不动"（2026-11-05 用户实测踩到）。
 * 参考实现里 WinForms 的 GroupBox 可以 Add 子控件，Win32 的 BS_GROUPBOX 不行。 */
static HWND par_off(HWND grp, int *px, int *py)
{
    RECT r;
    HWND par;
    if (!grp) return g_hMain;
    par = GetParent(grp);
    if (!par) return grp;                 /* 已经是顶层窗口，不改坐标 */
    if (!GetWindowRect(grp, &r)) return par;
    MapWindowPoints(HWND_DESKTOP, par, (LPPOINT)&r, 2);
    *px += r.left;
    *py += r.top;
    return par;
}

static HWND mk_label(HWND grp, const wchar_t *t, int x, int y, int w)
{
    int px = S(x), py = S(y);
    HWND par = par_off(grp, &px, &py);
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             px, py, S(w), S(22), par, NULL, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    return c;
}

static HWND mk_edit(HWND grp, const wchar_t *t, int x, int y, int w, BOOL pw, int id)
{
    DWORD st = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    int px = S(x), py = S(y);
    HWND par = par_off(grp, &px, &py);
    HWND c;
    if (pw) st |= ES_PASSWORD;
    c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", t, st,
                        px, py, S(w), S(23), par, (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    /* 只有口令框才换密码字符。这里曾经无条件发 EM_SETPASSWORDCHAR，
     * 结果地址/端口/设备 IP/配置文件全变成一串圆点（2026-11-05 截图发现）。 */
    if (pw) SendMessageW(c, EM_SETPASSWORDCHAR, (WPARAM)L'●', 0);
    return c;
}

static HWND mk_btn(HWND grp, const wchar_t *t, int x, int y, int w, int h, int id)
{
    int px = S(x), py = S(y);
    HWND par = par_off(grp, &px, &py);
    HWND c = CreateWindowExW(0, L"BUTTON", t,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             px, py, S(w), S(h), par, (HMENU)(INT_PTR)id, g_hi, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUi, TRUE);
    return c;
}

/* 按截图坐标摆（基准 744x600 客户区，S() 负责 DPI） */
static void build_ui(HWND h)
{
    HWND g1, g2, g3, g4;

    g1 = mk_group(h, L"① 服务器（frps）", 10, 6, 724, 92);
    mk_label(g1, L"地址", 14, 24, 40);
    g_hHost = mk_edit(g1, L"", 56, 24, 150, FALSE, ID_HOST);
    mk_label(g1, L"端口", 214, 24, 40);
    g_hPort = mk_edit(g1, L"7000", 254, 24, 60, FALSE, ID_PORT);
    mk_label(g1, L"令牌", 322, 24, 40);
    g_hTok  = mk_edit(g1, L"", 360, 24, 190, TRUE, ID_TOK);
    mk_label(g1, L"SSH 口令", 14, 54, 60);
    g_hSshPw = mk_edit(g1, L"", 78, 54, 110, TRUE, ID_SSHPW);
    mk_label(g1, L"（只用于 SSH 改 frps，不落盘）", 194, 54, 210);
    g_btn[0] = mk_btn(g1, L"生成新令牌", 560, 20, 140, 26, ID_B_GEN);
    g_btn[1] = mk_btn(g1, L"复制服务器命令", 560, 50, 140, 26, ID_B_COPY);

    g2 = mk_group(h, L"② 设备（ivsbox 箱体）", 10, 104, 724, 62);
    mk_label(g2, L"设备 IP", 14, 24, 55);
    g_hIp = mk_edit(g2, L"192.168.1.64", 72, 24, 120, FALSE, ID_IP);
    mk_label(g2, L"账号", 202, 24, 36);
    g_hUser = mk_edit(g2, L"admin", 240, 24, 70, FALSE, ID_USER);
    mk_label(g2, L"口令", 320, 24, 36);
    g_hDevPw = mk_edit(g2, L"", 358, 24, 100, TRUE, ID_DEVPW);
    mk_label(g2, L"（设备 Web 管理口令）", 466, 24, 240);

    g3 = mk_group(h, L"③ 本机（运维箱远程访问.exe 配置）", 10, 172, 724, 62);
    mk_label(g3, L"配置文件", 14, 24, 60);
    g_hIni = mk_edit(g3, L"", 78, 24, 620, FALSE, ID_INI);

    g4 = mk_group(h, L"④ 操作（都会先问一遍）", 10, 240, 724, 88);
    g_btn[2] = mk_btn(g4, L"一致性检查", 14, 20, 130, 30, ID_B_CHECK);
    g_btn[3] = mk_btn(g4, L"下发令牌到设备", 152, 20, 140, 30, ID_B_DEVTOK);
    g_btn[4] = mk_btn(g4, L"写入本机配置", 300, 20, 130, 30, ID_B_INITOK);
    g_btn[5] = mk_btn(g4, L"开通道", 438, 20, 80, 30, ID_B_ON);
    g_btn[6] = mk_btn(g4, L"关通道", 526, 20, 80, 30, ID_B_OFF);
    g_btn[7] = mk_btn(g4, L"一键轮换（三处同改）", 14, 52, 278, 30, ID_B_ROT);
    g_btn[8] = mk_btn(g4, L"保存日志", 300, 52, 130, 30, ID_B_SAVE);
    g_btn[9] = mk_btn(g4, L"清空", 438, 52, 80, 30, ID_B_CLR);

    g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                             ES_READONLY | ES_AUTOVSCROLL,
                             S(10), S(336), S(724), S(252), h,
                             (HMENU)(INT_PTR)ID_LOG, g_hi, NULL);
    SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    SendMessageW(g_hLog, EM_SETLIMITTEXT, (WPARAM)1024 * 512, 0);
}

static void banner(void)
{
    wchar_t ini[MAX_PATH];
    ini_default(ini, MAX_PATH);
    plainf(L"%s %s", APP_TITLE, APP_VER);
    plainf(L"本机配置：%s", ini);
    plainf(L"");
    plainf(L"用法（第一次就看这几行）：");
    plainf(L"  1) 先点「一致性检查」—— 只读，不改任何东西，回答'三处是不是同一串令牌'。");
    plainf(L"  2) 要换令牌：点「生成新令牌」→ 按提示点「下发令牌到设备」，再点「写入本机配置」。");
    plainf(L"     想让服务器一起换：在「SSH 口令」里填服务器口令，点「一键轮换（三处同改）」，");
    plainf(L"     它按 服务器 → 设备 → 本机 的顺序改，任一步失败会停下并说清现在是什么状态。");
    plainf(L"  3) 改完关掉并重开「运维箱远程访问.exe」，点「刷新」。");
    plainf(L"");
    plainf(L"说明：任何一步改的都是【真实生效】的文件/接口，没有'预演'，所以每个按钮都会先问你一遍。");
    plainf(L"");
}

static void get_text(HWND e, wchar_t *o, int cap)
{
    if (o && cap > 0) {
        GetWindowTextW(e, o, cap);
        o[cap - 1] = 0;
        trim(o);
    }
}

/* 口令框不清尾空格：口令里若真有空格，trim 掉就等于"帮你改了个错口令"。
 * 其余字段（IP/账号/路径/令牌）照旧去掉首尾空白 —— 手输常常带空格。 */
static void get_text_raw(HWND e, wchar_t *o, int cap)
{
    if (o && cap > 0) {
        GetWindowTextW(e, o, cap);
        o[cap - 1] = 0;
    }
}

static job_t *job_from_ui(int action)
{
    job_t *j = (job_t *)calloc(1, sizeof(job_t));
    wchar_t tmp[64];
    if (!j) return NULL;
    j->action = action;
    get_text(g_hIp, j->devip, 64);
    get_text(g_hUser, j->devuser, 64);
    get_text_raw(g_hDevPw, j->devpass, 128);
    get_text(g_hIni, j->ini, MAX_PATH);
    get_text(g_hHost, j->ecshost, 128);
    get_text_raw(g_hSshPw, j->ecspass, 128);
    get_text(g_hTok, j->token, 128);
    get_text(g_hPort, tmp, 64);
    return j;
}

static void set_buttons(BOOL en)
{
    int i;
    for (i = 0; i < NBUTTON; i++)
        if (g_btn[i]) EnableWindow(g_btn[i], en);
    EnableWindow(g_hIp, en);
    EnableWindow(g_hUser, en);
    EnableWindow(g_hDevPw, en);
    EnableWindow(g_hIni, en);
    EnableWindow(g_hHost, en);
    EnableWindow(g_hSshPw, en);
    EnableWindow(g_hTok, en);
    g_busy = !en;
}

static void start_job(job_t *j)
{
    HANDLE th;
    if (!j) return;
    set_buttons(FALSE);
    th = CreateThread(NULL, 0, worker_proc, j, 0, NULL);
    if (!th) { free(j); set_buttons(TRUE); }
    else CloseHandle(th);
}

static BOOL ask(const wchar_t *fmt, ...)
{
    wchar_t buf[1200];
    va_list ap;
    va_start(ap, fmt);
    if (vswprintf(buf, 1200, fmt, ap) < 0) buf[1199] = 0;
    va_end(ap);
    return MessageBoxW(g_hMain, buf, L"确认",
                       MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

static void copy_clip(const wchar_t *s)
{
    size_t n;
    HGLOBAL g;
    if (!OpenClipboard(g_hMain)) return;
    EmptyClipboard();
    n = (wcslen(s) + 1) * sizeof(wchar_t);
    g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) {
        void *p = GlobalLock(g);
        if (p) { memcpy(p, s, n); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); }
        else GlobalFree(g);
    }
    CloseClipboard();
}

/* 剪贴板里那条：bash -c '<脚本>'，内部单引号按 shell 规矩转义（' -> '\''） 。
 * 单独抽出来是为了 CLI 也能导同一串，好在本机 bash -n 验语法。 */
static void build_srv_cmdline(const wchar_t *tok, wchar_t *out, int cap)
{
    wchar_t script[4096];
    int i, n;
    srv_script(tok, script, 4096);
    n = swprintf(out, (size_t)cap, L"bash -c '");
    if (n < 0) { out[0] = 0; return; }
    for (i = 0; script[i] && n < cap - 8; i++) {
        if (script[i] == L'\'') {
            memcpy(out + n, L"'\\''", 4 * sizeof(wchar_t));
            n += 4;
        } else {
            out[n++] = script[i];
        }
    }
    out[n] = 0;
    wcscat(out, L"'");
}

/* 复制到剪贴板的那条命令：bash -c '<脚本>'，内部单引号按 shell 规矩转义 */
static void copy_server_cmd(void)
{
    wchar_t tok[128];
    static wchar_t line[64 * 1024];
    get_text(g_hTok, tok, 128);
    if (!tok_ok(tok)) {
        MessageBoxW(g_hMain, L"令牌为空或含非法字符，先生成/填一个再复制。",
                    APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    build_srv_cmdline(tok, line, 64 * 1024);
    copy_clip(line);
    plainf(L"已复制到剪贴板：到服务器上以 root 粘贴执行即可（没装 plink 时用这条）");
}

static void save_log(void)
{
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW o;
    wchar_t ts[32];
    SYSTEMTIME st;
    int len;
    wchar_t *buf;
    FILE *fp;

    GetLocalTime(&st);
    swprintf(ts, 32, L"通道配置-%04d%02d%02d-%02d%02d%02d.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    swprintf(path, MAX_PATH, L"%s", ts);

    ZeroMemory(&o, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_hMain;
    o.lpstrFilter = L"文本 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0";
    o.lpstrFile = path;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"txt";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&o)) return;

    len = GetWindowTextLengthW(g_hLog);
    buf = (wchar_t *)malloc(((size_t)len + 2) * sizeof(wchar_t));
    if (!buf) return;
    GetWindowTextW(g_hLog, buf, len + 1);

    fp = _wfopen(path, L"wb");
    if (!fp) { free(buf); return; }
    fwrite("\xEF\xBB\xBF", 1, 3, fp);       /* UTF-8 BOM：记事本/Excel 才不乱码 */
    {
        char u8[4096];
        wchar_t *p = buf, *q;
        do {
            q = wcschr(p, L'\r');
            if (q) *q = 0;
            to_u8(p, u8, sizeof u8);
            fwrite(u8, 1, strlen(u8), fp);
            fwrite("\r\n", 1, 2, fp);
            if (!q) break;
            p = q + 1;
            if (*p == L'\n') p++;
        } while (*p);
    }
    fclose(fp);
    free(buf);
    plainf(L"日志已保存：%s", path);
}

/* ------------------------------------------------------------ 截图自检 */

/* 把一段宽文本按 UTF-8（带 BOM）落盘。--shot 用它把日志框内容也存一份，
 * 这样"点按钮到底有没有反应"可以直接看文本，不必靠人读截图。 */
static BOOL dump_text(const wchar_t *path, const wchar_t *s)
{
    FILE *f = _wfopen(path, L"wb");
    if (!f) return FALSE;
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    while (s && *s) {
        wchar_t chunk[1024];
        char u8[4096];
        int n = 0;
        while (*s && n < 1023) chunk[n++] = *s++;
        chunk[n] = 0;
        to_u8(chunk, u8, sizeof u8);
        fwrite(u8, 1, strlen(u8), f);
    }
    fclose(f);
    return TRUE;
}

/* 只在 --shot 里用：PrintWindow 抓整个窗口（含标题栏），写成 24 位 BMP。
 * 用 BMP 是为了不引 GDI+/WIC —— 验证布局不需要花哨格式。 */
static BOOL save_window_bmp(HWND h, const wchar_t *path)
{
    RECT r;
    int w, ht, stride, y;
    HDC hdc, mem;
    HBITMAP bmp;
    HGDIOBJ old;
    BITMAPINFO bi;
    unsigned char *bits;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    HANDLE f;
    DWORD wr = 0;
    BOOL ok = FALSE;

    if (!GetWindowRect(h, &r)) return FALSE;
    w = r.right - r.left;
    ht = r.bottom - r.top;
    if (w <= 0 || ht <= 0) return FALSE;

    hdc = GetWindowDC(h);
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, w, ht);
    old = SelectObject(mem, bmp);
    PrintWindow(h, mem, 0);

    stride = ((w * 3 + 3) & ~3);
    bits = (unsigned char *)calloc(1, (size_t)stride * ht);
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = ht;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    if (bits && GetDIBits(hdc, bmp, 0, (UINT)ht, bits, &bi, DIB_RGB_COLORS)) {
        ZeroMemory(&ih, sizeof ih);
        ih.biSize = sizeof ih;
        ih.biWidth = w;
        ih.biHeight = ht;
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = (DWORD)stride * ht;

        ZeroMemory(&fh, sizeof fh);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof ih;
        fh.bfSize = fh.bfOffBits + ih.biSizeImage;

        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ok = WriteFile(f, &fh, sizeof fh, &wr, NULL) &&
                 WriteFile(f, &ih, sizeof ih, &wr, NULL) &&
                 WriteFile(f, bits, ih.biSizeImage, &wr, NULL);
            CloseHandle(f);
        }
    }
    (void)y;
    if (bits) free(bits);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(h, hdc);
    return ok;
}

/* ---------------------------------------------------------------- 窗口 */

/* 抽消息抽 ms 毫秒（--shot 自检用；正常运行时走下面的 GetMessage 循环） */
static void pump_ms(MSG *m, DWORD ms)
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        while (PeekMessageW(m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(m);
            DispatchMessageW(m);
        }
        Sleep(20);
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)w;
    switch (m) {
    case WM_CREATE:
        build_ui(h);
        return 0;

    case WM_LOG: {
        wchar_t *p = (wchar_t *)l;
        log_line(p);
        free(p);
        return 0;
    }

    case WM_DONE:
        set_buttons(TRUE);
        InterlockedExchange(&g_done, 1);
        /* --shot 自检时不要弹模态框，否则消息泵被 MessageBox 卡住，截不到图 */
        if ((int)w > 0 && !g_shot[0])
            MessageBoxW(h, L"有失败项，看下方输出。", L"完成", MB_OK | MB_ICONWARNING);
        return 0;

    case WM_CTLCOLORSTATIC:
        if ((HWND)l == g_hLog) {
            SetBkColor((HDC)w, RGB(250, 250, 250));
            return (LRESULT)g_brLog;
        }
        break;

    case WM_COMMAND: {
        int id = LOWORD(w);
        if (HIWORD(w) != BN_CLICKED) break;
        switch (id) {
        case ID_B_GEN: {
            wchar_t t[128];
            tok_new(t, 128);
            SetWindowTextW(g_hTok, t);
            SendMessageW(g_hTok, EM_SETPASSWORDCHAR, 0, 0);  /* 明文，方便抄到别处 */
            InvalidateRect(g_hTok, NULL, TRUE);
            plainf(L"已生成新令牌（明文显示，方便抄到别处）：%s", t);
            return 0;
        }
        case ID_B_COPY:
            copy_server_cmd();
            return 0;
        case ID_B_CHECK: {
            job_t *j = job_from_ui(ACT_CHECK);
            start_job(j);
            return 0;
        }
        case ID_B_DEVTOK: {
            job_t *j = job_from_ui(ACT_DEVTOK);
            if (!j) return 0;
            if (!j->token[0]) {
                MessageBoxW(h, L"令牌是空的，先点「生成新令牌」。", APP_TITLE,
                            MB_OK | MB_ICONWARNING);
                free(j);
                return 0;
            }
            if (!ask(L"把令牌下发给设备 %s？\r\n\r\n设备会重写 secure/tunnel.token 与 "
                     L"frpc.toml，\r\nfrpc 立刻重连（几秒内通道会闪断一次）。", j->devip)) {
                free(j);
                return 0;
            }
            start_job(j);
            return 0;
        }
        case ID_B_INITOK: {
            job_t *j = job_from_ui(ACT_INITOK);
            if (!j) return 0;
            if (!j->token[0]) {
                MessageBoxW(h, L"令牌是空的，先点「生成新令牌」。", APP_TITLE,
                            MB_OK | MB_ICONWARNING);
                free(j);
                return 0;
            }
            if (!ask(L"把令牌写进本机配置？\r\n\r\n%s\r\n\r\n（要重开 exe 才生效）", j->ini)) {
                free(j);
                return 0;
            }
            start_job(j);
            return 0;
        }
        case ID_B_ON:
        case ID_B_OFF: {
            BOOL on = (id == ID_B_ON);
            job_t *j = job_from_ui(on ? ACT_ON : ACT_OFF);
            if (!j) return 0;
            if (!ask(on ? L"把设备 %s 的远程通道打开？\r\n\r\n（等于对外开一个可 SSH 的入口）"
                        : L"把设备 %s 的远程通道关掉？", j->devip)) {
                free(j);
                return 0;
            }
            start_job(j);
            return 0;
        }
        case ID_B_ROT: {
            job_t *j = job_from_ui(ACT_ROT);
            if (!j) return 0;
            if (!j->ecspass[0]) {
                MessageBoxW(h, L"「SSH 口令」是空的 —— 一键轮换必须先改服务器。\r\n"
                               L"不想动服务器就用「下发令牌到设备」+「写入本机配置」。",
                            APP_TITLE, MB_OK | MB_ICONWARNING);
                free(j);
                return 0;
            }
            if (!ask(L"一键轮换会用同一串令牌依次改动：\r\n\r\n"
                     L"  1) 服务器 %s（需 SSH 口令，会重启 frps/frpv）\r\n"
                     L"  2) 设备 %s\r\n  3) 本机 ini\r\n\r\n"
                     L"注意：该 frps 上【所有】设备会先断线，直到各自下发新令牌。\r\n\r\n继续？",
                     j->ecshost, j->devip)) {
                free(j);
                return 0;
            }
            start_job(j);
            return 0;
        }
        case ID_B_SAVE:
            save_log();
            return 0;
        case ID_B_CLR:
            SetWindowTextW(g_hLog, L"");
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_CLOSE:
        if (g_busy &&
            MessageBoxW(h, L"还有动作在跑，现在关掉会中断它（服务器/设备可能改到一半）。\r\n\r\n仍然关闭？",
                        APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return 0;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------------------------------------------------------- CLI */

/* 只给自动化/自检用：--cli <动作> ... --log <文件>，中文日志落盘（不往控制台打，
 * 免得 GBK 控制台把 UTF-8 变乱码）。退出码 0=全成功，1=有 [X]。 */
static int run_cli(int argc, wchar_t **argv)
{
    job_t j;
    const wchar_t *act = NULL;
    const wchar_t *logp = NULL;
    const wchar_t *outp = NULL;
    int i, rc;

    ZeroMemory(&j, sizeof j);
    swprintf(j.devuser, 64, L"%s", L"admin");
    ini_default(j.ini, MAX_PATH);

    for (i = 0; i < argc; i++) {
        const wchar_t *a = argv[i];
        const wchar_t *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (wcscmp(a, L"--cli") == 0 && v) { act = v; i++; }
        else if (wcscmp(a, L"--ip") == 0 && v) { swprintf(j.devip, 64, L"%s", v); i++; }
        else if (wcscmp(a, L"--user") == 0 && v) { swprintf(j.devuser, 64, L"%s", v); i++; }
        else if (wcscmp(a, L"--pass") == 0 && v) { swprintf(j.devpass, 128, L"%s", v); i++; }
        else if (wcscmp(a, L"--ini") == 0 && v) { swprintf(j.ini, MAX_PATH, L"%s", v); i++; }
        else if (wcscmp(a, L"--token") == 0 && v) { swprintf(j.token, 128, L"%s", v); i++; }
        else if (wcscmp(a, L"--server") == 0 && v) { swprintf(j.ecshost, 128, L"%s", v); i++; }
        else if (wcscmp(a, L"--ssh-pass") == 0 && v) { swprintf(j.ecspass, 128, L"%s", v); i++; }
        else if (wcscmp(a, L"--log") == 0 && v) { logp = v; i++; }
        else if (wcscmp(a, L"--out") == 0 && v) { outp = v; i++; }
    }
    if (!act) {
        fputs("usage: tunnelcfg.exe --cli <read|check|device-token|ini-token|on|off|rotate> "
              "[--ip x] [--user u] [--pass p] [--ini path] [--token t] [--server h] "
              "[--ssh-pass p] [--log file]\n", stdout);
        return 2;
    }
    if (logp) {
        g_cliLog = _wfopen(logp, L"wb");
        if (g_cliLog) fwrite("\xEF\xBB\xBF", 1, 3, g_cliLog);
    }
    g_cli = TRUE;

    if (wcscmp(act, L"read") == 0) act_read(&j);
    else if (wcscmp(act, L"check") == 0) act_check(&j);
    else if (wcscmp(act, L"device-token") == 0) act_devtoken(&j, j.token);
    else if (wcscmp(act, L"ini-token") == 0) act_initoken(&j, j.token);
    else if (wcscmp(act, L"on") == 0) act_switch(&j, TRUE);
    else if (wcscmp(act, L"off") == 0) act_switch(&j, FALSE);
    else if (wcscmp(act, L"rotate") == 0) act_rotate(&j, j.token);
    else if (wcscmp(act, L"show-script") == 0) {
        /* 只导出、不连服务器：--out 拿到远端 bash 脚本（可 bash -n 验），
         * 日志里给出「复制服务器命令」那条 bash -c '...' 的原文 */
        wchar_t raw[4096];
        static wchar_t cmd[64 * 1024];
        if (!tok_ok(j.token)) badf(L"令牌含非法字符或长度越界");
        else {
            srv_script(j.token, raw, 4096);
            plainf(L"--- 远端脚本（%d 行）---", 0);
            {
                int ln = 0;
                const wchar_t *p = raw;
                while (*p) {
                    const wchar_t *e = wcschr(p, L'\n');
                    size_t n = e ? (size_t)(e - p) : wcslen(p);
                    wchar_t one[1024];
                    if (n > 1023) n = 1023;
                    memcpy(one, p, n * sizeof(wchar_t));
                    one[n] = 0;
                    plainf(L"%s", one);
                    ln++;
                    if (!e) break;
                    p = e + 1;
                }
                plainf(L"--- 共 %d 行 ---", ln);
            }
            build_srv_cmdline(j.token, cmd, 64 * 1024);
            plainf(L"--- 剪贴板那条 ---");
            plainf(L"%s", cmd);
            if (outp) {
                char u8[8192];
                FILE *f;
                to_u8(raw, u8, sizeof u8);
                f = _wfopen(outp, L"wb");
                if (f) {
                    char *p2 = u8;
                    char *q2;
                    while ((q2 = strchr(p2, '\r')) != NULL) { *q2 = 0; fputs(p2, f); fputs("\n", f); p2 = q2 + 1; }
                    fputs(p2, f);
                    fclose(f);
                    okf(L"脚本已写到 %s", outp);
                } else {
                    badf(L"写不了 %s", outp);
                }
            }
        }
    }
    else {
        fputs("unknown action\n", stdout);
        return 2;
    }
    if (g_cliLog) { fclose(g_cliLog); g_cliLog = NULL; }
    rc = (g_fail > 0) ? 1 : 0;
    printf("RESULT: %s (%d fail)\n", rc ? "FAIL" : "OK", g_fail);
    return rc;
}

/* ---------------------------------------------------------------- 入口 */

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    HWND h;
    RECT rc;
    int argc = 0;
    wchar_t **argv;
    WSADATA wsd;
    HDC sdc;
    DWORD wstyle;

    (void)hp;
    (void)cmd;
    g_hi = hi;

    sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (g_dpi <= 0) g_dpi = 96;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* --cli：不开窗口，跑完退出 */
    if (argc > 1 && wcscmp(argv[1], L"--cli") == 0) {
        int rc;
        WSAStartup(MAKEWORD(2, 2), &wsd);
        rc = run_cli(argc, argv);
        WSACleanup();
        LocalFree(argv);
        return rc;
    }
    if (argc > 2 && wcscmp(argv[1], L"--shot") == 0) {
        swprintf(g_shot, MAX_PATH, L"%s", argv[2]);
        if (argc > 3 && wcscmp(argv[3], L"check") == 0) g_shotCheck = TRUE;
    }

    WSAStartup(MAKEWORD(2, 2), &wsd);
    SetProcessDPIAware();

    g_fontUi = make_font(L"Microsoft YaHei UI", 9, FW_NORMAL);
    g_fontMono = make_font(L"Consolas", 9, FW_NORMAL);
    g_brLog = CreateSolidBrush(RGB(250, 250, 250));

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ivsboxTunnelCfg";
    RegisterClassExW(&wc);

    /* 不可缩放：客户区恒为 744x600（按 DPI 缩放），布局就不会被拉花 */
    wstyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    rc.left = 0; rc.top = 0; rc.right = S(744); rc.bottom = S(600);
    AdjustWindowRectEx(&rc, wstyle, FALSE, WS_EX_APPWINDOW);
    h = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                        L"运维箱通道配置 1.1  ——  设备 / 本机 / 服务器 三处一处改",
                        wstyle, CW_USEDEFAULT, CW_USEDEFAULT,
                        rc.right - rc.left, rc.bottom - rc.top,
                        NULL, NULL, hi, NULL);
    if (!h) return 1;
    g_hMain = h;

    {
        wchar_t ini[MAX_PATH];
        wchar_t t[160];
        ini_default(ini, MAX_PATH);
        SetWindowTextW(g_hIni, ini);
        if (ini_get(ini, L"host", t, 160)) SetWindowTextW(g_hHost, t);
        else SetWindowTextW(g_hHost, ECS_HOST_DEF);
        if (ini_get(ini, L"port", t, 160)) SetWindowTextW(g_hPort, t);
        if (ini_get(ini, L"token", t, 160)) SetWindowTextW(g_hTok, t);
    }

    ShowWindow(h, show);
    UpdateWindow(h);
    banner();
    SetFocus(g_hIp);

    if (g_shot[0]) {
        /* 自检：先画完截一张；带 check 参数时再自动点一次「一致性检查」，
         * 验证 job_from_ui -> worker 线程 -> PostMessage(WM_LOG) -> WM_DONE
         * 这条链路真的通（手点没法自动化，但这条正是最容易写错的地方）。 */
        DWORD t0 = GetTickCount();
        pump_ms(&msg, 900);
        if (g_shotCheck) {
            /* 必须点**真按钮**（BM_CLICK → 按钮给父窗口发 BN_CLICKED）。
             * 早先是直接 PostMessage(WM_COMMAND) 到主窗口，那是假通过：
             * 它绕过了"按钮的父窗口到底是谁"这一环，正好把
             * "点按钮没反应"的 bug 藏住了（2026-11-05 用户实测踩到）。 */
            HWND b = GetDlgItem(h, ID_B_CHECK);
            if (b) SendMessageW(b, BM_CLICK, 0, 0);
            else plainf(L"[X] 自检：GetDlgItem 找不到「一致性检查」按钮");
            while (b && !g_done && GetTickCount() - t0 < 45000) pump_ms(&msg, 100);
            pump_ms(&msg, 400);
        }
        save_window_bmp(h, g_shot);
        {
            /* 日志框内容另存成同名 .txt：截图只能"看"，文本能"验" */
            wchar_t txt[MAX_PATH + 8];
            int n = GetWindowTextLengthW(g_hLog);
            wchar_t *buf = (wchar_t *)malloc(((size_t)n + 2) * sizeof(wchar_t));
            if (buf) {
                GetWindowTextW(g_hLog, buf, n + 1);
                swprintf(txt, MAX_PATH + 8, L"%s.txt", g_shot);
                dump_text(txt, buf);
                free(buf);
            }
        }
        DestroyWindow(h);
        LocalFree(argv);
        WSACleanup();
        return 0;
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    WSACleanup();
    return 0;
}
