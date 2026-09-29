/*
 * IVSBox 本地进程通道（libivcore，功能开发计划 M1-S7）
 *
 * ============================ 命名 ============================
 * 本模块原名 "IPC"，2026-09-29 按用户要求改名。原因：本项目 M4 设备管理对接的
 * 就是海康/大华**网络摄像机**，业内 "IPC" 即 IP Camera，同一个缩写在一个仓库里
 * 指两件事必然出事。中文正文仍可写"进程间通信"，**英文文件名 / 类型名 / 函数
 * 前缀一律用 chan**。架构文档与本计划文档中的对应字样已同步更改。
 *
 * ============================ 它是什么 ============================
 * 同一块板子上同时跑着三个各自独立的程序（`ivsboxd` 主控、`ivsbox-media` 媒体、
 * `ivsbox-web` 网页），操作系统默认让它们互相隔离：看不见对方的内存，也不能直接
 * 指挥对方干活。本模块提供的就是它们之间"传话与派活"的那条通道。
 *
 * 职责边界（**刻意收窄**）：只负责"把一条完整消息从一个程序递到另一个程序"--
 * 建接听点、接连接、查来电话的是谁、收一整包、发一整包、超时、关闭。
 * **它不理解载荷内容**：载荷对它是 `void *` + `len` 的一段不透明字节。
 *
 * 为什么不懂载荷（三条理由，都不是洁癖）：
 *   1. 依赖纪律。libivcore 只允许依赖 libc，链接顺序把 core 排在最后并用
 *      `-Wl,--no-undefined` 卡着。一旦在这里解析 JSON，就必须给 core 引入外部
 *      库依赖，代价是整个依赖方向表都要改（见架构 §15.5）。
 *   2. 架构 §5.1 本来就要求支持两种载荷：控制和查询用 JSON，媒体内部高频状态
 *      可用版本化定长结构。通道层若只懂 JSON，第二种载荷就无处安放。
 *   3. 载荷解析放到上层之后，"用哪个 JSON 库"这个选型可以推迟到真正需要它的
 *      那一步再定，不必现在为一个还没被调用的能力绑死依赖。
 *
 * 另外本模块**不绑定事件循环**：它不引用 iv_reactor，只提供 fd 级接口，由调用方
 * 把接听点 fd 与连接 fd 注册进自己的循环。理由：`ivsbox-media` / `ivsbox-web`
 * 是独立进程、各有自己的控制循环，绑死某一个会让另外两个用不了；解绑之后本模块
 * 还能在单测里完全脱离 Reactor 验证（与 iv_watchdog 用 pipe 假 fd 是同一思路）。
 *
 * ============================ 传输选型 ============================
 * `AF_UNIX` + `SOCK_SEQPACKET`（架构 §5.1）。
 *
 * 为什么是 SEQPACKET 而不是更常见的 STREAM：**内核替我们保留消息边界**。一次
 * recv 必定拿到一条完整消息，不必自己做"半包拼接 / 粘包拆分"这套容易出错的状态机。
 * 配套的纪律是"整包超过接收缓冲就整包丢弃"（见 iv_chan_recv），这条语义是
 * SEQPACKET 给的，在 STREAM 上做不到这么干净。
 *
 * 为什么是 AF_UNIX 而不是回环 TCP：① 内核直接给出对端身份（`SO_PEERCRED`），
 * 由内核在连接建立时记录、**对端伪造不了**，回环 TCP 做不到；② 接听点就是一个
 * 文件，`0660` 权限位由内核天然限制谁能连；③ AF_UNIX 协议族本身具备经
 * `SCM_RIGHTS` 辅助消息传递文件描述符的能力，为架构 §5.1"超限数据通过文件
 * 句柄传递"保留了将来落地的余地。
 * **注意（口径更正，S7-02）：本模块当前实现并不提供 SCM_RIGHTS 传 fd**，
 * send/recv 只搬运头 + 载荷字节；大块数据走受控路径（对端落盘、通道里传文件
 * 路径，见 IV_CHAN_MAX_PAYLOAD 的说明）。将来真正实现传 fd 后，此处口径再更新。
 *
 * **数据全程在内核内存里搬运，不经过网络协议栈**。因此本模块与板子的网络
 * 通不通、有没有网卡、有没有 IP 完全无关（这一点常被误解，故明确写出）。
 *
 * ============================ 平台事实 ============================
 *   - 两端（板端 armv7 / 编译 VM x86_64）均为小端，故本协议**显式按小端定义**、
 *     不做字节序转换；遇到大端目标会在下面用 `#error` 当场拦下，而不是悄悄错位。
 *   - `SOCK_SEQPACKET` 支持 `sendmsg` 的多段 iovec，故 iv_chan_send 用
 *     "头 + 载荷"两段直接发出，**不需要把两者拼进一块临时缓冲**。
 *   - 板端 `/var/run` 是指向 `../run` 的符号链接，`/run` 为 tmpfs、mode 755
 *     root:root。**普通用户在 /run 下建不出子目录**，所以接听点的父目录必须由
 *     init 脚本以 root 预建并 chown（见 IV_CHAN_PATH_DEFAULT 的说明）。
 */
