idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp32-camera esp_driver_gpio esp_driver_ledc driver esp_hal_clock esp_wifi nvs_flash)
