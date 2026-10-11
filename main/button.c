/**
 * @file button.c
 * @brief 按键检测 - 消抖 + 短按/长按/双击 (可移植模块)
 *        按键接法：上拉输入，低电平有效
 * @note  GPIO 引脚配置见 button.h
 */
#include "button.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "button";

/* 状态机状态 */
typedef enum {
    ST_IDLE = 0,           /* 空闲，等待按下 */
    ST_DEBOUNCE_PRESS,     /* 按下消抖 */
    ST_PRESSED,            /* 已确认按下，等待松开或长按超时 */
    ST_DEBOUNCE_RELEASE,   /* 松开消抖 */
    ST_WAIT_2ND_PRESS,     /* 等待第二次按下 (双击窗口) */
    ST_DEBOUNCE_2ND,       /* 第二次按下消抖 */
    ST_WAIT_2ND_RELEASE,   /* 等待第二次松开 */
} KeyState_t;

/* 按键实例 */
typedef struct {
    gpio_num_t    gpio;
    KeyState_t    state;
    uint32_t      tick;           /* 状态计时 */
    KeyEvent_t    event;          /* 待读取的事件 */
} Key_t;

static Key_t keys[KEY_NUM] = {
    { .gpio = KEY_GPIO_1, .state = ST_IDLE },
    { .gpio = KEY_GPIO_2, .state = ST_IDLE },
    { .gpio = KEY_GPIO_3, .state = ST_IDLE },
    { .gpio = KEY_GPIO_4, .state = ST_IDLE },
};

void Key_Init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KEY_GPIO_1) |
                        (1ULL << KEY_GPIO_2) |
                        (1ULL << KEY_GPIO_3) |
                        (1ULL << KEY_GPIO_4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    for (int i = 0; i < KEY_NUM; i++) {
        keys[i].state = ST_IDLE;
        keys[i].tick = 0;
        keys[i].event = KEY_EVENT_NONE;
    }

    ESP_LOGI(TAG, "Keys initialized (pull-up input, active low)");
}

/* 读取引脚: 1=按下, 0=释放 */
static inline uint8_t read_pin(Key_t *k)
{
    return (gpio_get_level(k->gpio) == 0) ? 1 : 0;
}

/* 按键扫描 (周期性调用, 建议 10ms) */
void Key_Scan(void)
{
    uint32_t now = xTaskGetTickCount();

    for (int i = 0; i < KEY_NUM; i++) {
        Key_t *k = &keys[i];
        uint8_t pressed = read_pin(k);

        switch (k->state) {

        case ST_IDLE:
            if (pressed) {
                k->state = ST_DEBOUNCE_PRESS;
                k->tick = now;
            }
            break;

        case ST_DEBOUNCE_PRESS:
            if (!pressed) {
                k->state = ST_IDLE;
            } else if ((now - k->tick) >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
                k->state = ST_PRESSED;
            }
            break;

        case ST_PRESSED:
            if (!pressed) {
                k->state = ST_DEBOUNCE_RELEASE;
                k->tick = now;
            } else if ((now - k->tick) >= pdMS_TO_TICKS(KEY_LONG_PRESS_MS)) {
                k->event = KEY_EVENT_LONG_PRESS;
                k->state = ST_IDLE;
            }
            break;

        case ST_DEBOUNCE_RELEASE:
            if (pressed) {
                k->state = ST_PRESSED;
            } else if ((now - k->tick) >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
                k->state = ST_WAIT_2ND_PRESS;
                k->tick = now;
            }
            break;

        case ST_WAIT_2ND_PRESS:
            if (pressed) {
                k->state = ST_DEBOUNCE_2ND;
                k->tick = now;
            } else if ((now - k->tick) >= pdMS_TO_TICKS(KEY_DOUBLE_CLICK_MS)) {
                k->event = KEY_EVENT_SHORT_PRESS;
                k->state = ST_IDLE;
            }
            break;

        case ST_DEBOUNCE_2ND:
            if (!pressed) {
                k->state = ST_WAIT_2ND_PRESS;
            } else if ((now - k->tick) >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
                k->state = ST_WAIT_2ND_RELEASE;
            }
            break;

        case ST_WAIT_2ND_RELEASE:
            if (!pressed) {
                k->event = KEY_EVENT_DOUBLE_CLICK;
                k->state = ST_IDLE;
            }
            break;
        }
    }
}

/* 获取按键事件 (读取后自动清除) */
KeyEvent_t Key_GetEvent(KeyId_t id)
{
    if (id >= KEY_NUM) return KEY_EVENT_NONE;
    KeyEvent_t e = keys[id].event;
    keys[id].event = KEY_EVENT_NONE;
    return e;
}

/* 获取按键当前状态 (0=释放, 1=按下) */
uint8_t Key_IsPressed(KeyId_t id)
{
    if (id >= KEY_NUM) return 0;
    return read_pin(&keys[id]);
}
