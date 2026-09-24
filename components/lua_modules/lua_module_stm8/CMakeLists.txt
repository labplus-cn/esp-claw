idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "src"
    REQUIRES
        cap_lua
    PRIV_REQUIRES
        espressif__esp_board_manager
        espressif__i2c_bus
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
