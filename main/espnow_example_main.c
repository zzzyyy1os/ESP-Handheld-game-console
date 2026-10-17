/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   This example shows how to use ESPNOW.
   Prepare two device, one for sending ESPNOW data and another for receiving
   ESPNOW data.
*/
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <dirent.h>

/* 喇叭音量 (0-100), 修改此处调音量 */
#define SPK_TEST_VOLUME   5
#define MP3_PLAY_VOLUME   50      /* MP3 播放音量 (0-100) */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_example.h"
#include "driver/i2c_master.h"
#include "esp_system.h"
#include "oled.h"
#include "rocker.h"
#include "button.h"
#include "max98357a.h"
#include "sdcard.h"
#include "mp3play.h"
#include "lcd.h"

#define ESPNOW_MAXDELAY 512

#if 0 /* 只测显示屏: WiFi/ESPNOW 停用 (恢复: 删本行与下方对应 #endif, 并恢复 app_main 里的调用) */
static const char *TAG = "espnow_example";

static QueueHandle_t s_example_espnow_queue = NULL;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

/* WiFi should start before using ESPNOW */
static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (tx_info == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t * mac_addr = recv_info->src_addr;
    uint8_t * des_addr = recv_info->des_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    if (IS_BROADCAST_ADDR(des_addr)) {
        /* If added a peer with encryption before, the receive packets may be
         * encrypted as peer-to-peer message or unencrypted over the broadcast channel.
         * Users can check the destination address to distinguish it.
         */
        ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
    } else {
        ESP_LOGD(TAG, "Receive unicast ESPNOW data");
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

/* Parse received ESPNOW data. */
int example_espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *state, uint16_t *seq, uint32_t *magic)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(example_espnow_data_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    *state = buf->state;
    *seq = buf->seq_num;
    *magic = buf->magic;
    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        return buf->type;
    }

    return -1;
}

/* Prepare ESPNOW data to be sent. */
void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->state = send_param->state;
    buf->seq_num = s_example_espnow_seq[buf->type]++;
    buf->crc = 0;
    buf->magic = send_param->magic;
    /* Fill all remaining bytes after the data with random values */
    esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    uint32_t recv_magic = 0;
    bool is_broadcast = false;
    int ret;

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Start sending broadcast data");

    /* Start sending broadcast ESPNOW data. */
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                ESP_LOGD(TAG, "Send data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

                if (is_broadcast && (send_param->broadcast == false)) {
                    break;
                }

                if (!is_broadcast) {
                    send_param->count--;
                    if (send_param->count == 0) {
                        ESP_LOGI(TAG, "Send done");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                }

                /* Delay a while before sending the next data. */
                if (send_param->delay > 0) {
                    vTaskDelay(send_param->delay/portTICK_PERIOD_MS);
                }

                ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(send_cb->mac_addr));

                memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                example_espnow_data_prepare(send_param);

                /* Send the next data after the previous data is sent. */
                if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                    ESP_LOGE(TAG, "Send error");
                    example_espnow_deinit(send_param);
                    vTaskDelete(NULL);
                }
                break;
            }
            case EXAMPLE_ESPNOW_RECV_CB:
            {
                example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_state, &recv_seq, &recv_magic);
                free(recv_cb->data);
                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                    ESP_LOGI(TAG, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG, "Malloc peer information fail");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = true;
                        memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                    }

                    /* Indicates that the device has received broadcast ESPNOW data. */
                    if (send_param->state == 0) {
                        send_param->state = 1;
                    }

                    /* If receive broadcast ESPNOW data which indicates that the other device has received
                     * broadcast ESPNOW data and the local magic number is bigger than that in the received
                     * broadcast ESPNOW data, stop sending broadcast ESPNOW data and start sending unicast
                     * ESPNOW data.
                     */
                    if (recv_state == 1) {
                        /* The device which has the bigger magic number sends ESPNOW data, the other one
                         * receives ESPNOW data.
                         */
                        if (send_param->unicast == false && send_param->magic >= recv_magic) {
                    	    ESP_LOGI(TAG, "Start sending unicast data");
                    	    ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(recv_cb->mac_addr));

                    	    /* Start sending unicast ESPNOW data. */
                            memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                            example_espnow_data_prepare(send_param);
                            if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                                ESP_LOGE(TAG, "Send error");
                                example_espnow_deinit(send_param);
                                vTaskDelete(NULL);
                            }
                            else {
                                send_param->broadcast = false;
                                send_param->unicast = true;
                            }
                        }
                    }
                }
                else if (ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
                    ESP_LOGI(TAG, "Receive %dth unicast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW data. */
                    send_param->broadcast = false;
                }
                else {
                    ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }
}

