/*
 * 本地进程通道实现（libivcore）。
 *
 * 本文件是"同一块板子上两个程序之间怎么递一条完整消息"的实现，不含任何业务语义：
 * 载荷对它是 void* + len。定位、传输选型理由见 include/ivsbox/iv_chan.h。
 *
 * 三条实现纪律，改动时请一并守住：
 *   1. **不做隐藏的内存分配**。接收缓冲由调用方提供（iv_chan_recv 的 buf/cap），
 *      发送用 sendmsg 两段 iovec 直接发出，全文件没有一处 malloc。
 *      嵌入式下"内存从哪来"必须一眼看得见。
 *   2. **不产生 SIGPIPE**。发送一律带 MSG_NOSIGNAL，对端先关闭时只返回 IV_ECONN。
 *      （实测补充：本传输 SEQPACKET 上内核本就不送 SIGPIPE，该标志是"换流式传输
 *      也不用改"的预防性令牌，见 iv_chan_send 的说明。）
 *   3. **不依赖 errno 做返回码**。对外返回 iv_ret.h 的负数码，errno 保留原值
 *      供调用方诊断（两者互不覆盖）。理由：调用方不该被迫 include <errno.h>
 *      才能区分"超时"和"被拒"。
 */
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "ivsbox/iv_chan.h"
#include "ivsbox/iv_ret.h"

/* ---------------------------------------------------------------------------
 * 消息头工具
 * ------------------------------------------------------------------------- */

const char *iv_chan_type_name(uint16_t type)
{
    switch (type) {
    case IV_CHAN_TYPE_REQ:
        return "REQ";
    case IV_CHAN_TYPE_RSP:
        return "RSP";
    case IV_CHAN_TYPE_EVT:
        return "EVT";
    default:
        return "UNKNOWN";
    }
}

int iv_chan_hdr_init(ivs_chan_hdr_t *hdr, uint16_t type, uint64_t request_id,
                     uint32_t flags, uint32_t payload_len)
{
    if (hdr == NULL)
        return IV_EINVAL;
    if (type != IV_CHAN_TYPE_REQ && type != IV_CHAN_TYPE_RSP && type != IV_CHAN_TYPE_EVT)
        return IV_EINVAL;
    if (payload_len > IV_CHAN_MAX_PAYLOAD)
        return IV_ERANGE;

    hdr->magic       = IV_CHAN_MAGIC;
    hdr->version     = IV_CHAN_VERSION;
    hdr->type        = type;
    hdr->flags       = flags;
    hdr->pad0        = 0u; /* 显式填充，恒 0；非 0 会被对端当成布局不一致 */
    hdr->request_id  = request_id;
    hdr->payload_len = payload_len;
    hdr->reserved    = 0u;

    return IV_OK;
}

int iv_chan_hdr_check(const ivs_chan_hdr_t *hdr)
{
    if (hdr == NULL)
        return IV_EINVAL;

    if (hdr->magic != IV_CHAN_MAGIC)
        return IV_EPROTO;
    if (hdr->version != IV_CHAN_VERSION)
        return IV_EPROTO;
    if (hdr->type != IV_CHAN_TYPE_REQ && hdr->type != IV_CHAN_TYPE_RSP &&
        hdr->type != IV_CHAN_TYPE_EVT)
        return IV_EPROTO;

    if (hdr->payload_len > IV_CHAN_MAX_PAYLOAD)
        return IV_ERANGE;

    /* 刻意不检查 pad0 / reserved：它们保留给未来使用，接收侧必须容忍非 0 值，
     * 否则将来用它们传新语义时就变成破坏性变更。*/
    return IV_OK;
}

/* ---------------------------------------------------------------------------
 * 访问白名单
 * ------------------------------------------------------------------------- */

void iv_chan_acl_default(iv_chan_acl_t *acl)
{
    if (acl == NULL)
        return;
    acl->uids        = NULL;
    acl->count       = 0u;
    acl->allow_root  = 1;
    acl->allow_owner = 1; /* 默认不写死任何 uid：放行谁留给装配时决定 */
}

