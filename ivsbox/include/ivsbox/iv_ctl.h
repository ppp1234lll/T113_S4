/*
 * IVSBox 本地通道方法路由（libivmodules，功能开发计划 M5-S5.1 / S5.2 服务端）
 *
 * ============================ 它是什么 ============================
 * `iv_chan`（libivcore）只负责"把一条完整消息从一个进程递到另一个进程"，
 * **看不懂载荷**。本模块补上服务端那一半：把 `ivsboxd` 手里的事实（配置、状态
 * 镜像、设备档案、SNMP 结果、系统操作）按**方法名**暴露给同机的其它进程
 * （`ivsbox-web`，将来的 `ivsbox-media`）。
 *
 * 分工上它**不含业务逻辑**：取值一律调既有模块的只读出口，改配置一律调
 * `iv_config` 的写入接口，它只做三件事 —— **解析请求 → 分发 → 拼响应**。
 *
 * ============================ 为什么单独成模块 ============================
 * 与 `iv_agent` 同理：写进 `src/app/main.c` 则 app 层 .c 不进静态库、单测链接
 * 不到符号，只能整机冒烟。落成 modules 层独立模块后可单测、可反向证伪。
 * `main.c` 因此退回"只把 fd 挂进 reactor、收到就交给本模块"。
 *
 * ============================ 载荷协议（本地通道控制协议 v1） ============================
 * 请求载荷二选一（**首字节是 `{` 当 JSON，否则当裸方法名**）：
 *   1) 裸方法名：`system.snapshot` —— **S10 就有的老形态，必须继续支持**
 *      （`tests/integration/test_ctl_snapshot.c` 依赖它，且它是最省事的探活）。
 *   2) JSON：`{"m":"<方法名>","p":{ ... 方法参数 ... }}`，`p` 可省略。
 *
 * 响应载荷：
 *   - `system.snapshot` **保持老格式**：
 *       `{"method":"system.snapshot","version":"<ver>","status":"ok"}`
 *     （既有集成用例逐字断言了 `"method":"system.snapshot"`、`"status":"ok"`、
 *      `"version":"` 三处，改了就是破坏兼容，别动。）
 *   - 其余方法统一：成功 `{"ok":true,"d":<数据>}`；
 *     失败 `{"ok":false,"e":<负的 IV_* 码>,"msg":"<一句话>"}`。
 *
 * **未知方法不产生任何应答**：返回 `IV_ENOTSUP` 且**一个字节都不写 out**，
 * 由调用方（`main.c`）静默丢弃。这条口径来自 S10，且被集成用例钉死
 * （"an unknown method produces no response"）—— 不是随手定的：给探测脚本
 * 回一个"方法不存在"等于告诉对方本机在跑什么。
 *
 * ============================ 方法表 ============================
 *   system.snapshot    —                   进程与版本快照（老格式）
 *   sys.overview       —                   总览页数据（version/uptime/net/cameras/devices/snmp/status/storage；见 iv_ctl.c）
 *   config.get         {"key":"net.wan"}   取一个键的值与来源
 *   config.class       {"cls":"net"}       取一个一级分类下的全部键（配置页）
 *   config.all         —                   取全部键（导出/备份）
 *   config.defaults    —                   内置默认值表（Web 显示"出厂默认"）
 *   config.apply       {"ver":N,"items":{...}}  事务提交（版本不符即拒绝，S5.4）
 *   device.list        —                   设备发现档案（S4.2）
 *   snmp.result        {"which":"cam"|"sw"} SNMP 采集结果（S4.3）
 *   snmp.run           {"which":"cam"|"sw"} 触发一轮采集（S4.3）
 *   camera.search      —                   触发一轮设备发现（S4.2）
 *   system.time        {"ts":<秒>}         设系统墙钟（运维页"同步时间"）
 *   system.reboot      —                   请求重启（运维页"重启"）
 *   snapshot.take      {"path":"..."}      抓拍（M6-S6.2，入队即回；path 省略
 *                                          则按媒体桥接默认目录自动命名）
 *   record.start       {"id":N,"url":"rtsp://..."}  开始录第 N 路（M6-S6.3）
 *   record.stop        {"id":N}            停第 N 路
 *   record.status      {"id":N}            第 N 路在录状态（兼对端保活）
 *   ota.status         —                   升级状态（pending/版本/bootcount/回滚次数；M7-S7.2）
 *   ota.request        {"path":"/...","restart":true}
 *                                          落一个"请装这个包"请求（M7-S7.2）。
 *                                          **不做安装**：包须已落在 `<data_root>/ota/` 下，
 *                                          真正改盘的安装由 monitor.sh 拉起 ivsbox-updater 完成。
 *   tunnel.status      —                   受控远程通道状态（开关/剩余秒/服务集合/能力位/
 *                                          统计/上次会话；**不含钥匙与令牌**；M8-S8.3）
 *   tunnel.enable      {"svcs":["web","camera:0"],"ttl":3600}
 *                                          开通道（服务白名单 + 有效期，ttl 省略取
 *                                          `sys.tunnel.ttl_s`）。**只落配置与标记**，
 *                                          frpc 由 monitor.sh 拉起。
 *   tunnel.disable     —                   关通道（清标记 + 留档）。
 *
 * ============================ 依赖与并发 ============================
 *   - 依赖方向：本模块在 libivmodules，允许依赖 libivcore/libivhal 与外部库；
 *     **只有 core/hal 被禁止引 json-c**（架构 §15.5），模块层用它合规。
 *   - **json-c 只允许出现在模块层的配置与本文件夹**（2026-10-13 M5 修订：原先
 *     "仅 config" 的措辞已随本模块落地更新，见 iv_config.c 文件头）。
 *   - 只用 json-c 0.13 的 API 子集（板端 sysroot 是 0.13.1）：
 *     `json_object_object_add` 在该版本**返回 void**，故不做返回值检查。
 *   - **单线程**：全部接口只在装配层（Reactor 线程）调用，内部无锁。系统操作
 *     钩子（重启/对时）也同步执行，**不得阻塞**（架构 §4.2）。
 */
