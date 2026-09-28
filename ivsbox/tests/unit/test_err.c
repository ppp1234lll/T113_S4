/*
 * iv_err 单元测试
 *
 * 重点：码值与单片机工程（General_Version/main，APP/User/inc/error.h）**共用**，
 * 下表用字面量逐条钉死；任何一侧单方面改动都会在这里失败。
 */
#include "ivsbox/iv_err.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct {
    uint32_t code;    /* iv_err.h 中的宏（组合宏算出） */
    uint32_t literal; /* 单片机工程 error.h 中的实际数值 */
    const char *name; /* iv_strerror() 必须返回的稳定符号名 */
} k_golden[] = {
    { IV_ERR_OK, 0x00000000u, "OK" },

    /* 电量类 */
    { IV_ERR_ELEC_MAIN_AC, 0x10100000u, "ELEC_MAIN_AC" },
    { IV_ERR_ELEC_ACDC_MODULE, 0x10200000u, "ELEC_ACDC_MODULE" },
    { IV_ERR_ELEC_AC_OVER_V, 0x10300000u, "ELEC_AC_OVER_V" },
    { IV_ERR_ELEC_AC_LOW_V, 0x10400000u, "ELEC_AC_LOW_V" },
    { IV_ERR_ELEC_AC_OVER_C, 0x10500000u, "ELEC_AC_OVER_C" },
    { IV_ERR_ELEC_AC_LEAKAGE, 0x10600000u, "ELEC_AC_LEAKAGE" },
    { IV_ERR_ELEC_AC_MCB, 0x10700000u, "ELEC_AC_MCB" },
    { IV_ERR_ELEC_GROUND_FAULT, 0x10800000u, "ELEC_GROUND_FAULT" },
    { IV_ERR_ELEC_AC_LN_FAULT, 0x10900000u, "ELEC_AC_LN_FAULT" },

    /* 网络类 */
    { IV_ERR_NET_LAN_PORT, 0x20100000u, "NET_LAN_PORT" },
    { IV_ERR_NET_MAIN_FAULT, 0x20200000u, "NET_MAIN_FAULT" },
    { IV_ERR_NET_MAIN2_FAULT, 0x20300000u, "NET_MAIN2_FAULT" },
    { IV_ERR_NET_CAMERA1_FAULT, 0x20410000u, "NET_CAMERA1_FAULT" },
    { IV_ERR_NET_CAMERA2_FAULT, 0x20420000u, "NET_CAMERA2_FAULT" },
    { IV_ERR_NET_CAMERA3_FAULT, 0x20430000u, "NET_CAMERA3_FAULT" },
    { IV_ERR_NET_CAMERA4_FAULT, 0x20440000u, "NET_CAMERA4_FAULT" },
    { IV_ERR_NET_CAMERA5_FAULT, 0x20450000u, "NET_CAMERA5_FAULT" },
    { IV_ERR_NET_CAMERA6_FAULT, 0x20460000u, "NET_CAMERA6_FAULT" },
    { IV_ERR_NET_MAIN_DELAY, 0x20500000u, "NET_MAIN_DELAY" },
    { IV_ERR_NET_MAIN2_DELAY, 0x20600000u, "NET_MAIN2_DELAY" },
    { IV_ERR_NET_CAMERA1_DELAY, 0x20710000u, "NET_CAMERA1_DELAY" },
    { IV_ERR_NET_CAMERA2_DELAY, 0x20720000u, "NET_CAMERA2_DELAY" },
    { IV_ERR_NET_CAMERA3_DELAY, 0x20730000u, "NET_CAMERA3_DELAY" },
    { IV_ERR_NET_CAMERA4_DELAY, 0x20740000u, "NET_CAMERA4_DELAY" },
    { IV_ERR_NET_CAMERA5_DELAY, 0x20750000u, "NET_CAMERA5_DELAY" },
    { IV_ERR_NET_CAMERA6_DELAY, 0x20760000u, "NET_CAMERA6_DELAY" },
    { IV_ERR_NET_MAIN_LOSS, 0x20800000u, "NET_MAIN_LOSS" },
    { IV_ERR_NET_MAIN2_LOSS, 0x20900000u, "NET_MAIN2_LOSS" },
    { IV_ERR_NET_CAMERA1_LOSS, 0x20A10000u, "NET_CAMERA1_LOSS" },
    { IV_ERR_NET_CAMERA2_LOSS, 0x20A20000u, "NET_CAMERA2_LOSS" },
    { IV_ERR_NET_CAMERA3_LOSS, 0x20A30000u, "NET_CAMERA3_LOSS" },
    { IV_ERR_NET_CAMERA4_LOSS, 0x20A40000u, "NET_CAMERA4_LOSS" },
    { IV_ERR_NET_CAMERA5_LOSS, 0x20A50000u, "NET_CAMERA5_LOSS" },
    { IV_ERR_NET_CAMERA6_LOSS, 0x20A60000u, "NET_CAMERA6_LOSS" },
    { IV_ERR_NET_MAIN_IP_UNCONFIG, 0x20B00000u, "NET_MAIN_IP_UNCONFIG" },
    { IV_ERR_NET_MAIN2_IP_UNCONFIG, 0x20C00000u, "NET_MAIN2_IP_UNCONFIG" },

    /* 传感器类 */
    { IV_ERR_SENSOR_TEMP_HIGH, 0x30100000u, "SENSOR_TEMP_HIGH" },
    { IV_ERR_SENSOR_TEMP_LOW, 0x30200000u, "SENSOR_TEMP_LOW" },
    { IV_ERR_SENSOR_HUMI_HIGH, 0x30300000u, "SENSOR_HUMI_HIGH" },
    { IV_ERR_SENSOR_BOX_TILT, 0x30400000u, "SENSOR_BOX_TILT" },
    { IV_ERR_SENSOR_DOOR_OPEN, 0x30500000u, "SENSOR_DOOR_OPEN" },
    { IV_ERR_SENSOR_WATER_LEAK, 0x30600000u, "SENSOR_WATER_LEAK" },
    { IV_ERR_SENSOR_SPD_FAULT, 0x30700000u, "SENSOR_SPD_FAULT" },
};

