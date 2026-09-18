# MicroPython interpreter as ESP-IDF component for ESP-Claw cap_mpy.
#
# Uses MicroPython's cmake infrastructure:
# - py/py.cmake → MICROPY_SOURCE_PY, MICROPY_INC_CORE, micropy_gather_target_properties()
# - py/mkrules.cmake → QSTR/moduledefs/root_pointer generation
#
# Port-layer and extmod sources are curated manually to avoid pulling in
# unneeded dependencies (network, bluetooth, USB, etc.).

# ── MicroPython source paths ──────────────────────────────────────────
set(MICROPY_DIR ${CMAKE_CURRENT_LIST_DIR}/../../micropython_source)
set(MICROPY_PY_DIR ${MICROPY_DIR}/py)
set(MICROPY_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
set(MICROPY_ESP32_PORT_DIR ${MICROPY_DIR}/ports/esp32)
set(MICROPY_EXTMOD_DIR ${MICROPY_DIR}/extmod)
set(MICROPY_OOFATFS_DIR ${MICROPY_DIR}/lib/oofatfs)

# ── Include MicroPython core cmake ────────────────────────────────────
include(${MICROPY_DIR}/py/py.cmake)

# ── Create usermod INTERFACE target (needed by mkrules.cmake) ────────
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT TARGET usermod)
        add_library(usermod INTERFACE)
    endif()
    if(NOT TARGET usermod_lvgl)
        add_library(usermod_lvgl INTERFACE)
    endif()
    target_link_libraries(usermod INTERFACE usermod_lvgl)
endif()
set(MICROPY_SOURCE_USERMOD "")
set(MICROPY_INC_USERMOD "")

# ── Shared runtime sources ───────────────────────────────────────────
list(APPEND MICROPY_SOURCE_SHARED
    ${MICROPY_DIR}/shared/readline/readline.c
    ${MICROPY_DIR}/shared/netutils/netutils.c
    ${MICROPY_DIR}/shared/timeutils/timeutils.c
    ${MICROPY_DIR}/shared/runtime/interrupt_char.c
    ${MICROPY_DIR}/shared/runtime/mpirq.c
    ${MICROPY_DIR}/shared/runtime/stdout_helpers.c
    ${MICROPY_DIR}/shared/runtime/sys_stdio_mphal.c
    ${MICROPY_DIR}/shared/runtime/pyexec.c
)

# ── GC helper ────────────────────────────────────────────────────────
list(APPEND MICROPY_SOURCE_GCHELPER
    ${MICROPY_DIR}/shared/runtime/gchelper_generic.c
)

# ── Shared libc helpers ──────────────────────────────────────────────
list(APPEND MICROPY_SOURCE_LIBC
    ${MICROPY_DIR}/shared/libc/abort_.c
    ${MICROPY_DIR}/shared/libc/printf.c
)

# ── FAT filesystem library ───────────────────────────────────────────
# Use ESP-IDF's fatfs component instead of MicroPython's oofatfs to avoid
# multiple definition conflicts. The ESP-IDF fatfs provides the same API.
# (ff.c and ffunicode.c are provided by the fatfs component via REQUIRES)

# ── Math library (single-precision libm) ─────────────────────────────
list(APPEND MICROPY_SOURCE_LIBM
    ${MICROPY_DIR}/lib/libm/acoshf.c
    ${MICROPY_DIR}/lib/libm/asinfacosf.c
    ${MICROPY_DIR}/lib/libm/asinhf.c
    ${MICROPY_DIR}/lib/libm/atan2f.c
    ${MICROPY_DIR}/lib/libm/atanf.c
    ${MICROPY_DIR}/lib/libm/atanhf.c
    ${MICROPY_DIR}/lib/libm/ef_rem_pio2.c
    ${MICROPY_DIR}/lib/libm/erf_lgamma.c
    ${MICROPY_DIR}/lib/libm/fmodf.c
    ${MICROPY_DIR}/lib/libm/kf_cos.c
    ${MICROPY_DIR}/lib/libm/kf_rem_pio2.c
    ${MICROPY_DIR}/lib/libm/kf_sin.c
    ${MICROPY_DIR}/lib/libm/kf_tan.c
    ${MICROPY_DIR}/lib/libm/log1pf.c
    ${MICROPY_DIR}/lib/libm/math.c
    ${MICROPY_DIR}/lib/libm/nearbyintf.c
    ${MICROPY_DIR}/lib/libm/roundf.c
    ${MICROPY_DIR}/lib/libm/sf_cos.c
    ${MICROPY_DIR}/lib/libm/sf_erf.c
    ${MICROPY_DIR}/lib/libm/sf_frexp.c
    ${MICROPY_DIR}/lib/libm/sf_ldexp.c
    ${MICROPY_DIR}/lib/libm/sf_modf.c
    ${MICROPY_DIR}/lib/libm/sf_sin.c
    ${MICROPY_DIR}/lib/libm/sf_tan.c
    ${MICROPY_DIR}/lib/libm/wf_lgamma.c
    ${MICROPY_DIR}/lib/libm/wf_tgamma.c
    ${MICROPY_DIR}/lib/libm/ef_sqrt.c
)

