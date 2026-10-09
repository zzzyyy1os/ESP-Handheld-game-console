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
#include "esp_rom_sys.h"
#include "espnow_example.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/i2c_master.h"
#include "oled.h"

#define ESPNOW_MAXDELAY 512

static const char *TAG = "espnow_example";

/* ADC Configuration */
#define ADC_CHANNELS       4
#define ADC_ATTEN          ADC_ATTEN_DB_12
#define ADC_BITWIDTH       ADC_BITWIDTH_12
#define ADC_DEADZONE       120    /* 死区范围，中心附近视为0 */

/* 各通道静止时的中心值 (根据实际测量) */
static const int adc_center[ADC_CHANNELS] = {
    1942,  /* CH0: LX */
    1997,  /* CH1: LY */
    1910,  /* CH2: RY */
    1968,  /* CH3: RX */
};
#define OUTPUT_MAX         100    /* 输出最大值 */

/* ADC1 Channel to GPIO mapping for ESP32-S3:
 *   ADC1_CH6 -> GPIO7 -> LX (左摇杆X轴)
 *   ADC1_CH7 -> GPIO8 -> LY (左摇杆Y轴)
 *   ADC1_CH4 -> GPIO5 -> RY (右摇杆Y轴)
 *   ADC1_CH5 -> GPIO6 -> RX (右摇杆X轴)
 */
static const adc_channel_t adc_channels[ADC_CHANNELS] = {
    ADC_CHANNEL_6,  // GPIO7 -> LX
    ADC_CHANNEL_7,  // GPIO8 -> LY
    ADC_CHANNEL_4,  // GPIO5 -> RY
    ADC_CHANNEL_5,  // GPIO6 -> RX
};

static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc_cali_handles[ADC_CHANNELS] = {NULL};

/* ADC calibration init */
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel,
                                  adc_atten_t atten, adc_cali_handle_t *out_handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH,
    };
    return (adc_cali_create_scheme_curve_fitting(&cali_config, out_handle) == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH,
    };
    return (adc_cali_create_scheme_line_fitting(&cali_config, out_handle) == ESP_OK);
#else
    return false;
#endif
}

/* ADC initialization */
static void adc_init(void)
{
    /* ADC1 Init */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    /* Configure each channel */
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH,
    };

    for (int i = 0; i < ADC_CHANNELS; i++) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, adc_channels[i], &chan_config));
        /* Init calibration */
        if (!adc_calibration_init(ADC_UNIT_1, adc_channels[i], ADC_ATTEN, &adc_cali_handles[i])) {
            ESP_LOGW(TAG, "ADC calibration failed for channel %d (GPIO%d)", i, i + 1);
        }
    }

    ESP_LOGI(TAG, "ADC initialized: LX=GPIO7, LY=GPIO8, RY=GPIO5, RX=GPIO6");
}

/* Map ADC raw value to -100 ~ +100 */
static int map_adc_to_range(int raw, int ch)
{
    int center = adc_center[ch];
    int diff = raw - center;

    /* 死区处理：中心附近视为0 */
    if (diff > -ADC_DEADZONE && diff < ADC_DEADZONE) {
        return 0;
    }

    /* 正方向映射 */
    if (diff > 0) {
        int mapped = (diff - ADC_DEADZONE) * OUTPUT_MAX / (4095 - center - ADC_DEADZONE);
        return (mapped > OUTPUT_MAX) ? OUTPUT_MAX : mapped;
    }

    /* 负方向映射 */
    int mapped = (-diff - ADC_DEADZONE) * OUTPUT_MAX / (center - ADC_DEADZONE);
    return (mapped > OUTPUT_MAX) ? -OUTPUT_MAX : -mapped;
}

/* Shared joystick values for OLED display */
static volatile int joy_lx = 0, joy_ly = 0, joy_ry = 0, joy_rx = 0;

/* Read ADC values, map to -100~+100, and log via serial */
static void adc_read_task(void *pvParameter)
{
    int raw[ADC_CHANNELS];
    const int samples = 32;  /* 每通道采样次数 */

    while (1) {
        /* 每个通道独立连续采样，采样完再切下一个通道 */
        int sum[ADC_CHANNELS] = {0};
        for (int i = 0; i < ADC_CHANNELS; i++) {
            esp_rom_delay_us(500);  /* 切换通道前等待500us */
            for (int s = 0; s < samples; s++) {
                int val;
                ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, adc_channels[i], &val));
                sum[i] += val;
            }
        }
        for (int i = 0; i < ADC_CHANNELS; i++) {
            raw[i] = sum[i] / samples;
        }

        /* 映射到 -100 ~ +100 */
        int lx = map_adc_to_range(raw[0], 0);  // GPIO7 -> LX
        int ly = map_adc_to_range(raw[1], 1);  // GPIO8 -> LY
        int ry = -map_adc_to_range(raw[2], 2); // GPIO5 -> RY (反转)
        int rx = -map_adc_to_range(raw[3], 3); // GPIO6 -> RX (反转)

        /* 更新共享变量 */
        joy_lx = lx;
        joy_ly = ly;
        joy_ry = ry;
        joy_rx = rx;

        /* 临时调试：打印原始值，确定RX/RY的中心值 */
        ESP_LOGI(TAG, "raw: %4d %4d %4d %4d", raw[0], raw[1], raw[2], raw[3]);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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

/* ========== I2C & OLED ========== */
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

/* OLED display task */
static void oled_task(void *pvParameter)
{
    /* Wait for OLED power stable */
    vTaskDelay(pdMS_TO_TICKS(500));
    OLED_Init(oled_i2c_bus_handle);

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
        /* Update dynamic values */
        OLED_ClearArea(24, 16, 36, 8);
        OLED_ClearArea(88, 16, 36, 8);
        OLED_ClearArea(24, 32, 36, 8);
        OLED_ClearArea(88, 32, 36, 8);

        OLED_ShowNum(24, 16, joy_lx, 4);
        OLED_ShowNum(88, 16, joy_ly, 4);
        OLED_ShowNum(24, 32, joy_ry, 4);
        OLED_ShowNum(88, 32, joy_rx, 4);

        OLED_Display();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    // Initialize ADC (4 channels: GPIO7-10)
    adc_init();

    // Initialize I2C for OLED
    oled_i2c_init();

    // Start ADC reading task
    xTaskCreate(adc_read_task, "adc_read_task", 4096, NULL, 5, NULL);

    // Start OLED display task
    xTaskCreate(oled_task, "oled_task", 4096, NULL, 3, NULL);

    example_wifi_init();
    example_espnow_init();
}
