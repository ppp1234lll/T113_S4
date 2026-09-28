/*
 * IVSBox 错误码字典实现：码值 → 稳定 ASCII 符号名
 *
 * 码值由 iv_err.h 的组合宏在编译期算出，本文件只做只读查表；
 * 数值正确性由 tests/unit/test_err.c 用字面量逐条钉死（防两侧单方面改动）。
 */
#include "ivsbox/iv_err.h"

#include <stddef.h>

/* 表项顺序按码值升序，便于人工比对；sizeof 求长度，增删码值只改 iv_err.h 与本表 */
static const struct {
    uint32_t code;
    const char *name;
} k_err_names[] = {
    { IV_ERR_OK, "OK" },

    /* 电量类 ELEC */
    { IV_ERR_ELEC_MAIN_AC, "ELEC_MAIN_AC" },
    { IV_ERR_ELEC_ACDC_MODULE, "ELEC_ACDC_MODULE" },
    { IV_ERR_ELEC_AC_OVER_V, "ELEC_AC_OVER_V" },
    { IV_ERR_ELEC_AC_LOW_V, "ELEC_AC_LOW_V" },
    { IV_ERR_ELEC_AC_OVER_C, "ELEC_AC_OVER_C" },
    { IV_ERR_ELEC_AC_LEAKAGE, "ELEC_AC_LEAKAGE" },
    { IV_ERR_ELEC_AC_MCB, "ELEC_AC_MCB" },
    { IV_ERR_ELEC_GROUND_FAULT, "ELEC_GROUND_FAULT" },
    { IV_ERR_ELEC_AC_LN_FAULT, "ELEC_AC_LN_FAULT" },

    /* 网络类 NET */
    { IV_ERR_NET_LAN_PORT, "NET_LAN_PORT" },
    { IV_ERR_NET_MAIN_FAULT, "NET_MAIN_FAULT" },
    { IV_ERR_NET_MAIN2_FAULT, "NET_MAIN2_FAULT" },
    { IV_ERR_NET_CAMERA1_FAULT, "NET_CAMERA1_FAULT" },
    { IV_ERR_NET_CAMERA2_FAULT, "NET_CAMERA2_FAULT" },
    { IV_ERR_NET_CAMERA3_FAULT, "NET_CAMERA3_FAULT" },
    { IV_ERR_NET_CAMERA4_FAULT, "NET_CAMERA4_FAULT" },
    { IV_ERR_NET_CAMERA5_FAULT, "NET_CAMERA5_FAULT" },
    { IV_ERR_NET_CAMERA6_FAULT, "NET_CAMERA6_FAULT" },
    { IV_ERR_NET_MAIN_DELAY, "NET_MAIN_DELAY" },
    { IV_ERR_NET_MAIN2_DELAY, "NET_MAIN2_DELAY" },
    { IV_ERR_NET_CAMERA1_DELAY, "NET_CAMERA1_DELAY" },
    { IV_ERR_NET_CAMERA2_DELAY, "NET_CAMERA2_DELAY" },
    { IV_ERR_NET_CAMERA3_DELAY, "NET_CAMERA3_DELAY" },
    { IV_ERR_NET_CAMERA4_DELAY, "NET_CAMERA4_DELAY" },
    { IV_ERR_NET_CAMERA5_DELAY, "NET_CAMERA5_DELAY" },
    { IV_ERR_NET_CAMERA6_DELAY, "NET_CAMERA6_DELAY" },
    { IV_ERR_NET_MAIN_LOSS, "NET_MAIN_LOSS" },
    { IV_ERR_NET_MAIN2_LOSS, "NET_MAIN2_LOSS" },
    { IV_ERR_NET_CAMERA1_LOSS, "NET_CAMERA1_LOSS" },
    { IV_ERR_NET_CAMERA2_LOSS, "NET_CAMERA2_LOSS" },
    { IV_ERR_NET_CAMERA3_LOSS, "NET_CAMERA3_LOSS" },
    { IV_ERR_NET_CAMERA4_LOSS, "NET_CAMERA4_LOSS" },
    { IV_ERR_NET_CAMERA5_LOSS, "NET_CAMERA5_LOSS" },
    { IV_ERR_NET_CAMERA6_LOSS, "NET_CAMERA6_LOSS" },
    { IV_ERR_NET_MAIN_IP_UNCONFIG, "NET_MAIN_IP_UNCONFIG" },
    { IV_ERR_NET_MAIN2_IP_UNCONFIG, "NET_MAIN2_IP_UNCONFIG" },

    /* 传感器类 SENSOR */
    { IV_ERR_SENSOR_TEMP_HIGH, "SENSOR_TEMP_HIGH" },
    { IV_ERR_SENSOR_TEMP_LOW, "SENSOR_TEMP_LOW" },
    { IV_ERR_SENSOR_HUMI_HIGH, "SENSOR_HUMI_HIGH" },
    { IV_ERR_SENSOR_BOX_TILT, "SENSOR_BOX_TILT" },
    { IV_ERR_SENSOR_DOOR_OPEN, "SENSOR_DOOR_OPEN" },
    { IV_ERR_SENSOR_WATER_LEAK, "SENSOR_WATER_LEAK" },
    { IV_ERR_SENSOR_SPD_FAULT, "SENSOR_SPD_FAULT" },
};

const char *iv_strerror(uint32_t code)
{
    size_t i;

    for (i = 0; i < sizeof(k_err_names) / sizeof(k_err_names[0]); i++) {
        if (k_err_names[i].code == code) {
            return k_err_names[i].name;
        }
    }

    return "UNKNOWN";
}