#ifndef IVSBOX_IV_CTL_H
#define IVSBOX_IV_CTL_H

#include <stddef.h>
#include <stdint.h>

#include "ivsbox/iv_agent.h"
#include "ivsbox/iv_config.h"
#include "ivsbox/iv_db.h"
#include "ivsbox/iv_ret.h"
#include "ivsbox/iv_tunnel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 方法名长度上限（含结尾 NUL）。`system.snapshot` 才 15，留足余量给将来。 */
#define IV_CTL_METHOD_MAX 48u

/* 单条响应载荷上限。按 config.defaults 实测约 12 KiB（119 条默认值 × 每条约
 * 100 字节）估，取 32 KiB 留一倍余量，仍远小于通道的 64 KiB 单包闸门。 */
#define IV_CTL_RSP_MAX (32u * 1024u)

/* 一次 config.apply 最多提交的键数（防止一条请求把 128 项的配置表一次全换）。 */
#define IV_CTL_APPLY_ITEMS_MAX 64

/*
 * 媒体动作表（M6-S6.1）：本模块**不直接依赖** iv_media（便于单测注入假实现），
 * 装配层把绑定 iv_media 的表挂进 ctx。media 为 NULL 时四个媒体方法一律
 * IV_ENOTSUP（"能调但没接"与"不许调"要能区分，同 do_reboot 口径）。
 * 语义与 iv_media 对应函数一致：capture 为**入队即回**（异步抓拍，成败走
 * 统计与日志）；record 三接口同步（毫秒级回环 HTTP）。
 */
typedef struct {
    int (*capture)(const char *path, char *out_path, size_t cap, void *arg);
    int (*record_start)(int id, const char *url, void *arg);
    int (*record_stop)(int id, void *arg);
    int (*record_status)(int id, int *recording, void *arg);
    void *arg;
} iv_ctl_media_ops_t;

/*
 * 上下文：本模块**不持有**这些句柄，只在调用瞬间借用。
 *   cfg   —— 必需，NULL 则除 system.snapshot 外的方法都返回 IV_ESTATE。
 *   agent —— 可空；为空时 sys.overview / device.* / snmp.* / camera.search 降级。
 *   db    —— 可空；审计落盘（`control.db` 表结构属 S9 交付物，当前不建表，
 *            写操作只在这里留一个明确的接缝，不虚构表）。
 *   media —— 可空（M6）；为空时 snapshot.take / record.* 返回 IV_ENOTSUP。
 *   do_reboot / do_settime —— 可空；为空时对应方法返回 IV_ENOTSUP
 *            （**"能调但没接"与"不许调"要能区分**，故不静默假装成功）。
 */
typedef struct {
    iv_config_t *cfg;
    iv_agent_t  *agent;
    iv_db_t     *db;

    /* S9 延伸（2026-10-15）：写操作审计落 control.db 的接缝。可空 ⇒ 只写日志。
     * `db` 字段保留（snapshot 等方法仍用），审计统一走 store（绑定式写入）。 */
    iv_store_t  *store;

    const iv_ctl_media_ops_t *media;
    void                     *media_arg;

    int (*do_reboot)(void *arg);
    void *reboot_arg;

    /* 设系统墙钟；wall_sec 为 Unix 秒。返回 IV_OK 表示已生效。 */
    int (*do_settime)(int64_t wall_sec, void *arg);
    void *settime_arg;

    /* OTA 落点（M7-S7.2）：只给 ota.status / ota.request 用。可空 → iv_upd 的
     * 编译期默认（/opt/ivsbox ＋ /mnt/UDISK/ivsbox，见 iv_upd.h）。
     * **本模块绝不写 pending.json 或 current/previous 链接** —— 改版本的唯一写者
     * 是 ivsbox-updater，这里只读状态、只落一个请求文件（单写者原则，架构 §10.4）。 */
    const char *ota_app_root;
    const char *ota_data_root;

    /* 受控远程通道（M8-S8.3）。可空 ⇒ `tunnel.*` 三个方法返回 IV_ENOTSUP（与 media
     * 同一口径：**"能调但没接"与"不许调"要能分开**）。
     * 本模块**只做三件事**：读状态、调 `iv_tunnel` 的闸门、拼响应。它**不**碰 frpc
     * 进程（归 `scripts/frp.sh` + `monitor.sh`）、**不**读 secretKey（钥匙只落在
     * 0600 的 `frpc.toml` 里）、**不**把令牌回给调用方。 */
    iv_tunnel_t *tunnel;
} iv_ctl_ctx_t;