#ifndef IVSBOX_IV_CHAN_H
#define IVSBOX_IV_CHAN_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 常量与限制 ============================ */

/* 接听点默认路径。
 * **父目录不归本模块创建**：`/run` 是 tmpfs（755 root:root），普通用户建不了
 * 子目录；而"目录由谁建、属主是谁、权限多少"是部署决定，服务端偷偷 mkdir 只会
 * 把部署错误藏起来。故 iv_chan_listen() 在父目录不存在时直接报 IV_ENOENT。
 * 父目录的创建与 chown 放 init 脚本（计划 §S10）。*/
#define IV_CHAN_PATH_DEFAULT "/var/run/ivsbox/ctl"

/* 接听点文件权限。0660 = 属主与同组可读写，其他用户连都连不上（内核在 connect
 * 时按文件写权限拦截）。这是第一道门槛，SO_PEERCRED 白名单是第二道。*/
#define IV_CHAN_MODE_DEFAULT ((unsigned)0660)

/* listen 的 backlog。同时连上来的客户端本来就只有三两个（三个常驻进程 + 调试
 * 工具），8 足够；真正需要防的是"连接数无上限"，那由调用方自己数连接数。*/
#define IV_CHAN_BACKLOG_DEFAULT 8

/* 协议版本。字段布局有任何变化都必须递增，服务端据此拒绝老客户端。*/
#define IV_CHAN_VERSION ((uint16_t)1u)

/* 消息头魔数。按**内存字节序**看依次是 'I' 'V' 'S' 'B'（架构 §5.1 的 'IVSB'）。
 * 小端机上该 32 位值的数值即 0x42535649。*/
#define IV_CHAN_MAGIC ((uint32_t)0x42535649u)

/* 消息头字节数。与 ivs_chan_hdr_t 的实际尺寸绑定，由下方编译期断言锁定。*/
#define IV_CHAN_HDR_SIZE ((size_t)32u)

/* **单包载荷上限**（架构 §5.1"单包最大长度固定"里的那个"固定"）。
 * 取 64 KiB 的理由：控制/查询类消息典型几十到几百字节，10 倍以上余量；
 * 万一真有 64 KiB 的 JSON，解析峰值内存约 200~400 KB，板子承受得起。
 * **这是一道闸门，不是建议**：一超过 64 KiB，按架构 §5.1 就必须改走
 * "文件句柄 / 任务 ID / 分片协议"，绝不能把图片、录像塞进载荷。
 * 为什么专门强调：JSON 是文本格式，二进制必须转 base64（体积 +33%），
 * 且发送方要同时持有"原图 / base64 串 / 拼好的 JSON"三份、接收方再来一遍，
 * 一张 1 MB 的图峰值就能吃掉 5~10 MB 内存 —— 而这条通道是**单包语义**，
 * 整包必须一次性完整躺在内存里，无法边收边处理。图片正确的走法是：
 * 媒体进程落盘成文件，通道里只传文件路径（**当前实现不提供 SCM_RIGHTS 传
 * 文件描述符**，见文件头"传输选型"的口径更正），网页端再用 HTTP
 * 把文件流式发给浏览器，由内核在"磁盘 → socket"之间搬运，用户态缓冲可以
 * 固定几十 KB、与文件大小无关。*/
