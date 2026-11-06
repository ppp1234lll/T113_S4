/*
 * 运维箱助手（单文件 exe）—— 配置 · 令牌 · 远程连接 合到一个窗口
 * ---------------------------------------------------------------------------
 * 把原来两个工具（「运维箱通道配置」＋「运维箱远程访问」）并成一个，并把
 * 「找设备 / 登录 / 下发令牌 / 开通道 / 建隧道 / 打开画面」串成一次点击。
 *
 * 用户只填两样：**设备 SN** 和**设备 Web 口令**。其余全部自动：
 *   · 服务器地址/端口/令牌：内嵌出厂值（可用「服务器设置」改）
 *   · 设备 IP：本机网段自动扫（HTTP 指纹 ＋ 登录后核对 /api/tunnel 的 sn），
 *     找不到但没有直连条件时退回"走已有隧道 127.0.0.1:18080"
 *   · 摄像机列表 / 连通性 / 隧道端口 / SD 卡路径：从设备读，不让人填
 *
 * 令牌语义（写在这里防犯浑）：`auth.token` 是 **frps 的**，设备侧 frpc 与
 * 本机 visitor 都必须等于它。所以"生成新令牌"必须**三处同改**，只改设备或只改
 * 本机会让通道立刻失效 —— 界面上的三个按钮按这个事实设计，不是三个等价选项。
 *
 * 远程连接那半沿用「运维箱远程访问」的成熟路径（stcp 访客 → 设备 22 →
 * ssh -L 转发到摄像机/设备 Web），所以仍需内嵌 frpc 与一把专用密钥；
 * 下载与摄像机探测走 ssh，配置与令牌走设备 Web（WinHTTP）。
 *
 * 编译：见 build.sh。字符一律走 W API，源码 UTF-8。
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <aclapi.h>
#include <shlobj.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define APP_TITLE   L"运维箱助手"
#define APP_VER     L"1.0.0"
/* 内嵌配置的版本戳。remote.ini 里 [meta] cfg 与它不一致（含"根本没有"）就
 * 认定是旧版留下的过期配置，用编译进去的出厂值整体覆盖一次。这样换令牌、
 * 换默认 SN 之后，现场那台机器上残留的旧 remote.ini 不会再压住新 exe
 * ——「打开就能用、不用先删文件/先点服务器设置」。
 * **改令牌或改默认 SN 时必须同时改这一行**，否则新 exe 仍会沿用旧 ini。 */
#define CFG_STAMP   L"2026-11-06.1"
#define FRP_SERVER  L"root@127.0.0.1"
#define FRP_PORT    16000
#define WEB_PORT    18080
#define MAX_CAMS    8

/* 资源 ID（res.rc 里同名） */
#define RES_FRPC    100
#define RES_KEY     102

/* 控件 ID */
#define ID_LIST     1001
#define ID_BTN_CONN 1002
#define ID_BTN_DISC 1003
#define ID_BTN_WEB  1004
#define ID_BTN_DL   1005
#define ID_BTN_REF  1006
#define ID_LOG      1007
#define ID_STATUS   1008

#define ID_DL_PATH   2001
#define ID_DL_GO     2002
#define ID_DL_LIST   2003
#define ID_DL_GET    2004
#define ID_DL_INFO   2005
#define ID_DL_UP     2006
#define ID_DL_BROWSE 2007
#define ID_DL_SAVE   2008
#define ID_DL_OPEN   2009
#define ID_DL_PROG   2010
#define ID_DL_SELALL 2011
#define ID_DL_SAVE_LBL 2012
#define ID_DL_TIP      2013

/* 设备（SN）与口令 —— 用户唯一要填的两项 */
#define ID_LBL_SN    1101
#define ID_CMB_SN    1102
#define ID_BTN_LINK  1103       /* 「连接设备」＝自动找 IP＋登录＋读通道状态 */
#define ID_BTN_SET   1104
#define ID_LBL_SRV   1105
#define ID_LBL_PW    1106
#define ID_EDT_PW    1107       /* 设备 Web 口令（type=password） */
#define ID_LBL_DEV   1108       /* 定位到的 IP / 通道状态文案 */
#define ID_BTN_ON    1110       /* 开通道 */
#define ID_BTN_OFF   1111       /* 关通道 */
#define ID_BTN_TOK   1112       /* 生成令牌… */
#define ID_LBL_TUN   1113       /* 通道/令牌/服务器 一行摘要 */

/* 服务器设置对话框 */
#define ID_SET_HOST  1201
#define ID_SET_PORT  1202
#define ID_SET_TOK   1203
#define ID_SET_OK    1204
#define ID_SET_CANCEL 1205
#define ID_SET_HINT  1206

/* 令牌对话框 */
#define ID_TK_EDT    1301
#define ID_TK_GEN    1302
#define ID_TK_LOCAL  1303
#define ID_TK_DEV    1304
#define ID_TK_ALL    1305
#define ID_TK_CLOSE  1306
#define ID_TK_HINT   1307
#define ID_TK_SRVPW  1308
#define ID_TK_SRVLBL 1309
#define ID_TK_COPY   1310

/* 通用"输入一行文本"对话框（出厂口令改密用） */
#define ID_ASK_EDIT  1401
#define ID_ASK_OK    1402
#define ID_ASK_CANCEL 1403
#define ID_ASK_HINT  1404

/* 自定义消息 */
#define WM_CAMS_READY  (WM_APP + 1)
#define WM_CAM_STATE   (WM_APP + 2)
#define WM_BUSY        (WM_APP + 3)
#define WM_DL_PROGRESS (WM_APP + 4)
#define WM_DL_DONE     (WM_APP + 5)
#define WM_DL_LISTED   (WM_APP + 6)
#define WM_LINK_STATE  (WM_APP + 7)   /* 设备区状态变了，刷摘要与状态行 */
#define WM_NEED_PASS   (WM_APP + 8)   /* 设备还在出厂口令：让用户设新口令 */

typedef struct {
    int  idx;
    char ip[40];
    char vendor[16];
    char stream[16];
    int  state;          /* -1 未知 0 OK 1 ping不通 2 端口不通 */
} Cam;

static Cam   g_cams[MAX_CAMS];
static int   g_ncams;
static CRITICAL_SECTION g_cs;

static wchar_t g_dir[MAX_PATH];     /* %LOCALAPPDATA%\ivsbox-assist */
static wchar_t g_frpc[MAX_PATH];
static wchar_t g_toml[MAX_PATH];
static wchar_t g_key[MAX_PATH];
static wchar_t g_ssh[MAX_PATH];
static wchar_t g_frpcLog[MAX_PATH];

static HWND  g_hMain, g_hList, g_hLog, g_hStatus;
static HWND  g_hBtnConn, g_hBtnDisc, g_hBtnWeb, g_hBtnDl, g_hBtnRef;
static HFONT g_font;

static HANDLE g_frpcProc, g_sshProc;
static DWORD  g_frpcPid, g_sshPid;
static int    g_curCh = -1;         /* 当前已建隧道的通道；-1 无 */
static int    g_selCh = -1;
static BOOL   g_busy = FALSE;
static BOOL   g_quiet = FALSE;      /* 自检模式：不弹窗、不刷界面 */

static BOOL   g_webOpen = FALSE;    /* 当前隧道是"设备 Web"而不是摄像机 */

/* --------------------------------------------------- 设备侧（Web）状态
 * 「连接设备」做的事：定位 IP → 登录 → 读 /api/tunnel。
 * 这三样都是"设备现在怎么样"的快照，界面上的摘要行直接照它渲染。 */
static wchar_t g_devPass[64]  = L"";      /* 设备 Web 口令（存盘，下次免填） */
static wchar_t g_devUser[32]  = L"admin"; /* 账号固定 admin，不做输入框 */
static wchar_t g_devIp[64]    = L"";      /* 定位到的 IP（可写 host:port，排障用） */
static BOOL    g_devUp        = FALSE;    /* 已登录且 SN 对得上 */
static BOOL    g_tunOn        = FALSE;    /* 设备侧通道开关 */
static int     g_tunTokLen    = 0;        /* 设备侧令牌长度（设备不回明文） */
static BOOL    g_tunTokSet    = FALSE;
static wchar_t g_tunSrv[128]  = L"";      /* 设备上报的服务器地址 */
static int     g_tunSrvPort   = 0;

/* ------------------------------------------------------ 设备 / 服务器参数
 * 1.1.0 起这三项不再写死在 payload 的 visitor.toml 里，而是运行时按
 *   secretKey = hex(MD5(SN + 令牌))[0..8) 现算、现写 visitor.toml。
 * 出厂默认值取自本工具一直面向的那台服务器；用户改过的值存
 * %LOCALAPPDATA%\ivsbox-assist\assist.ini，下次启动沿用。
 */
#define MAX_SNS   24

static wchar_t g_ini[MAX_PATH];         /* assist.ini */
static wchar_t g_exeDir[MAX_PATH];      /* exe 所在目录（读 sns.txt 用） */
static wchar_t g_srvHost[128] = L"47.95.22.96";
static int     g_srvPort      = 7000;
/* 出厂令牌：**必须与 frps 当前的值一致**，否则新机器第一次打开就用旧令牌登不上，
 * 现场表现为"frpc 起来了又没了 / 通道永远未连接"。2026-11-06 09:26 同步为
 * frps 2026-11-05 16:57 轮换后的值（旧值 6a8fdb6b…c3 已作废）。
 * 令牌再轮换时**这里也要跟着改并重新打包**，否则新机器又要手工配一次。 */
static wchar_t g_token[128]   = L"e0cb7a015d5fe75b7e8fa2340388e714";
static wchar_t g_sn[64]       = L"0A19B4";
static wchar_t g_sns[MAX_SNS][64];
static int     g_nsns;
static HWND    g_hCmbSn, g_hBtnLink, g_hBtnSet, g_hLblSn, g_hLblSrv;
static HWND    g_hEdtPw, g_hLblPw, g_hLblDev, g_hLblTun, g_hBtnOn, g_hBtnOff, g_hBtnTok;
static int     g_frpcGen;               /* 当前 visitor.toml 的代数 */
static int     g_frpcProcGen = -1;      /* 正在跑的 frpc 吃的是哪一代 */
/* 状态行末尾的提示（读不到设备配置时用）。update_status_line() 每拍会重写
 * 整行，所以提示必须存在这里让它带上，而不是直接 SetWindowTextW —— 那样
 * 会被随后的 WM_BUSY 覆盖掉。 */
static wchar_t g_note[192];

/* 截图模式（--shot）：定义在这里是因为 WM_NEED_PASS 要先判断"是否自动化"。 */
static wchar_t g_shotPath[MAX_PATH];
static int     g_shotDelay = 20000;
/* 自动化（--shot / 自检）用：设备还在出厂口令时，命令行直接给新口令，
 * 不弹模态框 —— 否则截图永远等不到窗口收尾。 */
static wchar_t g_cliNewPass[64];

/* 令牌对话框用：待下发的令牌、服务器 SSH 口令（只在「连服务器一起改」时用） */
static wchar_t g_tokNew[128];
static wchar_t g_srvPw[64];

/* 窄串 -> 宽串（MinGW 的宽 printf 对 %S 语义不可靠，一律显式转） */
static wchar_t *W(const char *s)
{
    static wchar_t bufs[4][320];
    static int n;
    wchar_t *b = bufs[n = (n + 1) & 3];
    if (!s) s = "";
    mbstowcs(b, s, 319);
    b[319] = 0;
    return b;
}

static wchar_t g_dlPath[512] = L"/mnt/sdcard/video";
static wchar_t g_dlSaveDir[MAX_PATH];     /* 下载保存目录 */
static HWND    g_hDlg, g_hDlPath, g_hDlList, g_hDlInfo;

/* ---------------------------------------------------------------- 日志 */

static void log_add(const wchar_t *line)
{
    wchar_t ts[32];
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(ts, 32, L"%02d:%02d:%02d  ", st.wHour, st.wMinute, st.wSecond);
    if (!g_quiet) {
        int len = GetWindowTextLengthW(g_hLog);
        SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)ts);
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
    /* 同时落盘，现场排障可以直接看文件 */
    wchar_t f[MAX_PATH + 16];
    swprintf(f, MAX_PATH + 16, L"%s\\run.log", g_dir);
    FILE *fp = _wfopen(f, L"a, ccs=UTF-8");
    if (fp) { fwprintf(fp, L"%s%s\n", ts, line); fclose(fp); }
}

static void logw(const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1024, fmt, ap);
    va_end(ap);
    buf[1023] = 0;
    log_add(buf);
}

/* ------------------------------------------------------- 资源释放/路径 */

static BOOL extract_res(int id, const wchar_t *path)
{
    HRSRC hr = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!hr) return FALSE;
    HGLOBAL hg = LoadResource(NULL, hr);
    if (!hg) return FALSE;
    void *p = LockResource(hg);
    DWORD n = SizeofResource(NULL, hr);
    if (!p || !n) return FALSE;

    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        HANDLE h0 = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (h0 != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h0, NULL);
            CloseHandle(h0);
            if (sz == n) return TRUE;   /* 大小一致就不再写，避免每启动都写盘 */
        }
    }
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, p, n, &wr, NULL) && wr == n;
    CloseHandle(h);
    return ok;
}

/* 私钥只给当前用户：ssh 会拒绝"其他用户可读"的私钥
 * （UNPROTECTED PRIVATE KEY FILE / bad permissions） */
static void restrict_to_me(const wchar_t *path)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    DWORD sz = 0;
    GetTokenInformation(tok, TokenUser, NULL, 0, &sz);
    TOKEN_USER *tu = (TOKEN_USER *)malloc(sz ? sz : 64);
    if (!tu || !GetTokenInformation(tok, TokenUser, tu, sz, &sz)) {
        CloseHandle(tok);
        free(tu);
        return;
    }
    CloseHandle(tok);

    EXPLICIT_ACCESSW ea;
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR)tu->User.Sid;

    PACL acl = NULL;
    if (SetEntriesInAclW(1, &ea, NULL, &acl) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION |
                              PROTECTED_DACL_SECURITY_INFORMATION,
                              NULL, NULL, acl, NULL);
        LocalFree(acl);
    }
    free(tu);
}

/* 逐级建目录：CreateDirectoryW 只建一级 —— %LOCALAPPDATA% 通常已存在所以没暴雷，
 * 但把工作目录指到不存在的父目录（测试、绿色版放 U 盘）时，后面每一次落盘
 * （ini/toml/frpc/log）都会静默失败，现象是"程序能开但什么都不记"。 */
static void mkdir_p(const wchar_t *path)
{
    wchar_t tmp[MAX_PATH];
    size_t n = wcslen(path);
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    for (size_t i = 0; i < n; i++) tmp[i] = path[i];
    tmp[n] = 0;
    for (wchar_t *p = tmp + 3; *p; p++) {      /* 从 "X:\" 之后开始 */
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

static void ensure_paths(void)
{
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n) { GetTempPathW(MAX_PATH, base); }
    swprintf(g_dir, MAX_PATH, L"%s\\ivsbox-assist", base);
    mkdir_p(g_dir);

    swprintf(g_frpc,    MAX_PATH, L"%s\\frpc.exe", g_dir);
    swprintf(g_toml,    MAX_PATH, L"%s\\visitor.toml", g_dir);
    swprintf(g_key,     MAX_PATH, L"%s\\id_ed25519", g_dir);
    swprintf(g_frpcLog, MAX_PATH, L"%s\\frpc.log", g_dir);
    swprintf(g_ini,     MAX_PATH, L"%s\\assist.ini", g_dir);

    /* exe 自己所在的目录：允许在旁边放一份 sns.txt 批量灌 SN（见 sn_list_load） */
    {
        DWORD n2 = GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
        if (n2 && n2 < MAX_PATH) {
            wchar_t *p = wcsrchr(g_exeDir, L'\\');
            if (p) *p = 0;
        } else {
            g_exeDir[0] = 0;
        }
    }

    if (!g_dlSaveDir[0]) {
        wchar_t prof[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH))
            swprintf(g_dlSaveDir, MAX_PATH, L"%s\\Downloads", prof);
        else
            wcscpy(g_dlSaveDir, g_dir);
    }

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(g_ssh, MAX_PATH, L"%s\\OpenSSH\\ssh.exe", sys);
    if (GetFileAttributesW(g_ssh) == INVALID_FILE_ATTRIBUTES)
        wcscpy(g_ssh, L"ssh.exe");
}

/* ------------------------------------------------------------- 进程工具 */

/* 跑一条命令，抓 stdout；返回退出码（-1 = 起不来/超时）。cmdline 会被拷一份。 */
static int run_capture(const wchar_t *cmdline, char *out, int cap, int timeout_ms)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 64 * 1024)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, NULL);

    wchar_t *cmd = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    CloseHandle(wr);
    if (si.hStdInput != INVALID_HANDLE_VALUE) CloseHandle(si.hStdInput);
    if (!ok) { CloseHandle(rd); return -1; }

    if (out && cap > 0) out[0] = 0;
    int used = 0;
    char buf[4096];
    DWORD got = 0;
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 30) == WAIT_OBJECT_0) {
                /* 进程退了，把剩下的读干净 */
                while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got > 0) {
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
            if ((int)(GetTickCount() - t0) > timeout_ms) {
                TerminateProcess(pi.hProcess, 0);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
                return -2;
            }
            continue;
        }
        if (!ReadFile(rd, buf, sizeof(buf), &got, NULL) || got == 0) break;
        if (out && used < cap - 1) {
            int cp = (int)got;
            if (used + cp > cap - 1) cp = cap - 1 - used;
            memcpy(out + used, buf, cp);
            used += cp;
            out[used] = 0;
        }
    }
    WaitForSingleObject(pi.hProcess, 500);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
}

static BOOL port_open(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return FALSE;
    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    BOOL ok = connect(s, (struct sockaddr *)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

static int http_code(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    int code = -1;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        const char *req = "GET / HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
        send(s, req, (int)strlen(req), 0);
        char buf[256];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            char *sp = strchr(buf, ' ');
            if (sp) code = atoi(sp + 1);
        }
    }
    closesocket(s);
    return code;
}

/* 生成 ssh 命令行的公共前缀 */
static void ssh_prefix(wchar_t *dst, int need_stdin)
{
    swprintf(dst, 1024,
             L"\"%s\" %s-C -i \"%s\" -p %d -o BatchMode=yes "
             L"-o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL "
             L"-o ConnectTimeout=10 -o ServerAliveInterval=20 "
             L"-o LogLevel=ERROR %s",
             g_ssh, need_stdin ? L"" : L"-n ", g_key, FRP_PORT, FRP_SERVER);
}

/* ======================================================================
 * 设备身份（SN）与通道客户端配置：运行时生成，不再写死
 * ----------------------------------------------------------------------
 * 设备侧（iv_tunnel.c）渲染 frpc.toml 用的是
 *     secretKey = 小写 hex(MD5(SN + auth.token)) 的前 8 字节 —— 16 个 hex 字符
 * 这里必须逐位算出同一个值，否则 stcp 握手失败，而且两端都不报"钥匙错"
 * （现象是 visitor 一直 start 不成功 / 连上去立刻断）。
 *
 * 以前这份 visitor.toml 是打包进 exe 资源的固定文件（SN 0B2CC1），换台设备
 * 就得重新编译。现在改成：SN / 服务器地址 / 端口 / 令牌 都可在界面上改，
 * 每次改动都重算 secretKey 并重写 visitor.toml，frpc 随之重启。
 * ====================================================================== */

static void kill_tunnel(void);        /* 下面才定义，先声明 */
static void update_status_line(void);

/* 宽串按 UTF-8 落成窄串（TOML 是 UTF-8 文本） */
static void to_utf8(const wchar_t *w, char *o, int cap)
{
    o[0] = 0;
    if (!w || !w[0] || cap <= 0) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, o, cap, NULL, NULL);
    o[cap - 1] = 0;
}

/* SN：设备序列号，只允许字母数字和 - _（挡掉引号/换行污染 TOML 的路子） */
static BOOL sn_ok(const wchar_t *sn)
{
    size_t n = sn ? wcslen(sn) : 0;
    if (n < 1 || n > 40) return FALSE;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = sn[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') || c == L'-' || c == L'_'))
            return FALSE;
    }
    return TRUE;
}

/* 令牌：与设备侧 iv_tunnel_token_valid() 的白名单对齐（1..63 位） */
static BOOL tok_ok(const wchar_t *t)
{
    size_t n = t ? wcslen(t) : 0;
    if (n < 1 || n > 63) return FALSE;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = t[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') ||
              c == L'-' || c == L'_' || c == L'.' || c == L'~' ||
              c == L'+' || c == L'/' || c == L'=' || c == L'@'))
            return FALSE;
    }
    return TRUE;
}

/* 16 字节 → 32 hex 的新令牌。用系统 CSPRNG，不自己搓种子。
 * 只在「生成令牌」里用；出厂令牌是内嵌的常量（见 g_token）。 */
static void tok_new(wchar_t *out, int cap)
{
    unsigned char b[16];
    if (cap < 33) { if (cap > 0) out[0] = 0; return; }
    if (BCryptGenRandom(NULL, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        memset(b, 0, sizeof b);
    if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) {
        DWORD t = GetTickCount();           /* 极不可能；退化成 tick，也不出全 0 */
        memcpy(b, &t, 4);
    }
    for (int i = 0; i < 16; i++) swprintf(out + i * 2, (size_t)(cap - i * 2), L"%02x", b[i]);
}

/* 日志/界面里只露头尾 */
static void mask_tok(const wchar_t *t, wchar_t *o, int cap)
{
    size_t n = t ? wcslen(t) : 0;
    if (n == 0) { swprintf(o, (size_t)cap, L"(空)"); return; }
    if (n <= 8) { swprintf(o, (size_t)cap, L"%s", t); return; }
    swprintf(o, (size_t)cap, L"%.4s…%.4s (%d 位)", t, t + n - 4, (int)n);
}

/* 服务器地址：IP 或域名，允许 : 与 . 与 - */
static BOOL host_ok(const wchar_t *h)
{
    size_t n = h ? wcslen(h) : 0;
    if (n < 1 || n > 100) return FALSE;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = h[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') ||
              c == L'.' || c == L'-' || c == L':' || c == L'_'))
            return FALSE;
    }
    return TRUE;
}

