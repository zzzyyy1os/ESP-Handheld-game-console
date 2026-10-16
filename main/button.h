/**
 * @file button.h
 * @brief 按键检测模块 - 消抖 + 短按/长按/双击 (可移植模块)
 *
 * 使用方法：
 *   1. 修改下方"硬件配置"区的 GPIO 宏定义
 *   2. Key_Init();
 *   3. 周期性调用 Key_Scan() (建议 10ms)
 *   4. Key_GetEvent(id) 获取事件 (读取后自动清除)
 *
 * 需要 ESP-IDF 组件: driver (GPIO)
 */
#ifndef __BUTTON_H__
#define __BUTTON_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gpio.h"
#include <stdint.h>

/* ===== 硬件配置（移植时修改此处） ===== */
#define KEY_NUM               3

#define KEY_GPIO_1            GPIO_NUM_15
#define KEY_GPIO_2            GPIO_NUM_16
#define KEY_GPIO_3            GPIO_NUM_21

/* 时序参数 */
#define KEY_DEBOUNCE_MS       20     /* 消抖时间 */
#define KEY_LONG_PRESS_MS     1000   /* 长按阈值 */
#define KEY_DOUBLE_CLICK_MS   300    /* 双击间隔窗口 */

/* ===== 类型定义 ===== */

typedef enum {
    KEY_1 = 0,
    KEY_2 = 1,
    KEY_3 = 2,
} KeyId_t;

typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_SHORT_PRESS,   /* 短按 (松开后触发) */
    KEY_EVENT_LONG_PRESS,    /* 长按 (达到阈值触发一次) */
    KEY_EVENT_DOUBLE_CLICK,  /* 双击 */
} KeyEvent_t;

/* ===== API ===== */

void Key_Init(void);
void Key_Scan(void);                      /* 周期性调用 (建议 10ms) */
KeyEvent_t Key_GetEvent(KeyId_t id);      /* 获取事件 (返回后自动清除) */
uint8_t Key_IsPressed(KeyId_t id);        /* 获取按键当前状态 (0=释放, 1=按下) */

#ifdef __cplusplus
}
#endif

#endif /* __BUTTON_H__ */
