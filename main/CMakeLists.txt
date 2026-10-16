idf_component_register(SRCS "sdcard.c" "max98357a.c" "mp3play.c" "oled.c" "rocker.c" "button.c" "espnow_example_main.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_adc driver esp_driver_i2s fatfs sdmmc espressif__esp_audio_codec
                    INCLUDE_DIRS ".")
