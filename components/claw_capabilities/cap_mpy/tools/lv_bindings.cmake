# LVGL MicroPython bindings generation (configure-time).
#
# Generates lv_mp.c and qstrdefsport.h at CMake configure time using
# execute_process(), so the files exist before any build step.
# This avoids "Cannot find source file" errors after fullclean.
#
# Must be included BEFORE py/mkrules.cmake so that LV_MP_OUTPUT
# can be added to MICROPY_SOURCE_QSTR.
#
# The caller must have defined: LVGL_SRC_DIR, LV_BINDINGS_DIR, MICROPY_INC_CORE,
# MICROPY_GENHDR_BUILD_DIR, CMAKE_BINARY_DIR.

if(NOT LVGL_SRC_DIR)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Get LVGL component compile definitions
set(LVGL_TARGET "")
foreach(_candidate __idf_lvgl __idf_lvgl__lvgl)
    if(TARGET ${_candidate})
        set(LVGL_TARGET ${_candidate})
        break()
    endif()
endforeach()

set(LVGL_COMPILE_DEFS "")
if(LVGL_TARGET)
    get_target_property(LVGL_COMPILE_DEFS ${LVGL_TARGET} INTERFACE_COMPILE_DEFINITIONS)
    if(NOT LVGL_COMPILE_DEFS)
        set(LVGL_COMPILE_DEFS "")
    endif()
endif()

# Build include flags for the C preprocessor
set(LV_PP_INCLUDES
    "-I${LVGL_SRC_DIR}"
    "-I${LVGL_SRC_DIR}/src"
    "-I${LVGL_SRC_DIR}/../"
)

# Build definition flags from LVGL compile definitions
set(LV_PP_DEFS "")
if(LVGL_COMPILE_DEFS)
    foreach(def IN LISTS LVGL_COMPILE_DEFS)
        if(NOT def MATCHES "^\\$<")
            list(APPEND LV_PP_DEFS "-D${def}")
        endif()
    endforeach()
endif()

# Also get global compile definitions (CONFIG_LV_* from sdkconfig)
idf_build_get_property(GLOBAL_COMPILE_DEFS COMPILE_DEFINITIONS)
if(GLOBAL_COMPILE_DEFS)
    foreach(def IN LISTS GLOBAL_COMPILE_DEFS)
        if(def MATCHES "^CONFIG_LV_")
            list(APPEND LV_PP_DEFS "-D${def}")
        endif()
    endforeach()
endif()

set(LV_MP_OUTPUT "${CMAKE_BINARY_DIR}/lv_mp.c")
set(LV_MP_PP "${LV_MP_OUTPUT}.pp")
set(LV_MP_PP_FILTERED "${LV_MP_PP}.filtered")
set(LV_MP_JSON "${LV_MP_OUTPUT}.json")

# Only regenerate when lvgl.h is newer than lv_mp.c (incremental).
set(_need_lv_gen TRUE)
if(EXISTS ${LV_MP_OUTPUT} AND EXISTS ${LV_MP_PP} AND EXISTS ${LV_MP_PP_FILTERED})
    file(TIMESTAMP ${LVGL_SRC_DIR}/lvgl.h _lv_ts)
    file(TIMESTAMP ${LV_MP_OUTPUT} _out_ts)
    if(_out_ts STREQUAL "" OR _lv_ts STRGREATER _out_ts)
        set(_need_lv_gen TRUE)
    else()
        set(_need_lv_gen FALSE)
    endif()
endif()

if(_need_lv_gen)
    message(STATUS "LV_BINDINGS: generating lv_mp.c (configure-time)")

    # Step 1: Preprocess lvgl.h
    execute_process(
        COMMAND ${CMAKE_C_COMPILER} -E -DPYCPARSER
            ${LV_PP_INCLUDES}
            ${LV_PP_DEFS}
            -I${LV_BINDINGS_DIR}/pycparser/utils/fake_libc_include
            -I${MICROPY_INC_CORE}
            -include ${CMAKE_BINARY_DIR}/config/sdkconfig.h
            ${LVGL_SRC_DIR}/lvgl.h
        OUTPUT_FILE ${LV_MP_PP}
        RESULT_VARIABLE _pp_result
    )
    if(NOT _pp_result EQUAL 0)
        message(WARNING "LV_BINDINGS: preprocessing lvgl.h failed (${_pp_result})")
    endif()

    # Step 2: Filter out internal headers (Python replaces awk for execute_process compatibility)
    set(LV_FILTER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/filter_lv_preprocessed.py)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${LV_FILTER_SCRIPT}
            ${LV_MP_PP} ${LV_MP_PP_FILTERED}
            lv_obj_style_internal.h lv_obj_style_internal_gen.h
        RESULT_VARIABLE _filter_result
    )
    if(NOT _filter_result EQUAL 0)
        message(WARNING "LV_BINDINGS: filtering preprocessed output failed (${_filter_result})")
    endif()

    # Step 3: Generate lv_mp.c
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${LV_BINDINGS_DIR}/gen/gen_mpy.py
            -M lvgl -MP lv
            -MD ${LV_MP_JSON}
            -E ${LV_MP_PP_FILTERED}
            ${LVGL_SRC_DIR}/lvgl.h
        OUTPUT_FILE ${LV_MP_OUTPUT}
        RESULT_VARIABLE _gen_result
    )
    if(NOT _gen_result EQUAL 0)
        message(WARNING "LV_BINDINGS: gen_mpy.py failed (${_gen_result})")
    endif()
endif()

# Step 4: Extract LVGL QSTR definitions from generated lv_mp.c.
# Always run — qstrdefsport.h may be missing after fullclean.
set(LVGL_QSTR_EXTRACT_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/extract_lvgl_qstrs.py)
set(_need_qstr_extract TRUE)
if(EXISTS ${MICROPY_GENHDR_BUILD_DIR}/qstrdefsport.h AND EXISTS ${LV_MP_OUTPUT})
    file(TIMESTAMP ${LV_MP_OUTPUT} _lv_ts)
    file(TIMESTAMP ${MICROPY_GENHDR_BUILD_DIR}/qstrdefsport.h _qstr_ts)
    if(_qstr_ts STRGREATER _lv_ts OR _qstr_ts STREQUAL _lv_ts)
        set(_need_qstr_extract FALSE)
    endif()
endif()

if(_need_qstr_extract AND EXISTS ${LV_MP_OUTPUT})
    message(STATUS "LV_BINDINGS: extracting LVGL QSTR definitions from lv_mp.c")
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${LVGL_QSTR_EXTRACT_SCRIPT}
            ${LV_MP_OUTPUT}
            ${MICROPY_GENHDR_BUILD_DIR}/qstrdefsport.h
        RESULT_VARIABLE _extract_result
    )
endif()