int iv_chan_acl_permits(const iv_chan_acl_t *acl, uid_t uid, uid_t owner)
{
    size_t i;

    if (acl == NULL)
        return 0;

    if (acl->allow_root && uid == (uid_t)0)
        return 1;

    if (acl->allow_owner && owner != (uid_t)-1 && uid == owner)
        return 1;

    if (acl->uids != NULL) {
        for (i = 0u; i < acl->count; i++) {
            if (acl->uids[i] == uid)
                return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 接听点
 * ------------------------------------------------------------------------- */

int iv_chan_listen(const char *path, unsigned mode, int backlog)
{
    struct sockaddr_un addr;
    struct stat        st;
    size_t             plen;
    int                fd;
    int                rc;

    if (path == NULL)
        path = IV_CHAN_PATH_DEFAULT;
    if (mode == 0u)
        mode = IV_CHAN_MODE_DEFAULT;
    if (backlog <= 0)
        backlog = IV_CHAN_BACKLOG_DEFAULT;

    plen = strlen(path);
    if (plen == 0u || plen >= sizeof(addr.sun_path))
        return IV_ERANGE;

    /* 路径占用检查。
     * 是**上一个 socket 文件**（进程被 kill -9、没走正常清理）→ unlink 后重建，
     * 否则服务重启永远起不来。
     * 是**别的任何东西**（普通文件 / 目录 / 符号链接）→ 拒绝。绝不 unlink 非
     * socket：那等于把"路径写错"这种低级错误变成"删掉别人的文件"这种事故。*/
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode))
            return IV_EEXIST;
        if (unlink(path) != 0)
            return IV_EIO;
    } else if (errno != ENOENT) {
        return IV_EIO;
    }

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return IV_EFAIL;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, plen); /* 尾部靠上面的 memset 保证 NUL 结尾 */

    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        rc = (errno == ENOENT) ? IV_ENOENT : IV_EIO;
        (void)close(fd);
        return rc;
    }

    /* 权限在 bind 之后**显式 chmod**：socket 文件的模式会被进程 umask 削减，
     * 而 umask 是环境决定的东西，安全边界不能依赖它恰好是什么。
     * 下面两条失败路径都要先 close/unlink 再返回，而那两个调用都可能改写 errno；
     * 头文件承诺"errno 保留原值供诊断"，故就地存下再复原。*/
    if (chmod(path, (mode_t)mode) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path); /* 不留一个权限不对的接听点 */
        errno = saved;
        return IV_EIO;
    }

    if (listen(fd, backlog) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return IV_EIO;
    }

    return fd;
}

int iv_chan_listen_close(int listen_fd, const char *path)
{
    int rc = IV_OK;

    if (listen_fd >= 0 && close(listen_fd) != 0)
        rc = IV_EIO;

    /* path 为 NULL 时**只关 fd、不动文件系统**：避免"只想关连接却顺手删了
     * 别人路径"这种意外。要清理接听点文件就必须显式把路径给出来。*/
    if (path != NULL && unlink(path) != 0 && errno != ENOENT)
        rc = IV_EIO;

    return rc;
}

int iv_chan_accept(int listen_fd, const iv_chan_acl_t *acl, iv_chan_peer_t *peer)
{
    struct sockaddr_un addr;
    struct ucred       cred;
    socklen_t          alen = (socklen_t)sizeof(addr);
    socklen_t          clen = (socklen_t)sizeof(cred);
    iv_chan_acl_t      def;
    struct stat        st;
    uid_t              owner = (uid_t)-1;
    int                fd;

    if (listen_fd < 0)
        return IV_EINVAL;

    fd = accept4(listen_fd, (struct sockaddr *)&addr, &alen, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IV_EAGAIN; /* 没有待处理连接，正常现象 */
        return IV_ECONN;
    }

    /* SO_PEERCRED：内核在连接建立那一刻记录下来，**对端无法伪造**。
     * 这是 AF_UNIX 相对回环 TCP 的核心优势，也是本模块把 SO_PEERCRED 当作
     * 鉴权依据而不是"参考信息"的原因。*/
    memset(&cred, 0, sizeof(cred));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0) {
        (void)close(fd);
        return IV_EFAIL;
    }

    if (acl == NULL) {
        iv_chan_acl_default(&def);
        acl = &def;
    }
    /* 取接听点属主用于 allow_owner 规则。**取不到时 owner 保持 -1（未知）**，
     * 而 iv_chan_acl_permits() 明确"未知不算命中" ⇒ 该规则静默不生效。
     * 方向是 fail-closed（少放行、不会误放行），且此处 listen_fd 刚被 accept4
     * 用过、fstat 实际不可达，故不为此加返回值。*/
    if (acl->allow_owner && fstat(listen_fd, &st) == 0)
        owner = st.st_uid;

    if (!iv_chan_acl_permits(acl, cred.uid, owner)) {
        /* **不发送任何回应**直接关闭：不告诉对方"你被拒了"，免得给扫描脚本
         * 当路标。对端看到的是连接建立后立刻 EOF，与"服务刚好退出"无法区分。*/
        (void)close(fd);
        return IV_EAUTH;
    }

    if (peer != NULL) {
        peer->pid = cred.pid;
        peer->uid = cred.uid;
        peer->gid = cred.gid;
    }

    return fd;
}

