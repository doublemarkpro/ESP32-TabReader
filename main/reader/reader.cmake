set(READER_SOURCES
    "reader/main.cc" "reader/reader_application.cc" "reader/reader_display.cc"
    "reader/transfer_server.cc" "reader/board_runtime.cc"
    "settings.cc" "device_state_machine.cc" "display/display.cc"
    "boards/common/backlight.cc" "boards/common/i2c_device.cc"
    "boards/m5stack/tab5-reader/tab5_reader.cc"
    "boards/m5stack/tab5/esp_lcd_st7121.c" "boards/m5stack/tab5/esp_lcd_st7123.c"
    "han_dictionary/reader/epub_reader.cc"
    "display/lvgl_display/jpg/jpeg_to_image.c"
    "reader/assets/reader_ui_24.c")
list(TRANSFORM READER_SOURCES PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
set(READER_INCLUDE_DIRS "." "reader" "boards/common" "boards/m5stack/tab5"
    "display" "display/lvgl_display" "audio" "audio/demuxer" "protocols")
list(TRANSFORM READER_INCLUDE_DIRS PREPEND "${CMAKE_CURRENT_LIST_DIR}/../")
idf_component_register(SRCS ${READER_SOURCES}
    INCLUDE_DIRS ${READER_INCLUDE_DIRS}
    PRIV_REQUIRES esp_timer esp_pm esp_psram nvs_flash esp_event esp_netif esp_wifi
        esp_http_server app_update fatfs sdmmc esp_driver_sdspi esp_driver_spi
        esp_driver_i2c esp_driver_ledc esp_lcd esp_driver_ppa esp_driver_jpeg
        espressif__esp_lvgl_port lvgl__lvgl espressif__freetype
        espressif__esp_lcd_ili9881c espressif__esp_lcd_touch_gt911
        espressif__esp_lcd_touch_st7123 espressif__esp_tinyusb
        espressif__esp_new_jpeg espressif__cjson espressif__esp_hosted 78__esp-wifi-connect)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_get_property(READER_LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${READER_LVGL_LIB} PRIVATE idf::espressif__freetype)
    target_link_libraries(${COMPONENT_LIB} PRIVATE idf::espressif__freetype)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_TYPE="m5stack-tab5-reader"
        BOARD_NAME="${BOARD_NAME}" BOARD_MANUFACTURER="m5stack" LV_LVGL_H_INCLUDE_SIMPLE)
endif()