# ── Selected extmod sources ─────────────────────────────────────────
# Only include modules we need. Disabled modules (BT, network, etc.) are
# simply not listed here, avoiding unnecessary compilation and dependencies.
list(APPEND MICROPY_SOURCE_EXTMOD
    # VFS / filesystem
    ${MICROPY_EXTMOD_DIR}/vfs.c
    ${MICROPY_EXTMOD_DIR}/vfs_blockdev.c
    ${MICROPY_EXTMOD_DIR}/vfs_fat.c
    ${MICROPY_EXTMOD_DIR}/vfs_fat_diskio.c
    ${MICROPY_EXTMOD_DIR}/vfs_fat_file.c
    ${MICROPY_EXTMOD_DIR}/vfs_posix.c
    ${MICROPY_EXTMOD_DIR}/vfs_posix_file.c
    ${MICROPY_EXTMOD_DIR}/vfs_reader.c
    # Python standard modules
    ${MICROPY_EXTMOD_DIR}/modjson.c
    ${MICROPY_EXTMOD_DIR}/modos.c
    ${MICROPY_EXTMOD_DIR}/modtime.c
    ${MICROPY_EXTMOD_DIR}/modbinascii.c
    ${MICROPY_EXTMOD_DIR}/moductypes.c
    ${MICROPY_EXTMOD_DIR}/modhashlib.c
    ${MICROPY_EXTMOD_DIR}/modheapq.c
    ${MICROPY_EXTMOD_DIR}/modrandom.c
    ${MICROPY_EXTMOD_DIR}/modre.c
    ${MICROPY_EXTMOD_DIR}/modasyncio.c
    ${MICROPY_EXTMOD_DIR}/modonewire.c
    ${MICROPY_EXTMOD_DIR}/modvfs.c
    ${MICROPY_EXTMOD_DIR}/modplatform.c
    ${MICROPY_EXTMOD_DIR}/modmarshal.c
    ${MICROPY_EXTMOD_DIR}/modcryptolib.c
    ${MICROPY_EXTMOD_DIR}/moddeflate.c
    ${MICROPY_EXTMOD_DIR}/modselect.c
    # Machine module core
    ${MICROPY_EXTMOD_DIR}/modmachine.c
    ${MICROPY_EXTMOD_DIR}/machine_pinbase.c
    ${MICROPY_EXTMOD_DIR}/virtpin.c
)

# ── Port sources: our custom files ───────────────────────────────────
list(APPEND MICROPY_SOURCE_PORT
    ${CMAKE_CURRENT_LIST_DIR}/src/mphalport.c
    ${CMAKE_CURRENT_LIST_DIR}/src/gccollect.c
    ${CMAKE_CURRENT_LIST_DIR}/src/lvgl_roots.c
    ${CMAKE_CURRENT_LIST_DIR}/src/modmachine_esp32.c
)

# ── Port sources: selected official ESP32 port files ─────────────────
list(APPEND MICROPY_SOURCE_PORT
    ${MICROPY_ESP32_PORT_DIR}/machine_pin.c
)

# ── Component sources ────────────────────────────────────────────────
list(APPEND CAP_MPY_SOURCE
    ${CMAKE_CURRENT_LIST_DIR}/src/cap_mpy.c
    ${CMAKE_CURRENT_LIST_DIR}/src/cap_mpy_async.c
    ${CMAKE_CURRENT_LIST_DIR}/src/cmd_cap_mpy.c
)

# ── All sources combined ─────────────────────────────────────────────
set(ALL_MPY_SRCS
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_GCHELPER}
    ${MICROPY_SOURCE_LIBC}
    ${MICROPY_SOURCE_FATFS}
    ${MICROPY_SOURCE_LIBM}
    ${MICROPY_SOURCE_PORT}
    ${CAP_MPY_SOURCE}
)

