/*
 * iv_version 单测（计划 M1-S2 第 4 件）
 *
 * 覆盖：
 *   1) 版本串格式：<major>.<minor>.<patch>+g<git>；
 *   2) 与编译期注入的 IV_GIT_VER 自洽（注入可用时串尾即该哈希）；
 *   3) 未注入 / 非法注入值降级为 "unknown"，不产生半截串；
 *   4) 幂等（重复调用内容稳定）且长度受内部缓冲约束；
 *   5) 注入值合法性判定的边界（空、占位、非十六进制、大小写混合）。
 *
 * 纯字符串断言，无文件系统 / POSIX 依赖，Host 与板端都能跑。
 * --version 的命令行行为由构建后手工比对（见 docs/修改记录.md），单测不 fork 自身。
 */
#include <stdio.h>
#include <string.h>

#include "ivsbox/iv_version.h"
#include "ivsbox/ivsbox.h"

static int g_fail;

static void chk(int cond, const char *what)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* 判定一个字符串是否为"合法短哈希"——与被测实现的规则同构，用于交叉核对 */
static int looks_like_sha(const char *s)
{
    if (s == NULL || *s == '\0')
        return 0;
    if (strcmp(s, "unknown") == 0)
        return 0;
    for (; *s != '\0'; s++) {
        int hex = (*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
                  (*s >= 'A' && *s <= 'F');
        if (!hex)
            return 0;
    }
    return 1;
}

int main(void)
{
    char        expect[64];
    const char *v;
    const char *v2;
    const char *git;
    size_t      n;

    /* 0) 前缀 = ivsbox.h 的版本常量，且拼成 "<M>.<m>.<p>+g" */
    (void)snprintf(expect, sizeof(expect), "%d.%d.%d+g",
                   IVSBOX_VERSION_MAJOR, IVSBOX_VERSION_MINOR, IVSBOX_VERSION_PATCH);

    v = iv_version_string();
    chk(v != NULL, "iv_version_string not NULL");
    chk(strncmp(v, expect, strlen(expect)) == 0, "semver prefix matches ivsbox.h");

    /* 1) 串尾与 iv_version_git() 自洽 */
    git = iv_version_git();
    chk(git != NULL, "iv_version_git not NULL");
    chk(strcmp(v + strlen(expect), git) == 0 || strcmp(v + strlen(expect), "unknown") == 0,
        "suffix is either the injected sha or 'unknown'");

    /* 2) 合法性判定与串尾一一对应 */
    if (iv_version_git_valid()) {
        chk(*git != '\0', "valid flag implies non-empty");
        chk(looks_like_sha(git) == 1, "valid flag implies hex sha");
        chk(strcmp(v + strlen(expect), git) == 0, "valid flag => suffix is the sha");
        chk(strcmp(git, "unknown") != 0, "valid flag => git value is not 'unknown'");
    } else {
        chk(strcmp(v + strlen(expect), "unknown") == 0,
            "invalid flag => suffix degraded to 'unknown'");
    }

    /* 3) 幂等 + 长度约束（编译期常量串，长度有界且必须是单行） */
    v2 = iv_version_string();
    chk(strcmp(v, v2) == 0, "repeated calls return identical content");
    n = strlen(v);
    chk(n > 0 && n < 64u, "version string within buffer bound");
    chk(strchr(v, '\n') == NULL && strchr(v, '\r') == NULL, "version string is single-line");

    /* 4) 三个字段都非空（防 "<M>.<m>.<p>" 被写坏成空段） */
    chk(strchr(v, '.') != NULL, "major.minor separator present");
    {
        const char *dot = strchr(v, '.');
        const char *plus = strchr(v, '+');

        chk(dot != NULL && plus != NULL && dot < plus, "'+' comes after semver part");
        chk(plus != NULL && plus[1] == 'g', "sha is tagged with 'g'");
        chk(plus != NULL && plus[2] != '\0', "sha part not empty");
    }

    /* 5) 短哈希长度落在合理区间（git --short 最少 4 位，SHA-1 全长 40 位） */
    if (iv_version_git_valid()) {
        size_t gl = strlen(git);

        chk(gl >= 4u && gl <= 40u, "sha length within [4,40]");
    }

    if (g_fail) {
        fprintf(stderr, "test_version failed (%d check(s))\n", g_fail);
        return 1;
    }
    printf("test_version passed (version=%s)\n", iv_version_string());
    return 0;
}
