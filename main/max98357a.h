/**
 * @file max98357a.h
 * @brief MAX98357A I2S 数字功放驱动 (可移植模块)
 *
 * 使用方法：
 *   1. 修改下方"硬件配置"区的引脚宏 (BCLK/WS/DOUT)
 *   2. MAX98357A_Init(32000);              // 采样率按应用传入 (22050/32000)
 *   3. MAX98357A_SetVolume(50);            // 0-100
 *   4. MAX98357A_Write(samples, frames);   // 16bit 有符号立体声交错, 阻塞写入
 *
 * 需要 ESP-IDF 组件: esp_driver_i2s (新版 driver/i2s_std.h), driver (GPIO)
 */
#ifndef __MAX98357A_H__
#define __MAX98357A_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===== 硬件配置（移植时修改此处） ===== */
#define MAX98357A_GPIO_BCLK          41
#define MAX98357A_GPIO_WS            42      /* LRCLK */
#define MAX98357A_GPIO_DOUT          40

#define MAX98357A_AMP_ENABLE_IO      (-1)    /* SD_MODE/功放使能脚, -1 = 不使用(硬件拉高) */
#define MAX98357A_AMP_ENABLE_INVERT  0       /* 1 = 低电平使能功放 */

#define MAX98357A_DEFAULT_SAMPLE_RATE 32000
#define MAX98357A_DMA_DESC_NUM        4       /* DMA 描述符个数 */
#define MAX98357A_DMA_FRAME_NUM       180     /* 每个 DMA 缓冲帧数 */
#define MAX98357A_DEFAULT_VOLUME      50      /* 0-100 */
#define MAX98357A_WRITE_TIMEOUT_MS    1000    /* 单次写入超时 */

/* ===== 类型定义 ===== */
/* 样本格式: 16bit 有符号, 立体声交错 [L0 R0 L1 R1 ...], 1帧 = 2 个 int16 */

/* ===== API ===== */
void   MAX98357A_Init(int sample_rate);                  /* <=0 时用 MAX98357A_DEFAULT_SAMPLE_RATE */
void   MAX98357A_Deinit(void);
size_t MAX98357A_Write(const int16_t *samples, size_t frames); /* 返回已写入帧数 */
void   MAX98357A_SetVolume(int percent);                 /* 0-100, 超出自动钳位 */
void   MAX98357A_SetMute(bool mute);
int    MAX98357A_SetSampleRate(int sample_rate);         /* 热切换, 返回生效采样率; 失败返回 0 */

#ifdef __cplusplus
}
#endif

#endif /* __MAX98357A_H__ */
