idf_component_register()

# claw_video 2.4.1~1 decrements the open reference even when a removed USB
# camera was rejected before the reference was incremented. Patch the managed
# source at configure time until the registry component contains the fix.
set(CLAW_VIDEO_SOURCE_DIR "${CMAKE_SOURCE_DIR}/managed_components/tda-2030__claw_video")
set(CLAW_VIDEO_CORE_SOURCE "${CLAW_VIDEO_SOURCE_DIR}/src/esp_video.c")
set(CLAW_VIDEO_UVC_SOURCE "${CLAW_VIDEO_SOURCE_DIR}/src/device/esp_video_usb_uvc_device.c")

function(claw_video_replace_source FILE_PATH OLD_TEXT NEW_TEXT PATCH_NAME)
    if(NOT EXISTS "${FILE_PATH}")
        message(FATAL_ERROR "[esp_video] ${PATCH_NAME}: source file not found: ${FILE_PATH}")
    endif()

    file(READ "${FILE_PATH}" FILE_CONTENT)
    string(FIND "${FILE_CONTENT}" "${NEW_TEXT}" NEW_TEXT_OFFSET)
    if(NOT NEW_TEXT_OFFSET EQUAL -1)
        message(STATUS "[esp_video] ${PATCH_NAME}: already applied")
        return()
    endif()

    string(FIND "${FILE_CONTENT}" "${OLD_TEXT}" OLD_TEXT_OFFSET)
    if(OLD_TEXT_OFFSET EQUAL -1)
        message(FATAL_ERROR "[esp_video] ${PATCH_NAME}: source pattern not found in ${FILE_PATH}")
    endif()

    string(REPLACE "${OLD_TEXT}" "${NEW_TEXT}" FILE_CONTENT "${FILE_CONTENT}")
    file(WRITE "${FILE_PATH}" "${FILE_CONTENT}")
    message(STATUS "[esp_video] ${PATCH_NAME}: applied")
endfunction()

claw_video_replace_source(
    "${CLAW_VIDEO_CORE_SOURCE}"
    [=[    bool found = false;
    struct esp_video *video;]=]
    [=[    bool found = false;
    bool reference_added = false;
    struct esp_video *video;]=]
    "usb_uvc_open_reference_guard"
)

claw_video_replace_source(
    "${CLAW_VIDEO_CORE_SOURCE}"
    [=[    video->reference++;
    if (video->reference > 1) {]=]
    [=[    video->reference++;
    reference_added = true;
    if (video->reference > 1) {]=]
    "usb_uvc_open_reference_mark"
)

claw_video_replace_source(
    "${CLAW_VIDEO_CORE_SOURCE}"
    [=[    if (ret != ESP_OK) {
        video->reference--;
    } else {]=]
    [=[    if (ret != ESP_OK) {
        if (reference_added) {
            video->reference--;
        }
    } else {]=]
    "usb_uvc_open_reference_release"
)

claw_video_replace_source(
    "${CLAW_VIDEO_UVC_SOURCE}"
    [=[            esp_video_destroy(video);

            if (core->uvc_video[i].ready_sem) {]=]
    [=[            esp_err_t ret = esp_video_destroy(video);
            if (ret != ESP_OK) {
                return ret;
            }

            if (core->uvc_video[i].ready_sem) {]=]
    "usb_uvc_destroy_result"
)