# ── QSTR scan sources ───────────────────────────────────────────────
set(MICROPY_SOURCE_QSTR
    ${MICROPY_SOURCE_PY}
    ${MICROPY_SOURCE_EXTMOD}
    ${MICROPY_SOURCE_USERMOD}
    ${MICROPY_SOURCE_SHARED}
    ${MICROPY_SOURCE_PORT}
)

# ── LVGL binding paths ──────────────────────────────────────────────
set(LV_BINDINGS_DIR ${CMAKE_CURRENT_LIST_DIR}/usercmodule/lv_binding_micropython)
get_filename_component(_lvgl_candidate
    "${CMAKE_CURRENT_LIST_DIR}/../../../application/edge_agent/managed_components/lvgl__lvgl"
    ABSOLUTE)
if(EXISTS "${_lvgl_candidate}/lvgl.h")
    set(LVGL_SRC_DIR "${_lvgl_candidate}")
endif()
if(NOT LVGL_SRC_DIR)
    message(WARNING "LVGL source dir not found; LVGL bindings will not be generated")
else()
    message(STATUS "LVGL source dir: ${LVGL_SRC_DIR}")
endif()

# ── Build output directories ─────────────────────────────────────────
set(MICROPY_GENHDR_DIR ${CMAKE_BINARY_DIR}/cap_mpy)
file(MAKE_DIRECTORY ${MICROPY_GENHDR_DIR})

set(MICROPY_GENHDR_BUILD_DIR ${CMAKE_BINARY_DIR}/genhdr)
file(MAKE_DIRECTORY ${MICROPY_GENHDR_BUILD_DIR})

# Ensure generated source files exist for CMake's configure-step validation.
# After fullclean, build/ is empty and custom command outputs don't exist yet.
# lv_mp.c is generated at configure time by lv_bindings.cmake (execute_process).
# pins.c is still a build-time custom command, so we need a placeholder.
file(TOUCH ${CMAKE_BINARY_DIR}/pins.c)

# qstrdefsport.h generated by lv_bindings (extracts QSTRs from lv_mp.c)
set(MICROPY_QSTRDEFS_PORT ${MICROPY_GENHDR_BUILD_DIR}/qstrdefsport.h)
list(APPEND MICROPY_SOURCE_QSTR ${MICROPY_QSTRDEFS_PORT})

# ── Include directories ──────────────────────────────────────────────
set(CAP_MPY_INC_DIRS
    ${MICROPY_INC_CORE}
    ${MICROPY_INC_USERMOD}
    ${MICROPY_PORT_DIR}                    # our mpconfigport.h, mphalport.h
    ${MICROPY_ESP32_PORT_DIR}              # official port headers (modmachine.h, etc.)
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${MICROPY_GENHDR_DIR}
    ${MICROPY_GENHDR_BUILD_DIR}
    ${CMAKE_BINARY_DIR}
)

# ── Register as IDF component ────────────────────────────────────────
idf_component_register(
    SRCS ${ALL_MPY_SRCS}
    INCLUDE_DIRS ${CAP_MPY_INC_DIRS}
    REQUIRES
        driver
        esp_timer
        esp_system
        heap
        esp_psram
        esp_rom
        freertos
        hal
        log
        newlib
        soc
        vfs
        fatfs
        esp_driver_i2c
        esp_driver_gpio
        esp_board_manager
        console
        lvgl
)

# ── Set MICROPY_TARGET for mkrules.cmake ─────────────────────────────
set(MICROPY_TARGET ${COMPONENT_TARGET})

# ── Compile definitions (BEFORE mkrules.cmake so they appear in QSTR) ─
# ffconf.h is copied to genhdr build dir (in include path), so we define
# FFCONF_H as just "ffconf.h" — the preprocessor finds it via -I path.
# This avoids complex path escaping issues with -DFFCONF_H=\"full/path\".
file(COPY ${MICROPY_OOFATFS_DIR}/ffconf.h DESTINATION ${MICROPY_GENHDR_BUILD_DIR})

target_compile_definitions(${MICROPY_TARGET} PUBLIC
    FFCONF_H="ffconf.h"
    MICROPY_VFS=1
    MICROPY_VFS_FAT=1
    MICROPY_VFS_LFS2=0
    LFS1_NO_MALLOC LFS1_NO_DEBUG LFS1_NO_WARN LFS1_NO_ERROR LFS1_NO_ASSERT
    LFS2_NO_MALLOC LFS2_NO_DEBUG LFS2_NO_WARN LFS2_NO_ERROR LFS2_NO_ASSERT
    MICROPY_PY_BLUETOOTH=0
    MICROPY_PY_NETWORK=0
    MICROPY_PY_SOCKET=0
    MICROPY_PY_LWIP=0
    MICROPY_PY_SSL=0
    MICROPY_PY_FRAMEBUF=0
    MICROPY_PY_WEBSOCKET=0
    MICROPY_PY_WEBREPL=0
)