#define IV_CHAN_MAX_PAYLOAD ((uint32_t)(64u * 1024u))

/* 一次 iv_chan_recv() 要开的接收缓冲**最小**尺寸。
 * 少于此值时，合法消息就可能因缓冲不足被整包丢弃（返回 IV_ERANGE）。*/
#define IV_CHAN_RECV_CAP_MIN (IV_CHAN_HDR_SIZE + (size_t)IV_CHAN_MAX_PAYLOAD)

/* 消息类型（载荷语义由上层定义，通道只认这三个方向）。
 * 长任务不在类型上区分：架构 §5.1 规定"长任务立即返回 task_id"，那是一个
 * 普通响应（IV_CHAN_TYPE_RSP），task_id 写在载荷里，进度与结果走事件
 * （IV_CHAN_TYPE_EVT）。通道层因此不需要知道哪些方法耗时。*/
#define IV_CHAN_TYPE_REQ ((uint16_t)1u) /* 请求：客户端 → 服务端 */
#define IV_CHAN_TYPE_RSP ((uint16_t)2u) /* 响应：服务端 → 客户端，request_id 必须回填 */
#define IV_CHAN_TYPE_EVT ((uint16_t)3u) /* 事件：服务端主动推送，无对应请求 */

/* ============================ 消息头 ============================ */

/* 定长消息头（架构 §5.1）。字段顺序与架构文档一致，**唯一新增的是 pad0**：
 * 原布局把 uint64 的 request_id 放在 flags 之后（偏移 12），而 8 字节成员的
 * 对齐要求两端不同 —— x86-64 要求 8 字节对齐、编译器会在偏移 12 处插入 4 字节
 * 隐式填充（sizeof = 32）；32 位 ARM 的 AAPCS 同样按 8 字节对齐，本该也是 32，
 * 但这类"依赖 ABI 恰好一致"的假设一旦换工具链就会静默错位，而错位的表现是
 * "字段读到隔壁的值"这种最难查的故障。故改为**显式填充**：把 request_id 顶到
 * 偏移 16，布局由源码逐字节确定，不依赖任何 alignof 承诺。
 * 下面的 _Static_assert 会把 sizeof 与全部偏移钉死在编译期，两端任何差异
 * 都是**编译失败**而不是运行期错位。
 * 注意：这是"编码前必须冻结的接口"（架构 §18），本改动已同步进架构文档。*/
typedef struct {
    uint32_t magic;       /* 偏移  0：IV_CHAN_MAGIC */
    uint16_t version;     /* 偏移  4：IV_CHAN_VERSION */
    uint16_t type;        /* 偏移  6：IV_CHAN_TYPE_* */
    uint32_t flags;       /* 偏移  8：位标志，语义由上层定义；通道层不解释 */
    uint32_t pad0;        /* 偏移 12：**显式填充**，恒置 0；存在的唯一理由是让
                           *          request_id 落在 8 字节边界上，见上 */
    uint64_t request_id;  /* 偏移 16：请求关联号。响应必须回填请求的值；事件填 0 */
    uint32_t payload_len; /* 偏移 24：载荷字节数，<= IV_CHAN_MAX_PAYLOAD */
    uint32_t reserved;    /* 偏移 28：保留，恒置 0 */
} ivs_chan_hdr_t;         /* 尺寸 32，无隐式填充 */

/* 字节序先于一切：本协议按小端定义且不做转换，大端目标必须当场失败。*/
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "iv_chan: protocol is defined little-endian; this target is big-endian"
#endif

_Static_assert(sizeof(ivs_chan_hdr_t) == IV_CHAN_HDR_SIZE,
               "ivs_chan_hdr_t must be exactly IV_CHAN_HDR_SIZE bytes");