/* ---------------------------------------------------------------------------
 * 连接端
 * ------------------------------------------------------------------------- */

int iv_chan_connect(const char *path)
{
    struct sockaddr_un addr;
    size_t             plen;
    int                fd;
    int                rc;

    if (path == NULL)
        path = IV_CHAN_PATH_DEFAULT;

    plen = strlen(path);
    if (plen == 0u || plen >= sizeof(addr.sun_path))
        return IV_ERANGE;

    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return IV_EFAIL;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, plen);

    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        if (errno == ENOENT)
            rc = IV_ENOENT; /* 接听点不存在 = 服务没起 */
        else if (errno == EACCES || errno == EPERM)
            rc = IV_EAUTH; /* 被文件权限这道门槛挡下 */
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            rc = IV_EBUSY; /* backlog 满，稍后重试 */
        else
            rc = IV_ECONN;
        (void)close(fd);
        return rc;
    }

    return fd;
}

/* ---------------------------------------------------------------------------
 * 收发
 * ------------------------------------------------------------------------- */

int iv_chan_recv(int fd, void *buf, size_t cap, ivs_chan_hdr_t *hdr, size_t *payload_len)
{
    ssize_t n;
    int     rc;

    if (fd < 0 || buf == NULL || hdr == NULL || payload_len == NULL)
        return IV_EINVAL;

    /* cap 必须至少装得下一个头，否则连类型都读不出来，不如直接拒绝。
     * 注意合法消息需要 cap >= IV_CHAN_RECV_CAP_MIN，见头文件。*/
    if (cap < IV_CHAN_HDR_SIZE)
        return IV_EINVAL;

    /* MSG_TRUNC 的语义：返回值是**整包长度**，即使它大于 cap；
     * 而实际只往 buf 里写了前 cap 字节。
     * 这里只调一次 recv，因此不存在"先探长度再接收"两次系统调用之间的竞态。
     * 接收缓冲不够时该包会被内核整包丢弃（SEQPACKET 是面向消息的，不存在
     * "读半截、剩下一半留队列"的情况）—— 这正是我们要的"拒绝超长"语义。*/
    n = recv(fd, buf, cap, MSG_TRUNC);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IV_EAGAIN;
        /* 两种内核行为都要兜住：MSG_TRUNC 下按文档应返回真实长度，但内核
         * 在"缓冲装不下整包"这条路径上也可能直接给 EMSGSIZE。两者都表示
         * "这个包我们收不下、且已被丢弃"。*/
        if (errno == EMSGSIZE)
            return IV_ERANGE;
        return IV_ECONN;
    }
    if (n == 0)
        return IV_ECONN; /* 对端已关闭 */

    if ((size_t)n > cap)
        return IV_ERANGE; /* 包比缓冲大，已被丢弃，buf 内容不可用 */

    if ((size_t)n < IV_CHAN_HDR_SIZE)
        return IV_EPROTO; /* 连一个消息头都不够 */

    memcpy(hdr, buf, IV_CHAN_HDR_SIZE);

    rc = iv_chan_hdr_check(hdr);
    if (rc != IV_OK)
        return rc;

    /* 头里声明的 payload_len 必须与实际收到的包长严丝合缝。
     * 少了这条，一个伪造 payload_len 的对端就能让上层按错误的边界去读缓冲。*/
    if (IV_CHAN_HDR_SIZE + (size_t)hdr->payload_len != (size_t)n)
        return IV_EPROTO;

    *payload_len = (size_t)hdr->payload_len;
    return IV_OK;
}

/* 距离 t0 已过去的毫秒数；clock_gettime 失败返回 -1。
 * 只用 CLOCK_MONOTONIC：墙钟会被 NTP / 手动改时拨动，而超时预算必须单调。*/
static int64_t elapsed_ms_from(const struct timespec *t0)
{
    struct timespec now;
    int64_t         ms;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return (int64_t)-1;

    ms = ((int64_t)now.tv_sec - (int64_t)t0->tv_sec) * 1000;
    ms += ((int64_t)now.tv_nsec - (int64_t)t0->tv_nsec) / 1000000;
    return ms;
}

