# X-Card board-manager device drivers.
# This component provides the custom-device implementations consumed by
# esp_board_manager through the board entry registration path.

set(X_CARD_BMGR_DEVICE_DIRS
    x_card_sensors
    x_card_power
    x_card_ext
    x_card_sound
    esp_lcd_jd9853
)

set(X_CARD_BMGR_DEVICE_INCLUDE_DIRS
    x_card_sensors
    x_card_power
    x_card_ext
    x_card_sound
    esp_lcd_jd9853/include
)

idf_component_register(
    SRC_DIRS ${X_CARD_BMGR_DEVICE_DIRS}
    INCLUDE_DIRS ${X_CARD_BMGR_DEVICE_INCLUDE_DIRS}
    PRIV_REQUIRES esp_board_manager esp_lcd driver freertos esp_timer
)

# Keep the registration symbols from the custom-device implementations alive
# so board-manager can discover them through the linker-section entries.
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