/* secretKey = hex(MD5(sn + token))[0..8)，小写。用系统 CryptoAPI 算 MD5，
 * 免得自己实现出隐蔽 bug（设备侧是 OpenSSL/md5 手写，黄金向量见 selftest）。 */
static BOOL md5_hex16(const char *sn, const char *token, char out[17])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hh = 0;
    unsigned char dg[16];
    DWORD n = sizeof(dg);
    BOOL ok = FALSE;

    out[0] = 0;
    if (!sn || !token) return FALSE;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return FALSE;
    if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hh)) {
        if (CryptHashData(hh, (BYTE *)sn, (DWORD)strlen(sn), 0) &&
            CryptHashData(hh, (BYTE *)token, (DWORD)strlen(token), 0) &&
            CryptGetHashParam(hh, HP_HASHVAL, dg, &n, 0) && n == 16) {
            for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02x", dg[i]);
            out[16] = 0;
            ok = TRUE;
        }
        CryptDestroyHash(hh);
    }
    CryptReleaseContext(prov, 0);
    return ok;
}

/* 按当前 SN/服务器/令牌重写 visitor.toml；成功返回 TRUE */
static BOOL toml_write(void)
{
    char sn[80], tok[160], host[200], key[17];
    if (!sn_ok(g_sn) || !tok_ok(g_token) || !host_ok(g_srvHost) ||
        g_srvPort <= 0 || g_srvPort > 65535)
        return FALSE;

    to_utf8(g_sn, sn, sizeof(sn));
    to_utf8(g_token, tok, sizeof(tok));
    to_utf8(g_srvHost, host, sizeof(host));
    if (!md5_hex16(sn, tok, key)) return FALSE;

    char body[1200];
    int n = snprintf(body, sizeof(body),
                     "serverAddr = \"%s\"\n"
                     "serverPort = %d\n"
                     "auth.token = \"%s\"\n"
                     "loginFailExit = false\n"
                     "\n"
                     "[[visitors]]\n"
                     "name = \"visitor_%s\"\n"
                     "type = \"stcp\"\n"
                     "serverName = \"%s\"\n"
                     "secretKey = \"%s\"\n"
                     "bindAddr = \"127.0.0.1\"\n"
                     "bindPort = %d\n",
                     host, g_srvPort, tok, sn, sn, key, FRP_PORT);
    if (n <= 0 || n >= (int)sizeof(body)) return FALSE;

    HANDLE h = CreateFileW(g_toml, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, body, (DWORD)n, &wr, NULL) && wr == (DWORD)n;
    CloseHandle(h);
    if (ok) g_frpcGen++;            /* 配置换代，ensure_frpc 据此决定要不要重启 */
    return ok;
}

/* ---------------------------------------------------------- 参数持久化 */

static void sn_remember(const wchar_t *sn)
{
    int at = -1;
    for (int i = 0; i < g_nsns; i++)
        if (!wcscmp(g_sns[i], sn)) { at = i; break; }
    if (at == 0) return;
    if (at > 0) {                       /* 提到最前 */
        for (int i = at; i > 0; i--) wcscpy(g_sns[i], g_sns[i - 1]);
    } else {                            /* 插入最前 */
        if (g_nsns < MAX_SNS) g_nsns++;
        for (int i = g_nsns - 1; i > 0; i--) wcscpy(g_sns[i], g_sns[i - 1]);
    }
    wcscpy(g_sns[0], sn);
}

static void cfg_save(void)
{
    wchar_t b[32];
    WritePrivateProfileStringW(L"meta", L"cfg", CFG_STAMP, g_ini);
    WritePrivateProfileStringW(L"server", L"host", g_srvHost, g_ini);
    swprintf(b, 32, L"%d", g_srvPort);
    WritePrivateProfileStringW(L"server", L"port", b, g_ini);
    WritePrivateProfileStringW(L"server", L"token", g_token, g_ini);
    WritePrivateProfileStringW(L"device", L"sn", g_sn, g_ini);
    /* 设备 Web 口令与上次定位到的 IP：存下来，下次打开不用再敲、也不用再扫 */
    WritePrivateProfileStringW(L"device", L"pass", g_devPass, g_ini);
    WritePrivateProfileStringW(L"device", L"ip", g_devIp, g_ini);
    WritePrivateProfileStringW(L"device", L"user", g_devUser, g_ini);
    for (int i = 0; i < g_nsns; i++) {
        wchar_t k[16];
        swprintf(k, 16, L"sn%d", i);
        WritePrivateProfileStringW(L"devices", k, g_sns[i], g_ini);
    }
}

/* 用户在 exe 旁边放一份 sns.txt（一行一个 SN，# 开头是注释）就能整批出现，
 * 省得每次手打；不想要就删掉这个文件。 */
static void sn_list_load(void)
{
    if (!g_exeDir[0]) return;
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\sns.txt", g_exeDir);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return;

    char line[256];
    int added = 0;
    while (fgets(line, sizeof(line), f) && g_nsns < MAX_SNS) {
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#' || !*p) continue;
        char *e = p;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
        *e = 0;
        wchar_t w[64];
        if (mbstowcs(w, p, 63) == (size_t)-1) continue;
        w[63] = 0;
        if (!sn_ok(w)) continue;
        BOOL dup = FALSE;
        for (int i = 0; i < g_nsns; i++)
            if (!wcscmp(g_sns[i], w)) dup = TRUE;
        if (dup) continue;
        wcscpy(g_sns[g_nsns++], w);
        added++;
    }
    fclose(f);
    if (added) logw(L"sns.txt：载入 %d 个 SN", added);
}

static void cfg_load(void)
{
    wchar_t b[256];

    /* 版本戳：assist.ini 里记着"这份配置是哪一版 exe 写的"。逻辑沿用
     * 「运维箱远程访问」1.1.4：戳不一致（含根本没有 ini）就只认编译进去的出厂值
     * 并立刻写回；戳一致时读 ini，用户改过的值不会被每次启动重置。 */
    b[0] = 0;
    GetPrivateProfileStringW(L"meta", L"cfg", L"", b, 256, g_ini);
    b[255] = 0;
    BOOL have_ini = GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES;
    BOOL stale = wcscmp(b, CFG_STAMP) != 0;

    if (stale && have_ini) {
        logw(L"旧的 assist.ini 已过期（配置戳 \"%s\" ≠ 本程序 \"" CFG_STAMP L"\"），改用内置配置", b);
        logw(L"    服务器 %s:%d    设备 SN=%s", g_srvHost, g_srvPort, g_sn);
    } else if (stale) {
        logw(L"首次运行：使用内置配置（服务器 %s:%d，设备 SN=%s）",
             g_srvHost, g_srvPort, g_sn);
    } else {
        GetPrivateProfileStringW(L"server", L"host", g_srvHost, b, 256, g_ini);
        b[255] = 0;
        if (host_ok(b)) wcscpy(g_srvHost, b);

        g_srvPort = GetPrivateProfileIntW(L"server", L"port", g_srvPort, g_ini);
        if (g_srvPort <= 0 || g_srvPort > 65535) g_srvPort = 7000;

        GetPrivateProfileStringW(L"server", L"token", g_token, b, 256, g_ini);
        b[255] = 0;
        if (tok_ok(b)) wcscpy(g_token, b);

        GetPrivateProfileStringW(L"device", L"sn", g_sn, b, 256, g_ini);
        b[255] = 0;
        if (sn_ok(b)) wcscpy(g_sn, b);
    }

    /* 口令与 IP 属于"这台机器的现场状态"，**不受版本戳影响**：
     * 换了版本也保留，省得现场每次重敲口令（这正是用户的要求）。 */
    GetPrivateProfileStringW(L"device", L"pass", g_devPass, g_devPass, 64, g_ini);
    g_devPass[63] = 0;
    GetPrivateProfileStringW(L"device", L"user", g_devUser, b, 256, g_ini);
    b[255] = 0;
    if (b[0]) wcscpy(g_devUser, b);
    GetPrivateProfileStringW(L"device", L"ip", g_devIp, g_devIp, 64, g_ini);
    g_devIp[63] = 0;

    g_nsns = 0;
    for (int i = 0; i < MAX_SNS; i++) {
        wchar_t k[16];
        swprintf(k, 16, L"sn%d", i);
        b[0] = 0;
        GetPrivateProfileStringW(L"devices", k, L"", b, 256, g_ini);
        b[255] = 0;
        if (b[0] && sn_ok(b)) wcscpy(g_sns[g_nsns++], b);
    }
    /* 出厂默认设备列表（2026-11-06）：新机器第一次运行既没有 remote.ini 的
     * [devices] 段、exe 旁边也没有 sns.txt，下拉框里就只有 sn_remember() 补进来的
     * 那一个 SN，换台箱子还得手敲。这里给一份出厂清单，做到"打开即可选"。
     * **一旦 ini/sns.txt 里有条目就完全以它们为准**（g_nsns != 0），不掺默认值，
     * 免得现场删掉的 SN 每次升级又冒出来。 */
    if (g_nsns == 0) {
        static const wchar_t *const dflt[] = { L"0A19B4", L"0B2CC1" };
        for (int i = 0; i < (int)(sizeof dflt / sizeof dflt[0]) && g_nsns < MAX_SNS; i++)
            wcscpy(g_sns[g_nsns++], dflt[i]);
    }
    sn_list_load();
    if (sn_ok(g_sn)) sn_remember(g_sn);
    /* 出厂值立刻写回：否则下次启动又是"戳对不上"，日志每次刷同一句话 */
    if (stale) {
        cfg_save();
        if (have_ini)
            logw(L"已用内置配置覆盖 %s（下拉框里的历史 SN 照旧保留）", g_ini);
    }
}

/* 下拉框内容 = 历史 SN（最近的在前）*/
static void sn_combo_fill(void)
{
    if (!g_hCmbSn) return;
    SendMessageW(g_hCmbSn, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_nsns; i++)
        SendMessageW(g_hCmbSn, CB_ADDSTRING, 0, (LPARAM)g_sns[i]);
    SetWindowTextW(g_hCmbSn, g_sn);
}

/* 停掉本工具拉起的 frpc（换 SN 必须停，否则它还在按老 toml 跑） */
static void kill_frpc(void)
{
    if (g_frpcProc) {
        TerminateProcess(g_frpcProc, 0);
        CloseHandle(g_frpcProc);
        g_frpcProc = NULL;
        g_frpcPid = 0;
        logw(L"已停 frpc（配置变了要按新的重拉）");
    }
    g_frpcProcGen = -1;
    if (port_open(FRP_PORT)) {
        /* 端口还开着但不是我们起的：多半是上次崩溃/强杀留下的 frpc.exe */
        char out[256] = "";
        run_capture(L"taskkill /F /IM frpc.exe", out, sizeof(out), 5000);
        logw(L"清理了残留的 frpc.exe（否则它会继续按旧 SN 跑）");
        Sleep(300);
    }
}

/* 切换设备：改 SN → 重写 visitor.toml → 停掉旧 frpc（下次操作时按新配置拉起） */
static BOOL sn_apply(const wchar_t *sn, wchar_t *why, int cap)
{
    if (!sn_ok(sn)) {
        if (why) swprintf(why, cap, L"SN 只能包含字母/数字/-/_，长度 1..40");
        return FALSE;
    }
    BOOL changed = wcscmp(sn, g_sn) != 0;
    if (changed) {
        kill_tunnel();
        kill_frpc();
        wcscpy(g_sn, sn);
    }
    if (!toml_write()) {
        if (why) swprintf(why, cap, L"写 visitor.toml 失败（检查服务器地址/端口/令牌）");
        return FALSE;
    }
    sn_remember(g_sn);
    cfg_save();
    update_status_line();
    if (changed) logw(L"已切换到设备 SN=%s（secretKey 已按新 SN 重算）", g_sn);
    return TRUE;
}

/* 把下拉框里当前显示的 SN（去首尾空白）取出来；没填就是空串 */
static void combo_sn(wchar_t *out, int cap)
{
    out[0] = 0;
    if (!g_hCmbSn) return;
    GetWindowTextW(g_hCmbSn, out, cap - 1);
    out[cap - 1] = 0;
    wchar_t *p = out;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != out) memmove(out, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t L = wcslen(out);
    while (L && (out[L - 1] == L' ' || out[L - 1] == L'\t')) out[--L] = 0;
}

/* 让"当前设备"跟上界面上显示的 SN。用户在下拉框里手打或选了但没点
 * 「切换」，直接按「连接 / 设备 Web / 刷新」也不该连到上一台去 —— 1.1.4 起
 * 这几个动作先自己把 SN 同步一次。返回 FALSE 表示 SN 不合法（已提示过）。 */
static BOOL sync_sn_from_combo(HWND h)
{
    wchar_t sn[64], why[160] = L"";
    combo_sn(sn, 64);
    if (!sn[0] || !wcscmp(sn, g_sn)) return TRUE;
    if (!sn_apply(sn, why, 160)) {
        MessageBoxW(h, why, APP_TITLE, MB_ICONWARNING);
        return FALSE;
    }
    sn_combo_fill();
    return TRUE;
}

/* ======================================================================
 * 设备 Web（配置/令牌/通道）—— WinHTTP 层
 * ----------------------------------------------------------------------
 * 这一层只做三件事：登录拿会话、读 `/api/tunnel`、写令牌/开关。所有字段
 * 口径都以 `web/src/iv_web_api.c` 的实际实现为准（不是照文档猜）：
 *   POST /api/login          {"user","pass"} → {"ok":1,"token":"…"}
 *   GET  /api/tunnel         → {"data":{"on":b,"sn":"…","token_set":b,
 *                              "token_len":N,"server":"…","server_port":N,…}}
 *   POST /api/tunnel/config  {"token","ttl","enable"}   ← enable 必须跟现状走
 *   POST /api/tunnel/enable|disable  {}
 *   POST /api/passwd         {"new"}   出厂口令期（must_change=1）不校验旧口令
 * 会话靠 `Cookie: ivs_sid=<token>`，8 小时有效、进程重启失效。
 * ==================================================================== */

static void u82w(const char *s, wchar_t *o, int cap)
{
    o[0] = 0;
    if (!s || cap <= 0) return;
    mbstowcs(o, s, (size_t)cap - 1);
    o[cap - 1] = 0;
}

/* 取 key 对应的值起点（字符串返回第一个非引号字符，数字/布尔返回数字起点） */
static const char *jget(const char *js, const char *key)
{
    if (!js || !key) return NULL;
    char pat[96];
    int n = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof pat) return NULL;
    const char *p = js;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + n;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p += n; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        return q;
    }
    return NULL;
}

static BOOL jstr(const char *js, const char *key, wchar_t *out, int cap)
{
    const char *p = jget(js, key);
    if (out && cap > 0) out[0] = 0;
    if (!p || *p != '"') return FALSE;
    p++;
    char tmp[256];
    int n = 0;
    while (*p && *p != '"' && n < (int)sizeof tmp - 1) {
        if (*p == '\\' && p[1]) p++;
        tmp[n++] = *p++;
    }
    tmp[n] = 0;
    if (out) u82w(tmp, out, cap);
    return TRUE;
}

static int jint(const char *js, const char *key, int dflt)
{
    const char *p = jget(js, key);
    if (!p) return dflt;
    if (*p == '-' || (*p >= '0' && *p <= '9')) return (int)strtol(p, NULL, 10);
    return dflt;
}

static BOOL jbool(const char *js, const char *key, BOOL dflt)
{
    const char *p = jget(js, key);
    if (!p) return dflt;
    if (!strncmp(p, "true", 4) || *p == '1') return TRUE;
    if (!strncmp(p, "false", 5) || *p == '0') return FALSE;
    return dflt;
}