static esp_err_t example_espnow_init(void)
{
    example_espnow_send_param_t *send_param;

    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(example_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(example_espnow_recv_cb) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG, "Malloc peer information fail");
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(example_espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(example_espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->state = 0;
    send_param->magic = esp_random();
    send_param->count = CONFIG_ESPNOW_SEND_COUNT;
    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        free(send_param);
        vQueueDelete(s_example_espnow_queue);
        s_example_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    example_espnow_data_prepare(send_param);

    xTaskCreate(example_espnow_task, "example_espnow_task", 2560, send_param, 4, NULL);

    return ESP_OK;
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vQueueDelete(s_example_espnow_queue);
    s_example_espnow_queue = NULL;
    esp_now_deinit();
}
#endif /* 只测显示屏: WiFi/ESPNOW 停用 */

/* ========== I2C & OLED ========== */
#if 0 /* OLED 与 LCD 引脚冲突: OLED SDA=GPIO3 = LCD_RST=GPIO3, 二者只能启用一个。
         启用 LCD 测试(lcd_test_task)时关闭 OLED; 要用 OLED 时恢复本段及下方
         oled_task / oled_i2c_init() 调用, 并注释 lcd_test_task 的 xTaskCreate */
#define OLED_I2C_PORT       I2C_NUM_1
#define OLED_I2C_SDA_IO     3
#define OLED_I2C_SCL_IO     4
#define OLED_I2C_FREQ_HZ    400000

static i2c_master_bus_handle_t oled_i2c_bus_handle = NULL;

static void oled_i2c_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_I2C_SDA_IO,
        .scl_io_num = OLED_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &oled_i2c_bus_handle));
    ESP_LOGI(TAG, "I2C initialized: SDA=GPIO%d, SCL=GPIO%d", OLED_I2C_SDA_IO, OLED_I2C_SCL_IO);
}
#endif /* OLED I2C */

