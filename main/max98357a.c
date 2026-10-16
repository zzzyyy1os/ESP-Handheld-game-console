/**
 * @file max98357a.c
 * @brief MAX98357A I2S 数字功放驱动 (可移植模块)
 * @note  GPIO 引脚配置见 max98357a.h
 *
 * 移植自 retro-go drivers/audio/i2s.c (Ext DAC 分支), 改用新版 i2s_std API:
 * 16bit 有符号立体声 / Philips 标准 I2S / 主机发送 / DMA 4x180 帧 / 软件音量缩放
 */
#include "max98357a.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "max98357a";

static i2s_chan_handle_t s_tx_chan = NULL;
static int s_volume = MAX98357A_DEFAULT_VOLUME;
static bool s_muted = false;
static int s_sample_rate = 0;

/* 可选功放使能脚 (SD_MODE) 控制 */
#if MAX98357A_AMP_ENABLE_IO >= 0
static inline void amp_enable_set(bool on)
{
#if MAX98357A_AMP_ENABLE_INVERT
    gpio_set_level(MAX98357A_AMP_ENABLE_IO, on ? 0 : 1);
#else
    gpio_set_level(MAX98357A_AMP_ENABLE_IO, on ? 1 : 0);
#endif
}
#endif

void MAX98357A_Init(int sample_rate)
{
    if (s_tx_chan) {
        return;                                     /* 已初始化 */
    }
    if (sample_rate <= 0) {
        sample_rate = MAX98357A_DEFAULT_SAMPLE_RATE;
    }
    s_volume = MAX98357A_DEFAULT_VOLUME;
    s_muted = false;

    /* 1) 分配 TX 通道 (主机, 仅发送) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = MAX98357A_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = MAX98357A_DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;                     /* DMA 欠载自动补零, 避免残留杂音 (略增中断开销) */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    /* 2) 标准模式 (Philips / 16bit / 立体声) */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MAX98357A_GPIO_BCLK,
            .ws = MAX98357A_GPIO_WS,
            .dout = MAX98357A_GPIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));

    /* 3) 启动通道 */
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

#if MAX98357A_AMP_ENABLE_IO >= 0
    gpio_reset_pin(MAX98357A_AMP_ENABLE_IO);
    gpio_set_direction(MAX98357A_AMP_ENABLE_IO, GPIO_MODE_OUTPUT);
    amp_enable_set(true);
#endif

    s_sample_rate = sample_rate;
    ESP_LOGI(TAG, "MAX98357A initialized: %d Hz, BCLK=%d WS=%d DOUT=%d, vol=%d",
             sample_rate, MAX98357A_GPIO_BCLK, MAX98357A_GPIO_WS,
             MAX98357A_GPIO_DOUT, s_volume);
}

void MAX98357A_Deinit(void)
{
    if (!s_tx_chan) {
        return;
    }
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    s_sample_rate = 0;
    gpio_reset_pin(MAX98357A_GPIO_BCLK);
    gpio_reset_pin(MAX98357A_GPIO_WS);
    gpio_reset_pin(MAX98357A_GPIO_DOUT);
#if MAX98357A_AMP_ENABLE_IO >= 0
    gpio_reset_pin(MAX98357A_AMP_ENABLE_IO);
#endif
    ESP_LOGI(TAG, "deinitialized");
}

size_t MAX98357A_Write(const int16_t *samples, size_t frames)
{
    if (!s_tx_chan || !samples || frames == 0) {
        return 0;
    }

    const int vol = s_muted ? 0 : s_volume;
    int16_t chunk[MAX98357A_DMA_FRAME_NUM * 2];     /* L/R 交错栈缓冲 */
    size_t done = 0;

    while (done < frames) {
        size_t n = frames - done;
        if (n > MAX98357A_DMA_FRAME_NUM) {
            n = MAX98357A_DMA_FRAME_NUM;
        }

        if (vol == 100) {
            memcpy(chunk, samples + done * 2, n * 2 * sizeof(int16_t));
        } else {
            const int16_t *src = samples + done * 2;
            for (size_t i = 0; i < n * 2; ++i) {
                chunk[i] = (int16_t)(((int32_t)src[i] * vol) / 100);
            }
        }

        size_t written_bytes = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, chunk,
                                          n * 2 * sizeof(int16_t),
                                          &written_bytes,
                                          MAX98357A_WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write err=%s, %u/%u bytes", esp_err_to_name(err),
                     (unsigned)written_bytes, (unsigned)(n * 2 * sizeof(int16_t)));
            break;
        }
        done += n;
    }
    return done;
}

void MAX98357A_SetVolume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
}

void MAX98357A_SetMute(bool mute)
{
    s_muted = mute;
    /* 冲刷 DMA: 静音时输出 0, 避免残留旧样本 */
    if (s_tx_chan) {
        int16_t silence[MAX98357A_DMA_FRAME_NUM * 2] = {0};
        MAX98357A_Write(silence, MAX98357A_DMA_FRAME_NUM);
    }
#if MAX98357A_AMP_ENABLE_IO >= 0
    amp_enable_set(!mute);
#endif
    ESP_LOGI(TAG, "mute=%d", (int)mute);
}

int MAX98357A_SetSampleRate(int sample_rate)
{
    if (!s_tx_chan || sample_rate <= 0) {
        return 0;
    }
    if (sample_rate == s_sample_rate) {
        return s_sample_rate;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_ERROR_CHECK(i2s_channel_disable(s_tx_chan));
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx_chan);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set sample rate %d failed: %s", sample_rate, esp_err_to_name(err));
        i2s_channel_enable(s_tx_chan);              /* 恢复运行 */
        return 0;
    }
    s_sample_rate = sample_rate;
    ESP_LOGI(TAG, "sample rate -> %d Hz", sample_rate);
    return s_sample_rate;
}