_Static_assert(offsetof(ivs_chan_hdr_t, magic) == 0u, "iv_chan: magic offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, version) == 4u, "iv_chan: version offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, type) == 6u, "iv_chan: type offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, flags) == 8u, "iv_chan: flags offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, pad0) == 12u, "iv_chan: pad0 offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, request_id) == 16u, "iv_chan: request_id offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, payload_len) == 24u,
               "iv_chan: payload_len offset drifted");
_Static_assert(offsetof(ivs_chan_hdr_t, reserved) == 28u, "iv_chan: reserved offset drifted");

/* ============================ 对端身份与访问控制 ============================ */

/* 对端身份，来自内核的 SO_PEERCRED（连接建立瞬间记录，对端无法伪造）。
 * pid 仅供日志与诊断，**不要**拿它做鉴权（pid 会复用）。*/
typedef struct {
    pid_t pid;
    uid_t uid;
    gid_t gid;
} iv_chan_peer_t;

/* 访问白名单。三者是"或"的关系：命中任意一条即放行。
 * 为什么还要一道白名单：接听点的 0660 权限只能区分"属主 / 同组 / 其他"三档，
 * 表达不了"允许 A 和 C，但不允许同组的 B"。SO_PEERCRED 补上这一层。
 * 默认（iv_chan_acl_default）＝ 只放行 root 与接听点属主，**不写死任何 uid**，
 * 具体放行谁留给 S10 装配时决定（板端 /etc/passwd 目前还没有 ivsbox 用户）。*/
typedef struct {
    const uid_t *uids;   /* 显式放行的 uid 列表；NULL 表示不用这一条 */
    size_t       count;  /* uids 的元素个数 */
    int          allow_root;  /* 1 = 放行 uid 0 */
    int          allow_owner; /* 1 = 放行与接听点文件属主相同的 uid */
} iv_chan_acl_t;

/* ============================ 消息头工具（纯函数，无需 fd） ============================ */

/* 填一个消息头：校验 type 合法、payload_len 不超上限，其余字段按参数写入，
 * pad0/reserved 恒置 0。
 * 返回 IV_OK；参数非法 IV_EINVAL；payload_len 超限 IV_ERANGE。*/
int iv_chan_hdr_init(ivs_chan_hdr_t *hdr, uint16_t type, uint64_t request_id,
                     uint32_t flags, uint32_t payload_len);

/* 校验一个消息头：monotonic 检查 magic / version / type / payload_len。
 * 返回 IV_OK；magic/version/type 不符 IV_EPROTO；payload_len 超限 IV_ERANGE。
 * 注意本函数**不检查 pad0 与 reserved**：它们保留给未来使用，接收侧必须容忍
 * 非 0 值（"发送方是不认识的新版本"与"收到垃圾"要能区分开）。*/
int iv_chan_hdr_check(const ivs_chan_hdr_t *hdr);

/* 类型名，仅供日志。返回常量 ASCII 字符串，任意线程可调用；
 * 未登记的类型返回 "UNKNOWN"（需要数值请直接按 uint16 打印）。*/
const char *iv_chan_type_name(uint16_t type);

/* ============================ 访问白名单 ============================ */

/* 填默认白名单：allow_root = 1、allow_owner = 1、uids = NULL、count = 0。*/
void iv_chan_acl_default(iv_chan_acl_t *acl);

/* 判断某个 uid 是否放行。owner 传接听点文件的属主 uid；未知时传 (uid_t)-1。
 * 返回 1 = 放行，0 = 拒绝。
 * 纯函数，不碰 fd —— 因此鉴权规则本身可以脱离 socket 单测。*/
int iv_chan_acl_permits(const iv_chan_acl_t *acl, uid_t uid, uid_t owner);

/* ============================ 接听点（服务端） ============================ */