/* host 可写 `ip:port`（排障时对着 18081 上的临时实例验一遍用） */
static BOOL http_req(const wchar_t *host, const wchar_t *method, const wchar_t *path,
                     const char *body, const wchar_t *sid,
                     char *out, int cap, int *code, wchar_t *err, int errcap)
{
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    wchar_t hbuf[128], hdr[640];
    unsigned short port = 80;
    BOOL got = FALSE;
    DWORD clen = 0, status = 0, slen = sizeof(status);

    if (out && cap > 0) out[0] = 0;
    if (code) *code = 0;
    if (err && errcap > 0) err[0] = 0;

    {
        const wchar_t *colon = NULL;
        for (const wchar_t *q = host; q && *q; q++) if (*q == L':') colon = q;
        if (colon) {
            size_t n = (size_t)(colon - host);
            unsigned long pv = wcstoul(colon + 1, NULL, 10);
            if (n > 127) n = 127;
            memcpy(hbuf, host, n * sizeof(wchar_t));
            hbuf[n] = 0;
            if (pv > 0 && pv <= 65535) port = (unsigned short)pv;
        } else {
            swprintf(hbuf, 128, L"%s", host ? host : L"");
        }
    }

    hs = WinHttpOpen(L"ivsbox-assist/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) {
        if (err) swprintf(err, (size_t)errcap, L"WinHttpOpen 失败（%lu）", GetLastError());
        return FALSE;
    }
    /* 局域网设备：绝不走代理。连接 6 s、接收 20 s（写令牌要重写配置并拉起 frpc） */
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
    if (sid && sid[0]) swprintf(hdr, 640, L"Cookie: ivs_sid=%s\r\n", sid);
    if (body) wcscat(hdr, L"Content-Type: application/json\r\n");

    if (!WinHttpSendRequest(hr, hdr, (DWORD)-1L, (LPVOID)body,
                            body ? (DWORD)strlen(body) : 0,
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
        if ((int)avail <= 0) break;
        if (!WinHttpReadData(hr, out + clen, avail, &got2)) break;
        clen += got2;
        if (got2 == 0) break;
    }
    if (out) out[clen] = 0;
    got = TRUE;

done:
    if (hr) WinHttpCloseHandle(hr);
    if (hc) WinHttpCloseHandle(hc);
    if (hs) WinHttpCloseHandle(hs);
    return got;
}

/* 设备自己那句话（`{"ok":false,"e":N,"msg":"…"}`）——别只报 HTTP 码，
 * 409 的真实含义是"已经是你要的状态"，见「运维箱通道配置」1.1i 的同款教训。 */
static void dev_err_text(const char *js, int code, const wchar_t *what,
                         wchar_t *err, int errcap)
{
    wchar_t m[256];
    int e = jint(js, "e", 0);
    if (!jstr(js, "msg", m, 256)) m[0] = 0;
    if (m[0])
        swprintf(err, (size_t)errcap, L"%s（HTTP %d, e=%d）：%s", what, code, e, m);
    else
        swprintf(err, (size_t)errcap, L"%s（HTTP %d, e=%d）", what, code, e);
}

/* 登录一次，拿到会话；must_change=1 时用 *must 回报（此时除改密外全 403） */
static BOOL dev_login(const wchar_t *ip, const wchar_t *user, const wchar_t *pass,
                      wchar_t *sid, int sidcap, BOOL *must,
                      wchar_t *err, int errcap)
{
    char u[192], p[384], ue[384], pe[768], body[1400], js[2048];
    int code = 0;

    if (must) *must = FALSE;
    to_utf8(user, u, sizeof u);
    to_utf8(pass, p, sizeof p);
    {
        /* 手写 JSON 转义（账号口令里出现 " 和 \ 的概率不低） */
        int n = 0;
        ue[0] = 0;
        for (const char *q = u; *q && n < (int)sizeof ue - 2; q++) {
            if (*q == '"' || *q == '\\') ue[n++] = '\\';
            ue[n++] = *q;
        }
        ue[n] = 0;
        n = 0;
        pe[0] = 0;
        for (const char *q = p; *q && n < (int)sizeof pe - 2; q++) {
            if (*q == '"' || *q == '\\') pe[n++] = '\\';
            pe[n++] = *q;
        }
        pe[n] = 0;
    }
    snprintf(body, sizeof body, "{\"user\":\"%s\",\"pass\":\"%s\"}", ue, pe);

    if (!http_req(ip, L"POST", L"/api/login", body, NULL,
                  js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code == 200 && jbool(js, "ok", FALSE)) {
        if (!jstr(js, "token", sid, sidcap)) {
            swprintf(err, (size_t)errcap, L"登录成功但响应里没有 token（Web 后端太旧？）");
            return FALSE;
        }
        return TRUE;
    }
    if (code == 401) {
        int e = jint(js, "e", 0);
        swprintf(err, (size_t)errcap,
                 L"口令不对（HTTP 401, e=%d）。出厂默认 admin/88888888 只在新机器上有效", e);
        return FALSE;
    }
    dev_err_text(js, code, L"登录被拒", err, errcap);
    return FALSE;
}

/* 读 /api/tunnel 并把结果落到 g_tun* 上；sn 与期望不符返回 FALSE（*sn_out 带实际值） */
static BOOL dev_read_tunnel(const wchar_t *ip, const wchar_t *sid,
                            wchar_t *sn_out, int sncap, BOOL *must_change,
                            wchar_t *err, int errcap)
{
    char js[4096];
    int code = 0;
    wchar_t sn[64] = L"";

    if (must_change) *must_change = FALSE;
    (void)sncap;
    if (!http_req(ip, L"GET", L"/api/tunnel", NULL, sid,
                  js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code == 403) {
        if (must_change) *must_change = TRUE;
        swprintf(err, (size_t)errcap, L"设备还在出厂口令状态，除改密外一律 403（must change password）");
        return FALSE;
    }
    if (code != 200 || !jbool(js, "ok", FALSE)) {
        dev_err_text(js, code, L"读通道状态被拒", err, errcap);
        return FALSE;
    }
    /* 数据可能包在 data 里，也可能直接平铺 —— 两种都试 */
    const char *d = jget(js, "data");
    if (!d || *d != '{') d = js;
    (void)jstr(d, "sn", sn, 64);
    if (sn_out) wcscpy(sn_out, sn);
    g_tunOn     = jbool(d, "on", FALSE);
    g_tunTokSet = jbool(d, "token_set", FALSE);
    g_tunTokLen = jint(d, "token_len", 0);
    jstr(d, "server", g_tunSrv, 128);
    g_tunSrvPort = jint(d, "server_port", 0);
    return TRUE;
}

/* 写令牌。enable 必须跟"现在开着没有"走：写死 true 会把关着的通道打开
 * （「运维通道配置」1.1i 的血债，这里不重犯）。 */
static BOOL dev_set_token(const wchar_t *ip, const wchar_t *sid, const wchar_t *tok,
                          BOOL enable, wchar_t *err, int errcap)
{
    char t[256], body[512], js[2048];
    int code = 0;
    to_utf8(tok, t, sizeof t);
    snprintf(body, sizeof body, "{\"token\":\"%s\",\"ttl\":0,\"enable\":%s}",
             t, enable ? "true" : "false");
    if (!http_req(ip, L"POST", L"/api/tunnel/config", body, sid,
                  js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code != 200 || !jbool(js, "ok", TRUE)) {
        dev_err_text(js, code, L"下发令牌被拒", err, errcap);
        return FALSE;
    }
    return TRUE;
}

/* 开关通道。*was 回传"其实本来就是这状态"（409 + e=-7 = IV_ESTATE） */
static BOOL dev_switch(const wchar_t *ip, const wchar_t *sid, BOOL on, BOOL *was,
                       int *code_out, wchar_t *err, int errcap)
{
    char js[2048];
    int code = 0;
    if (was) *was = FALSE;
    if (code_out) *code_out = 0;
    if (!http_req(ip, L"POST", on ? L"/api/tunnel/enable" : L"/api/tunnel/disable",
                  "{}", sid, js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code_out) *code_out = code;
    if (code == 200) return TRUE;
    if (code == 409 && jint(js, "e", 0) == -7) {
        if (was) *was = TRUE;
        return TRUE;
    }
    dev_err_text(js, code, L"开关被拒", err, errcap);
    return FALSE;
}

/* 出厂口令期改密（服务端此时不校验旧口令，只要新口令合复杂度） */
static BOOL dev_set_pass(const wchar_t *ip, const wchar_t *sid, const wchar_t *nw,
                         wchar_t *err, int errcap)
{
    char p[256], body[384], js[2048];
    int code = 0;
    to_utf8(nw, p, sizeof p);
    snprintf(body, sizeof body, "{\"new\":\"%s\"}", p);
    if (!http_req(ip, L"POST", L"/api/passwd", body, sid,
                  js, sizeof js, &code, err, errcap))
        return FALSE;
    if (code != 200 || !jbool(js, "ok", TRUE)) {
        dev_err_text(js, code, L"改密被拒", err, errcap);
        return FALSE;
    }
    return TRUE;
}

/* ======================================================================
 * 设备定位：本机网段扫 HTTP ＋ 登录后核 SN
 * ----------------------------------------------------------------------
 * 用户只给 SN 和口令，**不给 IP**。做法：
 *   1. 上次记住的 IP 先试（99% 的重复使用走这条，零等待）；
 *   2. 本机每块网卡的 /24（掩码比 /24 窄就按实际范围，最多 254 个地址）
 *      并发探 80 端口 → 只对活着的做指纹（GET / 里有 "IVSBox"）；
 *   3. 逐个登录，用 `/api/tunnel` 回的 `sn` 跟用户填的比，**对上才算找到**。
 * 所以"扫到别的箱子"不会连错 —— SN 是最后的裁决，不是 IP。
 * ==================================================================== */

typedef struct {
    unsigned int ip;        /* host 字节序 */
    int          hit;
    int          prio;      /* 0 = 默认路由所在 /24（先扫这段），1 = 其它网卡 */
} ScanSlot;

#define SCAN_MAX   1024
#define SCAN_THR   32

static ScanSlot g_scan[SCAN_MAX];
static int      g_nscan;
static volatile LONG g_scanNext;

/* 非阻塞 connect + select，超时 ms 毫秒；成功 TRUE */
static BOOL tcp_probe(unsigned int ip_host, int port, int ms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return FALSE;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    struct sockaddr_in a;
    ZeroMemory(&a, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(ip_host);

    BOOL ok = FALSE;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        ok = TRUE;
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wf, ef;
        FD_ZERO(&wf);
        FD_ZERO(&ef);
        FD_SET(s, &wf);
        FD_SET(s, &ef);
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int r = select(0, NULL, &wf, &ef, &tv);
        if (r > 0 && FD_ISSET(s, &wf) && !FD_ISSET(s, &ef)) {
            int soerr = 0, sl = sizeof soerr;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) == 0 && soerr == 0)
                ok = TRUE;
        }
    }
    closesocket(s);
    return ok;
}

/* 本机默认路由所在网卡的 /24（网络地址）；拿不到就返回 0（表示不分区） */
static unsigned int route_net(void)
{
    PMIB_IPFORWARDTABLE ft;
    PMIB_IPADDRTABLE t;
    DWORD sz, i, ifidx = 0;
    unsigned int net = 0;

    sz = 0;
    if (GetIpForwardTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER) return 0;
    ft = (PMIB_IPFORWARDTABLE)malloc(sz);
    if (!ft) return 0;
    if (GetIpForwardTable(ft, &sz, FALSE) == NO_ERROR) {
        for (i = 0; i < ft->dwNumEntries; i++) {
            if (ft->table[i].dwForwardDest == 0 && ft->table[i].dwForwardMask == 0) {
                ifidx = ft->table[i].dwForwardIfIndex;
                break;
            }
        }
    }
    free(ft);
    if (!ifidx) return 0;

    sz = 0;
    if (GetIpAddrTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER) return 0;
    t = (PMIB_IPADDRTABLE)malloc(sz);
    if (!t) return 0;
    if (GetIpAddrTable(t, &sz, FALSE) == NO_ERROR) {
        for (i = 0; i < t->dwNumEntries; i++) {
            if (t->table[i].dwIndex == ifidx) {
                net = ntohl(t->table[i].dwAddr) & 0xffffff00u;
                break;
            }
        }
    }
    free(t);
    return net;
}

/* prio 从参数传进来：0/1 只探对应分区，-1 全探 */
static DWORD WINAPI scan_thread(LPVOID p)
{
    int prio = (int)(INT_PTR)p;
    for (;;) {
        LONG i = InterlockedIncrement(&g_scanNext) - 1;
        if (i >= g_nscan) break;
        if (g_scan[i].hit) continue;                       /* 上一轮已探活 */
        if (prio >= 0 && g_scan[i].prio != prio) continue;
        if (tcp_probe(g_scan[i].ip, 80, 250)) g_scan[i].hit = 1;
    }
    return 0;
}

/* 本机所有 IPv4 网段里的候选地址填进 g_scan；返回个数 */
static int scan_fill_local(void)
{
    PMIB_IPADDRTABLE t = NULL;
    DWORD sz = 0, i;
    unsigned int pnet;
    g_nscan = 0;
    pnet = route_net();      /* 默认路由那段排在最前，现场 99% 的设备在这里 */

    if (GetIpAddrTable(NULL, &sz, FALSE) != ERROR_INSUFFICIENT_BUFFER) return 0;
    t = (PMIB_IPADDRTABLE)malloc(sz);
    if (!t) return 0;
    if (GetIpAddrTable(t, &sz, FALSE) != NO_ERROR) { free(t); return 0; }

    for (i = 0; i < t->dwNumEntries && g_nscan < SCAN_MAX; i++) {
        unsigned int a = ntohl(t->table[i].dwAddr);
        unsigned int m = ntohl(t->table[i].dwMask);
        unsigned char first = (unsigned char)((a >> 24) & 0xff);
        if (a == 0 || first == 127 || first == 0 || m == 0 || m == 0xffffffffu)
            continue;
        /* 169.254/16 是链路本地（网线没 DHCP 时的自配地址），扫它纯属浪费时间 */
        if (first == 169 && ((a >> 16) & 0xff) == 254) continue;

        /* 只扫"自己所在的那一段"：掩码比 /24 宽就退化成 /24，避免一次扫几万个地址 */
        unsigned int base, cnt;
        int bits = 0;
        for (unsigned int mm = m; mm & 0x80000000u; mm <<= 1) bits++;
        if (bits <= 24) {
            base = a & 0xffffff00u;
            cnt = 254;
        } else {
            unsigned int span = (1u << (32 - bits));
            base = a & m;
            cnt = span > 2 ? span - 2 : 0;
            if (cnt > 254) cnt = 254;
            /* 取 IP 所在的那 254 段，别从段首开始扫（/16 的段首离得很远） */
            base = a & 0xffffff00u;
            cnt = 254;
        }
        unsigned int self = a;
        for (unsigned int k = 1; k <= cnt && g_nscan < SCAN_MAX; k++) {
            unsigned int ip = base + k;
            if (ip == self) continue;
            g_scan[g_nscan].ip = ip;
            g_scan[g_nscan].hit = 0;
            g_scan[g_nscan].prio = (!pnet || base == pnet) ? 0 : 1;
            g_nscan++;
        }
    }
    free(t);
    return g_nscan;
}

/* 扫一遍本机网段，把活着 80 端口的地址拷出来。
 * prio: 0 只扫默认路由那段 / 1 只扫其它网卡 / -1 全扫。
 * 分段是为了省时间：默认路由那段 254 个地址约 2 秒，其它网卡（VMware、WSL、
 * 虚拟网桥…）只在第一段没找到时才扫 —— 否则一次 1000 个地址要 8 秒。 */
static int scan_hosts(int prio, unsigned int *out, int cap)
{
    if (!g_nscan && !scan_fill_local()) return 0;
    g_scanNext = 0;
    HANDLE th[SCAN_THR];
    int nt = 0;
    for (int i = 0; i < SCAN_THR; i++) {
        th[i] = CreateThread(NULL, 0, scan_thread, (LPVOID)(INT_PTR)prio, 0, NULL);
        if (th[i]) nt++;
    }
    for (int i = 0; i < nt; i++) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }

    int n = 0;
    for (int i = 0; i < g_nscan && n < cap; i++)
        if (g_scan[i].hit && (prio < 0 || g_scan[i].prio == prio)) out[n++] = g_scan[i].ip;
    return n;
}

/* 是不是一台 IVSBox：GET / 的页面里有 "IVSBox"（登录页标题） */
static BOOL looks_ivsbox(const wchar_t *host)
{
    char js[8192];
    int code = 0;
    wchar_t err[128];
    if (!http_req(host, L"GET", L"/", NULL, NULL, js, sizeof js, &code, err, 128))
        return FALSE;
    if (code != 200) return FALSE;
    return strstr(js, "IVSBox") != NULL || strstr(js, "ivsbox") != NULL;
}

/* 定位结果 */
static BOOL g_devLocated;
static wchar_t g_locateMsg[256];
/* 出厂口令改完之后要重跑哪个动作（10=连接设备 / 16=只读探查）：
 * 改了密就直接"连上并开通道"是越权，得按用户刚才点的那个动作重来。 */
static int g_needPassAct = 10;

/* 把 ip 填成 "a.b.c.d" */
static void ip_w(unsigned int ip, wchar_t *o, int cap)
{
    swprintf(o, (size_t)cap, L"%u.%u.%u.%u",
             (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

/* 找到设备并登录，成功后 g_devIp 有效、g_devUp=TRUE。
 * want_sn 为空表示"随便一台"（手工填 IP 时用）。
 * *need_pass=TRUE 表示"找到了，但设备还在出厂口令状态"，g_locateMsg 回传它的地址。 */
static BOOL dev_locate(const wchar_t *want_sn, const wchar_t *pass, BOOL *need_pass,
                       wchar_t *err, int errcap)
{
    wchar_t sid[192], sn[64], wip[64];
    BOOL must = FALSE;

    if (need_pass) *need_pass = FALSE;
    g_devLocated = FALSE;
    g_locateMsg[0] = 0;

    /* 1. 上次记住的 IP —— 重复使用走这条，零等待 */
    if (g_devIp[0]) {
        logw(L"先试上次记住的设备地址 %s ...", g_devIp);
        if (dev_login(g_devIp, g_devUser, pass, sid, 192, &must, err, errcap)) {
            if (dev_read_tunnel(g_devIp, sid, sn, 64, &must, err, errcap)) {
                if (!want_sn || !want_sn[0] || !wcscmp(sn, want_sn)) {
                    g_devLocated = g_devUp = TRUE;
                    sn_remember(sn);
                    return TRUE;
                }
                swprintf(err, (size_t)errcap, L"%s 是设备 %s，不是 %s", g_devIp, sn, want_sn);
                logw(L"[!] %s", err);
                g_devIp[0] = 0;          /* 确实不是这台，别再白试一遍 */
            } else if (must) {
                /* 口令对上了（只有这台认这个口令），但它还在出厂口令期：
                 * 除改密外一律 403，SN 读不出来。这就是要找的那台，别去扫别的。 */
                if (need_pass) *need_pass = TRUE;
                swprintf(g_locateMsg, 256, L"%s", g_devIp);
                swprintf(err, (size_t)errcap,
                         L"设备 %s 还在出厂口令状态（除改密外全部 403），必须先设新口令", g_devIp);
                return FALSE;
            } else {
                logw(L"    读不到通道状态（%s），改扫网段", err);
                g_devIp[0] = 0;
            }
        } else {
            logw(L"    没用上（%s），改扫网段", err);
            g_devIp[0] = 0;
        }
    }

    /* 2. 扫网段：先扫默认路由那段（约 2 秒），没找到才扫其它网卡 */
    for (int stage = 0; stage < 2; stage++) {
        unsigned int hits[64], cand[64];
        int nh, nc = 0;

        nh = scan_hosts(stage, hits, 64);
        if (!nh) {
            if (stage == 0) logw(L"    默认路由网段没扫到 HTTP 服务，改扫其它网卡");
            continue;
        }
        logw(L"    %s有 %d 个 HTTP 服务，逐个认身份",
             stage ? L"其它网卡" : L"默认路由网段", nh);
        for (int i = 0; i < nh && nc < 64; i++) {
            ip_w(hits[i], wip, 64);
            if (looks_ivsbox(wip)) cand[nc++] = hits[i];
        }
        if (!nc) continue;

        /* 3. 逐个登录核 SN —— SN 才是裁决，不看 IP 顺序 */
        for (int i = 0; i < nc; i++) {
            ip_w(cand[i], wip, 64);
            logw(L"    认 %s ...", wip);
            if (!dev_login(wip, g_devUser, pass, sid, 192, &must, err, errcap)) {
                logw(L"      %s", err);
                continue;
            }
            if (!dev_read_tunnel(wip, sid, sn, 64, &must, err, errcap)) {
                if (must) {
                    if (need_pass) *need_pass = TRUE;
                    swprintf(g_locateMsg, 256, L"%s", wip);
                    swprintf(err, (size_t)errcap,
                             L"设备 %s 还在出厂口令状态（除改密外全部 403），必须先设新口令", wip);
                    return FALSE;
                }
                logw(L"      %s", err);
                continue;
            }
            if (want_sn && want_sn[0] && wcscmp(sn, want_sn)) {
                logw(L"      这台 SN=%s，不是要的 %s，跳过", sn, want_sn);
                continue;
            }
            wcscpy(g_devIp, wip);
            g_devLocated = g_devUp = TRUE;
            sn_remember(sn);
            cfg_save();
            return TRUE;
        }
    }

    if (want_sn && want_sn[0])
        swprintf(err, (size_t)errcap,
                 L"本机网段没找到 SN=%s 的 IVSBox（或口令不对）。设备接在本机网口/交换机上了吗？"
                 L"也可以点「服务器设置」旁边手工填设备 IP", want_sn);
    else
        swprintf(err, (size_t)errcap, L"本机网段没找到 IVSBox 设备");
    return FALSE;
}

/* ------------------------------------------------------- 服务器设置对话框 */
static HWND g_hSet, g_hSetHost, g_hSetPort, g_hSetTok;
static BOOL g_setDone, g_setOK;

static LRESULT CALLBACK set_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_SET_OK) {
            wchar_t host[128] = L"", port[16] = L"", tok[128] = L"";
            GetWindowTextW(g_hSetHost, host, 127);
            GetWindowTextW(g_hSetPort, port, 15);
            GetWindowTextW(g_hSetTok, tok, 127);

            int p = _wtoi(port);
            if (!host_ok(host)) {
                MessageBoxW(g_hSet, L"服务器地址不合法（只能用字母数字 . - : _）",
                            APP_TITLE, MB_ICONWARNING);
                SetFocus(g_hSetHost);
                return 0;
            }
            if (p <= 0 || p > 65535) {
                MessageBoxW(g_hSet, L"端口必须是 1..65535", APP_TITLE, MB_ICONWARNING);
                SetFocus(g_hSetPort);
                return 0;
            }
            if (!tok_ok(tok)) {
                MessageBoxW(g_hSet, L"令牌只允许 0-9 A-Z a-z - _ . ~ + / = @，长度 1..63",
                            APP_TITLE, MB_ICONWARNING);
                SetFocus(g_hSetTok);
                return 0;
            }

            BOOL chg = wcscmp(host, g_srvHost) || p != g_srvPort || wcscmp(tok, g_token);
            if (chg) {
                kill_tunnel();
                kill_frpc();
                wcscpy(g_srvHost, host);
                g_srvPort = p;
                wcscpy(g_token, tok);
            }
            if (!toml_write()) {
                MessageBoxW(g_hSet, L"写 visitor.toml 失败（看看目录是否可写）",
                            APP_TITLE, MB_ICONERROR);
                return 0;
            }
            cfg_save();
            update_status_line();
            if (chg)
                logw(L"服务器已改为 %s:%d，令牌已更新；各设备的 secretKey 会按新令牌重算",
                     g_srvHost, g_srvPort);
            g_setOK = TRUE;
            g_setDone = TRUE;
            return 0;
        }
        if (id == ID_SET_CANCEL || id == IDCANCEL) { g_setDone = TRUE; return 0; }
        break;
    }
    case WM_CLOSE:
        g_setDone = TRUE;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 只建窗并显示，不跑消息循环（--openset 截图/联调用） */
static BOOL settings_create(HWND owner)
{
    static BOOL cls_done;
    HINSTANCE hi = GetModuleHandleW(NULL);

    if (!cls_done) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = set_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"IVSET";
        RegisterClassExW(&wc);
        cls_done = TRUE;
    }

    RECT ro;
    GetWindowRect(owner, &ro);
    int ww = 460, wh = 260;
    int x = ro.left + ((ro.right - ro.left) - ww) / 2;
    int y = ro.top + ((ro.bottom - ro.top) - wh) / 2;

    g_setDone = FALSE;
    g_setOK = FALSE;

    g_hSet = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                             L"IVSET", L"通道服务器设置",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, ww, wh,
                             owner, NULL, hi, NULL);
    if (!g_hSet) return FALSE;

    HWND lb1 = CreateWindowExW(0, L"STATIC", L"frps 地址", WS_CHILD | WS_VISIBLE,
                               16, 18, 90, 20, g_hSet, NULL, NULL, NULL);
    g_hSetHost = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_srvHost,
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 110, 15, 320, 24, g_hSet, (HMENU)ID_SET_HOST, NULL, NULL);
    HWND lb2 = CreateWindowExW(0, L"STATIC", L"端口", WS_CHILD | WS_VISIBLE,
                               16, 54, 90, 20, g_hSet, NULL, NULL, NULL);
    wchar_t pt[16];
    swprintf(pt, 16, L"%d", g_srvPort);
    g_hSetPort = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pt,
                                 WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                 110, 51, 90, 24, g_hSet, (HMENU)ID_SET_PORT, NULL, NULL);
    HWND lb3 = CreateWindowExW(0, L"STATIC", L"令牌", WS_CHILD | WS_VISIBLE,
                               16, 90, 90, 20, g_hSet, NULL, NULL, NULL);
    g_hSetTok = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_token,
                                WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
                                110, 87, 320, 24, g_hSet, (HMENU)ID_SET_TOK, NULL, NULL);

    HWND hint = CreateWindowExW(0, L"STATIC",
                                L"令牌要和服务器 frps 的 auth.token、设备上的一致。\n"
                                L"保存后，secretKey 按「SN+令牌」重新推导。",
                                WS_CHILD | WS_VISIBLE, 16, 122, 420, 40,
                                g_hSet, (HMENU)ID_SET_HINT, NULL, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              250, 178, 90, 30, g_hSet, (HMENU)ID_SET_OK, NULL, NULL);
    HWND cc = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                              350, 178, 90, 30, g_hSet, (HMENU)ID_SET_CANCEL, NULL, NULL);

    HWND ks[] = { lb1, g_hSetHost, lb2, g_hSetPort, lb3, g_hSetTok, hint, ok, cc };
    for (int i = 0; i < 9; i++)
        SendMessageW(ks[i], WM_SETFONT, (WPARAM)g_font, TRUE);

    EnableWindow(owner, FALSE);
    ShowWindow(g_hSet, SW_SHOW);
    SetFocus(g_hSetHost);
    SendMessageW(g_hSetHost, EM_SETSEL, 0, -1);
    return TRUE;
}

