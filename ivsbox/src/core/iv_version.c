/*
 * 版本信息实现（计划 M1-S2 第 4 件）
 *
 * 只依赖 libc，无 HAL 依赖，可进 libivcore 并在 Host 上跑单测。
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_version.h"
#include "ivsbox/ivsbox.h"

/* Makefile 注入；缺失时退回占位值，保证任何编译方式都能过 */
#ifndef IV_GIT_VER
#define IV_GIT_VER "unknown"
#endif

#define IV_VERSION_UNKNOWN  "unknown"
#define IV_VERSION_BUF_MAX  64u

static char s_version[IV_VERSION_BUF_MAX];
static int  s_version_ready;

/*
 * 短哈希合法性：非空、非占位值、且全部为十六进制字符。
 * 不用固定长度判断——git rev-parse --short 在对象库很大时会自动加长。
 */
static int git_ver_usable(const char *g)
{
    if (g == NULL || *g == '\0')
        return 0;
    if (strcmp(g, IV_VERSION_UNKNOWN) == 0)
        return 0;
    for (; *g != '\0'; g++) {
        if (isxdigit((unsigned char)*g) == 0)
            return 0;
    }
    return 1;
}

const char *iv_version_git(void)
{
    return IV_GIT_VER;
}

int iv_version_git_valid(void)
{
    return git_ver_usable(IV_GIT_VER);
}

const char *iv_version_string(void)
{
    if (s_version_ready == 0) {
        /*
         * 三个版本宏与 IV_GIT_VER 都是编译期字符串常量，最长输出远小于缓冲，
         * 不会触发 -Wformat-truncation（-Werror 下会直接断编译）。
         */
        (void)snprintf(s_version, sizeof(s_version), "%d.%d.%d+g%s",
                       IVSBOX_VERSION_MAJOR, IVSBOX_VERSION_MINOR,
                       IVSBOX_VERSION_PATCH,
                       (iv_version_git_valid() != 0) ? IV_GIT_VER : IV_VERSION_UNKNOWN);
        s_version_ready = 1;
    }
    return s_version;
}
