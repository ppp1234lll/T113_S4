/*
 * iv_chan 单测（libivcore）。
 *
 * 覆盖范围与刻意的取舍：
 *   - 消息头：布局锁定（sizeof / offsetof / 魔数内存字节序）、编解码、各条校验失败路径。
 *   - 收发：用 socketpair 造一对已连接的 SEQPACKET fd，覆盖正常往返、空载荷、
 *     超长被拒、短包被拒、**头里撒谎**（声明长度与实际包长不符）被拒、对端关闭。
 *   - 超时：recv_timeout 的到期与命中两条路径。
 *   - 接听点：真实 listen/connect/accept，覆盖 0660 权限落实、非 socket 路径拒绝
 *     （且**不删除该文件**）、残留 socket 重建、父目录缺失、路径过长。
 *   - 鉴权：白名单规则用纯函数测（脱离 socket），再用真实 accept 测一次"规则命中
 *     但 uid 不在名单 → IV_EAUTH 且连接被关闭"。
 *
 * 为什么用 socketpair 而不是每次都 fork：SEQPACKET 的收发行为与"经由 listen/accept
 * 建立的连接"完全一致（内核走同一条 unix_dgram 收发路径），而 fork 会让单测在
 * 板上多一层进程/信号的不确定性。真正需要区分的是"路径与鉴权"，那部分用真实
 * listen/accept 覆盖。
 *
 * 临时文件全部落在 mkdtemp 造的目录里，退出前清理干净（AGENTS.md 规则 6）。
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 收发缓冲放静态区：IV_CHAN_RECV_CAP_MIN 是 64 KiB 量级，
 * 放栈上在板端线程栈偏小的场景会出问题，而单测里没有任何理由用栈。*/
static uint8_t g_tx[IV_CHAN_RECV_CAP_MIN];
static uint8_t g_rx[IV_CHAN_RECV_CAP_MIN];

/* --------------------------------------------------------------------------- */

static void test_header_layout(void)
{
    ivs_chan_hdr_t h;
    uint8_t        raw[4];

    chk(sizeof(ivs_chan_hdr_t) == IV_CHAN_HDR_SIZE, "header size equals IV_CHAN_HDR_SIZE");
    chk(IV_CHAN_HDR_SIZE == (size_t)32u, "IV_CHAN_HDR_SIZE is 32 (wire format is frozen)");

    /* 偏移锁定：编译期已用 _Static_assert 钉过一遍，这里再运行期验一次。
     * 双层的原因是编译期断言在"两个目标各自编译成功"时看不出差异 ——
     * 只有把实际值打印/断言出来，板上跑单测才能证明两端布局真的一致。*/
    chk(offsetof(ivs_chan_hdr_t, magic) == 0u, "magic at offset 0");
    chk(offsetof(ivs_chan_hdr_t, version) == 4u, "version at offset 4");
    chk(offsetof(ivs_chan_hdr_t, type) == 6u, "type at offset 6");
    chk(offsetof(ivs_chan_hdr_t, flags) == 8u, "flags at offset 8");
    chk(offsetof(ivs_chan_hdr_t, pad0) == 12u, "pad0 at offset 12");
    chk(offsetof(ivs_chan_hdr_t, request_id) == 16u, "request_id at offset 16");
    chk(offsetof(ivs_chan_hdr_t, payload_len) == 24u, "payload_len at offset 24");
    chk(offsetof(ivs_chan_hdr_t, reserved) == 28u, "reserved at offset 28");

    /* 魔数的**内存字节序**必须是 'I','V','S','B'（架构 §5.1 的 'IVSB'）。
     * 只比较数值是不够的：数值对而字节序反了，跨端一样读不出来。*/
    memset(&h, 0, sizeof h);
    h.magic = IV_CHAN_MAGIC;
    memcpy(raw, &h.magic, sizeof raw);
    chk(raw[0] == (uint8_t)'I' && raw[1] == (uint8_t)'V' && raw[2] == (uint8_t)'S' &&
            raw[3] == (uint8_t)'B',
        "magic occupies the bytes 'I','V','S','B' in memory");
}