/* 模态：建窗 → 自转消息循环 → 关窗 */
static BOOL settings_dialog(HWND owner)
{
    if (!settings_create(owner)) return FALSE;

    MSG msg;
    while (!g_setDone && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hSet, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    DestroyWindow(g_hSet);
    g_hSet = NULL;
    return g_setOK;
}

/* 起 frpc（若 16000 已在监听就认为已经起来了） */
static BOOL ensure_frpc(void)
{
    /* 已经在跑的 frpc 是不是按「当前这一代」visitor.toml 起的？
     * 换 SN / 换服务器都会让 visitor.toml 换代（g_frpcGen++），
     * 老进程必须停掉重拉 —— 否则它一直按旧 SN 连，现象是"切了没用"。 */
    if (g_frpcProc) {
        DWORD c = 0;
        if (GetExitCodeProcess(g_frpcProc, &c) && c == STILL_ACTIVE) {
            if (g_frpcProcGen == g_frpcGen && port_open(FRP_PORT)) return TRUE;
            kill_frpc();
        } else {
            CloseHandle(g_frpcProc);
            g_frpcProc = NULL;
            g_frpcProcGen = -1;
        }
    }
    if (port_open(FRP_PORT)) {
        /* 16000 被占着但不是我们起的（上次崩了/强杀留下的），它吃的是旧 toml */
        logw(L"16000 已被占用但不是我起的 frpc，先清掉再按当前 SN 重拉");
        kill_frpc();
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hlog = CreateFileW(g_frpcLog, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hlog;
    si.hStdError = hlog;
    si.hStdInput = INVALID_HANDLE_VALUE;

    wchar_t cmd[1024];
    swprintf(cmd, 1024, L"\"%s\" -c \"%s\"", g_frpc, g_toml);
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, g_dir, &si, &pi);
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    if (!ok) { logw(L"[X] 起不来 frpc.exe（%lu）", GetLastError()); return FALSE; }

    g_frpcProc = pi.hProcess;
    g_frpcPid = pi.dwProcessId;
    g_frpcProcGen = g_frpcGen;      /* 记下这代进程吃的是哪一版配置 */
    CloseHandle(pi.hThread);
    logw(L"启动 frpc（pid %lu，SN=%s -> %s:%d），等通道就绪 ...",
         g_frpcPid, g_sn, g_srvHost, g_srvPort);

    for (int i = 0; i < 60; i++) {
        if (port_open(FRP_PORT)) {
            logw(L"通道就绪（本地 %d 已监听）", FRP_PORT);
            /* 让底部状态行马上从"启动中"变"运行中"，不用等整轮干完 */
            if (g_hMain) PostMessageW(g_hMain, WM_BUSY, 0, 0);
            return TRUE;
        }
        Sleep(200);
    }
    logw(L"[X] 20 秒内 %d 没起来，看 %s", FRP_PORT, g_frpcLog);
    if (g_hMain) PostMessageW(g_hMain, WM_BUSY, 0, 0);
    return FALSE;
}

/* 关掉当前隧道 */
static void kill_tunnel(void)
{
    if (g_sshProc) {
        TerminateProcess(g_sshProc, 0);
        CloseHandle(g_sshProc);
        g_sshProc = NULL;
        logw(L"已断开隧道");
    }
    if (g_curCh >= 0) logw(L"（第 %d 路隧道已关闭）", g_curCh + 1);
    if (g_webOpen) logw(L"（设备 Web 隧道已关闭）");
    g_curCh = -1;
    g_webOpen = FALSE;
}

/* ------------------------------------------------ 摄像机列表 / 连通性 */

/* 只认 a.b.c.d 形式的 IPv4：空串、"(unset)"、0.0.0.0 都不算一路 */
static BOOL ipv4_ok(const char *s)
{
    int dots = 0, seg = 0, digits = 0;
    if (!s || !*s) return FALSE;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            seg = seg * 10 + (*p - '0');
            if (++digits > 3) return FALSE;
        } else if (*p == '.') {
            if (digits == 0 || seg > 255) return FALSE;
            dots++;
            seg = 0;
            digits = 0;
        } else {
            return FALSE;
        }
    }
    if (dots != 3 || digits == 0 || seg > 255) return FALSE;
    return strcmp(s, "0.0.0.0") != 0;
}

static void parse_cams(const char *txt)
{
    EnterCriticalSection(&g_cs);
    g_ncams = 0;
    const char *p = txt;
    while (*p && g_ncams < MAX_CAMS) {
        int idx = atoi(p);
        const char *sp1 = strchr(p, ' ');
        if (idx <= 0 || !sp1) break;
        const char *ip = sp1 + 1;
        while (*ip == ' ') ip++;
        const char *sp2 = strchr(ip, ' ');
        if (!sp2) break;
        const char *vend = sp2 + 1;
        while (*vend == ' ') vend++;
        const char *sp3 = strchr(vend, ' ');
        const char *nl = strchr(p, '\n');
        if (!nl) nl = p + strlen(p);

        Cam *c = &g_cams[g_ncams++];
        ZeroMemory(c, sizeof(*c));
        c->idx = idx;
        int n = (int)(sp2 - ip);
        if (n > 39) n = 39;
        memcpy(c->ip, ip, n);
        c->ip[n] = 0;
        if (!ipv4_ok(c->ip)) {      /* 不是有效 IPv4：这一行不算一路 */
            g_ncams--;
            p = nl;
            while (*p == '\r' || *p == '\n') p++;
            continue;
        }
        if (sp3 && sp3 < nl) {
            int m = (int)(sp3 - vend);
            if (m > 15) m = 15;
            memcpy(c->vendor, vend, m);
            c->vendor[m] = 0;
            const char *s2 = sp3 + 1;
            while (*s2 == ' ') s2++;
            int k = (int)(nl - s2);
            if (k < 0) k = 0;
            if (k > 15) k = 15;
            memcpy(c->stream, s2, k);
            c->stream[k] = 0;
        }
        c->state = -1;
        p = nl;
        while (*p == '\r' || *p == '\n') p++;
    }
    LeaveCriticalSection(&g_cs);
}

static int probe_cam(const char *ip)
{
    wchar_t pre[1024], cmd[1600];
    ssh_prefix(pre, 0);
    swprintf(cmd, 1600,
             L"%s \"python3 /opt/ivsbox/scripts/checkcam.py %s\"", pre, W(ip));
    char out[256];
    int rc = run_capture(cmd, out, sizeof(out), 25000);
    if (rc == -1 || rc == -2) return -1;
    if (strstr(out, "OK"))     return 0;
    if (strstr(out, "NG:PING")) return 1;
    if (strstr(out, "NG:PORT")) return 2;
    return -1;
}

/* ------------------------------------------------------------ 界面刷新 */

static void list_set(int row, int col, const wchar_t *text)
{
    if (g_quiet || !g_hList) return;
    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.iSubItem = col;
    it.pszText = (LPWSTR)text;
    SendMessageW(g_hList, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

static void fill_list(void)
{
    if (g_quiet || !g_hList) return;
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_ncams; i++) {
        wchar_t n[16], ip[64], st[64], url[64];
        swprintf(n, 16, L"%d", g_cams[i].idx);
        mbstowcs(ip, g_cams[i].ip, 63); ip[63] = 0;
        const wchar_t *s = L"探测中 ...";
        if (g_cams[i].state == 0) s = L"可连接";
        else if (g_cams[i].state == 1) s = L"网络不通";
        else if (g_cams[i].state == 2) s = L"80 端口不通";
        wcscpy(st, s);
        swprintf(url, 64, L"http://127.0.0.1:%d", 9089 + g_cams[i].idx);

        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = n;
        SendMessageW(g_hList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        list_set(i, 3, url);
        list_set(i, 1, ip);
        list_set(i, 2, st);
        if (g_cams[i].state == 0) list_set(i, 4, L"");
    }
    LeaveCriticalSection(&g_cs);
}

static void update_status_line(void)
{
    if (g_quiet) return;
    wchar_t b[512], r[384];
    if (g_webOpen)
        swprintf(r, 384, L"已连接 设备 Web -> http://127.0.0.1:%d   frpc：运行中",
                 WEB_PORT);
    else if (g_curCh >= 0)
        swprintf(r, 384, L"已连接第 %d 路 -> http://127.0.0.1:%d   frpc：运行中",
                 g_curCh + 1, 9089 + g_curCh + 1);
    else
        /* 启动/刷新那一轮里 frpc 还没监听上，这时写"未启动"会让人以为坏了：
         * 界面一打开就是这句话，而实际上 worker 正在拉通道。 */
        swprintf(r, 384, L"未连接   frpc：%s",
                 port_open(FRP_PORT) ? L"运行中" : (g_busy ? L"启动中…" : L"未启动"));
    /* 这一行只说"画面隧道"：设备通道开没开在上面那条摘要里，不要混在一起
     * （"通道：未连接" 曾被看成"设备通道没开"，两回事）。 */
    if (g_devUp && g_devIp[0])
        /* 设备定位之后，IP/SN 都在上面那条摘要里，这行只说画面隧道 */
        swprintf(b, 512, L"画面隧道：%s", r);
    else
        swprintf(b, 512, L"当前设备 %s   画面隧道：%s", g_sn, r);
    if (g_note[0]) {
        wcscat(b, L"   ← ");
        wcscat(b, g_note);
    }
    if (g_hStatus) SetWindowTextW(g_hStatus, b);

    if (g_hLblSrv) {
        /* 这一格很窄（左边是按钮），只放地址；令牌是否为空由通道摘要那行说 */
        wchar_t s[256];
        swprintf(s, 256, L"%s:%d%s", g_srvHost, g_srvPort,
                 tok_ok(g_token) ? L"" : L" ⚠令牌空");
        SetWindowTextW(g_hLblSrv, s);
    }
}

static void set_busy(BOOL b)
{
    g_busy = b;
    if (g_quiet) return;
    EnableWindow(g_hBtnConn, !b);
    EnableWindow(g_hBtnDisc, !b);
    EnableWindow(g_hBtnWeb, !b);
    EnableWindow(g_hBtnDl, !b);
    EnableWindow(g_hBtnRef, !b);
}

/* --------------------------------------------------------------- 工作线程 */

/* 在 frpc.log 末尾找一句话。
 * 为什么要这个：frpc 连不上时自己的日志明明写着原因（令牌不一致 / 服务器拒登），
 * 而界面上原来只给"网络通吗、设备通电吗、通道开了吗"这三条通用话术 —— 现场照着
 * 查一圈全是对的，最后才发现是令牌只改了两处（2026-11-05 08:10 就是这样）。
 * 只读最后 8 KiB：一次连接尝试的日志很短，没必要整份读。 */
static BOOL frpc_log_has(const char *needle)
{
    HANDLE h = CreateFileW(g_frpcLog, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return FALSE; }

    const DWORD win = 8192;
    DWORD toread = (sz.QuadPart > (LONGLONG)win) ? win : (DWORD)sz.QuadPart;
    if (sz.QuadPart > (LONGLONG)win) {
        LARGE_INTEGER off;
        off.QuadPart = sz.QuadPart - (LONGLONG)win;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    }

    char *buf = (char *)malloc(toread + 1u);
    if (!buf) { CloseHandle(h); return FALSE; }
    DWORD got = 0;
    ReadFile(h, buf, toread, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    BOOL hit = (strstr(buf, needle) != NULL);
    free(buf);
    return hit;
}

static void do_probe_all(void)
{
    if (!ensure_frpc()) {
        /* 先问 frpc 自己，别急着抛通用话术：日志里写清了是"令牌不是我这个"还是"连不上"。 */
        if (frpc_log_has("token in login doesn't match")) {
            logw(L"[X] 通道没起来：令牌和服务器不一致");
            logw(L"    frpc 原话：token in login doesn't match token from configuration");
            logw(L"    也就是说 %s:%d 那台 frps 的 auth.token，与这里填的不是同一个值。",
                 g_srvHost, g_srvPort);
            logw(L"    令牌必须三处同值：服务器 frps / 设备「远程通道」/ 本程序「服务器设置」。");
            logw(L"    只改了一两处，两边都登不上（设备那边也会报同一句）。");
            wcscpy(g_note, L"令牌与服务器不一致");
            update_status_line();
        } else if (frpc_log_has("token in login")) {
            logw(L"[X] 通道没起来：%s:%d 拒绝了本机的登录（令牌相关）",
                 g_srvHost, g_srvPort);
            wcscpy(g_note, L"服务器拒绝登录");
            update_status_line();
        } else if (frpc_log_has("start visitor success")) {
            logw(L"[X] 通道没起来：frpc 说 visitor 建好了，但本地 %d 没监听", FRP_PORT);
            logw(L"    上一次退出残留的 frpc 占着端口？关掉本程序重开一次。");
            wcscpy(g_note, L"本地端口被占用");
            update_status_line();
        } else {
            logw(L"[X] 通道没起来，先按下面步骤查：");
            logw(L"    1. 本机能上网吗（frpc 要连 %s:%d，当前 SN=%s）",
                 g_srvHost, g_srvPort, g_sn);
            logw(L"    2. 设备通电、4G 有信号吗");
            logw(L"    3. 设备上执行 python3 /opt/ivsbox/scripts/tun.py on");
            wcscpy(g_note, L"通道没起来（见日志）");
            update_status_line();
        }
        logw(L"    frpc 自己的日志：%s", g_frpcLog);
        return;
    }
    logw(L"读取摄像机列表 ...");
    wchar_t pre[1024], cmd[1400];
    ssh_prefix(pre, 0);
    swprintf(cmd, 1400, L"%s \"python3 /opt/ivsbox/scripts/cams.py\"", pre);
    char out[2048];
    int rc = run_capture(cmd, out, sizeof(out), 20000);
    if (rc == 255) {
        /* frpc 刚开始监听的那一两秒，stcp 访客会话还没建好，第一下 ssh 常常是
         * "Connection reset"。等 1.2 s 重试一次，别让现场以为设备坏了。 */
        logw(L"    第一下 ssh 没成，1.2 秒后重试一次（访客会话刚建立）...");
        Sleep(1200);
        out[0] = 0;
        rc = run_capture(cmd, out, sizeof(out), 20000);
    }
    if (rc != 0 || !out[0]) {
        /* 这里以前写死 .200/.106/.108 兜底：设备只配了 2 路时，界面上会凭空
         * 多出第 3 路，还会去 ping 一个根本没配的地址。宁可空着也不能编。 */
        if (rc == 255) {
            logw(L"[X] 连不上设备（ssh 失败），摄像机列表为空");
            /* ssh 自己的那句话才是线索（Connection reset / Permission denied /
             * kex_exchange_identification…），别只报"失败" */
            {
                char fl[256];
                int n = 0;
                while (out[n] && out[n] != '\r' && out[n] != '\n' && n < 255) n++;
                memcpy(fl, out, (size_t)n);
                fl[n] = 0;
                if (n) logw(L"    ssh 原话：%s", W(fl));
            }
            if (g_devUp)
                logw(L"    设备 Web 那边是通的，说明网线/口令都没问题；这一半走的是隧道"
                     L"（stcp→设备 22），失败点在这条路上");
            logw(L"    按这三条查：");
            logw(L"    1. 设备上的 frpc 真在跑吗（设备日志 /mnt/UDISK/log/frpc.log 有没有"
                 L" login to server success）");
            logw(L"    2. 设备通道%s（设备 Web 读到的就是这个状态）",
                 g_devUp ? (g_tunOn ? L"已开着，那问题多半在令牌不一致" : L"是关着的 —— 点「开通道」")
                         : L"状态未知，先点「连接设备」");
            logw(L"    3. 设备 /root/.ssh/authorized_keys 里有没有本程序的公钥（DEPLOY.txt）");
        } else {
            logw(L"[X] ssh 通了，但设备上的 cams.py 没跑成（rc=%d），列表为空", rc);
            logw(L"    设备上需要 /opt/ivsbox/scripts/cams.py（DEPLOY.txt 那一步）");
        }
        logw(L"    不做任何猜测性填充：显示几路就是设备上真配了几路。");
        wcscpy(g_note, L"读不到设备配置（见日志）");
        update_status_line();
        EnterCriticalSection(&g_cs);
        g_ncams = 0;
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hMain, WM_CAMS_READY, 0, 0);
        return;
    }
    parse_cams(out);
    EnterCriticalSection(&g_cs);
    int n = g_ncams;
    LeaveCriticalSection(&g_cs);
    if (n == 0) {
        logw(L"[!] 设备上没有任何一路摄像机配了 IP（Web「摄像机」页填好 IP 再点刷新）");
        wcscpy(g_note, L"设备未配摄像机 IP");
        update_status_line();
        PostMessageW(g_hMain, WM_CAMS_READY, 0, 0);
        return;
    }
    g_note[0] = 0;
    logw(L"设备上共 %d 路摄像机，开始探测连通性 ...", n);
    PostMessageW(g_hMain, WM_CAMS_READY, 0, 0);

    for (int i = 0; i < n; i++) {
        char ip[40];
        EnterCriticalSection(&g_cs);
        strcpy(ip, g_cams[i].ip);
        LeaveCriticalSection(&g_cs);
        int st = probe_cam(ip);
        EnterCriticalSection(&g_cs);
        g_cams[i].state = st;
        LeaveCriticalSection(&g_cs);
        PostMessageW(g_hMain, WM_CAM_STATE, (WPARAM)i, (LPARAM)st);
        const wchar_t *t = st == 0 ? L"可连接" :
                           st == 1 ? L"网络不通（不同网段/没接线/没通电）" :
                           st == 2 ? L"ping 通但 80 端口不通" : L"探测失败";
        logw(L"  第 %d 路 %s -> %s", i + 1, W(ip), t);
    }
    logw(L"探测完成。不通的那几路连不上的原因在摄像机侧或网段，先按提示查线。");
}

/* 起一条 ssh -L 隧道（本地 port -> 设备 ip:80）；成功返回 TRUE */
static BOOL tunnel_start(const char *ip, int port)
{
    kill_tunnel();
    Sleep(300);

    wchar_t pre[1024], cmd[1600];
    ssh_prefix(pre, 0);
    swprintf(cmd, 1600,
             L"%s -o ExitOnForwardFailure=yes -L %d:%s:80 -N %s",
             pre, port, W(ip), FRP_SERVER);

    wchar_t logp[MAX_PATH + 16];
    swprintf(logp, MAX_PATH + 16, L"%s\\tunnel.log", g_dir);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hlog = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hlog;
    si.hStdError = hlog;
    si.hStdInput = INVALID_HANDLE_VALUE;

    wchar_t *cc = _wcsdup(cmd);
    BOOL ok = CreateProcessW(NULL, cc, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, g_dir, &si, &pi);
    free(cc);
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    if (!ok) { logw(L"[X] 起不来 ssh（%lu）", GetLastError()); return FALSE; }

    g_sshProc = pi.hProcess;
    g_sshPid = pi.dwProcessId;
    CloseHandle(pi.hThread);

    BOOL up = FALSE;
    for (int i = 0; i < 50; i++) {
        if (port_open(port)) { up = TRUE; break; }
        DWORD c = 0;
        if (GetExitCodeProcess(pi.hProcess, &c) && c != STILL_ACTIVE) break;
        Sleep(200);
    }
    if (!up) {
        char err[600] = "";
        HANDLE h = CreateFileW(logp, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD r = 0;
            ReadFile(h, err, sizeof(err) - 1, &r, NULL);
            err[r] = 0;
            CloseHandle(h);
        }
        logw(L"[X] 隧道没起来。ssh 输出：%s", W(err));
        TerminateProcess(g_sshProc, 0);
        CloseHandle(g_sshProc);
        g_sshProc = NULL;
        if (!g_quiet)
            MessageBoxW(g_hMain,
                        L"隧道没起来。多半是设备侧通道没开或 4G 掉线。\n\n"
                        L"设备上执行：python3 /opt/ivsbox/scripts/tun.py on\n"
                        L"再看一下本窗口底部日志和 tunnel.log。",
                        L"通道不通", MB_ICONERROR | MB_OK);
        return FALSE;
    }
    return TRUE;
}

static BOOL do_connect(int ch, BOOL open_browser)
{
    EnterCriticalSection(&g_cs);
    if (ch < 0 || ch >= g_ncams) { LeaveCriticalSection(&g_cs); return FALSE; }
    char ip[40];
    strcpy(ip, g_cams[ch].ip);
    LeaveCriticalSection(&g_cs);
    int port = 9089 + ch + 1;

    logw(L"---- 第 %d 路 %s ----", ch + 1, W(ip));
    if (!ensure_frpc()) return FALSE;

    logw(L"预检：从设备侧探测 %s ...", W(ip));
    int st = probe_cam(ip);
    if (st != 0) {
        const wchar_t *why = st == 1 ? L"设备 ping 不通这个地址（网段不同 / 没接线 / 没通电）"
                                     : st == 2 ? L"ping 通但 80 端口连不上（服务没起 / 端口不是 80）"
                                               : L"探测本身失败（通道或脚本问题）";
        logw(L"[X] 网络故障：这台摄像机从设备侧连不上");
        logw(L"    原因：%s", why);
        logw(L"    设备 eth0 是 192.168.1.120/24，配成别的网段的地址设备根本到不了");
        logw(L"    这次没有建立隧道，本地端口 %d 也没被占用", port);
        if (!g_quiet) {
            wchar_t m[1024];
            swprintf(m, 1024,
                     L"这台摄像机从设备侧连不上，没有建立隧道。\n\n"
                     L"目标：%s : 80\n原因：%s\n\n"
                     L"按顺序查：\n"
                     L"1. 摄像机通电了吗？网口指示灯亮不亮？\n"
                     L"2. 它和设备在同一网段吗？设备 eth0 = 192.168.1.120/24\n"
                     L"3. 设备 Web 里这条路填的 IP 和摄像机真实 IP 一致吗？\n"
                     L"4. 在设备上手工验证：ping %s",
                     W(ip), why, W(ip));
            MessageBoxW(g_hMain, m, L"网络故障", MB_ICONWARNING | MB_OK);
        }
        return FALSE;
    }
    logw(L"预检：通，开始转发");

    if (!tunnel_start(ip, port)) return FALSE;

    g_curCh = ch;
    int code = http_code(port);
    logw(L"[OK] http://127.0.0.1:%d 已通（首页 code=%d）", port, code);
    if (!g_quiet) {
        update_status_line();
        wchar_t u[64];
        swprintf(u, 64, L"http://127.0.0.1:%d", port);
        if (open_browser) ShellExecuteW(NULL, L"open", u, NULL, NULL, SW_SHOWNORMAL);
    }
    return TRUE;
}

/* 设备本机 Web（走同一条通道，转 18080 -> 设备 127.0.0.1:80） */
static void do_web(void)
{
    logw(L"---- 设备 Web ----");
    if (!ensure_frpc()) return;
    if (!tunnel_start("127.0.0.1", WEB_PORT)) return;
    g_webOpen = TRUE;
    g_curCh = -1;
    int code = http_code(WEB_PORT);
    logw(L"[OK] http://127.0.0.1:%d 已通（首页 code=%d）", WEB_PORT, code);
    if (!g_quiet) {
        update_status_line();
        wchar_t u[64];
        swprintf(u, 64, L"http://127.0.0.1:%d", WEB_PORT);
        ShellExecuteW(NULL, L"open", u, NULL, NULL, SW_SHOWNORMAL);
    }
}

/* ======================================================================
 * frps 侧改令牌（只在「连服务器一起改」时用到）
 * ----------------------------------------------------------------------
 * 需要 plink.exe（PuTTY）＋ root 口令；没有就如实说"没法改"，不要假装成功。
 * 脚本内容与「运维箱通道配置」的 srv_script 一致：备份 → 改 auth.token 与
 * webServer.password → 重启 frps → 自证 7000 在听。
 * ==================================================================== */
#define ECS_USER     L"root"
#define ECS_HOSTKEY  L"ssh-ed25519 255 SHA256:Uy99qmPWoPgHefIFj1Am23sr0QVoC8RnpS43+ENUID4"
#define PLINK_PATH   L"C:\\Program Files\\PuTTY\\plink.exe"

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
    wcscat(o, L"grep -h -E '^(auth\\.token|webServer\\.password)' "
              L"/etc/frp/frps.toml /etc/frp/visitor.toml\n");
    wcscat(o, L"systemctl restart frps\n");
    wcscat(o, L"systemctl restart frpv 2>/dev/null || true\n");
    wcscat(o, L"sleep 2\n");
    wcscat(o, L"systemctl is-active frps\n");
    wcscat(o, L"ss -lntp 2>/dev/null | grep -E ':(7000)[[:space:]]' || "
              L"echo \"WARN: 7000 not listening\"\n");
    o[cap - 1] = 0;
}

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

/* 写临时脚本 → plink -m 跑 → 删脚本（躲开命令行引号/换行那一摊） */
static BOOL srv_apply(const wchar_t *tok, const wchar_t *pw, wchar_t *err, int errcap)
{
    wchar_t plink[MAX_PATH], script[4096], tmpdir[MAX_PATH], tmpf[MAX_PATH], cmdline[1600];
    char *u8;
    int len, rc;
    static char out[32 * 1024];
    HANDLE h;
    DWORD wr = 0;
    BOOL ok = FALSE;

    if (!plink_find(plink, MAX_PATH)) {
        swprintf(err, (size_t)errcap,
                 L"没找到 plink.exe（%s）—— 无法自动改服务器；请找台装着 PuTTY 的机器，"
                 L"或手工执行服务器命令", PLINK_PATH);
        return FALSE;
    }
    srv_script(tok, script, 4096);
    u8 = (char *)malloc(16384);
    if (!u8) { swprintf(err, (size_t)errcap, L"内存不足"); return FALSE; }
    to_utf8(script, u8, 16384);
    len = (int)strlen(u8);

    GetTempPathW(MAX_PATH, tmpdir);
    swprintf(tmpf, MAX_PATH, L"%sivassist-%lu.sh", tmpdir, GetCurrentProcessId());
    h = CreateFileW(tmpf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(u8);
        swprintf(err, (size_t)errcap, L"写不了临时脚本 %s", tmpf);
        return FALSE;
    }
    {
        char *lf = (char *)malloc((size_t)len + 1);
        int i, n = 0;
        if (!lf) { CloseHandle(h); free(u8); return FALSE; }
        for (i = 0; i < len; i++) if (u8[i] != '\r') lf[n++] = u8[i];   /* 远端 sh 不认 CR */
        WriteFile(h, lf, (DWORD)n, &wr, NULL);
        free(lf);
    }
    CloseHandle(h);
    free(u8);

    swprintf(cmdline, 1600, L"\"%s\" -ssh -batch -pw \"%s\" -hostkey \"%s\" -m \"%s\" %s@%s",
             plink, pw, ECS_HOSTKEY, tmpf, ECS_USER, g_srvHost);
    rc = run_capture(cmdline, out, sizeof out, 90000);
    DeleteFileW(tmpf);

    if (rc == -1) {
        swprintf(err, (size_t)errcap, L"plink 起不来（%s）", plink);
    } else if (rc == -2) {
        swprintf(err, (size_t)errcap, L"plink 超过 90 s 没结束，已强杀");
    } else {
        char *p = out;
        while (p && *p) {
            char *e = strchr(p, '\n');
            wchar_t line[1600];
            if (e) *e = 0;
            {
                size_t ln = strlen(p);
                while (ln > 0 && (p[ln - 1] == '\r' || p[ln - 1] == ' ')) p[--ln] = 0;
            }
            if (*p) { u82w(p, line, 1600); logw(L"    %s", line); }
            if (!e) break;
            p = e + 1;
        }
        if (rc != 0) {
            swprintf(err, (size_t)errcap, L"服务器侧命令失败（rc=%d）—— 令牌没改成，服务未重启", rc);
        } else if (strstr(out, "WARN: 7000 not listening")) {
            swprintf(err, (size_t)errcap, L"frps 重启后 7000 没监听");
        } else {
            ok = TRUE;
        }
    }
    return ok;
}

/* ======================================================================
 * 三个"串起来"的动作（由 worker 调用，界面只管按按钮）
 * ==================================================================== */

/* 「连接设备」：定位 → 登录 → 读通道 → 对齐服务器地址 → 起本机通道 → 读摄像机 */
static void do_link_device(void)
{
    wchar_t pw[64], err[384] = L"";
    BOOL need_pass = FALSE;
    wchar_t sid[192], sn[64];

    GetWindowTextW(g_hEdtPw, pw, 63);
    pw[63] = 0;
    if (!pw[0]) {
        logw(L"[X] 口令是空的 —— 填设备 Web 的登录口令");
        return;
    }
    if (wcscmp(pw, g_devPass)) {
        wcscpy(g_devPass, pw);
        cfg_save();
    }

    logw(L"==== 连接设备 SN=%s ====", g_sn);
    if (!dev_locate(g_sn, pw, &need_pass, err, 384)) {
        logw(L"[X] %s", err);
        g_devUp = FALSE;
        if (need_pass) { g_needPassAct = 10; PostMessageW(g_hMain, WM_NEED_PASS, 0, 0); }
        PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
        return;
    }
    logw(L"[OK] 设备在 %s，SN 核对一致", g_devIp);
    logw(L"    通道%s；设备令牌 %s；服务器 %s:%d",
         g_tunOn ? L"开着" : L"关着",
         g_tunTokSet ? L"已设置" : L"未设置", g_tunSrv, g_tunSrvPort);

    PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);

    /* 1) 服务器地址对齐设备上报值：现场换过 frps 时，本机内置值就是错的，
     *    这一步让"用户什么都不用填"成立。设备没上报就保持原值。 */
    if (g_tunSrv[0] && g_tunSrvPort > 0 &&
        (wcscmp(g_tunSrv, g_srvHost) || g_tunSrvPort != g_srvPort)) {
        logw(L"    服务器地址与设备不一致，按设备的来：%s:%d（原来是 %s:%d）",
             g_tunSrv, g_tunSrvPort, g_srvHost, g_srvPort);
        wcscpy(g_srvHost, g_tunSrv);
        g_srvPort = g_tunSrvPort;
        toml_write();
        kill_frpc();
        cfg_save();
    }

    /* 2) 设备没令牌 ⇒ 一定连不上任何服务器，直接把本机这串下发过去并开通道。
     *    这是**唯一**会自动写令牌的情形：设备本来就没配置，写坏了也不比现状更差。 */
    if (!g_tunTokSet) {
        if (!dev_login(g_devIp, g_devUser, pw, sid, 192, NULL, err, 384)) {
            logw(L"[X] 重新登录失败：%s", err);
        } else if (!dev_set_token(g_devIp, sid, g_token, FALSE, err, 384)) {
            logw(L"[X] 下发令牌失败：%s", err);
        } else {
            logw(L"[OK] 设备原来没有令牌，已把本机的令牌下发过去（长度 %d）",
                 (int)wcslen(g_token));
            dev_read_tunnel(g_devIp, sid, sn, 64, NULL, err, 384);
        }
    } else if (g_tunTokLen != (int)wcslen(g_token)) {
        logw(L"[!] 设备令牌长度 %d，本机 %d —— 大概率不是同一串", g_tunTokLen, (int)wcslen(g_token));
        logw(L"    点「生成令牌…」→「连服务器一起改」，一次把三处改成同值；");
        logw(L"    或者确认服务器 frps 的 auth.token 后，手工在设备 Web 上填一致");
    }

    /* 3) 设备通道关着就打开（用户点「连接设备」的意思就是要用它） */
    if (g_devUp && !g_tunOn) {
        BOOL was = FALSE;
        int code = 0;
        if (dev_login(g_devIp, g_devUser, pw, sid, 192, NULL, err, 384)) {
            if (dev_switch(g_devIp, sid, TRUE, &was, &code, err, 384)) {
                logw(L"[OK] 设备通道原本关着，已打开");
                dev_read_tunnel(g_devIp, sid, sn, 64, NULL, err, 384);
            } else {
                logw(L"[X] 打开通道失败：%s", err);
            }
        }
    }

    /* 4) 本机 frpc + 摄像机列表（这一步通了才算真的能看画面） */
    do_probe_all();
    PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
}

