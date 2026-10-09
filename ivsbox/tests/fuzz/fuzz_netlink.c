/*
 * iv_netlink 模糊测试（开发计划 M3-S3.1：随机 rtnetlink 字节流灌机无 crash / 越界）
 *
 * 用法：fuzz_netlink [秒数]（默认 10；0 表示只跑一轮自检）
 *
 * 策略同 fuzz_frame：xorshift64* 伪随机 + 按概率混入"合法骨架但字段乱来"的
 * 报文（纯均匀随机打不进 RTA 属性遍历）。乱来的维度：消息类型、协议头字段、
 * 属性类型/长度/内容、nlmsg_len 谎报。回调里访问 evt 全部字段（asan/ubsan
 * 下越界立即暴露）。随机间隔 reset() 覆盖去重表回收路径。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "ivsbox/iv_netlink.h"
#include "ivsbox/iv_ret.h"

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000u /* 与 iv_netlink.c 的 IVNL_KF_LOWER_UP 同源 */
#endif

static uint64_t g_rs;

static uint64_t rs_next(void)
{
    g_rs ^= g_rs >> 12;
    g_rs ^= g_rs << 25;
    g_rs ^= g_rs >> 27;
    return g_rs * 2685821657736338717ULL;
}

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static volatile uint32_t g_sink;

static void on_evt(const iv_netlink_evt_t *e, void *arg)
{
    (void)arg;
    /* 全字段触一遍：asan 下越界在此暴露 */
    g_sink ^= (uint32_t)e->kind;
    g_sink ^= (uint32_t)e->ifindex;
    g_sink ^= (uint32_t)e->ifname[0];
    g_sink ^= (uint32_t)e->ifname[IV_NETLINK_IFNAME_LEN - 1];
    g_sink ^= e->link_flags;
    g_sink ^= e->prefixlen;
    g_sink ^= ((uint32_t)e->addr[0] << 24) | e->addr[3];
    g_sink ^= e->metric;
    g_sink ^= e->table;
}

int main(int argc, char **argv)
{
    double budget = argc > 1 ? atof(argv[1]) : 10.0;
    double t0 = now_sec();
    static iv_netlink_t nl;
    static uint8_t chunk[2048];
    uint64_t fed = 0, msgs = 0;

    g_rs = (uint64_t)time(NULL) * 2654435761ULL + 0x9E3779B97F4A7C15ULL;
    if (g_rs == 0)
        g_rs = 1;

    if (iv_netlink_init(&nl, on_evt, NULL) != IV_OK)
        return 1;

    while (now_sec() - t0 < budget) {
        size_t n = 0;
        unsigned loop;

        /* 每块拼 1~4 条报文：2/3 概率合法骨架，字段全随机 */
        for (loop = 0; loop < 1 + (unsigned)(rs_next() % 4u); loop++) {
            uint16_t type = (uint16_t)(rs_next() % 32u);
            struct nlmsghdr *nh;
            size_t flen = rs_next() % 24u; /* 三个协议头 8/12/16B，偶尔对偶尔截 */

            if (n + 128 > sizeof(chunk))
                break;
            if ((rs_next() & 2u) != 0) { /* 2/3 概率：真 rtnetlink 类型 */
                static const uint16_t want[] = {
                    RTM_NEWLINK, RTM_DELLINK, RTM_NEWADDR, RTM_DELADDR,
                    RTM_NEWROUTE, RTM_DELROUTE, NLMSG_ERROR, NLMSG_DONE,
                };
                type = want[rs_next() % (sizeof(want) / sizeof(want[0]))];
            }
            if (n + NLMSG_HDRLEN + 24 > sizeof(chunk))
                break;

            nh = (struct nlmsghdr *)(chunk + n);
            memset(chunk + n, 0, NLMSG_HDRLEN + 24);
            nh->nlmsg_len   = (uint32_t)(NLMSG_HDRLEN + flen);
            nh->nlmsg_type  = type;
            nh->nlmsg_flags = (uint16_t)rs_next();
            if (flen != 0) {
                unsigned k;

                for (k = 0; k < flen; k++)
                    chunk[n + NLMSG_HDRLEN + k] = (uint8_t)rs_next();
            }
            n += NLMSG_ALIGN(NLMSG_HDRLEN + flen);
            msgs++;

            /* 1/2 概率再挂一个随机属性（类型/长度全随机，含谎报长长度的） */
            if ((rs_next() & 1u) != 0 && n + 64 <= sizeof(chunk)) {
                struct rtattr *rta = (struct rtattr *)(chunk + n);
                size_t rlen = rs_next() % 40u;

                rta->rta_type = (uint16_t)(rs_next() % 40u);
                rta->rta_len  = (uint16_t)(sizeof(struct rtattr) + rlen);
                {
                    unsigned k;

                    for (k = 0; k < rlen; k++)
                        chunk[n + sizeof(struct rtattr) + k] = (uint8_t)rs_next();
                }
                /* nlmsg_len 回写：骨架对时合法，谎报时喂给解析器当垃圾 */
                nh->nlmsg_len += (uint32_t)RTA_ALIGN(sizeof(struct rtattr) + rlen);
                n += RTA_ALIGN(sizeof(struct rtattr) + rlen);
            }
        }
        while (n < 64)
            chunk[n++] = (uint8_t)rs_next();

        (void)iv_netlink_feed(&nl, chunk, n);
        if ((rs_next() & 63u) == 0)
            iv_netlink_reset(&nl);
        fed += n;
    }

    printf("fuzz_netlink done: %.1fs, %llu bytes, %llu msgs\n",
           now_sec() - t0, (unsigned long long)fed, (unsigned long long)msgs);
    return 0;
}