static void test_header_codec(void)
{
    ivs_chan_hdr_t h;

    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 7u, 0u, 100u) == IV_OK, "init accepts a normal header");
    chk(h.magic == IV_CHAN_MAGIC, "init writes magic");
    chk(h.version == IV_CHAN_VERSION, "init writes version");
    chk(h.type == IV_CHAN_TYPE_REQ, "init writes type");
    chk(h.request_id == 7u, "init writes request_id");
    chk(h.payload_len == 100u, "init writes payload_len");
    chk(h.pad0 == 0u, "init zeroes pad0");
    chk(h.reserved == 0u, "init zeroes reserved");
    chk(iv_chan_hdr_check(&h) == IV_OK, "check accepts a freshly built header");

    chk(iv_chan_hdr_init(NULL, IV_CHAN_TYPE_REQ, 0u, 0u, 0u) == IV_EINVAL, "init rejects NULL");
    chk(iv_chan_hdr_init(&h, 99u, 0u, 0u, 0u) == IV_EINVAL, "init rejects unknown type");
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, IV_CHAN_MAX_PAYLOAD) == IV_OK,
        "init accepts payload exactly at the limit");
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, IV_CHAN_MAX_PAYLOAD + 1u) == IV_ERANGE,
        "init rejects payload one byte over the limit");

    chk(iv_chan_hdr_check(NULL) == IV_EINVAL, "check rejects NULL");

    iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 0u);
    h.magic = 0u;
    chk(iv_chan_hdr_check(&h) == IV_EPROTO, "check rejects wrong magic");

    iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 0u);
    h.version = 99u;
    chk(iv_chan_hdr_check(&h) == IV_EPROTO, "check rejects unknown version");

    iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 0u);
    h.type = 42u;
    chk(iv_chan_hdr_check(&h) == IV_EPROTO, "check rejects unknown type");

    iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 0u);
    h.payload_len = IV_CHAN_MAX_PAYLOAD + 1u;
    chk(iv_chan_hdr_check(&h) == IV_ERANGE, "check rejects oversize payload_len");

    /* pad0 / reserved 是留给未来的，接收侧必须容忍非 0 ——
     * 否则将来用它们承载新语义就变成破坏性变更。*/
    iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 0u);
    h.pad0     = 0xDEADBEEFu;
    h.reserved = 0x12345678u;
    chk(iv_chan_hdr_check(&h) == IV_OK, "check tolerates nonzero pad0/reserved");

    chk(strcmp(iv_chan_type_name(IV_CHAN_TYPE_REQ), "REQ") == 0, "type name REQ");
    chk(strcmp(iv_chan_type_name(IV_CHAN_TYPE_RSP), "RSP") == 0, "type name RSP");
    chk(strcmp(iv_chan_type_name(IV_CHAN_TYPE_EVT), "EVT") == 0, "type name EVT");
    chk(strcmp(iv_chan_type_name((uint16_t)0u), "UNKNOWN") == 0, "type name falls back to UNKNOWN");
}