/* 通道开/关 */
static void do_switch_channel(BOOL on)
{
    wchar_t sid[192], sn[64], err[384] = L"";
    BOOL was = FALSE, must = FALSE;
    int code = 0;

    if (!g_devIp[0]) {
        logw(L"[X] 还不知道设备在哪 —— 先点「连接设备」");
        return;
    }
    if (!dev_login(g_devIp, g_devUser, g_devPass, sid, 192, &must, err, 384)) {
        logw(L"[X] %s", err);
        return;
    }
    if (!dev_switch(g_devIp, sid, on, &was, &code, err, 384)) {
        logw(L"[X] %s", err);
        return;
    }
    if (was)
        logw(L"[OK] 设备通道本来就是%s的（e=-7）—— 这次没有重复动作", on ? L"开" : L"关");
    else
        logw(L"[OK] 已%s设备通道", on ? L"开" : L"关");
    if (dev_read_tunnel(g_devIp, sid, sn, 64, &must, err, 384))
        logw(L"    现在：通道%s", g_tunOn ? L"开着" : L"关着");
    PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
}

/* 改令牌：13=只本机 14=设备+本机 15=服务器+设备+本机 */
static void do_token_apply(int act)
{
    wchar_t sid[192], sn[64], err[512] = L"";

    if (!tok_ok(g_tokNew)) {
        logw(L"[X] 令牌不合法");
        return;
    }
    logw(L"==== 改令牌（%s）====",
         act == 13 ? L"只用于本机" : act == 14 ? L"设备 + 本机" : L"服务器 + 设备 + 本机");

    /* 15：先改服务器。服务器失败就整体中止 —— 设备改了而服务器没改，
     * 这台的通道会立刻死掉，比不改更糟。 */
    if (act == 15) {
        logw(L"改服务器 %s 的 auth.token 并重启 frps ...", g_srvHost);
        if (!srv_apply(g_tokNew, g_srvPw, err, 512)) {
            logw(L"[X] %s", err);
            logw(L"    已中止：设备与本机都**没有**改动，现场照旧可用");
            return;
        }
        logw(L"[OK] 服务器已换成新令牌并重启");
    }

    if (act >= 14) {
        if (!g_devIp[0]) {
            logw(L"[X] 还不知道设备在哪 —— 先点「连接设备」，再改令牌");
            return;
        }
        if (!dev_login(g_devIp, g_devUser, g_devPass, sid, 192, NULL, err, 512)) {
            logw(L"[X] %s", err);
            return;
        }
        /* **下发前必须重读一次通道状态**：`enable` 写死或取旧值都会出事 ——
         * 写 true 会把关着的远程门打开，写 false 会把开着的通道关掉。
         * 用户的第 1 次点击往往是「生成令牌」而不是「连接设备」，那时
         * g_tunOn 还是初始的 false。（这是本项目 11-05 的老血债，别重犯。） */
        if (dev_read_tunnel(g_devIp, sid, sn, 64, NULL, err, 512))
            logw(L"    下发前现状：通道%s", g_tunOn ? L"开着" : L"关着");
        else
            logw(L"    [!] 读不到现状（%s），按“保持关着”下发", err);
        if (!dev_set_token(g_devIp, sid, g_tokNew, g_tunOn, err, 512)) {
            logw(L"[X] 下发到设备失败：%s", err);
            return;
        }
        logw(L"[OK] 已写入设备（通道%s）", g_tunOn ? L"仍然开着" : L"仍然关着");
        dev_read_tunnel(g_devIp, sid, sn, 64, NULL, err, 512);
    }

    /* 本机最后改：先改本机再改设备的话，中途失败会让本机与设备不一致 */
    kill_frpc();
    wcscpy(g_token, g_tokNew);
    if (!toml_write()) {
        logw(L"[X] 写 visitor.toml 失败");
        return;
    }
    cfg_save();
    {
        wchar_t mask[64];
        mask_tok(g_token, mask, 64);
        logw(L"[OK] 本机已用新令牌（%s）", mask);
    }

    if (act == 13)
        logw(L"[!] 只改了本机：服务器与设备仍是旧令牌，现在**连不上**。"
             L"要恢复就再点「连接设备」，或在服务器上同步成这一串");

    /* 改完立刻验一次：起 frpc，成败就看 frpc.log */
    do_probe_all();
    PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
}

/* 只读探查：定位 + 登录 + 读通道状态，**什么都不改**。
 * 用在"先看看这台设备现在什么情况"和自动化验证里。 */
static void do_probe_device(void)
{
    wchar_t pw[64], err[384] = L"";
    BOOL need_pass = FALSE;

    GetWindowTextW(g_hEdtPw, pw, 63);
    pw[63] = 0;
    if (!pw[0]) { logw(L"[X] 没填口令"); return; }
    logw(L"==== 只读探查 SN=%s（不改设备任何配置）====", g_sn);
    if (!dev_locate(g_sn, pw, &need_pass, err, 384)) {
        logw(L"[X] %s", err);
        g_devUp = FALSE;
        if (need_pass) { g_needPassAct = 16; PostMessageW(g_hMain, WM_NEED_PASS, 0, 0); }
        PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
        return;
    }
    logw(L"[OK] 设备在 %s", g_devIp);
    logw(L"    通道%s；设备令牌 %s（%d 位）；服务器 %s:%d",
         g_tunOn ? L"开着" : L"关着", g_tunTokSet ? L"已设置" : L"未设置",
         g_tunTokLen, g_tunSrv, g_tunSrvPort);
    if (g_tunTokLen && g_tunTokLen != (int)wcslen(g_token))
        logw(L"[!] 设备令牌长度与本机（%d）不同 —— 不是同一串，通道连不上",
             (int)wcslen(g_token));
    PostMessageW(g_hMain, WM_LINK_STATE, 0, 0);
}

static DWORD WINAPI worker(LPVOID p)
{
    int action = (int)(INT_PTR)p;
    set_busy(TRUE);
    if (action == 1) {
        do_probe_all();
    } else if (action == 2) {
        int ch = g_selCh;
        do_connect(ch, TRUE);
    } else if (action == 3) {
        kill_tunnel();
        logw(L"已断开全部隧道（frpc 保留，下次选路更快）");
    } else if (action == 4) {
        kill_tunnel();
    } else if (action == 5) {
        do_web();
    } else if (action == 10) {
        do_link_device();
    } else if (action == 11) {
        do_switch_channel(TRUE);
    } else if (action == 12) {
        do_switch_channel(FALSE);
    } else if (action == 13 || action == 14 || action == 15) {
        do_token_apply(action);
    } else if (action == 16) {
        do_probe_device();
    }
    set_busy(FALSE);
    PostMessageW(g_hMain, WM_BUSY, 0, 0);
    return 0;
}

/* ------------------------------------------------------ 下载（SD 卡浏览） */
/* 一次 ssh 取回整个目录：大小|mtime|类型|名字（设备上是 GNU stat，支持 -c）
 * 这样做的好处：一个来回就能把文件列表连大小、时间一起带回来，
 * 不用每行再问一次设备；几百个文件也不慢。 */
typedef struct {
    wchar_t   name[256];
    BOOL      isdir;
    long long size;
    long long mtime;
} Entry;

static Entry   g_entries[1024];
static int     g_nentries;
static volatile BOOL g_dlCancel = FALSE;
static BOOL    g_dlBusy = FALSE;

static int     g_dlQty = 0;
static wchar_t g_dlQueue[512][520];
static long long g_dlAllBytes = 0, g_dlDoneBytes = 0;
static wchar_t g_dlCurName[256];

static HWND g_hDlSave, g_hDlProg;
static HWND g_hDlBtnGet, g_hDlBtnBrowse, g_hDlBtnAll, g_hDlBtnOpen;

static void fmt_size(long long v, wchar_t *o, int cap)
{
    if (v >= (1LL << 30))      swprintf(o, cap, L"%.2f GB", v / 1073741824.0);
    else if (v >= (1LL << 20)) swprintf(o, cap, L"%.2f MB", v / 1048576.0);
    else if (v >= 1024)        swprintf(o, cap, L"%.1f KB", v / 1024.0);
    else                       swprintf(o, cap, L"%lld B", v);
}

static void fmt_time(long long t, wchar_t *o, int cap)
{
    time_t tt = (time_t)t;
    struct tm *lt = localtime(&tt);
    if (!lt || t <= 0) { wcscpy(o, L"-"); return; }
    swprintf(o, cap, L"%04d-%02d-%02d %02d:%02d",
             lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
             lt->tm_hour, lt->tm_min);
}

/* 目录排前面，同类按名字排 */
static int ent_cmp(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->isdir != y->isdir) return x->isdir ? -1 : 1;
    return _wcsicmp(x->name, y->name);
}

/* 去掉尾部多余的 '/'（根目录保留） */
static void path_trim(wchar_t *p)
{
    size_t L = wcslen(p);
    while (L > 1 && p[L - 1] == L'/') p[--L] = 0;
    if (!L) wcscpy(p, L"/");
}

/* 读远端目录，填 g_entries，然后让界面刷新 */
static void dl_list(void)
{
    wchar_t pre[1024], cmd[2400];
    ssh_prefix(pre, 0);
    swprintf(cmd, 2400,
             L"%s \"cd '%s' 2>/dev/null || exit 97; "
             L"stat -c '%%s|%%Y|%%F|%%n' * 2>/dev/null; exit 0\"",
             pre, g_dlPath);
    static char out[192 * 1024];
    int rc = run_capture(cmd, out, sizeof(out), 25000);
    g_nentries = 0;
    if (rc == 97) logw(L"[X] 设备上没有这个目录：%s", g_dlPath);
    else if (rc != 0) logw(L"[X] 列目录失败（rc=%d）：%s", rc, g_dlPath);

    char *line = strtok(out, "\n");
    while (line && g_nentries < 1024) {
        char *b1 = strchr(line, '|');
        char *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
        char *b3 = b2 ? strchr(b2 + 1, '|') : NULL;
        if (b3) {
            *b1 = *b2 = *b3 = 0;
            Entry *e = &g_entries[g_nentries++];
            ZeroMemory(e, sizeof(*e));
            e->size  = _atoi64(line);
            e->mtime = _atoi64(b1 + 1);
            e->isdir = !strcmp(b2 + 1, "directory");
            char *nm = b3 + 1;
            while (*nm == ' ') nm++;
            mbstowcs(e->name, nm, 255);
            e->name[255] = 0;
        }
        line = strtok(NULL, "\n");
    }
    qsort(g_entries, g_nentries, sizeof(Entry), ent_cmp);
    logw(L"列目录 %s：%d 项", g_dlPath, g_nentries);
    /* 用 SendMessage 保证"填表"一定在本次读取之后完成（PostMessage 曾出现列表空白） */
    if (g_hDlg && IsWindow(g_hDlg)) SendMessageW(g_hDlg, WM_DL_LISTED, 0, 0);
}

static DWORD WINAPI dl_worker(LPVOID p)
{
    (void)p;
    if (!ensure_frpc()) {
        logw(L"[X] 通道没起来，读不了 SD 卡目录");
        return 0;
    }
    dl_list();
    return 0;
}

static void dl_layout(HWND h);