/* 建立接听点并开始监听。
 *   path  : NULL 时用 IV_CHAN_PATH_DEFAULT
 *   mode  : 0 时用 IV_CHAN_MODE_DEFAULT
 *   backlog: <= 0 时用 IV_CHAN_BACKLOG_DEFAULT
 *
 * 返回 >= 0 的 listen fd（已设 CLOEXEC + NONBLOCK），或 < 0 的 IV_* 错误码：
 *   IV_ERANGE  路径为空或长到放不进 sun_path
 *   IV_EEXIST  路径已被**非 socket** 的东西占用（普通文件 / 目录 / 符号链接…）
 *   IV_ENOENT  父目录不存在（**不自动创建**，见 IV_CHAN_PATH_DEFAULT 说明）
 *   IV_EIO     其他系统调用失败（unlink / bind / chmod / listen）；errno 保留原值
 *
 * **errno 契约（S7-03）**：本模块所有失败路径均保证 errno 为原始系统调用的值
 * （失败路径上的 close()/unlink() 等清理调用不会冲掉它），可与 IV_* 返回码
 * 一并用于诊断。
 *
 * 路径已被**上一个 socket 文件**占用时（进程被 kill 没走正常清理）会被 unlink
 * 后重建 —— 这是必要的，否则服务重启永远起不来。但**绝不 unlink 非 socket**：
 * 那等于把"路径写错"变成"删掉别人的文件"。
 *
 * 上面"lstat 判是 socket → unlink → bind"这段**不是原子的**，其间存在一个
 * TOCTOU 窗口。但后果是安全的：窗口里若有人抢先在该路径放了东西，`bind` 会以
 * EADDRINUSE 失败并返回 IV_EIO（失败路径**不再 unlink**），结果是"启动失败"，
 * 而不是"删掉别人的文件"。接听点父目录由 root 预建、威胁模型下能在这个窗口里
 * 动手的只有 root，故不做原子化处理，仅备案。
 *
 * 调用方负责把返回的 fd 注册进自己的事件循环，并在退出时调
 * iv_chan_listen_close() 清理路径。**单实例假设见 iv_chan_listen_close()。*/
int iv_chan_listen(const char *path, unsigned mode, int backlog);

/* 接受一个连接并完成鉴权。
 *   acl  : NULL 时用 iv_chan_acl_default()
 *   peer : 可为 NULL；非 NULL 时填对端身份
 *
 * 返回 >= 0 的已连接 fd（CLOEXEC + NONBLOCK），或 < 0 的 IV_* 错误码：
 *   IV_EAGAIN  当前没有待处理的连接（非阻塞，正常现象）
 *   IV_EAUTH   对端 uid 不在白名单内 —— **连接已被关闭且不发送任何回应**
 *              （不告诉对方"你被拒了"，免得给探测脚本当路标）
 *   IV_ECONN   其他 accept 失败
 *
 * 注意第一道门槛在内核：不属于接听点属主/同组的进程在 connect 阶段就会被
 * 文件权限挡掉（EACCES），根本走不到这里。本函数是第二道。
 *
 * `allow_owner` 规则依赖 `fstat()` 取接听点属主：**取不到时属主按"未知"处理，
 * 该规则静默不生效**（"未知"不算命中）。方向上是 fail-closed，不会因此放行
 * 陌生人；实际不可达（listen_fd 刚被 accept4 用过）。*/
int iv_chan_accept(int listen_fd, const iv_chan_acl_t *acl, iv_chan_peer_t *peer);

