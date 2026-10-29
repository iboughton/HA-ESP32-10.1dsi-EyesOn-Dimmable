
idf_component_register(
    SRCS "esp32_p4_wifi6_touch_lcd_4b.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    # BETTA fork: on ESP-IDF 6 the legacy "driver" component no longer pulls in
    # the peripheral drivers, so require the ones this BSP's headers use directly.
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_sdmmc sdmmc
    PRIV_REQUIRES esp_lcd usb spiffs fatfs
)
