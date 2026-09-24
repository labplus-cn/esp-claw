idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "src"
    PRIV_REQUIRES
        cap_lua
        usb_otg
)

idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)