target_compile_options(${MICROPY_TARGET} PUBLIC
    -Wno-clobbered
    -Wno-deprecated-declarations
    -Wno-missing-field-initializers
    -Wno-old-style-declaration
    -Wno-unused-function
    -Wno-error=implicit-function-declaration
)

# ── Gather IDF component properties for QSTR preprocessing ───────────
# Replaces the extract_includes.py hack — gathers include paths from all
# resolved IDF components so the QSTR preprocessor can find ESP-IDF headers.
foreach(comp ${__COMPONENT_NAMES_RESOLVED})
    micropy_gather_target_properties(__idf_${comp})
    micropy_gather_target_properties(${comp})
endforeach()
list(APPEND MICROPY_CPP_DEF_EXTRA "ESP_PLATFORM")

# poll.h stub — ESP-IDF has sys/poll.h but not top-level poll.h.
# Create an empty stub so vfs_posix_file.c can include it during QSTR preprocessing.
file(WRITE ${MICROPY_GENHDR_BUILD_DIR}/poll.h "/* empty stub */")

# ── LVGL binding generation ─────────────────────────────────────────
# Included BEFORE mkrules.cmake so LV_MP_OUTPUT is in MICROPY_SOURCE_QSTR.
include(${CMAKE_CURRENT_LIST_DIR}/tools/lv_bindings.cmake)
if(LV_MP_OUTPUT)
    list(APPEND MICROPY_SOURCE_QSTR ${LV_MP_OUTPUT})
    list(APPEND MICROPY_CPP_DEF_EXTRA LV_CONF_SKIP)
endif()

# ── Pin generation variables (before mkrules.cmake for QSTR dependency) ─
set(GEN_PINS_PREFIX "${MICROPY_ESP32_PORT_DIR}/boards/pins_prefix.c")
set(GEN_PINS_MKPINS "${MICROPY_ESP32_PORT_DIR}/boards/make-pins.py")
set(GEN_PINS_SRC "${CMAKE_BINARY_DIR}/pins.c")
set(GEN_PINS_HDR "${MICROPY_GENHDR_BUILD_DIR}/pins.h")

# Add pins.h to QSTR sources so QSTR generation waits for pin generation.
list(APPEND MICROPY_SOURCE_QSTR ${GEN_PINS_HDR})

# ── Code generation rules (qstr, moduledefs, root_pointers) ─────────
include(${MICROPY_DIR}/py/mkrules.cmake)

# ── Pin generation custom command ───────────────────────────────────
target_sources(${MICROPY_TARGET} PRIVATE ${GEN_PINS_HDR})

add_custom_command(
    OUTPUT ${GEN_PINS_SRC} ${GEN_PINS_HDR}
    COMMAND ${Python3_EXECUTABLE} ${GEN_PINS_MKPINS}
        --prefix ${GEN_PINS_PREFIX}
        --output-source ${GEN_PINS_SRC}
        --output-header ${GEN_PINS_HDR}
    DEPENDS
        ${MICROPY_MPVERSION}
        ${GEN_PINS_MKPINS}
        ${GEN_PINS_PREFIX}
    VERBATIM
    COMMAND_EXPAND_LISTS
)

target_sources(${MICROPY_TARGET} PRIVATE ${GEN_PINS_SRC})

# ── Link usermod ─────────────────────────────────────────────────────
target_link_libraries(${MICROPY_TARGET} usermod)

# ── LVGL bindings: target operations ─────────────────────────────────
if(LVGL_SRC_DIR)
    target_sources(${COMPONENT_TARGET} PRIVATE ${LV_MP_OUTPUT})
    target_include_directories(${COMPONENT_TARGET} PRIVATE
        ${LV_BINDINGS_DIR}
        ${LV_BINDINGS_DIR}/include
    )
    target_sources(usermod_lvgl INTERFACE ${LV_MP_OUTPUT})
    target_include_directories(usermod_lvgl INTERFACE
        ${LV_BINDINGS_DIR}
        ${LV_BINDINGS_DIR}/include
    )
endif()
