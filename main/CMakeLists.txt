idf_component_register(SRCS "max98357a.c" "oled.c" "rocker.c" "button.c" "espnow_example_main.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_adc driver esp_driver_i2s
                    INCLUDE_DIRS ".")
