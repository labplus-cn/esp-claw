set(_usb_otg_srcs
    "src/usb_otg_core.c"
)

set(_usb_otg_priv_requires
    usb
)

if(CONFIG_USB_OTG_MSC)
    list(APPEND _usb_otg_srcs
        "src/usb_otg_msc.c"
        "src/usb_otg_desc.c"
    )
    list(APPEND _usb_otg_priv_requires
        esp_board_manager
        espressif__tinyusb
        fatfs
        vfs
        driver
    )
endif()

if(CONFIG_USB_OTG_HOST_UVC)
    list(APPEND _usb_otg_srcs "src/usb_otg_host.c")
    list(APPEND _usb_otg_priv_requires esp_video)
endif()

idf_component_register(
    SRCS
        ${_usb_otg_srcs}
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        ${_usb_otg_priv_requires}
)

if(CONFIG_USB_OTG_MSC)
    # TinyUSB callbacks (tud_descriptor_*_cb) are referenced from the
    # espressif__tinyusb static library. Without whole-archive linking the
    # linker drops them as "unused".
    idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)

    # The espressif__tinyusb component is the raw TinyUSB library — it does not
    # generate tusb_config.h from Kconfig. Its own source files #include
    # "tusb.h", which in turn #includes "tusb_config.h". Copy the project
    # configuration only when the MSC device stack is enabled.
    idf_component_get_property(_tusb_dir espressif__tinyusb COMPONENT_DIR)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/include/tusb_config.h"
        "${_tusb_dir}/src/tusb_config.h"
        COPYONLY
    )
endif()