/* 关闭接听点：关掉 fd，并且**仅当**路径上的对象仍是本 fd 对应的那个 socket 文件
 * 时才 unlink（避免留下下次启动要清理的残留）。
 * path 为 NULL 时**只关 fd、不 unlink** —— 把"关闭连接"与"清理路径"显式分开，
 * 免得"只想关 fd"的调用顺手删掉别人的路径。返回 IV_OK / IV_* 错误码。
 *
 * **unlink 的身份校验（S7-01）**：iv_chan_listen() 成功时按 listen fd 记录本次
 * bind 在 path 上创建的 socket 文件身份（fd+path+dev+ino，进程内登记表）；本函数
 * 按传入 fd 核销对应登记，仅当 fd/path 都匹配、且路径上
 * （lstat，**不跟随符号链接**）仍是那个文件时才 unlink。**路径对象在服务生存期内
 * 被替换**（例如第二个实例 bind 重建、或被人换了别的文件）时，身份不符 → 跳过
 * unlink、**静默返回**（不报错、不删新对象）。listen_fd < 0、path 为 NULL、或 fd
 * 不是本模块 listen() 创建的（无登记）时同样不删任何东西。
 * 为什么不能在 close 时拿 fd 与路径比对：AF_UNIX 套接字 fd 的 fstat() 返回的是
 * sockfs 伪文件系统 inode，与 bind() 在路径上创建的文件 inode 无关，永不相等，
 * 所以身份必须在 bind 时刻从路径上取。
 *
 * **不是全幂等的，两类情况要分清**：
 *   - unlink 侧幂等：路径本就不存在（ENOENT）视为成功，重复调不报错。
 *   - fd 侧**不**幂等：拿**已关闭的 fd** 再调一次会命中 `close()` 的 EBADF →
 *     返回 IV_EIO。这是刻意的（容忍 EBADF 会把"重复关闭"这种真实 bug 藏起来），
 *     调用方须保证每个 listen fd 只 close 一次。
 *
 * **单实例假设（重要）**：第二个实例 iv_chan_listen() 仍会把第一个实例（仍在
 * 运行）的接听点 unlink 重建 —— listen 侧的这一步不受本函数影响；但第一个实例
 * 退出时，本函数经上面的身份校验发现路径上已是别人的 socket，**不再反向删掉**
 * 第二个实例的接听点。历史上"双活部署下双方互相破坏"的闭环由此断开一半，
 * 剩下的 listen 侧重建风险仍由 S10 的单实例锁（启动时持有）兜底。*/
int iv_chan_listen_close(int listen_fd, const char *path);

/* ============================ 连接端 ============================ */

/* 连接到接听点。返回 >= 0 的已连接 fd（**已设 CLOEXEC + NONBLOCK**），
 * 或 < 0 的 IV_* 错误码：
 *   IV_ENOENT  接听点不存在（服务没起）
 *   IV_EAUTH   文件权限不允许（内核 EACCES/EPERM），即被第一道门槛挡下
 *   IV_ERANGE  路径为空或过长
 *   IV_EBUSY   对端 **backlog 已满**（内核 EAGAIN）—— 服务是活的、只是暂时排不上，
 *              **稍后重试即可**。S10 装配客户端时必须按"可重试"处理，
 *              不要与 IV_ENOENT（服务没起）混为一谈
 *   IV_EFAIL   本机资源不足（`socket()` 失败），**不是对端的问题**
 *   IV_ECONN   其他连接失败（含 ECONNREFUSED / EPROTOTYPE）
 * 返回值是**非阻塞** fd，因此后续 iv_chan_recv() 可能直接返回 IV_EAGAIN。
 * UDS 的 connect 是本地操作、不会阻塞等待网络，故不设超时参数；
 * "请求超时"由 iv_chan_recv_timeout() 覆盖。*/
int iv_chan_connect(const char *path);

/* ============================ 一条连接上的收发 ============================ */

/* 收一条完整消息。
 *   buf / cap : 调用方提供的接收缓冲。容量决定权交给调用方，是为了让本模块
 *               **不做任何隐藏的内存分配**（嵌入式下这一点比省事重要）。
 *               cap 只需 >= IV_CHAN_HDR_SIZE（32）；但**若需接收任意合法消息，
 *               cap 应 >= IV_CHAN_RECV_CAP_MIN** —— 小于该值时合法的大消息会被
 *               整包丢弃并返回 IV_ERANGE。反过来说，"只收小事件消息、刻意用小
 *               缓冲"是**合法用法**，本模块不会因为 cap 小而拒绝调用。
 *   返回值：
 *     IV_OK       成功。*hdr 填好；载荷位于 (uint8_t *)buf + IV_CHAN_HDR_SIZE，
 *                 长度为 *payload_len
 *     IV_ERANGE   整包长度 > cap，**该包已被内核整包丢弃**（SEQPACKET 语义），
 *                 缓冲内容不可用
 *     IV_EPROTO   包长不足一个头，或头校验不过，或**头里声明的 payload_len
 *                 与实际收到的包长不符**（后一条是防"头部撒谎"）
 *     IV_ECONN    对端已关闭（recv 返回 0）或连接出错
 *     IV_EAGAIN   非阻塞 fd 上当前无数据
 *     IV_EINVAL   参数非法
 *
 * **出错返回时 *hdr 与 *payload_len 的内容不可信**，调用方只能走错误分支、
 * 不得复用它们。具体地：IV_EPROTO 路径里 *hdr 已被写入**未经校验的原始头**；
 * IV_ERANGE / IV_EAGAIN / IV_ECONN / IV_EINVAL 路径完全不碰 *hdr；
 * *payload_len 只在 IV_OK 时写入（即成功时它一定是最新的，出错时是旧值或未定义）。
 * 成功时 *hdr 是**已通过 iv_chan_hdr_check() 且与实际包长自洽**的头。
 *
 * 实现说明：用 `recv(..., MSG_TRUNC)` 一次拿到**整包长度**——返回值大于 cap
 * 即说明真实包更大，此时内核已按 SEQPACKET 语义把整包丢弃，正好是我们想要的
 * "拒绝超长"而不是"读半截留下残渣"。因为只调一次 recv，不存在两次系统调用
 * 之间的竞态。*/