#define GOLDEN_COUNT (sizeof(k_golden) / sizeof(k_golden[0]))

/* 码值不得漂移（与单片机工程逐条比对） */
static int check_values(void)
{
    size_t i;

    for (i = 0; i < GOLDEN_COUNT; i++) {
        if (k_golden[i].code != k_golden[i].literal) {
            fprintf(stderr, "test_err: value drift on %s: 0x%08X != expected 0x%08X\n",
                    k_golden[i].name, (unsigned)k_golden[i].code, (unsigned)k_golden[i].literal);
            return 1;
        }
    }

    return 0;
}

/* 名字映射正确、名字唯一、且全为可打印 ASCII（板端控制台是 GBK） */
static int check_names(void)
{
    size_t i;
    size_t j;

    for (i = 0; i < GOLDEN_COUNT; i++) {
        const char *got = iv_strerror(k_golden[i].code);
        const char *p;

        if (strcmp(got, k_golden[i].name) != 0) {
            fprintf(stderr, "test_err: iv_strerror(0x%08X) = %s, expected %s\n",
                    (unsigned)k_golden[i].code, got, k_golden[i].name);
            return 1;
        }

        for (p = got; *p != '\0'; p++) {
            if ((unsigned char)*p < 0x21u || (unsigned char)*p > 0x7Eu) {
                fprintf(stderr, "test_err: non-ASCII name %s\n", got);
                return 1;
            }
        }

        for (j = i + 1; j < GOLDEN_COUNT; j++) {
            if (strcmp(k_golden[i].name, k_golden[j].name) == 0) {
                fprintf(stderr, "test_err: duplicated name %s\n", k_golden[i].name);
                return 1;
            }
        }
    }

    return 0;
}