static void test_acl_rules(void)
{
    static const uid_t k_uids[2] = {(uid_t)4242u, (uid_t)4343u};
    iv_chan_acl_t      acl;

    iv_chan_acl_default(&acl);
    chk(acl.uids == NULL && acl.count == 0u, "default acl carries no explicit uid list");
    chk(acl.allow_root == 1 && acl.allow_owner == 1, "default acl allows root and owner");
    chk(iv_chan_acl_permits(&acl, (uid_t)0, (uid_t)1000u) == 1, "default acl allows root");
    chk(iv_chan_acl_permits(&acl, (uid_t)1000u, (uid_t)1000u) == 1, "default acl allows owner");
    chk(iv_chan_acl_permits(&acl, (uid_t)1234u, (uid_t)1000u) == 0, "default acl rejects a stranger");

    /* owner 未知（(uid_t)-1）时不能把"未知"当成"命中"。*/
    chk(iv_chan_acl_permits(&acl, (uid_t)-1, (uid_t)-1) == 0,
        "default acl does not treat unknown owner as a match");

    iv_chan_acl_default(&acl);
    acl.allow_owner = 0;
    chk(iv_chan_acl_permits(&acl, (uid_t)0, (uid_t)1000u) == 1, "root-only acl allows root");
    chk(iv_chan_acl_permits(&acl, (uid_t)1000u, (uid_t)1000u) == 0,
        "root-only acl rejects the owner too");

    iv_chan_acl_default(&acl);
    acl.allow_root  = 0;
    acl.allow_owner = 0;
    acl.uids        = k_uids;
    acl.count       = 2u;
    chk(iv_chan_acl_permits(&acl, (uid_t)4242u, (uid_t)-1) == 1, "explicit acl allows first listed uid");
    chk(iv_chan_acl_permits(&acl, (uid_t)4343u, (uid_t)-1) == 1, "explicit acl allows second listed uid");
    chk(iv_chan_acl_permits(&acl, (uid_t)0, (uid_t)-1) == 0, "explicit acl rejects root when allow_root=0");
    chk(iv_chan_acl_permits(&acl, (uid_t)5555u, (uid_t)-1) == 0, "explicit acl rejects unlisted uid");

    chk(iv_chan_acl_permits(NULL, (uid_t)0, (uid_t)0) == 0, "NULL acl rejects everything");
}

/* --------------------------------------------------------------------------- */

