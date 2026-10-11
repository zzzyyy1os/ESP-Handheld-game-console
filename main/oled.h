/**
 * @file oled.h
 * @brief SSD1306 OLED 驱动 (可移植模块)
 *
 * 使用方法：
 *   1. 创建 I2C 总线: i2c_new_master_bus(...)
 *   2. OLED_Init(bus_handle);
 *   3. OLED_Clear(); OLED_ShowString(0, 0, "Hello"); OLED_Display();
 *
 * 需要 ESP-IDF 组件: driver (I2C master)
 */
#ifndef __OLED_H__
#define __OLED_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "driver/i2c_master.h"

/* ===== 硬件配置（移植时修改此处） ===== */
#define OLED_I2C_ADDR         0x3C      /* 7-bit I2C 地址 */
#define OLED_I2C_SCL_HZ       400000    /* I2C 时钟频率 */
#define OLED_I2C_TIMEOUT_MS   100       /* I2C 传输超时 */
#define OLED_CONTRAST         0xCF      /* SSD1306 对比度 0x00-0xFF */

/* OLED dimensions */
#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_PAGES          (OLED_HEIGHT / 8)

/* Font size */
#define OLED_FONT_WIDTH     6
#define OLED_FONT_HEIGHT    8

/* Function prototypes */
void OLED_Init(i2c_master_bus_handle_t bus_handle);
void OLED_Clear(void);
void OLED_Display(void);
void OLED_SetPixel(uint8_t x, uint8_t y, uint8_t color);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowNum(uint8_t x, uint8_t y, int32_t num, uint8_t len);
void OLED_ShowFloat(uint8_t x, uint8_t y, float num, uint8_t intLen, uint8_t decLen);
void OLED_DrawBMP(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, const uint8_t *bmp);
void OLED_ClearArea(uint8_t x, uint8_t y, uint8_t width, uint8_t height);

#ifdef __cplusplus
}
#endif

#endif /* __OLED_H__ */