/* 摇杆采样任务 */
#if 0 /* 只测显示屏, 外设停用 (要恢复: 去掉本行与对应 #endif, 并恢复 app_main 里的 xTaskCreate) */
static void rocker_task(void *pvParameter)
{
    rocker_value_t val;
    rocker_init();

    while (1) {
        rocker_read(&val);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* 按键扫描任务 */
static void button_task(void *pvParameter)
{
    static const char *evt_name[] = { "none", "short press", "long press", "double click" };
    static const char *BTN_TAG = "button";
    Key_Init();
    int status_cnt = 0;

    while (1) {
        Key_Scan();
        for (int i = 0; i < KEY_NUM; i++) {
            KeyEvent_t e = Key_GetEvent((KeyId_t)i);
            if (e != KEY_EVENT_NONE) {
                ESP_LOGI(BTN_TAG, "KEY_%d: %s", i + 1, evt_name[e]);
            }
        }

        /* 每500ms打印一次按键引脚状态 (诊断用) */
        if (++status_cnt >= 50) {
            status_cnt = 0;
            ESP_LOGI(BTN_TAG, "keys: %d %d %d",
                Key_IsPressed(KEY_1), Key_IsPressed(KEY_2),
                Key_IsPressed(KEY_3));
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif /* rocker_task + button_task */

/* OLED显示任务 */
#if 0 /* GPIO3 冲突, 同上: 启用 LCD 时关闭 OLED */
static void oled_task(void *pvParameter)
{
    /* Wait for OLED power stable */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "OLED init start, bus_handle=%p", (void *)oled_i2c_bus_handle);
    OLED_Init(oled_i2c_bus_handle);
    ESP_LOGI(TAG, "OLED init done");

    /* Static labels */
    OLED_Clear();
    OLED_ShowString(0,  0, "  JOYSTICK  MONITOR");
    OLED_ShowString(0, 16, "LX:");
    OLED_ShowString(64, 16, "LY:");
    OLED_ShowString(0, 32, "RY:");
    OLED_ShowString(64, 32, "RX:");
    OLED_ShowString(0, 48, "--------------------");
    OLED_Display();

    while (1) {
        rocker_value_t val;
        rocker_get(&val);

        /* Update dynamic values */
        OLED_ClearArea(24, 16, 36, 8);
        OLED_ClearArea(88, 16, 36, 8);
        OLED_ClearArea(24, 32, 36, 8);
        OLED_ClearArea(88, 32, 36, 8);

        OLED_ShowNum(24, 16, val.lx, 4);
        OLED_ShowNum(88, 16, val.ly, 4);
        OLED_ShowNum(24, 32, val.ry, 4);
        OLED_ShowNum(88, 32, val.rx, 4);

        OLED_Display();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif /* oled_task */

/* LCD 测试任务: 分层点亮 (背光诊断 → 初始化 → 白屏/灰阶判定 → 轮播测试画面)。
   只测显示屏, 不用 SD/摇杆/按键等外设。串口 tag: lcd_test */
static void lcd_test_task(void *pvParameter)
{
    static const char *LTAG = "lcd_test";

    vTaskDelay(pdMS_TO_TICKS(1000));   /* 等系统稳定 */

    /* phase 0: 背光诊断 (纯 GPIO/LEDC, 不依赖 SPI/初始化), ~10s
       肉眼观察哪一档亮, 回填 lcd.h 的 LCD_BCKL_INVERT/LCD_BCKL_MODE;
       全不亮 → 硬件问题 (GND 共地/BLK 线/供电/FPC), 停止查初始化 */
    ESP_LOGI(LTAG, "phase 0: backlight diag (~10s)");
    LCD_BacklightDiag();

    LCD_Init();
    LCD_Clear(LCD_BLACK);
    LCD_Sync();
    vTaskDelay(pdMS_TO_TICKS(80));     /* 清屏完成再开背光 (retro-go 经验值) */
    LCD_SetBacklight(LCD_BRIGHTNESS);

    /* phase 1: 白屏判定页 — 背光亮 + 此页黑/灰 → 试 lcd.h LCD_INVON; 面板活着的标准 */
    ESP_LOGI(LTAG, "phase 1: white screen (panel alive if not black)");
    LCD_Clear(LCD_WHITE);
    LCD_ShowString(8, 8, "WHITE SCREEN", LCD_BLACK, LCD_WHITE, 2);
    LCD_Sync();
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* phase 1b: 灰阶 — 有明暗渐变 = SPI 通信正常; 无渐变且全黑 = 没信号 */
    ESP_LOGI(LTAG, "phase 1b: gray gradient (check brightness steps)");
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)(i * 17);
        LCD_FillRect(0, i * 15, LCD_WIDTH, 15, LCD_RGB565(v, v, v));
    }
    LCD_ShowString(8, 8, "GRAYSCALE", LCD_RED, LCD_BLACK, 2);
    LCD_Sync();
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 开机画面 */
    LCD_ShowString(46, 70, "espnow-1 LCD TEST", LCD_CYAN, LCD_BLACK, 2);
    LCD_ShowString(94, 110, "ST7789 320x240", LCD_WHITE, LCD_BLACK, 1);
    LCD_ShowString(70, 130, "BOE3.2 seq @1MHz bring-up", LCD_GRAY, LCD_BLACK, 1);
    LCD_Sync();
    ESP_LOGI(LTAG, "init ok, start test loop");
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        /* 1) 纯色轮播: 查花屏/无显示/坏点 */
        static const struct { uint16_t c; const char *n; } solids[] = {
            {LCD_RED, "RED"}, {LCD_GREEN, "GREEN"}, {LCD_BLUE, "BLUE"},
            {LCD_WHITE, "WHITE"}, {LCD_BLACK, "BLACK"},
        };
        ESP_LOGI(LTAG, "phase 2: solid colors");
        for (int i = 0; i < 5; i++) {
            uint16_t text = (solids[i].c == LCD_WHITE) ? LCD_BLACK : LCD_WHITE;
            LCD_Clear(solids[i].c);
            LCD_ShowString(8, 8, solids[i].n, text, solids[i].c, 2);
            LCD_Sync();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        /* 2) 8 色竖条: 查 RGB/BGR 顺序与方向 (红蓝互换时改 lcd.h LCD_RGB_BGR) */
        static const uint16_t bars[8] = {
            LCD_BLACK, LCD_BLUE, LCD_GREEN, LCD_CYAN,
            LCD_RED, LCD_MAGENTA, LCD_YELLOW, LCD_WHITE,
        };
        ESP_LOGI(LTAG, "phase 3: color bars");
        for (int i = 0; i < 8; i++) {
            LCD_FillRect(i * 40, 0, 40, LCD_HEIGHT, bars[i]);
        }
        LCD_ShowString(8, 8, "COLOR BARS", LCD_WHITE, LCD_BLACK, 2);
        LCD_Sync();
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 3) 灰阶横条: 查反显 (梯度黑白颠倒时改 lcd.h LCD_INVON) */
        ESP_LOGI(LTAG, "phase 4: gray gradient");
        for (int i = 0; i < 16; i++) {
            uint8_t v = (uint8_t)(i * 17);
            LCD_FillRect(0, i * 15, LCD_WIDTH, 15, LCD_RGB565(v, v, v));
        }
        LCD_ShowString(8, 8, "GRAYSCALE", LCD_RED, LCD_BLACK, 2);
        LCD_Sync();
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 4) 棋盘: 查显示偏移/对齐 */
        ESP_LOGI(LTAG, "phase 5: checkerboard");
        for (int y = 0; y < LCD_HEIGHT; y += 16) {
            for (int x = 0; x < LCD_WIDTH; x += 16) {
                LCD_FillRect(x, y, 16, 16, ((x / 16 + y / 16) & 1) ? LCD_WHITE : LCD_BLACK);
            }
        }
        LCD_ShowString(8, 8, "CHECKER", LCD_RED, LCD_BLACK, 2);
        LCD_Sync();
        vTaskDelay(pdMS_TO_TICKS(2000));

        /* 5) 文字 3 档字号 */
        ESP_LOGI(LTAG, "phase 6: text");
        LCD_Clear(LCD_RGB565(0, 0, 64));
        LCD_ShowString(8, 8,  "ABCDEFGHIJKLM", LCD_WHITE, LCD_RGB565(0, 0, 64), 1);
        LCD_ShowString(8, 24, "NOPQRSTUVWXYZ", LCD_YELLOW, LCD_RGB565(0, 0, 64), 1);
        LCD_ShowString(8, 48, "0123456789 +-*/", LCD_GREEN, LCD_RGB565(0, 0, 64), 2);
        LCD_ShowString(8, 88, "Hello LCD!", LCD_CYAN, LCD_RGB565(0, 0, 64), 3);
        LCD_Sync();
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

#if 0 /* 诊断蜂鸣: 与 MP3 播放共用 I2S, 已由 mp3_play_task 替代; 需诊断时去掉 #if 0/#endif 并注释 mp3_play_task */
/* 扬声器诊断任务 (fw v3): 每 5s 一轮 = 0.4s 50Hz 方波嗡嗡测试 + 0.5s 440Hz 音调测试 */
static void spk_test_task(void *pvParameter)
{
    static const char *STAG = "spk_test";
    const int sample_rate = MAX98357A_DEFAULT_SAMPLE_RATE;   /* 32000 */
    const int fade_frames = 160;        /* 首尾各 5ms 淡入淡出 @32kHz */

    vTaskDelay(pdMS_TO_TICKS(1000));   /* 等系统稳定 */
    MAX98357A_Init(sample_rate);
    MAX98357A_SetVolume(SPK_TEST_VOLUME);
    ESP_LOGI(STAG, "fw v3 diag start: buzz 50Hz square + tone 440Hz sine, vol=%d", SPK_TEST_VOLUME);

    #define BEEP_CHUNK_FRAMES 1024
    static int16_t buf[BEEP_CHUNK_FRAMES * 2];
    int cycle = 0;

    while (1) {
        /* --- 1) 嗡嗡测试: 50Hz 方波 ±30000, 0.4s (功放/喇叭活着就该很响) --- */
        int total = sample_rate * 400 / 1000;
        int sent = 0;
        bool ok = true;
        while (sent < total) {
            int n = total - sent;
            if (n > BEEP_CHUNK_FRAMES) {
                n = BEEP_CHUNK_FRAMES;
            }
            for (int i = 0; i < n; i++) {
                int idx = sent + i;
                int16_t v = ((idx / (sample_rate / 100)) & 1) ? 30000 : -30000;  /* 每 320 帧翻转 */
                buf[i * 2]     = v;
                buf[i * 2 + 1] = v;
            }
            size_t wrote = MAX98357A_Write(buf, n);
            if (wrote < (size_t)n) {
                ESP_LOGE(STAG, "buzz write short: %u/%d", (unsigned)wrote, n);
                ok = false;
                break;
            }
            sent += n;
        }
        vTaskDelay(pdMS_TO_TICKS(200));

        /* --- 2) 音调测试: 440Hz 正弦 ±12000, 0.5s, 相位累加连续 + 淡入淡出 --- */
        const float dphi = 2.0f * (float)M_PI * 440.0f / (float)sample_rate;
        total = sample_rate / 2;
        sent = 0;
        float phase = 0.0f;
        while (sent < total) {
            int n = total - sent;
            if (n > BEEP_CHUNK_FRAMES) {
                n = BEEP_CHUNK_FRAMES;
            }
            for (int i = 0; i < n; i++) {
                float env = 1.0f;
                int idx = sent + i;
                if (idx < fade_frames) {
                    env = (float)idx / (float)(fade_frames - 1);
                } else if (idx >= total - fade_frames) {
                    env = (float)(total - 1 - idx) / (float)(fade_frames - 1);
                }
                int16_t v = (int16_t)(sinf(phase) * 12000.0f * env);
                buf[i * 2]     = v;
                buf[i * 2 + 1] = v;
                phase += dphi;
                if (phase >= 2.0f * (float)M_PI) {
                    phase -= 2.0f * (float)M_PI;
                }
            }
            size_t wrote = MAX98357A_Write(buf, n);
            if (wrote < (size_t)n) {
                ESP_LOGE(STAG, "tone write short: %u/%d", (unsigned)wrote, n);
                ok = false;
                break;
            }
            sent += n;
        }
        ESP_LOGI(STAG, "diag cycle %d %s (buzz+tone)", ++cycle, ok ? "done" : "FAILED");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
#endif /* spk_test_task */

/* MP3 播放任务: 待 SD 就绪后顺序播放根目录全部 .mp3, 播完循环列表 */
#if 0 /* 只测显示屏, SD/音频停用 (要恢复: 去掉本行与对应 #endif, 并恢复 app_main 里的 xTaskCreate) */
static void mp3_play_task(void *pvParameter)
{
    static const char *STAG = "mp3_test";
    char path[288];            /* "/sd/" + 长文件名 (FAT LFN 最长 255) + NUL */

    vTaskDelay(pdMS_TO_TICKS(1000));
    MAX98357A_Init(44100);                  /* MP3 常见 44.1kHz, 播放中按码流自动热切换 */
    MAX98357A_SetVolume(MP3_PLAY_VOLUME);
    ESP_LOGI(STAG, "mp3 player start, vol=%d", MP3_PLAY_VOLUME);

    while (1) {
        if (!SD_IsReady()) {
            ESP_LOGE(STAG, "SD not ready, wait 5s (mounting is done by sd_test_task)");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        DIR *dir = opendir(SD_MOUNT_POINT);
        if (!dir) {
            ESP_LOGE(STAG, "opendir %s failed", SD_MOUNT_POINT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int played = 0;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            const char *n = e->d_name;
            size_t len = strlen(n);
            /* 文件名以 .mp3 结尾 (不区分大小写) */
            if (len < 4 || n[len - 4] != '.' ||
                (n[len - 3] != 'm' && n[len - 3] != 'M') ||
                (n[len - 2] != 'p' && n[len - 2] != 'P') || n[len - 1] != '3') {
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, n);
            ESP_LOGI(STAG, "play %s", path);
            MAX98357A_SetVolume(MP3_PLAY_VOLUME);
            MP3Play_PlayFile(path);
            played++;
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        closedir(dir);

        if (played == 0) {
            ESP_LOGE(STAG, "no .mp3 found in %s, retry in 10s", SD_MOUNT_POINT);
            vTaskDelay(pdMS_TO_TICKS(10000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));   /* 列表播完, 停 2s 循环重播 */
        }
    }
}

/* SD 卡测试任务: 挂载后列出根目录 + 写/读测试文件, 之后每 10s 重复巡检 */
static void sd_test_task(void *pvParameter)
{
    static const char *STAG = "sd_test";
    char path[64];

    vTaskDelay(pdMS_TO_TICKS(2000));   /* 等系统稳定 */
    SD_Init();

    while (1) {
        if (!SD_IsReady()) {
            ESP_LOGE(STAG, "SD not ready, retry in 5s (insert card / check wiring)");
            vTaskDelay(pdMS_TO_TICKS(5000));
            SD_Init();
            continue;
        }

        /* 1) 列出根目录 */
        DIR *dir = opendir(SD_MOUNT_POINT);
        if (dir) {
            struct dirent *e;
            int n = 0;
            while ((e = readdir(dir)) != NULL && n < 20) {
                ESP_LOGI(STAG, "  %s %s", (e->d_type == DT_DIR) ? "[DIR ]" : "[FILE]", e->d_name);
                n++;
            }
            closedir(dir);
            ESP_LOGI(STAG, "root listing done (%d entries)", n);
        } else {
            ESP_LOGE(STAG, "opendir %s failed", SD_MOUNT_POINT);
        }

        /* 2) 写测试文件 */
        snprintf(path, sizeof(path), "%s/test.txt", SD_MOUNT_POINT);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "hello espnow-1 sd test\n");
            fclose(f);
            ESP_LOGI(STAG, "wrote %s", path);
        } else {
            ESP_LOGE(STAG, "open %s for write failed", path);
        }

        /* 3) 读回验证 */
        f = fopen(path, "r");
        if (f) {
            char line[64] = {0};
            if (fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                ESP_LOGI(STAG, "read back: \"%s\"", line);
            } else {
                ESP_LOGE(STAG, "read %s failed", path);
            }
            fclose(f);
        } else {
            ESP_LOGE(STAG, "open %s for read failed", path);
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
#endif /* mp3_play_task + sd_test_task */

void app_main(void)
{
    /* 只测显示屏: 串口仅输出 LCD 日志 (其他模块任务已停用, 日志顺带屏蔽) */
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set("lcd", ESP_LOG_INFO);
    esp_log_level_set("lcd_test", ESP_LOG_INFO);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // Initialize I2C for OLED
#if 0 /* GPIO3 冲突 (OLED SDA=LCD_RST=3), 启用 LCD 时关闭 OLED */
    oled_i2c_init();
#endif

    // Start rocker sampling task
#if 0 /* 只测显示屏, 外设停用 (rocker ADC 与 LCD 无冲突, 要用时恢复) */
    xTaskCreate(rocker_task, "rocker_task", 4096, NULL, 5, NULL);
#endif

    // Start button scan task
#if 0 /* 只测显示屏, 外设停用 */
    xTaskCreate(button_task, "button_task", 3072, NULL, 5, NULL);
#endif

    // Start OLED display task
#if 0 /* GPIO3 冲突, 同上 */
    xTaskCreate(oled_task, "oled_task", 4096, NULL, 3, NULL);
#endif

    // Start LCD test task (ST7789 320x240, spi_task 线程由 LCD_Init 创建)
    xTaskCreate(lcd_test_task, "lcd_test_task", 6144, NULL, 3, NULL);

    // Start speaker test task
#if 0 /* 只测显示屏, 音频停用 */
    // xTaskCreate(spk_test_task, "spk_test_task", 4096, NULL, 3, NULL);
#endif

    // Start MP3 play task
#if 0 /* 只测显示屏, SD/音频停用 */
    xTaskCreate(mp3_play_task, "mp3_play_task", 8192, NULL, 3, NULL);
#endif

    // Start SD card test task
#if 0 /* 只测显示屏, SD 停用 */
    xTaskCreate(sd_test_task, "sd_test_task", 4096, NULL, 3, NULL);
#endif

#if 0 /* 只测显示屏, 网络停用 (省电流, 排除射频干扰) */
    example_wifi_init();
    example_espnow_init();
#endif
}