static void lv_set(HWND lv, int row, int col, const wchar_t *t)
{
    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.iSubItem = col;
    it.pszText = (LPWSTR)t;
    SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

static void dl_fill(void)
{
    if (!g_hDlList) return;
    SendMessageW(g_hDlList, LVM_DELETEALLITEMS, 0, 0);

    LVITEMW it;
    ZeroMemory(&it, sizeof(it));
    it.mask = LVIF_TEXT;
    it.iItem = 0;
    it.pszText = L"..";
    SendMessageW(g_hDlList, LVM_INSERTITEMW, 0, (LPARAM)&it);
    lv_set(g_hDlList, 0, 1, L"");
    lv_set(g_hDlList, 0, 2, L"");

    for (int i = 0; i < g_nentries; i++) {
        wchar_t nm[320], sz[48], tm[48];
        if (g_entries[i].isdir) swprintf(nm, 320, L"%s/", g_entries[i].name);
        else wcscpy(nm, g_entries[i].name);
        if (g_entries[i].isdir) wcscpy(sz, L"<文件夹>");
        else fmt_size(g_entries[i].size, sz, 48);
        fmt_time(g_entries[i].mtime, tm, 48);

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i + 1;
        it.pszText = nm;
        SendMessageW(g_hDlList, LVM_INSERTITEMW, 0, (LPARAM)&it);
        lv_set(g_hDlList, i + 1, 1, sz);
        lv_set(g_hDlList, i + 1, 2, tm);
    }
    wchar_t info[760];
    if (g_nentries == 0)
        swprintf(info, 760, L"远程目录 %s      （这个目录是空的）", g_dlPath);
    else
        swprintf(info, 760, L"远程目录 %s      共 %d 项", g_dlPath, g_nentries);
    SetWindowTextW(g_hDlInfo, info);

    {
        int cnt = (int)SendMessageW(g_hDlList, LVM_GETITEMCOUNT, 0, 0);
        wchar_t first[128] = L"";
        if (cnt > 0) {
            LVITEMW gi;
            ZeroMemory(&gi, sizeof(gi));
            gi.iSubItem = 0;
            gi.pszText = first;
            gi.cchTextMax = 128;
            SendMessageW(g_hDlList, LVM_GETITEMTEXTW, 0, (LPARAM)&gi);
        }
        logw(L"列表刷新：%d 行，首行=[%s]", cnt, first);
    }
    /* 填完才有竖向滚动条，列宽要按"已经有滚动条"的客户区重算一次 */
    if (g_hDlg && IsWindow(g_hDlg)) dl_layout(g_hDlg);
}

static void dl_up(void)
{
    path_trim(g_dlPath);
    if (!wcscmp(g_dlPath, L"/")) return;
    wchar_t *p = wcsrchr(g_dlPath, L'/');
    if (!p) return;
    if (p == g_dlPath) g_dlPath[1] = 0;
    else *p = 0;
    SetWindowTextW(g_hDlPath, g_dlPath);
    CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
}

/* ------------------------------------------------ 真正下载（ssh cat 落盘） */

static int fetch_one(const wchar_t *remote, const wchar_t *local)
{
    wchar_t pre[1024], cmd[2400];
    ssh_prefix(pre, 1);            /* 要 stdin 就不要 -n */
    swprintf(cmd, 2400, L"%s \"cat '%s'\"", pre, remote);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 256 * 1024)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    /* stderr 绝对不能并进 stdout：这条管道里的字节会直接写成 .mp4，
     * 一旦 ssh 报错（Connection reset / permission denied），错误文本就变成
     * 文件开头几十个字节，而旧代码只看"读完了"就报成功 —— 现场表现是
     * "下载完成"却打不开。 */
    HANDLE hnul = CreateFileW(L"NUL", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = (hnul == INVALID_HANDLE_VALUE) ? wr : hnul;
    si.hStdInput = INVALID_HANDLE_VALUE;

    wchar_t *cc = _wcsdup(cmd);
    BOOL ok = CreateProcessW(NULL, cc, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, g_dir, &si, &pi);
    free(cc);
    CloseHandle(wr);
    if (hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
    if (!ok) { CloseHandle(rd); return -1; }

    HANDLE hf = CreateFileW(local, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        logw(L"[X] 写不了本地文件（%lu）：%s", GetLastError(), local);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        return -1;
    }

    char buf[65536];
    DWORD got = 0;
    long long written = 0;
    for (;;) {
        if (!ReadFile(rd, buf, sizeof(buf), &got, NULL) || got == 0) break;
        DWORD w2 = 0;
        WriteFile(hf, buf, got, &w2, NULL);
        written += got;
        g_dlDoneBytes += got;
        int pct = g_dlAllBytes > 0 ? (int)(g_dlDoneBytes * 1000 / g_dlAllBytes) : 0;
        if (pct > 1000) pct = 1000;
        if (g_hDlg && IsWindow(g_hDlg))
            PostMessageW(g_hDlg, WM_DL_PROGRESS, (WPARAM)pct, 0);
        if (g_dlCancel) break;
    }
    CloseHandle(hf);
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);

    /* 读到 EOF 不等于"下完了"：链路中途断掉时 ssh 会以非 0 退出，
     * 此时本地文件是残缺的。必须报失败，不能算进"下载完成"。 */
    if (!g_dlCancel && code != 0) {
        logw(L"[X] 传输中断（ssh 退出码 %lu），本地只写了 %.2f MB，文件不完整",
             code, written / 1048576.0);
        return -1;
    }
    if (!g_dlCancel && written == 0) {
        logw(L"[X] 一个字节都没收到（设备上文件不存在？路径里有空格？）");
        return -1;
    }
    return 0;
}

static DWORD WINAPI dl_get_worker(LPVOID p)
{
    (void)p;
    g_dlBusy = TRUE;
    g_dlCancel = FALSE;
    CreateDirectoryW(g_dlSaveDir, NULL);

    DWORD t0 = GetTickCount();
    long long bytes0 = g_dlDoneBytes;
    int done = 0;

    for (int q = 0; q < g_dlQty && !g_dlCancel; q++) {
        const wchar_t *rem = g_dlQueue[q];
        const wchar_t *base = wcsrchr(rem, L'/');
        base = base ? base + 1 : rem;
        wcscpy(g_dlCurName, base);
        wchar_t local[MAX_PATH * 2];
        swprintf(local, MAX_PATH * 2, L"%s\\%s", g_dlSaveDir, base);
        logw(L"下载 %d/%d：%s", q + 1, g_dlQty, rem);
        if (fetch_one(rem, local) == 0) done++;
        else logw(L"[X] 这个失败了：%s", rem);
    }

    DWORD ms = GetTickCount() - t0;
    if (!ms) ms = 1;
    long long delta = g_dlDoneBytes - bytes0;
    logw(L"[OK] 下载结束 %d/%d 个文件，共 %.2f MB，用时 %.1f 秒（%.0f KB/s）-> %s",
         done, g_dlQty, delta / 1048576.0, ms / 1000.0,
         delta / 1024.0 * 1000.0 / ms, g_dlSaveDir);
    g_dlBusy = FALSE;
    if (g_hDlg && IsWindow(g_hDlg)) PostMessageW(g_hDlg, WM_DL_DONE, 0, 0);
    return 0;
}

/* 下载中：其它按钮停用，但"下载选中"变成"取消下载"保持可点 */
static void dl_enable(BOOL on)
{
    EnableWindow(g_hDlBtnBrowse, on);
    EnableWindow(g_hDlBtnAll, on);
    EnableWindow(g_hDlBtnOpen, on);
    EnableWindow(g_hDlList, on);
    SetWindowTextW(g_hDlBtnGet, on ? L"下载选中" : L"取消下载");
}

/* 下载当前选中的那些条目（dir 跳过） */
static void dl_download_selected(HWND h)
{
    if (g_dlBusy) { g_dlCancel = TRUE; return; }

    int n = (int)SendMessageW(g_hDlList, LVM_GETSELECTEDCOUNT, 0, 0);
    if (n <= 0) {
        MessageBoxW(h, L"先在列表里选中要下载的文件（可按住 Ctrl 多选）",
                    APP_TITLE, MB_ICONINFORMATION);
        return;
    }
    GetWindowTextW(g_hDlSave, g_dlSaveDir, MAX_PATH);
    if (!g_dlSaveDir[0]) {
        MessageBoxW(h, L"先选一个保存目录", APP_TITLE, MB_ICONINFORMATION);
        return;
    }

    g_dlQty = 0;
    g_dlAllBytes = 0;
    g_dlDoneBytes = 0;
    int row = -1;
    while ((row = (int)SendMessageW(g_hDlList, LVM_GETNEXTITEM,
                                    (WPARAM)row, MAKELPARAM(LVNI_SELECTED, 0))) >= 0) {
        if (row == 0) continue;              /* ".." */
        int i = row - 1;
        if (i < 0 || i >= g_nentries || g_entries[i].isdir) continue;
        swprintf(g_dlQueue[g_dlQty], 520, L"%s/%s", g_dlPath, g_entries[i].name);
        g_dlAllBytes += g_entries[i].size;
        if (++g_dlQty >= 512) break;
    }
    if (g_dlQty == 0) {
        MessageBoxW(h, L"选中的都是文件夹，文件夹要先双击进去再选文件",
                    APP_TITLE, MB_ICONINFORMATION);
        return;
    }
    logw(L"下载 %d 个文件到 %s", g_dlQty, g_dlSaveDir);
    dl_enable(FALSE);
    SendMessageW(g_hDlProg, PBM_SETPOS, 0, 0);
    CreateThread(NULL, 0, dl_get_worker, NULL, 0, NULL);
}

static void pick_folder(HWND owner)
{
    BROWSEINFOW bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择把文件下载到哪个文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t p[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, p)) {
        wcscpy(g_dlSaveDir, p);
        SetWindowTextW(g_hDlSave, g_dlSaveDir);
    }
    CoTaskMemFree(pidl);
}

/* ------------------------------------------------------------ 下载窗口 */

/* 自适应布局：窗口拖大时列表跟着长，名称列吃掉多余宽度 */
static void dl_layout(HWND h)
{
    if (!g_hDlPath || !g_hDlList) return;
    RECT r;
    GetClientRect(h, &r);
    int W = r.right, H = r.bottom;
    const int pad = 14, rowH = 26, bGo = 66, bUp = 80, bSub = 70, bOpen = 70;
    const int bAll = 70, bGet = 138, btnRowH = 30, progH = 18;

    int w1 = W - pad * 2 - bGo - bUp - 16;
    if (w1 < 120) w1 = 120;
    MoveWindow(g_hDlPath, pad, 13, w1, rowH, TRUE);
    MoveWindow(GetDlgItem(h, ID_DL_GO), pad + w1 + 8, 13, bGo, rowH, TRUE);
    MoveWindow(GetDlgItem(h, ID_DL_UP), pad + w1 + 8 + bGo + 8, 13, bUp, rowH, TRUE);

    MoveWindow(g_hDlInfo, pad, 46, W - pad * 2, 20, TRUE);

    int btnRow = H - 10 - btnRowH;
    int progY  = btnRow - 10 - progH;
    int saveY  = progY - 8 - rowH;
    int listTop = 70;
    int listH = saveY - 8 - listTop;
    if (listH < 60) listH = 60;

    MoveWindow(g_hDlList, pad, listTop, W - pad * 2, listH, TRUE);

    int saveEditW = W - pad * 2 - 62 - bSub - bOpen - 16;
    if (saveEditW < 100) saveEditW = 100;
    MoveWindow(GetDlgItem(h, ID_DL_SAVE_LBL), pad, saveY + 3, 60, 20, TRUE);
    MoveWindow(g_hDlSave, pad + 62, saveY, saveEditW, rowH, TRUE);
    MoveWindow(g_hDlBtnBrowse, pad + 62 + saveEditW + 8, saveY, bSub, rowH, TRUE);
    MoveWindow(g_hDlBtnOpen, W - pad - bOpen, saveY, bOpen, rowH, TRUE);

    MoveWindow(g_hDlProg, pad, progY, W - pad * 2, progH, TRUE);

    int tipW = W - pad * 2 - bAll - bGet - 16;
    if (tipW < 80) tipW = 80;
    MoveWindow(GetDlgItem(h, ID_DL_TIP), pad, btnRow + 6, tipW, 20, TRUE);
    MoveWindow(g_hDlBtnAll, W - pad - bGet - 8 - bAll, btnRow, bAll, btnRowH, TRUE);
    MoveWindow(g_hDlBtnGet, W - pad - bGet, btnRow, bGet, btnRowH, TRUE);

    /* 名称列按列表**客户区**算（要扣掉竖向滚动条的宽度，否则会多出横向滚动条） */
    RECT lr;
    GetClientRect(g_hDlList, &lr);
    int nameW = lr.right - 110 - 160 - 2;
    if (nameW < 160) nameW = 160;
    SendMessageW(g_hDlList, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(nameW, 0));
}

static LRESULT CALLBACK dl_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"远程目录：", WS_CHILD | WS_VISIBLE,
                        14, 16, 76, 22, h, NULL, NULL, NULL);
        g_hDlPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlPath,
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    94, 13, 452, 26, h, (HMENU)ID_DL_PATH, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"转到", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        554, 13, 66, 26, h, (HMENU)ID_DL_GO, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"上一级", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        628, 13, 80, 26, h, (HMENU)ID_DL_UP, NULL, NULL);

        g_hDlInfo = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    14, 46, 700, 20, h, (HMENU)ID_DL_INFO, NULL, NULL);

        g_hDlList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | LVS_REPORT |
                                    LVS_SHOWSELALWAYS,
                                    14, 70, 700, 336, h, (HMENU)ID_DL_LIST, NULL, NULL);
        SendMessageW(g_hDlList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        {
            LVCOLUMNW c;
            ZeroMemory(&c, sizeof(c));
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            const wchar_t *hdr[] = { L"名称", L"大小", L"修改时间" };
            const int wd[] = { 350, 110, 160 };
            for (int i = 0; i < 3; i++) {
                c.pszText = (LPWSTR)hdr[i];
                c.cx = wd[i];
                c.iSubItem = i;
                SendMessageW(g_hDlList, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
            }
        }

        CreateWindowExW(0, L"STATIC", L"保存到：", WS_CHILD | WS_VISIBLE,
                        14, 418, 60, 22, h, (HMENU)ID_DL_SAVE_LBL, NULL, NULL);
        g_hDlSave = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_dlSaveDir,
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    76, 415, 486, 26, h, (HMENU)ID_DL_SAVE, NULL, NULL);
        g_hDlBtnBrowse = CreateWindowExW(0, L"BUTTON", L"选择...",
                                         WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                         570, 415, 70, 26, h, (HMENU)ID_DL_BROWSE, NULL, NULL);
        g_hDlBtnOpen = CreateWindowExW(0, L"BUTTON", L"打开",
                                       WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       644, 415, 70, 26, h, (HMENU)ID_DL_OPEN, NULL, NULL);

        g_hDlProg = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                    WS_CHILD | WS_VISIBLE, 14, 452, 700, 18,
                                    h, (HMENU)ID_DL_PROG, NULL, NULL);
        SendMessageW(g_hDlProg, PBM_SETRANGE32, 0, 1000);

        CreateWindowExW(0, L"STATIC",
                        L"双击进入文件夹，双击下载文件；Ctrl 可多选",
                        WS_CHILD | WS_VISIBLE, 14, 486, 470, 20, h,
                        (HMENU)ID_DL_TIP, NULL, NULL);
        g_hDlBtnAll = CreateWindowExW(0, L"BUTTON", L"全选",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      498, 480, 70, 30, h, (HMENU)ID_DL_SELALL, NULL, NULL);
        g_hDlBtnGet = CreateWindowExW(0, L"BUTTON", L"下载选中",
                                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                      576, 480, 138, 30, h, (HMENU)ID_DL_GET, NULL, NULL);

        if (g_font) {
            HWND ks[] = { g_hDlPath, g_hDlInfo, g_hDlList, g_hDlSave };
            for (int i = 0; i < 4; i++)
                SendMessageW(ks[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        dl_layout(h);
        SetFocus(g_hDlList);
        return 0;
    }

    case WM_SIZE:
        dl_layout(h);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 660;
        mm->ptMinTrackSize.y = 470;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_DL_GO:
            GetWindowTextW(g_hDlPath, g_dlPath, 512);
            path_trim(g_dlPath);
            SetWindowTextW(g_hDlPath, g_dlPath);
            CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
            return 0;
        case ID_DL_UP:
            dl_up();
            return 0;
        case ID_DL_BROWSE:
            pick_folder(h);
            return 0;
        case ID_DL_OPEN:
            GetWindowTextW(g_hDlSave, g_dlSaveDir, MAX_PATH);
            CreateDirectoryW(g_dlSaveDir, NULL);
            ShellExecuteW(NULL, L"open", g_dlSaveDir, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        case ID_DL_GET:
            dl_download_selected(h);
            return 0;
        case ID_DL_SELALL: {
            SetFocus(g_hDlList);
            int cnt = (int)SendMessageW(g_hDlList, LVM_GETITEMCOUNT, 0, 0);
            for (int i = 0; i < cnt; i++) {
                LVITEMW st;
                ZeroMemory(&st, sizeof(st));
                st.state = LVIS_SELECTED;
                st.stateMask = LVIS_SELECTED;
                SendMessageW(g_hDlList, LVM_SETITEMSTATE, (WPARAM)i, (LPARAM)&st);
            }
            return 0;
        }
        case ID_DL_LIST:
            if (HIWORD(w) == LBN_DBLCLK) { /* ListView 不会走这里，留空 */ }
            return 0;
        case IDCANCEL:
            DestroyWindow(h);
            return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == ID_DL_LIST && nh->code == NM_DBLCLK) {
            int row = (int)SendMessageW(g_hDlList, LVM_GETNEXTITEM, (WPARAM)-1,
                                        MAKELPARAM(LVNI_SELECTED, 0));
            if (row < 0) return 0;
            if (row == 0) { dl_up(); return 0; }
            int i = row - 1;
            if (i < 0 || i >= g_nentries) return 0;
            if (g_entries[i].isdir) {
                wchar_t tmp[600];
                wcscpy(tmp, g_dlPath);
                swprintf(g_dlPath, 512, L"%s/%s", tmp, g_entries[i].name);
                path_trim(g_dlPath);
                SetWindowTextW(g_hDlPath, g_dlPath);
                CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
            } else {
                GetWindowTextW(g_hDlSave, g_dlSaveDir, MAX_PATH);
                g_dlQty = 1;
                swprintf(g_dlQueue[0], 520, L"%s/%s", g_dlPath, g_entries[i].name);
                g_dlAllBytes = g_entries[i].size;
                g_dlDoneBytes = 0;
                dl_enable(FALSE);
                SendMessageW(g_hDlProg, PBM_SETPOS, 0, 0);
                CreateThread(NULL, 0, dl_get_worker, NULL, 0, NULL);
            }
        }
        return 0;
    }

    case WM_DL_LISTED:
        dl_fill();
        return 0;

    case WM_DL_PROGRESS: {
        wchar_t b[200];
        int pct = (int)w;
        SendMessageW(g_hDlProg, PBM_SETPOS, pct, 0);
        swprintf(b, 200, L"正在下载 %s    %d%%", g_dlCurName, pct / 10);
        SetWindowTextW(g_hDlInfo, b);
        return 0;
    }

    case WM_DL_DONE:
        dl_enable(TRUE);
        SendMessageW(g_hDlProg, PBM_SETPOS, g_dlCancel ? 0 : 1000, 0);
        {
            wchar_t b[400];
            swprintf(b, 400, L"已下载到 %s", g_dlSaveDir);
            SetWindowTextW(g_hDlInfo, b);
        }
        return 0;

    case WM_CLOSE:
        g_dlCancel = TRUE;
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void open_download(void)
{
    if (g_hDlg && IsWindow(g_hDlg)) { SetForegroundWindow(g_hDlg); return; }
    g_hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"IVDLG",
                             L"下载 SD 卡文件",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 748, 560,
                             g_hMain, NULL, NULL, NULL);
    if (!g_hDlg) return;
    SetWindowTextW(g_hDlSave, g_dlSaveDir);
    CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
}

/* ------------------------------------------------------------- 主窗口 */

/* ======================================================================
 * 设备区/通道区的界面刷新
 * ==================================================================== */

static void update_tun_line(void)
{
    if (g_hLblTun) {
        wchar_t b[512];
        if (!g_devUp) {
            wcscpy(b, L"通道：未读取设备 —— 填 SN 和口令，点「连接设备」");
        } else {
            wchar_t kt[64];
            if (!g_tunTokSet) wcscpy(kt, L"未设置");
            else swprintf(kt, 64, L"%d 位", g_tunTokLen);
            swprintf(b, 512, L"通道：%s   ·   设备令牌 %s   ·   服务器 %s:%d",
                     g_tunOn ? L"开着" : L"关着", kt,
                     g_tunSrv[0] ? g_tunSrv : L"(未上报)",
                     g_tunSrvPort ? g_tunSrvPort : g_srvPort);
        }
        SetWindowTextW(g_hLblTun, b);
    }
    if (g_hLblDev) {
        wchar_t b[256];
        if (!g_devUp) {
            b[0] = 0;
        } else {
            /* 这行右边压着三个按钮，宽度有限：令牌一致时不写（摘要行已有"设备令牌 N 位"），
             * 只在**不一致**时才追加，别让它被按钮裁掉。 */
            swprintf(b, 256, L"设备 %s   ·   SN %s   ·   本机通道 %s",
                     g_devIp, g_sn,
                     port_open(FRP_PORT) ? L"已就绪" : L"未启动");
            if (!g_tunTokSet) wcscat(b, L"   ·   ⚠设备没有令牌");
            else if (g_tunTokLen != (int)wcslen(g_token))
                wcscat(b, L"   ·   ⚠令牌与设备不一致");
        }
        SetWindowTextW(g_hLblDev, b);
    }
}
/* 设备口令复杂度：与设备侧 iv_web_pass_rule() 对齐（8..12，含大写/小写/数字） */
static BOOL pass_rule_ok(const wchar_t *p)
{
    size_t n = p ? wcslen(p) : 0;
    int lo = 0, up = 0, dg = 0;
    if (n < 8 || n > 12) return FALSE;
    for (size_t i = 0; i < n; i++) {
        wchar_t c = p[i];
        if (c < 0x20 || c == 0x7f) return FALSE;
        if (c >= L'a' && c <= L'z') lo = 1;
        else if (c >= L'A' && c <= L'Z') up = 1;
        else if (c >= L'0' && c <= L'9') dg = 1;
    }
    return lo && up && dg;
}