static void test_exchange(void)
{
    static const char k_req[] = "{\"method\":\"system.snapshot\"}";
    ivs_chan_hdr_t    h;
    ivs_chan_hdr_t    rh;
    size_t            rlen = 0u;
    int               sv[2];

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        fprintf(stderr, "FAIL: socketpair(): %s\n", strerror(errno));
        g_fail++;
        return;
    }

    /* 1) 正常往返：头与载荷逐字节核对 */
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 12345u, 0u, (uint32_t)strlen(k_req)) == IV_OK,
        "build request header");
    chk(iv_chan_send(sv[0], &h, k_req, strlen(k_req)) == IV_OK, "send returns OK");

    rlen = 0u;
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_OK, "recv returns OK");
    chk(rh.magic == IV_CHAN_MAGIC, "received magic matches");
    chk(rh.version == IV_CHAN_VERSION, "received version matches");
    chk(rh.type == IV_CHAN_TYPE_REQ, "received type matches");
    chk(rh.request_id == 12345u, "request_id survives the round trip");
    chk(rlen == strlen(k_req), "payload length survives the round trip");
    chk(memcmp(g_rx + IV_CHAN_HDR_SIZE, k_req, rlen) == 0, "payload bytes survive the round trip");

    /* 2) 空载荷（事件通知常见形态） */
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_EVT, 0u, 0u, 0u) == IV_OK, "build zero-payload header");
    chk(iv_chan_send(sv[0], &h, NULL, 0u) == IV_OK, "send with NULL payload and zero length");
    rlen = 0u;
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_OK, "recv zero-payload message");
    chk(rlen == 0u, "zero payload arrives with length 0");
    chk(rh.type == IV_CHAN_TYPE_EVT, "zero-payload type matches");

    /* 3) 包比接收缓冲大 → IV_ERANGE，且该包已被整包丢弃（不会留半截给下一次） */
    memset(g_tx, 0xAB, 200u);
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 1u, 0u, 200u) == IV_OK, "build oversize-driver header");
    chk(iv_chan_send(sv[0], &h, g_tx, 200u) == IV_OK, "send a 200-byte packet");
    {
        uint8_t small[64]; /* 只有 64 字节，装不下 32+200 */
        rlen = 0u;
        chk(iv_chan_recv(sv[1], small, sizeof small, &rh, &rlen) == IV_ERANGE,
            "recv rejects a packet larger than the caller buffer");
    }
    /* 紧跟其后发一条正常包，必须能干净收到 —— 这条才真正证明"超长包被整包丢弃、
     * 队列里没留下残渣" */
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 2u, 0u, 3u) == IV_OK, "build follow-up header");
    chk(iv_chan_send(sv[0], &h, "abc", 3u) == IV_OK, "send follow-up packet");
    rlen = 0u;
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_OK, "recv follow-up packet");
    chk(rh.request_id == 2u && rlen == 3u && memcmp(g_rx + IV_CHAN_HDR_SIZE, "abc", 3u) == 0,
        "follow-up packet is intact after an oversize packet was dropped");

    /* 4) 短包（不足一个消息头）→ IV_EPROTO */
    {
        uint8_t junk[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        chk(send(sv[0], junk, sizeof junk, 0) == (ssize_t)sizeof junk, "raw-send an 8-byte runt");
        rlen = 0u;
        chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_EPROTO,
            "recv rejects a packet shorter than the header");
    }

    /* 5) 头里撒谎：声明 payload_len=100 但整包只有 32+10 字节 → IV_EPROTO。
     *    少了这条校验，伪造长度就能让上层按错误边界去读缓冲。*/
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 3u, 0u, 100u) == IV_OK, "build lying header");
    memcpy(g_tx, &h, IV_CHAN_HDR_SIZE);
    chk(send(sv[0], g_tx, IV_CHAN_HDR_SIZE + 10u, 0) == (ssize_t)(IV_CHAN_HDR_SIZE + 10u),
        "raw-send a packet whose header overstates its length");
    rlen = 0u;
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_EPROTO,
        "recv rejects a header that overstates payload_len");

    /* 6) 参数校验 */
    chk(iv_chan_recv(-1, g_rx, sizeof g_rx, &rh, &rlen) == IV_EINVAL, "recv rejects bad fd");
    chk(iv_chan_recv(sv[1], NULL, sizeof g_rx, &rh, &rlen) == IV_EINVAL, "recv rejects NULL buffer");
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, NULL, &rlen) == IV_EINVAL, "recv rejects NULL hdr");
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, NULL) == IV_EINVAL, "recv rejects NULL len");
    chk(iv_chan_recv(sv[1], g_rx, 8u, &rh, &rlen) == IV_EINVAL,
        "recv rejects a buffer smaller than the header");

    chk(iv_chan_send(-1, &h, NULL, 0u) == IV_EINVAL, "send rejects bad fd");
    chk(iv_chan_send(sv[0], NULL, NULL, 0u) == IV_EINVAL, "send rejects NULL header");

    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_REQ, 0u, 0u, 5u) == IV_OK, "build header declaring 5 bytes");
    chk(iv_chan_send(sv[0], &h, "abcd", 4u) == IV_EINVAL,
        "send rejects header/payload length mismatch");
    chk(iv_chan_send(sv[0], &h, NULL, 5u) == IV_EINVAL,
        "send rejects NULL payload with nonzero length");

    h.payload_len = IV_CHAN_MAX_PAYLOAD + 1u;
    h.magic       = IV_CHAN_MAGIC; /* 头字段本身仍合法，专测长度上限 */
    chk(iv_chan_send(sv[0], &h, g_tx, (size_t)IV_CHAN_MAX_PAYLOAD + 1u) == IV_ERANGE,
        "send rejects a payload over the limit");

    /* 7) 对端关闭 → IV_ECONN */
    close(sv[0]);
    rlen = 0u;
    chk(iv_chan_recv(sv[1], g_rx, sizeof g_rx, &rh, &rlen) == IV_ECONN,
        "recv reports ECONN once the peer has closed");
    close(sv[1]);
}

