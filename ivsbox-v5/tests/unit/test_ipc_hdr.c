/* test_ipc_hdr.c —— IPC 消息头线格式与错误码最小回归
 *
 * 目的：把 §5.1 的消息头布局钉死。线格式一旦发布就不能悄悄变，
 *       任何编译器的对齐差异都应在编译期（_Static_assert）暴露，
 *       运行时行为则由本测试覆盖。
 *
 * 输出一律 ASCII。
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_err.h"
#include "ivsbox/iv_ipc.h"
#include "ivsbox/iv_version.h"

static int g_failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            (void)printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                \
        }                                                               \
    } while (0)

int main(void)
{
    ivs_ipc_hdr_t hdr;

    /* 线格式：32B（含 offset 12 的 4B 对齐空洞与尾部填充） */
    CHECK(sizeof(ivs_ipc_hdr_t) == 32);
    CHECK(offsetof(ivs_ipc_hdr_t, magic) == 0);
    CHECK(offsetof(ivs_ipc_hdr_t, version) == 4);
    CHECK(offsetof(ivs_ipc_hdr_t, type) == 6);
    CHECK(offsetof(ivs_ipc_hdr_t, flags) == 8);
    CHECK(offsetof(ivs_ipc_hdr_t, request_id) == 16);
    CHECK(offsetof(ivs_ipc_hdr_t, payload_len) == 24);
    CHECK(offsetof(ivs_ipc_hdr_t, reserved) == 28);

    /* 常量冻结 */
    CHECK(IVS_IPC_MAGIC == 0x42535649u);
    CHECK(IVS_IPC_PROTO_VERSION == 1u);
    CHECK(IVS_IPC_MAX_PAYLOAD == 65536u);
    CHECK(IVS_IPC_TYPE_REQ == 1u);
    CHECK(IVS_IPC_TYPE_CANCEL == 4u);

    /* 字段填装与读回 */
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = IVS_IPC_MAGIC;
    hdr.version = IVS_IPC_PROTO_VERSION;
    hdr.type = IVS_IPC_TYPE_REQ;
    hdr.flags = IVS_IPC_FLAG_URGENT;
    hdr.request_id = 0x1122334455667788ULL;
    hdr.payload_len = 0u;
    CHECK(hdr.magic == IVS_IPC_MAGIC);
    CHECK(hdr.request_id == 0x1122334455667788ULL);
    CHECK((hdr.flags & IVS_IPC_FLAG_URGENT) != 0u);

    /* 错误码：0 为成功，负值为失败，未知码不得崩溃 */
    CHECK(IVS_OK == 0);
    CHECK(IVS_ERR_BUSY < 0);
    CHECK(IVS_ERR_NOTREADY < 0);
    CHECK(strcmp(ivs_strerror(IVS_OK), "ok") == 0);
    CHECK(strcmp(ivs_strerror(IVS_ERR_TIMEOUT), "timeout") == 0);
    CHECK(strcmp(ivs_strerror(-9999), "unknown error") == 0);

    if (g_failures == 0) {
        (void)printf("test_ipc_hdr: all checks passed\n");
        return 0;
    }

    (void)printf("test_ipc_hdr: %d failure(s)\n", g_failures);
    return 1;
}
