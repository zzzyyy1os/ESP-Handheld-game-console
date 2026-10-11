/**
 * @file rocker.h
 * @brief 4通道ADC摇杆驱动 (可移植模块)
 *
 * 使用方法：
 *   1. 修改下方"硬件配置"区的宏定义
 *   2. rocker_init();
 *   3. rocker_read(&val) — 采样+滤波+映射，或 rocker_get(&val) — 仅读缓存
 *
 * 需要 ESP-IDF 组件: esp_adc
 */
#ifndef __ROCKER_H__
#define __ROCKER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_adc/adc_oneshot.h"

/* ===== 硬件配置（移植时修改此处） ===== */

/* ADC 通道映射 (ESP32-S3 ADC1) */
#define ROCKER_ADC_UNIT       ADC_UNIT_1
#define ROCKER_CH_LX          ADC_CHANNEL_6   /* GPIO7  */
#define ROCKER_CH_LY          ADC_CHANNEL_7   /* GPIO8  */
#define ROCKER_CH_RY          ADC_CHANNEL_4   /* GPIO5  */
#define ROCKER_CH_RX          ADC_CHANNEL_5   /* GPIO6  */

/* 各通道静止时的中心值 (根据实际测量校准) */
#define ROCKER_CENTER_LX      1942
#define ROCKER_CENTER_LY      1997
#define ROCKER_CENTER_RY      1910
#define ROCKER_CENTER_RX      1968

/* 参数配置 */
#define ROCKER_DEADZONE       120   /* 死区范围，中心附近视为0 */
#define ROCKER_SAMPLES        32    /* 每通道采样次数 */
#define ROCKER_OUTPUT_MAX     100   /* 输出最大值 (-100 ~ +100) */
#define ROCKER_INVERT_RY      1     /* 右摇杆Y轴反转 */
#define ROCKER_INVERT_RX      1     /* 右摇杆X轴反转 */
#define ROCKER_DEBUG_LOG      0     /* 调试打印开关 (1=开, 0=关) */

/* ===== API ===== */

/* 摇杆数值结构 (-100 ~ +100) */
typedef struct {
    int lx;
    int ly;
    int rx;
    int ry;
} rocker_value_t;

void rocker_init(void);
void rocker_read(rocker_value_t *val);   /* 采样+滤波+映射，并更新缓存 */
void rocker_get(rocker_value_t *val);    /* 仅读缓存 */

#ifdef __cplusplus
}
#endif

#endif /* __ROCKER_H__ */