/* 位段自洽：保留位为 0、拆解后能原样重组成、类型与符号名前缀一致 */
static int check_layout(void)
{
    size_t i;

    for (i = 0; i < GOLDEN_COUNT; i++) {
        uint32_t code = k_golden[i].code;
        uint32_t type = IV_ERR_TYPE_OF(code);
        uint32_t cat = IV_ERR_CAT_OF(code);
        uint32_t idx = IV_ERR_IDX_OF(code);
        const char *prefix;

        if ((code & 0x0000FFFFu) != 0u) {
            fprintf(stderr, "test_err: reserved low bits set in 0x%08X\n", (unsigned)code);
            return 1;
        }

        if (IV_ERR_MAKE(type, cat, idx) != code) {
            fprintf(stderr, "test_err: bit-field round-trip failed for 0x%08X\n", (unsigned)code);
            return 1;
        }

        if (code == IV_ERR_OK) {
            continue; /* 0 无类型前缀 */
        }

        prefix = (type == IV_ERR_TYPE_ELEC)     ? "ELEC_"
                 : (type == IV_ERR_TYPE_NET)    ? "NET_"
                 : (type == IV_ERR_TYPE_SENSOR) ? "SENSOR_"
                                                : NULL;
        if (prefix == NULL || strncmp(k_golden[i].name, prefix, strlen(prefix)) != 0) {
            fprintf(stderr, "test_err: type %u mismatches name %s\n", (unsigned)type,
                    k_golden[i].name);
            return 1;
        }
    }

    return 0;
}

/* 边界：0 是"OK"、未登记码是"UNKNOWN"、序号/类别拆分正确 */
static int check_edges(void)
{
    if (IV_ERR_OK != 0u) {
        fprintf(stderr, "test_err: IV_ERR_OK must be 0\n");
        return 1;
    }
    if (strcmp(iv_strerror(IV_ERR_OK), "OK") != 0) {
        fprintf(stderr, "test_err: iv_strerror(0) must be OK\n");
        return 1;
    }
    if (strcmp(iv_strerror(0xDEADBEEFu), "UNKNOWN") != 0) {
        fprintf(stderr, "test_err: unmapped code must be UNKNOWN\n");
        return 1;
    }
    if (IV_ERR_TYPE_OF(IV_ERR_NET_CAMERA1_FAULT) != IV_ERR_TYPE_NET ||
        IV_ERR_CAT_OF(IV_ERR_NET_CAMERA1_FAULT) != 4u ||
        IV_ERR_IDX_OF(IV_ERR_NET_CAMERA1_FAULT) != 1u) {
        fprintf(stderr, "test_err: NET_CAMERA1_FAULT must split into type 2 / cat 4 / idx 1\n");
        return 1;
    }
    if (IV_ERR_IDX_OF(IV_ERR_NET_CAMERA6_LOSS) != 6u ||
        IV_ERR_CAT_OF(IV_ERR_NET_MAIN2_IP_UNCONFIG) != 12u ||
        IV_ERR_IDX_OF(IV_ERR_NET_MAIN_FAULT) != 0u) {
        fprintf(stderr, "test_err: camera/loss/ip-unconfig split check failed\n");
        return 1;
    }

    return 0;
}

/* 编译期示例：常量字段越界会直接编不过（此处只放合法值） */
IV_ERR_ASSERT_FIELDS(IV_ERR_TYPE_NET, 12u, 6u);

/* 字段宽度自检：类别/序号/类型必须装得进各自位段，越界说明码值已进位到相邻字段 */
static int check_field_range(void)
{
    size_t i;

    IV_ERR_ASSERT_FIELDS(IV_ERR_TYPE_SENSOR, 7u, 0u); /* 块作用域同样可用 */

    for (i = 0; i < GOLDEN_COUNT; i++) {
        uint32_t code = k_golden[i].code;
        uint32_t type = IV_ERR_TYPE_OF(code);
        uint32_t cat = IV_ERR_CAT_OF(code);
        uint32_t idx = IV_ERR_IDX_OF(code);

        if (type > IV_ERR_TYPE_MAX || cat > IV_ERR_CAT_MAX || idx > IV_ERR_IDX_MAX) {
            fprintf(stderr, "test_err: field overflow in %s (type %u / cat %u / idx %u)\n",
                    k_golden[i].name, (unsigned)type, (unsigned)cat, (unsigned)idx);
            return 1;
        }
    }

    return 0;
}

int main(void)
{
    if (check_values() != 0 || check_names() != 0 || check_layout() != 0 || check_edges() != 0 ||
        check_field_range() != 0) {
        return 1;
    }

    printf("test_err passed (%u codes)\n", (unsigned)GOLDEN_COUNT);
    return 0;
}