int iv_chan_recv_timeout(int fd, void *buf, size_t cap, ivs_chan_hdr_t *hdr,
                         size_t *payload_len, int timeout_ms)
{
    struct pollfd   pfd;
    struct timespec t0;
    int64_t         used;
    int             wait_ms;
    int             rc;

    if (fd < 0 || timeout_ms < 0)
        return IV_EINVAL;

    /* 先取基准时刻，再进循环：预算从**进入本函数**开始算，而不是从第一次
     * poll 开始算，这样"取时刻本身"的耗时也计入预算。*/
    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0)
        return IV_EFAIL;

    pfd.fd     = fd;
    pfd.events = POLLIN;

    for (;;) {
        used = elapsed_ms_from(&t0);
        if (used < 0)
            return IV_EFAIL;

        /* 剩多少预算就等多少。clamp 到 0 而不是直接判超时：poll(0) 只做状态
         * 检查，能让"刚好在到点那一刻数据到了"不被误判成超时；而预算真耗尽时
         * poll(0) 必然返回 0 → IV_ETIMEDOUT，不会在这里空转。*/
        wait_ms = (int)((int64_t)timeout_ms - used);
        if (wait_ms < 0)
            wait_ms = 0;

        pfd.revents = 0;
        rc          = poll(&pfd, 1, wait_ms);
        if (rc > 0)
            break;
        if (rc == 0)
            return IV_ETIMEDOUT;

        /* rc < 0：只有 EINTR 值得重试，其余是硬错误。
         * **绝不把已经花掉的时间丢掉重来** —— 下一轮用的是剩余预算而不是完整的
         * timeout_ms。否则持续的信号流（SIGCHLD 之类，poll 在 Linux 上不因
         * SA_RESTART 而重启）会让"超时"永远不到期，与架构 §5.1「请求必须设置
         * 超时」的纪律相悖。*/
        if (errno != EINTR)
            return IV_EFAIL;
    }

    /* 可读以外的事件（POLLHUP / POLLERR / POLLNVAL）都表示这条连接完了。
     * 特别地 POLLHUP 之后仍可能有数据，但由下一次 recv 处理更清楚。*/
    if ((pfd.revents & POLLIN) == 0)
        return IV_ECONN;

    return iv_chan_recv(fd, buf, cap, hdr, payload_len);
}

int iv_chan_send(int fd, const ivs_chan_hdr_t *hdr, const void *payload, size_t payload_len)
{
    struct iovec  iov[2];
    struct msghdr msg;
    ssize_t       n;
    int           rc;

    if (fd < 0 || hdr == NULL)
        return IV_EINVAL;
    if (payload_len > (size_t)IV_CHAN_MAX_PAYLOAD)
        return IV_ERANGE;
    if (payload_len > 0u && payload == NULL)
        return IV_EINVAL;

    rc = iv_chan_hdr_check(hdr);
    if (rc != IV_OK)
        return rc;

    /* 头里写的长度与实际要发的载荷必须一致：两者不符时对端会按头里的长度解析，
     * 轻则报错、重则越界读，属于必须在发送侧就挡掉的错误。*/
    if ((size_t)hdr->payload_len != payload_len)
        return IV_EINVAL;

    /* SEQPACKET 的 sendmsg 支持多段 iovec，整条消息作为一个报文发出，
     * 因此不需要把"头 + 载荷"拼进一块临时缓冲（那会引入一次分配和一次拷贝）。*/
    iov[0].iov_base = (void *)(uintptr_t)hdr;
    iov[0].iov_len  = IV_CHAN_HDR_SIZE;
    iov[1].iov_base = (void *)(uintptr_t)payload;
    iov[1].iov_len  = payload_len;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov    = iov;
    msg.msg_iovlen = (payload_len > 0u) ? 2 : 1;

    /* MSG_NOSIGNAL：对端先关闭时只返回 EPIPE，不会给本进程送 SIGPIPE。
     * **但它在本传输上是预防性的**：实测 `AF_UNIX/SOCK_SEQPACKET` 上对端关闭后
     * sendmsg 只给 EPIPE、内核不送 SIGPIPE（带不带该标志结果逐字相同），
     * SIGPIPE 只在 SOCK_STREAM 上出现。详见头文件 iv_chan_send 的说明。*/
    n = sendmsg(fd, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return IV_EAGAIN; /* 一条都没发出去，调用方可安全重试 */
        return IV_ECONN;
    }

    /* SEQPACKET 是原子的：要么整包发出、要么一个字节都没发，不存在"短写"。
     * 真收到短写说明内核行为异常，按错误报出而不是假装成功。*/
    if ((size_t)n != IV_CHAN_HDR_SIZE + payload_len)
        return IV_EFAIL;

    return IV_OK;
}
