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

/* Makefile 注入；缺失时退回占位值，保证任何编译方式都能过。
 *
 * ============================ D-11：为什么占位值叫 nogit 而不是 unknown ============================
 * 原来的降级串是 `unknown`，`iv_version_string()` 又把它拼成 `1.0.0+gunknown`
 * —— 读起来像"哈希值恰好是 unknown"，而不像"这次构建取不到哈希"。现场与平台
 * 门户都按"版本串与实际一致"判（计划 M-01），看到 `gunknown` 只能判失败，却
 * 看不出该怎么修。
 *
 * 现在：**有哈希时 `1.0.0+g<sha>`，没有时 `1.0.0+nogit`**。后缀形态保留 `+`，
 * 让"版本 + 构建标识"这两段结构不变；标识本身换成一个自解释的词。同时
 * `iv_version_git()` 返回 `"nogit"`、`iv_version_git_valid()` 返回 0，两者与
 * 串尾严格一一对应（`git_ver_usable()` 里 `nogit` 含非十六进制字符，天然被拒，
 * 但仍然显式比一次——将来若有人把占位改成 `deadbeef` 这种全 hex 值，
 * 只靠字符集判断就会误判成合法哈希）。
 *
 * **这不是"把问题藏起来"**：真正的修法是构建时带上哈希 ——
 * `make arm GIT_VER=$(git rev-parse --short HEAD)`（Makefile 的普通赋值，命令行
 * 可覆盖）。VM 上的构建树不是 git 仓，所以那条注入是**必要**的，已写进
 * `docs/代码说明.adoc`。 */
#define IV_VERSION_NOGIT "nogit"
#ifndef IV_GIT_VER
#define IV_GIT_VER IV_VERSION_NOGIT
#endif
#ifndef IV_BUILD_DATE
#define IV_BUILD_DATE "00000000"
#endif


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

static const char s_version_nogit[] = IVV_STR(IVSBOX_VERSION_MAJOR) "."
                                      IVV_STR(IVSBOX_VERSION_MINOR) "."
                                      IVV_STR(IVSBOX_VERSION_PATCH) "+"
                                      IV_VERSION_NOGIT;

/* 平台视图的版本串：三段版本 + 构建日期（同属编译期常量，理由见头文件）。 */
static const char s_sysver[] = IVV_STR(IVSBOX_VERSION_MAJOR) "."
                               IVV_STR(IVSBOX_VERSION_MINOR) "."
                               IVV_STR(IVSBOX_VERSION_PATCH) "."
                               IV_BUILD_DATE;

/*
 * 短哈希合法性：非空、非占位值、且全部为十六进制字符。
 * 不用固定长度判断——git rev-parse --short 在对象库很大时会自动加长。
 *
 * 显式比一次 `IV_VERSION_NOGIT`，不只靠"字符集不合法"来拒：占位值将来若被改成
 * `deadbeef` 这种**全 hex** 的记号，单纯比字符集就会把它当成合法哈希。
 */
static int git_ver_usable(const char *g)
{
    if (g == NULL || *g == '\0')
        return 0;
    if (strcmp(g, IV_VERSION_NOGIT) == 0)
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
    return git_ver_usable(IV_GIT_VER) ? s_version_git : s_version_nogit;
}

const char *iv_version_build_date(void)
{
    return IV_BUILD_DATE;
}

const char *iv_version_sysver(void)
{
    return s_sysver;
}