int iv_chan_recv(int fd, void *buf, size_t cap, ivs_chan_hdr_t *hdr, size_t *payload_len);

/* 带超时的 iv_chan_recv：先 poll 等可读，超时返回 IV_ETIMEDOUT。
 * timeout_ms < 0 时返回 IV_EINVAL（**不支持"无限等待"** —— 架构 §5.1 明确
 * "请求必须设置超时"，给一个能睡死的接口等于给调用方留陷阱）。
 * 其余返回码与 iv_chan_recv 相同。
 *
 * **超时预算是全程有效的硬上限**：被信号打断（poll 返回 EINTR）时本函数
 * **在同一个调用内**用**剩余**预算继续等，绝不把已经花掉的时间丢掉重来 ——
 * 否则持续的信号流（SIGCHLD 之类）会把实际等待无限拉长，与上面"必须设置超时"
 * 的纪律相悖。因此返回值里**不会**出现"因 EINTR 而提前返回的 IV_EAGAIN"。
 *
 * 注意：poll 报告可读之后 iv_chan_recv 仍可能返回 IV_EAGAIN（极罕见）。
 * 那意味着调用方要**再调一次**本函数，而新的一次会以**全新的 timeout_ms** 重新
 * 计时 —— 本函数管不了跨调用的时间，调用方若在外层循环重试，须自行累计总预算。*/
int iv_chan_recv_timeout(int fd, void *buf, size_t cap, ivs_chan_hdr_t *hdr,
                         size_t *payload_len, int timeout_ms);

/* 发一条完整消息。
 *   hdr         : 头，内部会做一次 iv_chan_hdr_check
 *   payload     : 载荷，payload_len 为 0 时可为 NULL
 *   payload_len : 必须与 hdr->payload_len 一致
 * 返回 IV_OK；IV_EINVAL 参数不一致；IV_ERANGE 超限；IV_EPROTO 头不合法；
 * IV_EAGAIN 非阻塞且发送缓冲满（**该消息一条都没发出去**，可安全重试）；
 * IV_ECONN 对端已关闭或发送出错。
 *
 * 实现用 `sendmsg` 把"头 + 载荷"作为两段 iovec 一次发出，**不拼临时缓冲**。
 *
 * 关于 SIGPIPE：调用带 `MSG_NOSIGNAL`，但**别把它当成本模块抗 SIGPIPE 的依据** ——
 * 2026-09-29 双端实测（VM 内核 6.8.0 / 板端 5.4.61-rt37）：`AF_UNIX`
 * `SOCK_SEQPACKET` 上对端关闭后 `sendmsg` 只返回 `EPIPE`，**内核不送 SIGPIPE**，
 * 带不带该标志结果逐字相同；SIGPIPE 只在 `SOCK_STREAM` 上出现（同探针实测
 * rc=141）。保留该标志是预防性的：万一将来把传输换成 STREAM，本模块不用改就已免疫。
 * 架构 §4.1 要求的"主控屏蔽 SIGPIPE"仍由 S10 装配负责，本模块不代劳。*/
int iv_chan_send(int fd, const ivs_chan_hdr_t *hdr, const void *payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CHAN_H */
