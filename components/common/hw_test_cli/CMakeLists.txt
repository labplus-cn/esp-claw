set(_hw_test_srcs
    "hw_test_cli.c"
    "hw_test_display.c"
    "hw_test_audio.c"
    "hw_test_lvgl.c"
    "hw_test_button.c"
    "hw_test_imu.c"
    "hw_test_mag.c"
    "hw_test_als.c"
    "hw_test_baro.c"
    "lua_tool.c"
)

if(CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT)
    list(APPEND _hw_test_srcs "hw_test_camera.c")
endif()

if(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
    list(APPEND _hw_test_srcs "hw_test_touch.c")
endif()

idf_component_register(
    SRCS
        ${_hw_test_srcs}
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        console
        esp_board_manager
        display_service
        driver
        esp_lcd
        esp_lcd_touch
        vfs
        fatfs
        esp_psram
        mbedtls
        claw_core
)