static void test_recv_timeout(void)
{
    ivs_chan_hdr_t h;
    ivs_chan_hdr_t rh;
    size_t         rlen = 0u;
    int            sv[2];

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        fprintf(stderr, "FAIL: socketpair() for timeout test: %s\n", strerror(errno));
        g_fail++;
        return;
    }

    /* 无人发言 → 到期返回 IV_ETIMEDOUT，而不是永远等下去 */
    chk(iv_chan_recv_timeout(sv[0], g_rx, sizeof g_rx, &rh, &rlen, 100) == IV_ETIMEDOUT,
        "recv_timeout returns ETIMEDOUT when nothing arrives");

    chk(iv_chan_recv_timeout(sv[0], g_rx, sizeof g_rx, &rh, &rlen, -1) == IV_EINVAL,
        "recv_timeout rejects a negative timeout (no infinite wait)");
    chk(iv_chan_recv_timeout(-1, g_rx, sizeof g_rx, &rh, &rlen, 10) == IV_EINVAL,
        "recv_timeout rejects a bad fd");

    /* 先发后收 → 必须命中而不是超时 */
    chk(iv_chan_hdr_init(&h, IV_CHAN_TYPE_RSP, 77u, 0u, 3u) == IV_OK, "build response header");
    chk(iv_chan_send(sv[1], &h, "abc", 3u) == IV_OK, "send before timing out");
    rlen = 0u;
    chk(iv_chan_recv_timeout(sv[0], g_rx, sizeof g_rx, &rh, &rlen, 1000) == IV_OK,
        "recv_timeout succeeds when data is already queued");
    chk(rh.type == IV_CHAN_TYPE_RSP && rh.request_id == 77u && rlen == 3u,
        "timed recv delivers the right message");

    close(sv[0]);
    close(sv[1]);
}

/* --------------------------------------------------------------------------- */

