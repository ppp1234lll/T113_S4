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

/* 把版本数字宏展开成字符串字面量（两级宏：先展开参数再字符串化） */
#define IVV_STR_(x) #x
#define IVV_STR(x)  IVV_STR_(x)

/*
 * 两个编译期常量串：版本号与哈希全部在预处理期拼好，运行期零状态、零写入，
 * 天然线程安全（彻底移除旧的静态缓冲 + ready 标志——那个组合在多线程下
 * 既非原子也无可见性保证）。
 */
static const char s_version_git[] = IVV_STR(IVSBOX_VERSION_MAJOR) "."
                                    IVV_STR(IVSBOX_VERSION_MINOR) "."
                                    IVV_STR(IVSBOX_VERSION_PATCH) "+g"
                                    IV_GIT_VER;

static const char s_version_unknown[] = IVV_STR(IVSBOX_VERSION_MAJOR) "."
                                        IVV_STR(IVSBOX_VERSION_MINOR) "."
                                        IVV_STR(IVSBOX_VERSION_PATCH) "+g"
                                        IV_VERSION_UNKNOWN;

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
    return git_ver_usable(IV_GIT_VER) ? s_version_git : s_version_unknown;
}