/* ------------------------------------------------------ 通用"输入一行"对话框 */

static HWND  g_ask, g_askEdit, g_askLbl;
static BOOL  g_askOK, g_askPw;
static wchar_t *g_askOut;
static int    g_askCap;

static void ask_fit(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    int W = r.right;
    MoveWindow(g_askLbl, 14, 12, W - 28, 20, TRUE);
    MoveWindow(g_askEdit, 14, 38, W - 28, 24, TRUE);
    MoveWindow(GetDlgItem(h, ID_ASK_OK), W - 14 - 76 - 8 - 76, 74, 76, 28, TRUE);
    MoveWindow(GetDlgItem(h, ID_ASK_CANCEL), W - 14 - 76, 74, 76, 28, TRUE);
}

static LRESULT CALLBACK ask_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_askLbl = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                   14, 12, 400, 20, h, (HMENU)ID_ASK_HINT, NULL, NULL);
        g_askEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                    (g_askPw ? ES_PASSWORD : 0),
                                    14, 38, 400, 24, h, (HMENU)ID_ASK_EDIT, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        246, 74, 76, 28, h, (HMENU)ID_ASK_OK, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                        330, 74, 76, 28, h, (HMENU)ID_ASK_CANCEL, NULL, NULL);
        if (g_font) {
            HWND ks[] = { g_askLbl, g_askEdit, GetDlgItem(h, ID_ASK_OK),
                          GetDlgItem(h, ID_ASK_CANCEL) };
            for (int i = 0; i < 4; i++) SendMessageW(ks[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        SetFocus(g_askEdit);
        return 0;
    case WM_SIZE:
        ask_fit(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_ASK_OK) {
            g_askOK = TRUE;
            GetWindowTextW(g_askEdit, g_askOut, g_askCap - 1);
            g_askOut[g_askCap - 1] = 0;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == ID_ASK_CANCEL || LOWORD(w) == IDCANCEL) {
            g_askOK = FALSE;
            DestroyWindow(h);
            return 0;
        }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 弹一个"一行输入"，确定返回 TRUE 并写 out。密码模式显示圆点。 */
static BOOL ask_text(HWND owner, const wchar_t *title, const wchar_t *label,
                     const wchar_t *init, wchar_t *out, int cap, BOOL password)
{
    static BOOL cls_done;
    HINSTANCE hi = GetModuleHandleW(NULL);

    if (out && cap > 0) out[0] = 0;
    if (!cls_done) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ask_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"IVASK";
        RegisterClassExW(&wc);
        cls_done = TRUE;
    }

    g_askOK = FALSE;
    g_askPw = password;
    g_askOut = out;
    g_askCap = cap;

    RECT ro;
    GetWindowRect(owner ? owner : GetDesktopWindow(), &ro);
    int W = 420, H = 140;
    g_ask = CreateWindowExW(WS_EX_DLGMODALFRAME, L"IVASK", title,
                            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            ro.left + 80, ro.top + 80, W, H,
                            owner, NULL, hi, NULL);
    if (!g_ask) return FALSE;

    SetWindowTextW(g_askLbl, label);
    if (init && init[0]) SetWindowTextW(g_askEdit, init);
    ask_fit(g_ask);

    EnableWindow(owner, FALSE);
    MSG msg;
    while (g_ask && IsWindow(g_ask) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_ask, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    if (g_ask && IsWindow(g_ask)) DestroyWindow(g_ask);
    g_ask = NULL;
    SetForegroundWindow(owner);
    return g_askOK;
}

/* ======================================================================
 * 「生成令牌」对话框
 * ----------------------------------------------------------------------
 * 令牌是 **frps 的 auth.token**，设备侧 frpc 与本机 visitor 都必须等于它。
 * 所以这里的三个按钮不是等价选项，而是三种"影响范围"：
 *   ① 只用于本机      —— 不动设备。除非你同时在服务器上改，否则通道会不通。
 *   ② 下发到设备（+本机）—— 设备与本机同时换成新值。服务器没改 ⇒ 两边都登不上。
 *   ③ 连服务器一起改（推荐）—— 三处同改并重启 frps，改完才是"真的通"。
 * ②③ 都要在确认框里把后果写清楚，不许一句话点过去。
 * ==================================================================== */

static HWND g_tk, g_tkEdit, g_tkHint, g_tkSrvPw;

static void tk_fit(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    int W = r.right;
    MoveWindow(g_tkHint, 14, 12, W - 28, 40, TRUE);
    MoveWindow(g_tkEdit, 14, 58, W - 28 - 8 - 90, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_GEN), W - 14 - 90, 58, 90, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_COPY), 14, 92, 90, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_LOCAL), 112, 92, 120, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_DEV), 240, 92, 150, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_ALL), 398, 92, 150, 26, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_SRVLBL), 14, 130, 220, 20, TRUE);
    MoveWindow(g_tkSrvPw, 240, 128, 200, 24, TRUE);
    MoveWindow(GetDlgItem(h, ID_TK_CLOSE), W - 14 - 90, 160, 90, 28, TRUE);
}