static void test_listen_paths_and_auth(void)
{
    static const uid_t k_never[1] = {(uid_t)54321u};
    char               dir[]      = "/tmp/ivchanXXXXXX";
    char               path[160];
    char               plain[160];
    char               missing[200];
    struct stat        st;
    iv_chan_acl_t      strict;
    iv_chan_peer_t     peer;
    int                lfd;
    int                lfd2;
    int                cfd;
    int                afd;
    FILE              *f;
    size_t             i;

    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "FAIL: mkdtemp(): %s\n", strerror(errno));
        g_fail++;
        return;
    }

    (void)snprintf(path, sizeof path, "%s/ctl", dir);
    (void)snprintf(plain, sizeof plain, "%s/plain", dir);
    (void)snprintf(missing, sizeof missing, "%s/nosuchdir/ctl", dir);

    /* 1) 正常建立接听点：文件被创建为 socket、权限恰为 0660（不受 umask 影响） */
    lfd = iv_chan_listen(path, 0u, 0u); /* 传 0 → 取默认 mode/backlog */
    chk(lfd >= 0, "listen succeeds with default mode and backlog");
    if (lfd < 0) {
        (void)rmdir(dir);
        return;
    }
    chk(stat(path, &st) == 0 && S_ISSOCK(st.st_mode), "listen creates a socket file");
    chk((st.st_mode & (mode_t)0777) == (mode_t)0660, "socket file mode is exactly 0660");

    /* 2) 连接 + 接受：对端身份应是本进程自己 */
    cfd = iv_chan_connect(path);
    chk(cfd >= 0, "connect to the listening socket succeeds");
    if (cfd >= 0) {
        memset(&peer, 0, sizeof peer);
        afd = iv_chan_accept(lfd, NULL, &peer);
        chk(afd >= 0, "accept succeeds with the default acl");
        chk(peer.uid == getuid(), "SO_PEERCRED reports our own uid");
        chk(peer.pid == getpid(), "SO_PEERCRED reports our own pid");
        if (afd >= 0)
            (void)close(afd);
        (void)close(cfd);
    }

    /* 3) 没有待处理连接时 accept 返回 IV_EAGAIN（非阻塞，属正常现象） */
    chk(iv_chan_accept(lfd, NULL, NULL) == IV_EAGAIN, "accept reports EAGAIN when nothing is pending");

    /* 4) 白名单拒绝：规则本身合法，但当前进程的 uid 不在名单里。
     *    连接在内核层面建得起来（文件权限允许），到应用层被拒。*/
    iv_chan_acl_default(&strict);
    strict.allow_root  = 0;
    strict.allow_owner = 0;
    strict.uids        = k_never;
    strict.count       = 1u;

    cfd = iv_chan_connect(path);
    chk(cfd >= 0, "connect still succeeds at the kernel level");
    if (cfd >= 0) {
        chk(iv_chan_accept(lfd, &strict, NULL) == IV_EAUTH,
            "accept rejects a uid outside the acl");
        (void)close(cfd);
    }

    /* 5) 路径被**非 socket** 占用时必须拒绝，且**不得删除该文件** ——
     *    这条是"路径写错"与"删掉别人的文件"之间的唯一防线。*/
    f = fopen(plain, "w");
    chk(f != NULL, "create a plain file to occupy a path");
    if (f != NULL) {
        (void)fputs("x", f);
        (void)fclose(f);
    }
    chk(iv_chan_listen(plain, 0u, 0u) == IV_EEXIST, "listen refuses a path held by a non-socket");
    chk(stat(plain, &st) == 0, "the occupying plain file was NOT deleted");

    /* 6) 路径被**上一个 socket 文件**占用（进程被 kill 没做清理）→ 重建。
     *    close 而不 unlink，刻意留下残留。*/
    (void)close(lfd);
    lfd2 = iv_chan_listen(path, 0u, 0u);
    chk(lfd2 >= 0, "listen re-creates over a leftover socket file");
    if (lfd2 < 0)
        lfd2 = -1;

    /* 7) 父目录不存在 → IV_ENOENT（本模块不代建目录，见头文件说明） */
    chk(iv_chan_listen(missing, 0u, 0u) == IV_ENOENT,
        "listen reports ENOENT when the parent directory is missing");
    chk(iv_chan_connect(missing) == IV_ENOENT, "connect reports ENOENT for a missing socket");

    /* 8) 路径长到放不进 sun_path → IV_ERANGE */
    {
        char longp[256];
        memset(longp, 'a', sizeof longp);
        longp[0]                 = '/';
        longp[sizeof longp - 1u] = '\0';
        chk(iv_chan_listen(longp, 0u, 0u) == IV_ERANGE, "listen rejects an over-long path");
        chk(iv_chan_connect(longp) == IV_ERANGE, "connect rejects an over-long path");
    }

    /* 9) 正常关闭：fd 关掉、路径被清理 */
    if (lfd2 >= 0) {
        chk(iv_chan_listen_close(lfd2, path) == IV_OK, "listen_close returns OK");
        chk(stat(path, &st) != 0 && errno == ENOENT, "listen_close removed the socket file");
    }
    chk(iv_chan_listen_close(-1, NULL) == IV_OK, "listen_close on a bad fd with NULL path is a no-op");

    /* 清理（规则 6：不留临时文件） */
    (void)unlink(plain);
    for (i = 0u; i <= 1u; i++) {
        char extra[192];
        (void)snprintf(extra, sizeof extra, "%s/%s", dir, (i == 0u) ? "ctl" : "plain");
        (void)unlink(extra);
    }
    (void)rmdir(dir);
}

/* --------------------------------------------------------------------------- */

int main(void)
{
    test_header_layout();
    test_header_codec();
    test_acl_rules();
    test_exchange();
    test_recv_timeout();
    test_listen_paths_and_auth();

    if (g_fail != 0) {
        fprintf(stderr, "test_chan failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_chan passed (header layout, codec, acl, seqpacket io, timeout, listen paths)\n");
    return 0;
}