/*
 * 处理一条请求。`in`/`in_len` 是通道递上来的**原始载荷**（不必 NUL 结尾）。
 * `out`/`out_cap` 是响应缓冲，成功时 `*out_len` 为写入长度（不含 NUL；
 * 本模块保证顺带写一个 '\0' 便于调用方当 C 串用，故需 out_cap >= *out_len + 1）。
 *
 * 返回：
 *   IV_OK        已生成响应，调用方应回 IV_CHAN_TYPE_RSP
 *   IV_ENOTSUP   未知方法 —— **out 未被触碰**，调用方应静默丢弃（见文件头）
 *   IV_EINVAL    in/out/out_len 为 NULL 等调用方错误
 *   IV_ERANGE    响应超过 out_cap（调用方缓冲太小；生产缓冲给 IV_CTL_RSP_MAX）
 *   IV_ESTATE    该方法依赖的句柄不可用（如 cfg 为空）
 * 其余 IV_* 码（IV_EBUSY 版本冲突、IV_EPROTO 载荷坏）会**转成失败响应**
 * （`{"ok":false,...}`）返回 IV_OK，因为这些是"业务失败"，必须让 Web 看见。
 */
int iv_ctl_handle(const iv_ctl_ctx_t *ctx, const char *in, size_t in_len,
                  char *out, size_t out_cap, size_t *out_len);

/* ---------------------------------------------------------------------------
 * 网络页"默认网关"的候选与选择（M5-S5.5，2026-10-19 修正）
 *
 * ============================ 修的是什么 ============================
 * 断链之后内核**不删**默认路由，只是打一个 `RTNH_F_LINKDOWN`（`ip route` 里显示
 * 成 `default via 10.158.0.1 dev usb0 linkdown`）。原实现在 `/proc/net/route`
 * 里见到 `Destination==0 && Flags&RTF_UP` 就采信，于是网络页把一个已经断掉
 * 的链路报成"默认网关"，与 4G 页"SIM 卡未插入"自相矛盾。
 *
 * ============================ 为什么不能查 Flags ============================
 * `/proc/net/route` 的 `Flags` 列**不含 linkdown 位**：它是内核用
 * `fib_flag_trans()` 现算的，值域只有 `RTF_UP|RTF_GATEWAY|RTF_HOST|RTF_REJECT`
 * （见 linux-5.4 `net/ipv4/fib_trie.c`）。"读 Flags 过滤 linkdown"这条路不存在，
 * 不要在这上面浪费时间。
 *
 * ============================ 本模块的判据 ============================
 * **问路由的出口网卡自己有没有载波**。内核那个 linkdown 位本来就是按出口网卡的
 * carrier 算出来的，追问网卡与追问内核同源；而网卡状态经 `getifaddrs()` 一定
 * 拿得到（`ifa_flags` 是 `unsigned int`，装得下内核私有位 `IFF_LOWER_UP`）。
 * 判据与 `iv_netlink.h` 文件头写的事件路径一致：**`IFF_UP && IFF_LOWER_UP`**。
 * ------------------------------------------------------------------------- */
#define IV_CTL_ROUTE_MAX  4
/* 出口网卡名缓冲。取 32 而不是系统 `IFNAMSIZ`(16)：解析 `/proc/net/route` 时用
 * `%31s` 读入，两处宽度一致才不会在 `-Wformat-truncation` 下被判为可能截断。 */
#define IV_CTL_IFNAME_MAX 32

typedef struct {
    char ifname[IV_CTL_IFNAME_MAX]; /* 出口网卡名 */
    char gw[16];                    /* 点分十进制；空串＝网关是 0.0.0.0 */
    int  iface_ok;                  /* 出口就绪：存在 && IFF_UP && 有载波 */
} iv_ctl_route_t;

/*
 * 挑出第一个"出口就绪"的候选，返回其下标；全不可用返回 -1。
 *
 * 抽成纯函数只为能单测：这条判定错了就是"断掉的链路被当成默认网关"，
 * 而这在真机上极难稳定复现（要正好在掉线窗口里刷新页面）。
 */
int iv_ctl_route_pick(const iv_ctl_route_t *r, int n);

/* 已知方法名（按登记顺序），`i` 越界返回 NULL。给单测与 `/api/methods` 用。 */
const char *iv_ctl_method_name(int i);

/* 已知方法条数。 */
int iv_ctl_method_count(void);

#ifdef __cplusplus
}
#endif

#endif /* IVSBOX_IV_CTL_H */