static LRESULT CALLBACK tk_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        g_tkHint = CreateWindowExW(0, L"STATIC",
            L"令牌是服务器 frps 的 auth.token：设备侧、本机、服务器三处必须同一串。\n"
            L"不确定就用最右边的「连服务器一起改」，它会要下面的服务器 SSH 口令。",
            WS_CHILD | WS_VISIBLE, 14, 12, 520, 40, h, (HMENU)ID_TK_HINT, NULL, NULL);
        g_tkEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   14, 58, 420, 26, h, (HMENU)ID_TK_EDT, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"重新生成", WS_CHILD | WS_VISIBLE,
                        442, 58, 90, 26, h, (HMENU)ID_TK_GEN, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"复制", WS_CHILD | WS_VISIBLE,
                        14, 92, 90, 26, h, (HMENU)ID_TK_COPY, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"只用于本机", WS_CHILD | WS_VISIBLE,
                        112, 92, 120, 26, h, (HMENU)ID_TK_LOCAL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"下发到设备（+本机）", WS_CHILD | WS_VISIBLE,
                        240, 92, 150, 26, h, (HMENU)ID_TK_DEV, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"连服务器一起改", WS_CHILD | WS_VISIBLE,
                        398, 92, 150, 26, h, (HMENU)ID_TK_ALL, NULL, NULL);
        CreateWindowExW(0, L"STATIC", L"服务器 SSH 口令：", WS_CHILD | WS_VISIBLE,
                        14, 130, 220, 20, h, (HMENU)ID_TK_SRVLBL, NULL, NULL);
        g_tkSrvPw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_srvPw,
                                    WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
                                    240, 128, 200, 24, h, (HMENU)ID_TK_SRVPW, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE,
                        512, 160, 90, 28, h, (HMENU)ID_TK_CLOSE, NULL, NULL);
        if (g_font) {
            for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
                SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        return 0;
    case WM_SIZE:
        tk_fit(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_TK_GEN: {
            wchar_t t[64];
            tok_new(t, 64);
            SetWindowTextW(g_tkEdit, t);
            return 0;
        }
        case ID_TK_COPY: {
            wchar_t t[128];
            GetWindowTextW(g_tkEdit, t, 127);
            t[127] = 0;
            if (OpenClipboard(h)) {
                EmptyClipboard();
                size_t n = (wcslen(t) + 1) * sizeof(wchar_t);
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
                if (g) {
                    memcpy(GlobalLock(g), t, n);
                    GlobalUnlock(g);
                    SetClipboardData(CF_UNICODETEXT, g);
                }
                CloseClipboard();
                logw(L"令牌已复制到剪贴板（贴到别处请当心，等同钥匙）");
            }
            return 0;
        }
        case ID_TK_LOCAL:
        case ID_TK_DEV:
        case ID_TK_ALL: {
            wchar_t t[128];
            GetWindowTextW(g_tkEdit, t, 127);
            t[127] = 0;
            /* 去空白 */
            {
                wchar_t *p = t;
                while (*p == L' ' || *p == L'\t') p++;
                if (p != t) memmove(t, p, (wcslen(p) + 1) * sizeof(wchar_t));
                size_t L = wcslen(t);
                while (L && (t[L - 1] == L' ' || t[L - 1] == L'\t')) t[--L] = 0;
            }
            if (!tok_ok(t)) {
                MessageBoxW(h, L"令牌只能用 0-9 A-Z a-z 和 - _ . ~ + / = @，长度 1..63。",
                            APP_TITLE, MB_ICONWARNING);
                return 0;
            }
            int act = LOWORD(w) == ID_TK_LOCAL ? 13
                    : LOWORD(w) == ID_TK_DEV ? 14 : 15;
            if (act >= 14) {
                wchar_t m[768];
                swprintf(m, 768,
                    L"要下发这个令牌吗？\n\n"
                    L"  设备侧与本机都会换成：%s\n\n"
                    L"⚠ 令牌必须和服务器 frps 的 auth.token 完全一致。\n"
                    L"   如果服务器那边没改成同一串，设备与本机都会**立刻连不上**\n"
                    L"   （frpc 报 token in login doesn't match）。\n\n"
                    L"%s",
                    t,
                    act == 15 ? L"你选了「连服务器一起改」，会连 frps 一起改并重启它。"
                              : L"你选的是「只下发到设备」，服务器不会动 —— 除非确认服务器已经是这一串，"
                                L"否则请改用「连服务器一起改」。");
                if (MessageBoxW(h, m, APP_TITLE,
                                MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                    return 0;
            }
            if (act == 15) {
                GetWindowTextW(g_tkSrvPw, g_srvPw, 63);
                g_srvPw[63] = 0;
                if (!g_srvPw[0]) {
                    MessageBoxW(h, L"「连服务器一起改」需要 frps 那台机器的 SSH 口令（root）。",
                                APP_TITLE, MB_ICONINFORMATION);
                    SetFocus(g_tkSrvPw);
                    return 0;
                }
            }
            wcscpy(g_tokNew, t);
            DestroyWindow(h);
            g_tk = NULL;
            if (g_busy) {
                MessageBoxW(g_hMain, L"正在忙，等这一轮做完再改令牌。", APP_TITLE,
                            MB_ICONINFORMATION);
                return 0;
            }
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)act, 0, NULL);
            return 0;
        }
        case ID_TK_CLOSE:
        case IDCANCEL:
            DestroyWindow(h);
            g_tk = NULL;
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(h);
        g_tk = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void token_dialog(HWND owner)
{
    static BOOL cls_done;
    HINSTANCE hi = GetModuleHandleW(NULL);

    if (g_tk && IsWindow(g_tk)) { SetForegroundWindow(g_tk); return; }
    if (!cls_done) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = tk_proc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"IVTOK";
        RegisterClassExW(&wc);
        cls_done = TRUE;
    }
    RECT ro;
    GetWindowRect(owner, &ro);
    g_tk = CreateWindowExW(WS_EX_DLGMODALFRAME, L"IVTOK",
                           L"生成隧道令牌（三处同值才通）",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                           ro.left + 60, ro.top + 60, 640, 236, owner, NULL, hi, NULL);
    if (!g_tk) return;
    tk_fit(g_tk);
    SetWindowTextW(g_tkEdit, g_token);      /* 先显示"现在用的那串"，再点「重新生成」 */
}

static void layout(HWND h)
{
    RECT r;
    GetClientRect(h, &r);
    int W = r.right, H = r.bottom;
    /* 第一行：SN ＋ 口令 ＋ 连接设备 ＋ 服务器设置 */
    MoveWindow(g_hLblSn, 12, 14, 60, 20, TRUE);
    /* 下拉式 combo 的"高度"是展开后列表的高度，不是本体的高度 */
    MoveWindow(g_hCmbSn, 74, 11, 150, 260, TRUE);
    MoveWindow(g_hLblPw, 234, 14, 34, 20, TRUE);
    MoveWindow(g_hEdtPw, 270, 12, 104, 24, TRUE);
    MoveWindow(g_hBtnLink, 382, 10, 96, 26, TRUE);
    MoveWindow(g_hBtnSet, 486, 10, 92, 26, TRUE);
    MoveWindow(g_hLblSrv, 586, 14, W - 586 - 12 > 120 ? W - 586 - 12 : 120, 20, TRUE);

    /* 第二行：通道摘要（左）＋ 开/关/生成令牌（右上） */
    MoveWindow(g_hLblTun, 12, 46, W - 24, 20, TRUE);
    int bx = W - 12 - 252;
    if (bx < 268) bx = 268;
    MoveWindow(g_hBtnOn,  bx, 68, 70, 26, TRUE);
    MoveWindow(g_hBtnOff, bx + 78, 68, 70, 26, TRUE);
    MoveWindow(g_hBtnTok, bx + 156, 68, 96, 26, TRUE);
    {
        int lw = bx - 12 - 8;
        MoveWindow(g_hLblDev, 12, 72, lw > 60 ? lw : 60, 20, TRUE);
    }

    MoveWindow(g_hStatus, 12, 98, W - 24, 22, TRUE);
    int by = H - 46 - 142;                       /* 摄像机按钮那一行 */
    MoveWindow(g_hList, 12, 122, W - 24, by - 122 - 8, TRUE);
    int x = 12;
    HWND bs[] = { g_hBtnConn, g_hBtnDisc, g_hBtnWeb, g_hBtnDl, g_hBtnRef };
    const int wd[] = { 110, 70, 90, 90, 70 };
    for (int i = 0; i < 5; i++) {
        MoveWindow(bs[i], x, by, wd[i], 28, TRUE);
        x += wd[i] + 8;
    }
    MoveWindow(g_hLog, 12, by + 34, W - 24, 142, TRUE);
}

static void init_list(void)
{
    LVCOLUMNW c;
    ZeroMemory(&c, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    const wchar_t *hdr[] = { L"路", L"摄像机 IP", L"状态", L"本地地址" };
    const int w[] = { 50, 170, 190, 220 };
    for (int i = 0; i < 4; i++) {
        c.pszText = (LPWSTR)hdr[i];
        c.cx = w[i];
        c.iSubItem = i;
        SendMessageW(g_hList, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
    SendMessageW(g_hList, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        /* ---- 第一行：设备 SN（可选可输）＋ 口令 ＋ 「连接设备」 ----
         * 用户只填这两样；IP、服务器、令牌、通道全靠程序自己查。 */
        g_hLblSn = CreateWindowExW(0, L"STATIC", L"设备 SN", WS_CHILD | WS_VISIBLE,
                                   12, 14, 60, 20, h, (HMENU)ID_LBL_SN, NULL, NULL);
        g_hCmbSn = CreateWindowExW(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                                   74, 11, 150, 260, h, (HMENU)ID_CMB_SN, NULL, NULL);
        g_hLblPw = CreateWindowExW(0, L"STATIC", L"口令", WS_CHILD | WS_VISIBLE,
                                   234, 14, 34, 20, h, (HMENU)ID_LBL_PW, NULL, NULL);
        g_hEdtPw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD |
                                   ES_AUTOHSCROLL,
                                   270, 12, 104, 24, h, (HMENU)ID_EDT_PW, NULL, NULL);
        g_hBtnLink = CreateWindowExW(0, L"BUTTON", L"连接设备",
                                     WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                     382, 10, 96, 26, h, (HMENU)ID_BTN_LINK, NULL, NULL);
        g_hBtnSet = CreateWindowExW(0, L"BUTTON", L"服务器设置", WS_CHILD | WS_VISIBLE,
                                    486, 10, 92, 26, h, (HMENU)ID_BTN_SET, NULL, NULL);
        g_hLblSrv = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    586, 14, 300, 20, h, (HMENU)ID_LBL_SRV, NULL, NULL);

        /* ---- 第二行：通道摘要 ＋ 三个动作（开/关/生成令牌） ---- */
        g_hLblTun = CreateWindowExW(0, L"STATIC", L"通道：未读取设备（先填 SN 和口令，点「连接设备」）",
                                    WS_CHILD | WS_VISIBLE, 12, 46, 560, 20,
                                    h, (HMENU)ID_LBL_TUN, NULL, NULL);
        g_hBtnOn = CreateWindowExW(0, L"BUTTON", L"开通道", WS_CHILD | WS_VISIBLE,
                                   12, 68, 70, 26, h, (HMENU)ID_BTN_ON, NULL, NULL);
        g_hBtnOff = CreateWindowExW(0, L"BUTTON", L"关通道", WS_CHILD | WS_VISIBLE,
                                    88, 68, 70, 26, h, (HMENU)ID_BTN_OFF, NULL, NULL);
        g_hBtnTok = CreateWindowExW(0, L"BUTTON", L"生成令牌…", WS_CHILD | WS_VISIBLE,
                                    164, 68, 96, 26, h, (HMENU)ID_BTN_TOK, NULL, NULL);
        g_hLblDev = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    268, 72, 480, 20, h, (HMENU)ID_LBL_DEV, NULL, NULL);

        g_hStatus = CreateWindowExW(0, L"STATIC", L"正在初始化 ...",
                                    WS_CHILD | WS_VISIBLE, 12, 98, 600, 22,
                                    h, (HMENU)ID_STATUS, NULL, NULL);
        g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                  12, 122, 600, 200, h, (HMENU)ID_LIST, NULL, NULL);
        g_hBtnConn = CreateWindowExW(0, L"BUTTON", L"连接并打开", WS_CHILD | WS_VISIBLE,
                                     12, 250, 110, 28, h, (HMENU)ID_BTN_CONN, NULL, NULL);
        g_hBtnDisc = CreateWindowExW(0, L"BUTTON", L"断开", WS_CHILD | WS_VISIBLE,
                                     130, 250, 70, 28, h, (HMENU)ID_BTN_DISC, NULL, NULL);
        g_hBtnWeb = CreateWindowExW(0, L"BUTTON", L"设备 Web", WS_CHILD | WS_VISIBLE,
                                    208, 250, 90, 28, h, (HMENU)ID_BTN_WEB, NULL, NULL);
        g_hBtnDl = CreateWindowExW(0, L"BUTTON", L"下载文件", WS_CHILD | WS_VISIBLE,
                                   306, 250, 90, 28, h, (HMENU)ID_BTN_DL, NULL, NULL);
        g_hBtnRef = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE,
                                    404, 250, 70, 28, h, (HMENU)ID_BTN_REF, NULL, NULL);
        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL,
                                 12, 290, 600, 140, h, (HMENU)ID_LOG, NULL, NULL);

        HWND ks[] = { g_hLblSn, g_hCmbSn, g_hLblPw, g_hEdtPw, g_hBtnLink, g_hBtnSet,
                      g_hLblSrv, g_hLblTun, g_hBtnOn, g_hBtnOff, g_hBtnTok, g_hLblDev,
                      g_hStatus, g_hList, g_hBtnConn, g_hBtnDisc, g_hBtnWeb,
                      g_hBtnDl, g_hBtnRef, g_hLog };
        for (int i = 0; i < (int)(sizeof ks / sizeof ks[0]); i++)
            SendMessageW(ks[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        init_list();
        sn_combo_fill();
        if (g_devPass[0]) SetWindowTextW(g_hEdtPw, g_devPass);
        return 0;
    }
    case WM_SIZE:
        layout(h);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 720;
        mm->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        /* 「连接设备」：找 IP → 登录 → 读通道状态 → 顺手把本机 frpc 对齐。
         * 这是唯一的主按钮；SN 与口令就在旁边，别的一概不用填。 */
        case ID_BTN_LINK: {
            if (g_busy) {
                MessageBoxW(h, L"正在忙（连接/刷新/下发），等这一轮做完再点。",
                            APP_TITLE, MB_ICONINFORMATION);
                return 0;
            }
            wchar_t sn[64];
            combo_sn(sn, 64);
            if (!sn[0]) {
                MessageBoxW(h, L"先填设备 SN（设备 Web 首页或机壳标签上有）。",
                            APP_TITLE, MB_ICONINFORMATION);
                SetFocus(g_hCmbSn);
                return 0;
            }
            wchar_t pw[64];
            GetWindowTextW(g_hEdtPw, pw, 63);
            pw[63] = 0;
            if (!pw[0]) {
                MessageBoxW(h, L"填一下设备 Web 的口令（登录设备管理页用的那个）。",
                            APP_TITLE, MB_ICONINFORMATION);
                SetFocus(g_hEdtPw);
                return 0;
            }
            /* 先把界面上这个 SN 落到本机配置（换台设备时 secretKey 要重算） */
            if (!sync_sn_from_combo(h)) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)10, 0, NULL);
            return 0;
        }
        case ID_BTN_ON:
            if (g_busy) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)11, 0, NULL);
            return 0;
        case ID_BTN_OFF:
            if (g_busy) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)12, 0, NULL);
            return 0;
        case ID_BTN_TOK:
            if (g_busy) return 0;
            token_dialog(h);
            return 0;
        case ID_BTN_CONN: {
            if (g_busy) return 0;
            int row = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1,
                                        MAKELPARAM(LVNI_SELECTED, 0));
            if (row < 0) {
                MessageBoxW(h, L"先在列表里选一路摄像机", APP_TITLE, MB_ICONINFORMATION);
                return 0;
            }
            if (!sync_sn_from_combo(h)) return 0;
            g_selCh = row;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)2, 0, NULL);
            return 0;
        }
        case ID_BTN_DISC:
            if (g_busy) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)3, 0, NULL);
            return 0;
        case ID_BTN_WEB:
            if (g_busy) return 0;
            if (!sync_sn_from_combo(h)) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)5, 0, NULL);
            return 0;
        case ID_BTN_DL:
            if (g_busy) return 0;
            open_download();
            return 0;
        case ID_BTN_REF:
            if (g_busy) return 0;
            if (!sync_sn_from_combo(h)) return 0;
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)1, 0, NULL);
            return 0;

        case ID_CMB_SN:
            if (HIWORD(w) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(g_hCmbSn, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < g_nsns) {
                    wchar_t why[160] = L"";
                    if (!sn_apply(g_sns[i], why, 160))
                        MessageBoxW(h, why, APP_TITLE, MB_ICONWARNING);
                }
            }
            return 0;
        case ID_BTN_SET:
            if (g_busy) return 0;
            if (settings_dialog(h)) {
                sn_combo_fill();
                update_status_line();
            }
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)l;
        if (nh->idFrom == ID_LIST && nh->code == NM_DBLCLK) {
            if (!g_busy) {
                int row = (int)SendMessageW(g_hList, LVM_GETNEXTITEM, (WPARAM)-1,
                                            MAKELPARAM(LVNI_SELECTED, 0));
                if (row >= 0 && sync_sn_from_combo(h)) {
                    g_selCh = row;
                    CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)2, 0, NULL);
                }
            }
        }
        return 0;
    }
    case WM_CAMS_READY:
        fill_list();
        return 0;
    case WM_CAM_STATE: {
        int i = (int)w, st = (int)l;
        const wchar_t *s = st == 0 ? L"可连接" :
                           st == 1 ? L"网络不通" :
                           st == 2 ? L"80 端口不通" : L"探测失败";
        list_set(i, 2, s);
        return 0;
    }
    case WM_BUSY:
        update_status_line();
        update_tun_line();
        return 0;
    case WM_LINK_STATE:
        update_tun_line();
        update_status_line();
        return 0;
    case WM_NEED_PASS: {
        /* 设备还在出厂口令：服务端只放行 /api/passwd，其余一律 403。
         * 与其让现场"去浏览器改密再回来"，不如在这里一步改掉。
         * 自动化（--shot）下不弹模态，否则截图永远等不到窗口收尾。 */
        wchar_t nw[64];
        if (g_shotPath[0]) {
            if (!g_cliNewPass[0] || wcslen(g_cliNewPass) >= 64) {
                logw(L"[!] 设备还在出厂口令，需要先设新口令 —— 界面上会弹框让用户设");
                return 0;
            }
            wcscpy(nw, g_cliNewPass);     /* 自动化：命令行给新口令，不弹模态框 */
            logw(L"    自动化：用 --newpass 给的口令直接改密");
        } else {
            if (!ask_text(h, L"这台设备还在出厂口令状态，必须先设一个新口令才能操作。",
                          L"新口令（8–12 位，含大写、小写、数字）：", L"", nw, 64, TRUE))
                return 0;
            if (!pass_rule_ok(nw)) {
                MessageBoxW(h, L"新口令要 8–12 位，且同时含大写字母、小写字母和数字。",
                            APP_TITLE, MB_ICONWARNING);
                return 0;
            }
        }
        {
            wchar_t sid[192], err[256] = L"";
            const wchar_t *ip = g_locateMsg[0] ? g_locateMsg : g_devIp;
            const wchar_t *old = g_devPass[0] ? g_devPass : L"88888888";
            BOOL must = FALSE;
            if (!dev_login(ip, g_devUser, old, sid, 192, &must, err, 256)) {
                /* 刚才那遍登录是通的，这里再失败只能是口令被改过；如实报错 */
                MessageBoxW(h, err, APP_TITLE, MB_ICONWARNING);
                return 0;
            }
            if (!dev_set_pass(ip, sid, nw, err, 256)) {
                MessageBoxW(h, err, APP_TITLE, MB_ICONWARNING);
                return 0;
            }
            logw(L"[OK] 设备 %s 的口令已改（新口令已记住，下次不用再填）", ip);
            wcscpy(g_devPass, nw);
            SetWindowTextW(g_hEdtPw, nw);
            cfg_save();
            /* 按用户刚才点的那个动作重来：只读探查就别顺手开通道 */
            CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)g_needPassAct, 0, NULL);
        }
        return 0;
    }
    case WM_CLOSE:
        kill_tunnel();
        if (g_frpcProc) {
            TerminateProcess(g_frpcProc, 0);
            CloseHandle(g_frpcProc);
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ----------------------------------------------------------- 窗口截图 */
/* 给文档/远程支持用：--shot <file.bmp> [延迟毫秒]
 * 用 PrintWindow 抓客户区，不依赖任何外部截图工具。
 * 变量本体在文件上部（WM_NEED_PASS 要读它判断"是否自动化"）。 */

static void save_window_bmp(HWND h, const wchar_t *path)
{
    RECT r;
    GetClientRect(h, &r);
    int W = r.right, H = r.bottom;
    if (W <= 0 || H <= 0) return;

    HDC hdc = GetDC(h);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old = SelectObject(mem, bm);
    PrintWindow(h, mem, 1 /* PW_CLIENTONLY */);
    SelectObject(mem, old);
    ReleaseDC(h, hdc);

    BITMAPINFOHEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = W;
    bi.biHeight = H;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    int stride = ((W * 3 + 3) / 4) * 4;
    int imgsz = stride * H;
    BYTE *buf = (BYTE *)malloc(imgsz);
    HDC sdc = GetDC(NULL);
    GetDIBits(sdc, bm, 0, H, buf, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
    ReleaseDC(NULL, sdc);

    BITMAPFILEHEADER fh;
    ZeroMemory(&fh, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + imgsz;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, &fh, sizeof(fh), &wr, NULL);
        WriteFile(f, &bi, sizeof(bi), &wr, NULL);
        WriteFile(f, buf, imgsz, &wr, NULL);
        CloseHandle(f);
        logw(L"窗口截图已保存 %s（%dx%d）", path, W, H);
    }
    free(buf);
    DeleteObject(bm);
    DeleteDC(mem);
}

static DWORD WINAPI shot_thread(LPVOID p)
{
    (void)p;
    Sleep(g_shotDelay);
    HWND target = (g_hSet && IsWindow(g_hSet)) ? g_hSet :
                  (g_tk && IsWindow(g_tk)) ? g_tk :
                  (g_hDlg && IsWindow(g_hDlg)) ? g_hDlg : g_hMain;
    save_window_bmp(target, g_shotPath);
    PostMessageW(g_hMain, WM_CLOSE, 0, 0);
    return 0;
}

/* --------------------------------------------------------------- 自检 */

/* visitor.toml 里是不是这条 SN、且 secretKey 是 16 个 hex —— 自检用 */
static BOOL toml_matches(const wchar_t *sn)
{
    HANDLE h = CreateFileW(g_toml, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    char buf[2048];
    DWORD rd = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
    CloseHandle(h);
    if (!ok) return FALSE;
    buf[rd] = 0;

    char s[80], needle[120];
    to_utf8(sn, s, sizeof(s));
    snprintf(needle, sizeof(needle), "serverName = \"%s\"", s);
    if (!strstr(buf, needle)) return FALSE;

    const char *p = strstr(buf, "secretKey = \"");
    if (!p) return FALSE;
    p += strlen("secretKey = \"");
    for (int i = 0; i < 16; i++) {
        char c = p[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return FALSE;
    }
    return p[16] == '"';
}

static int g_fail;

static void chk(const wchar_t *what, BOOL ok, const wchar_t *extra)
{
    logw(L"[%s] %s%s%s", ok ? L"PASS" : L"FAIL", what,
         extra && extra[0] ? L"  " : L"", extra ? extra : L"");
    if (!ok) g_fail++;
}

static int selftest(void)
{
    logw(L"===== 自检开始 =====");

    chk(L"exe 目录就绪", GetFileAttributesW(g_dir) != INVALID_FILE_ATTRIBUTES, g_dir);
    chk(L"frpc.exe 已释放", GetFileAttributesW(g_frpc) != INVALID_FILE_ATTRIBUTES, g_frpc);
    chk(L"密钥已释放", GetFileAttributesW(g_key) != INVALID_FILE_ATTRIBUTES, g_key);

    /* ---- 1.1.0：换 SN 能不能真的换出对应的钥匙与配置 ---- */
    {
        /* 黄金向量：11-02 出厂 visitor.toml 里就是这一对（设备侧同样算法） */
        char k[17] = "";
        BOOL ok = md5_hex16("0B2CC1", "6a8fdb6b38967043d91aaede64fe96c3", k);
        chk(L"secretKey 推导（黄金向量 0B2CC1）",
            ok && !strcmp(k, "a13db1c3eb45118f"), W(k));

        char k2[17] = "";
        md5_hex16("0A19B4", "6a8fdb6b38967043d91aaede64fe96c3", k2);
        chk(L"不同 SN 推出不同 secretKey", strcmp(k, k2) != 0, W(k2));

        wchar_t oldSn[64];
        wcscpy(oldSn, g_sn);
        wchar_t why[160] = L"";

        chk(L"切到 0A19B4 后 visitor.toml 跟着换",
            sn_apply(L"0A19B4", why, 160) && toml_matches(L"0A19B4"), why);
        why[0] = 0;
        chk(L"切回原 SN 后 visitor.toml 还原",
            sn_apply(oldSn, why, 160) && toml_matches(oldSn), why);
        why[0] = 0;
        chk(L"带引号的非法 SN 被拒", !sn_apply(L"0A1\"9B4", why, 160), why);
        chk(L"当前 visitor.toml 与 SN 一致", toml_matches(g_sn), g_toml);
    }

    BOOL up = ensure_frpc();
    chk(L"frpc 通道就绪（16000）", up, L"");

    wchar_t pre[1024], cmd[1600];
    ssh_prefix(pre, 0);
    swprintf(cmd, 1600, L"%s \"echo SELFTEST-OK; hostname\"", pre);
    char out[512] = "";
    int rc = run_capture(cmd, out, sizeof(out), 20000);
    wchar_t wout[512] = L"";
    mbstowcs(wout, out, 511);
    chk(L"ssh 免密登录设备", rc == 0 && strstr(out, "SELFTEST-OK") != NULL, wout);

    do_probe_all();
    chk(L"读到摄像机列表", g_ncams > 0, L"");
    for (int i = 0; i < g_ncams; i++) {
        wchar_t b[256];
        wchar_t ipw[64];
        mbstowcs(ipw, g_cams[i].ip, 63);
        swprintf(b, 256, L"第%d路 %s state=%d", i + 1, ipw, g_cams[i].state);
        chk(L"探测结果", g_cams[i].state >= 0, b);
    }

    /* 连通的取第一路建隧道并验证 HTTP */
    int target = -1;
    for (int i = 0; i < g_ncams; i++)
        if (g_cams[i].state == 0) { target = i; break; }
    if (target >= 0) {
        BOOL ok = do_connect(target, FALSE);
        chk(L"建隧道", ok, L"");
        if (ok) {
            int code = http_code(9089 + target + 1);
            wchar_t b[128];
            swprintf(b, 128, L"code=%d", code);
            chk(L"隧道可访问摄像机 Web", code > 0 && code < 500, b);
        }
        kill_tunnel();
    } else {
        logw(L"[SKIP] 没有探测为可连接的摄像机，跳过隧道验证");
    }

    /* 不通的那一路：必须被预检拦住，不建隧道、不占端口 */
    {
        int bad = -1;
        for (int i = 0; i < g_ncams; i++)
            if (g_cams[i].state != 0) { bad = i; break; }
        if (bad >= 0) {
            int p = 9089 + bad + 1;
            BOOL ok = do_connect(bad, FALSE);
            wchar_t b[160];
            swprintf(b, 160, L"第%d路 port=%d", bad + 1, p);
            chk(L"不通的通道被预检拦住（不建隧道）", !ok && !port_open(p), b);
        } else {
            logw(L"[SKIP] 没有不通的通道，跳过「网络故障」验证");
        }
    }

    /* 下载：列 SD 卡目录 + 挑最小的一个文件下下来核对字节数 */
    wcscpy(g_dlPath, L"/mnt/sdcard/video");
    dl_list();
    {
        wchar_t b[256];
        swprintf(b, 256, L"%d 项", g_nentries);
        chk(L"列 SD 卡目录", g_nentries > 0, b);
    }
    /* 找一个"有文件"的子目录，取其中最小的两个文件一起下，验证队列 */
    wchar_t subdirs[16][300];
    int nsub = 0;
    for (int i = 0; i < g_nentries && nsub < 16; i++)
        if (g_entries[i].isdir) wcscpy(subdirs[nsub++], g_entries[i].name);

    wchar_t root[512];
    wcscpy(root, g_dlPath);
    wchar_t files[2][600];
    long long fsz[2] = { 0, 0 };
    int nf = 0;

    for (int r = -1; r < nsub && nf < 2; r++) {
        if (r >= 0) {
            wchar_t nm[300];
            wcscpy(nm, subdirs[r]);
            size_t L = wcslen(nm);
            if (L && nm[L - 1] == L'/') nm[L - 1] = 0;
            swprintf(g_dlPath, 512, L"%s/%s", root, nm);
            dl_list();
        }
        for (int i = 0; i < g_nentries && nf < 2; i++) {
            if (g_entries[i].isdir) continue;
            /* 跳过 0 字节：录像刚建好的文件就是 0（本机实测碰到过），
             * 拿它当样本会误判成"下载失败" */
            if (g_entries[i].size <= 0) continue;
            swprintf(files[nf], 600, L"%s/%s", g_dlPath, g_entries[i].name);
            fsz[nf] = g_entries[i].size;
            nf++;
        }
    }

    if (nf > 0) {
        g_dlQty = nf;
        g_dlAllBytes = 0;
        g_dlDoneBytes = 0;
        for (int i = 0; i < nf; i++) {
            wcscpy(g_dlQueue[i], files[i]);
            g_dlAllBytes += fsz[i];
        }
        logw(L"下载测试：%d 个文件，合计 %.2f MB", nf, g_dlAllBytes / 1048576.0);
        dl_get_worker(NULL);
        int okc = 0;
        for (int i = 0; i < nf; i++) {
            const wchar_t *base = wcsrchr(files[i], L'/');
            base = base ? base + 1 : files[i];
            wchar_t dst[MAX_PATH * 2];
            swprintf(dst, MAX_PATH * 2, L"%s\\%s", g_dlSaveDir, base);
            long long got = 0;
            HANDLE h = CreateFileW(dst, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li;
                if (GetFileSizeEx(h, &li)) got = li.QuadPart;
                CloseHandle(h);
            }
            wchar_t b[300];
            swprintf(b, 300, L"%s  本地 %lld / 远端 %lld", base, got, fsz[i]);
            chk(L"下载文件并核对大小", got == fsz[i] && got > 0, b);
            if (got == fsz[i] && got > 0) okc++;
            DeleteFileW(dst);   /* 自检产物不留 */
        }
        chk(L"多文件队列全部完成", okc == nf, L"");
    } else {
        logw(L"[SKIP] /mnt/sdcard/video 下没有普通文件，跳过下载验证");
    }

    /* 失败的通道应保证端口没被占用 */
    for (int i = 0; i < g_ncams; i++) {
        if (g_cams[i].state == 0) continue;
        int p = 9089 + i + 1;
        wchar_t b[128];
        swprintf(b, 128, L"port=%d", p);
        chk(L"不通的通道没有占端口", !port_open(p), b);
    }

    logw(L"===== 自检结束：%s（失败 %d 项）=====", g_fail ? L"有失败" : L"全部通过", g_fail);
    if (g_frpcProc) {
        TerminateProcess(g_frpcProc, 0);
        CloseHandle(g_frpcProc);
        g_frpcProc = NULL;
    }
    return g_fail ? 1 : 0;
}

/* --------------------------------------------------------------- 入口 */

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmdline, int show)
{
    (void)hp;
    InitializeCriticalSection(&g_cs);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    ensure_paths();
    if (GetFileAttributesW(g_dir) == INVALID_FILE_ATTRIBUTES) {
        /* 落不了盘就没有 ini/frpc/密钥/log，界面看着正常但什么都做不成 —— 明说 */
        wchar_t m[MAX_PATH + 160];
        if (strstr(cmdline, "--shot") == NULL && strstr(cmdline, "--selftest") == NULL) {
            swprintf(m, MAX_PATH + 160,
                     L"建不了工作目录：\n%s\n\n请检查权限后重试，或把这个程序放到有写权限的位置。",
                     g_dir);
            MessageBoxW(NULL, m, L"运维箱助手", MB_ICONERROR);
        }
        return 1;
    }
    BOOL self = strstr(cmdline, "--selftest") != NULL;
    g_quiet = self;

    BOOL shot = FALSE, opendl = FALSE, openset = FALSE;
    int dlw = 0, dlh = 0;
    wchar_t cliSn[64] = L"";        /* --sn <SN>：命令行临时指定设备 */
    wchar_t cliPw[64] = L"";        /* --pass <口令>：命令行给设备口令（自检/联调用） */
    wchar_t cliIp[64] = L"";        /* --devip <IP>：跳过扫描，直接用这个地址 */
    BOOL    cliLink = FALSE;        /* --link：启动后自动点一次「连接设备」 */
    BOOL    cliProbe = FALSE;       /* --probe：只定位+读状态，不改设备 */
    int     cliTun = 0;             /* --on / --off：开/关设备通道（=点按钮） */
    BOOL    cliTokDlg = FALSE;      /* --tokdlg：打开「生成令牌」对话框（截图用） */
    wchar_t cliTok[128] = L"";      /* --token <令牌>：直接下发（设备+本机），联调用 */
    {
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; i < argc; i++) {
            if (!wcscmp(argv[i], L"--opendl")) opendl = TRUE;
            if (!wcscmp(argv[i], L"--openset")) openset = TRUE;
            if (!wcscmp(argv[i], L"--link")) cliLink = TRUE;
            if (!wcscmp(argv[i], L"--probe")) cliProbe = TRUE;
            if (!wcscmp(argv[i], L"--on")) cliTun = 1;
            if (!wcscmp(argv[i], L"--off")) cliTun = 2;
            if (!wcscmp(argv[i], L"--tokdlg")) cliTokDlg = TRUE;
            if (!wcscmp(argv[i], L"--token") && i + 1 < argc) wcscpy(cliTok, argv[i + 1]);
            if (!wcscmp(argv[i], L"--sn") && i + 1 < argc) wcscpy(cliSn, argv[i + 1]);
            if (!wcscmp(argv[i], L"--pass") && i + 1 < argc) wcscpy(cliPw, argv[i + 1]);
            if (!wcscmp(argv[i], L"--devip") && i + 1 < argc) wcscpy(cliIp, argv[i + 1]);
            if (!wcscmp(argv[i], L"--newpass") && i + 1 < argc) wcscpy(g_cliNewPass, argv[i + 1]);
            if (!wcscmp(argv[i], L"--dlw") && i + 1 < argc) dlw = _wtoi(argv[i + 1]);
            if (!wcscmp(argv[i], L"--dlh") && i + 1 < argc) dlh = _wtoi(argv[i + 1]);
            if (!wcscmp(argv[i], L"--dlpath") && i + 1 < argc) {
                wcscpy(g_dlPath, argv[i + 1]);
                path_trim(g_dlPath);
            }
            if (!wcscmp(argv[i], L"--shot") && i + 1 < argc) {
                wcscpy(g_shotPath, argv[i + 1]);
                if (i + 2 < argc) g_shotDelay = _wtoi(argv[i + 2]);
                shot = TRUE;
            }
        }
        if (argv) LocalFree(argv);
    }

    extract_res(RES_FRPC, g_frpc);
    extract_res(RES_KEY, g_key);
    restrict_to_me(g_key);

    /* visitor.toml 不再用打包好的那一份：读配置 → 按当前 SN 现算 secretKey 现写。
     * 第一次运行没有 assist.ini，就用编译进去的出厂默认（见文件头）。 */
    cfg_load();
    if (cliSn[0] && sn_ok(cliSn)) wcscpy(g_sn, cliSn);
    if (cliPw[0]) { wcscpy(g_devPass, cliPw); }
    if (cliIp[0]) { wcscpy(g_devIp, cliIp); }
    {
        wchar_t why[160] = L"";
        if (!sn_apply(g_sn, why, 160))
            logw(L"[X] 生成 visitor.toml 失败：%s", why);
    }

    if (self) {
        int rc = selftest();
        WSACleanup();
        return rc;
    }

    INITCOMMONCONTROLSEX ic;
    ic.dwSize = sizeof(ic);
    ic.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"IVREMOTE";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dl_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"IVDLG";
    RegisterClassExW(&wc);

    g_hMain = CreateWindowExW(0, L"IVREMOTE",
                              APP_TITLE L"  " APP_VER,
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, 780, 660,
                              NULL, NULL, hi, NULL);
    if (!g_hMain) return 1;
    (void)show;

    logw(L"运维箱助手 %s（配置 · 令牌 · 远程连接 一体）", APP_VER);
    logw(L"工作目录：%s", g_dir);
    logw(L"当前设备：SN=%s    服务器：%s:%d    visitor.toml：按 SN 现算",
         g_sn, g_srvHost, g_srvPort);
    if (!g_devPass[0])
        logw(L"填好 SN 和口令后点「连接设备」：IP、通道、令牌都由程序自己查，不用填别的");
    update_status_line();
    update_tun_line();
    if (openset) settings_create(g_hMain);
    if (cliTokDlg) token_dialog(g_hMain);
    /* 开机先只读本机（不碰设备）：--link 或命令行给了口令才自动连设备 */
    if (cliLink) {
        CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)10, 0, NULL);
    } else if (cliProbe) {
        CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)16, 0, NULL);
    } else if (cliTun) {
        CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)(cliTun == 1 ? 11 : 12), 0, NULL);
    } else if (cliTok[0]) {
        wcscpy(g_tokNew, cliTok);
        CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)14, 0, NULL);
    } else {
        CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)1, 0, NULL);
    }
    if (opendl) {
        open_download();
        if (dlw > 0 && dlh > 0 && g_hDlg) {
            RECT rr;
            GetWindowRect(g_hDlg, &rr);
            SetWindowPos(g_hDlg, NULL, 0, 0, dlw, dlh, SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    if (shot) CreateThread(NULL, 0, shot_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return 0;
}
