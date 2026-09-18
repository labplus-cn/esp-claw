/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython port configuration for ESP-Claw cap_mpy component.
 */
#pragma once

#include <stdint.h>
#include <alloca.h>

/* Use a basic feature set to keep binary size manageable */
#ifndef MICROPY_CONFIG_ROM_LEVEL
#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)
#endif

/* Object representation and NLR */
#define MICROPY_OBJ_REPR                    (MICROPY_OBJ_REPR_A)
#define MICROPY_NLR_SETJMP                  (1)

/* Memory allocation */
#define MICROPY_ALLOC_PATH_MAX              (128)
#ifndef MICROPY_GC_INITIAL_HEAP_SIZE
#define MICROPY_GC_INITIAL_HEAP_SIZE        (128 * 1024)
#endif

/* Native emitter for RISC-V */
#if CONFIG_IDF_TARGET_ARCH_RISCV
#if CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT
#define MICROPY_EMIT_RV32                   (0)
#else
#define MICROPY_EMIT_RV32                   (1)
#if CONFIG_IDF_TARGET_ESP32P4
#define MICROPY_EMIT_RV32_ZCMP              (1)
#endif
#endif
#else
#define MICROPY_EMIT_XTENSAWIN              (1)
#endif

/* Optimisations */
#ifndef MICROPY_OPT_COMPUTED_GOTO
#define MICROPY_OPT_COMPUTED_GOTO           (1)
#endif

/* Python internal features */
#define MICROPY_READER_VFS                  (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW      (1)
#define MICROPY_STACK_CHECK_MARGIN          (1024)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_WARNINGS                    (1)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_STREAMS_POSIX_API           (1)
#define MICROPY_USE_INTERNAL_ERRNO          (0)
#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_ENABLE_SCHEDULER            (1)
#define MICROPY_SCHEDULER_DEPTH             (8)
#define MICROPY_SCHEDULER_STATIC_NODES      (1)
#define MICROPY_VFS                         (1)
#define MICROPY_VFS_POSIX                   (1)
#define MICROPY_PY_OS_STATVFS               (0)
#define MICROPY_PERSISTENT_CODE_LOAD        (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_EVENT_DRIVEN           (1)
#define MICROPY_KBD_EXCEPTION               (1)

/* Use setjmp-based GC register capture (portable, no arch-specific asm) */
#define MICROPY_GCREGS_SETJMP               (1)

/* Disable features not needed for Phase 1 */
#define MICROPY_PY_BLUETOOTH                (0)
#define MICROPY_PY_NETWORK                  (0)
#define MICROPY_PY_SOCKET                   (0)
#define MICROPY_PY_LWIP                     (0)
#define MICROPY_PY_SSL                      (0)
#define MICROPY_PY_WEBSOCKET                (0)
#define MICROPY_PY_WEBREPL                  (0)
#define MICROPY_PY_FRAMEBUF                 (0)
#define MICROPY_PY_MACHINE                  (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE      "ports/esp32/modmachine.c"
#define MICROPY_PY_MACHINE_RESET            (1)
#define MICROPY_PY_MACHINE_BARE_METAL_FUNCS (1)
#define MICROPY_PY_MACHINE_DISABLE_IRQ_ENABLE_IRQ (0)
#define MICROPY_PY_MACHINE_PIN_MAKE_NEW     mp_pin_make_new
#define MICROPY_PY_MACHINE_BITSTREAM        (0)
#define MICROPY_PY_MACHINE_DHT_READINTO     (0)
#define MICROPY_PY_MACHINE_PULSE            (0)
#define MICROPY_PY_MACHINE_PWM              (0)
#define MICROPY_PY_MACHINE_I2C              (0)
#define MICROPY_PY_MACHINE_I2C_TRANSFER_WRITE1 (0)
#define MICROPY_PY_MACHINE_SOFTI2C          (0)
#define MICROPY_PY_MACHINE_SPI              (0)
#define MICROPY_PY_MACHINE_SOFTSPI          (0)
#define MICROPY_PY_MACHINE_DAC              (0)
#define MICROPY_PY_MACHINE_I2S              (0)
#define MICROPY_PY_MACHINE_UART             (0)
#define MICROPY_PY_MACHINE_WDT              (0)
#define MICROPY_PY_MACHINE_SDCARD           (0)
#define MICROPY_PY_MACHINE_ADC              (0)
#define MICROPY_PY_MACHINE_ADC_BLOCK        (0)
#define MICROPY_PY_MACHINE_MEMX             (0)
#define MICROPY_PY_MACHINE_SIGNAL           (0)
#define MICROPY_HW_RTC_USER_MEM_MAX         (0)
#define MICROPY_HW_ESP_NEW_I2C_DRIVER       (0)

/* Enable useful standard modules */
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_OS                       (1)
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_SELECT                   (0)

/* Board name */
#define MICROPY_HW_BOARD_NAME               "ESP-Claw"
#define MICROPY_HW_MCU_NAME                 "ESP32-P4"

/* Enable help text */
#define MICROPY_PY_BUILTINS_HELP            (1)

/* GC uses split heap for PSRAM support */
#define MICROPY_GC_SPLIT_HEAP               (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO          (1)

/* Map MP_STATE_PORT to MP_STATE_VM (same as official ESP32 port) */
#define MP_STATE_PORT                       MP_STATE_VM

/* Event poll hook — simplified version without threading/socket events */
#define MICROPY_EVENT_POLL_HOOK \
    do { \
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
    } while (0);

/* Required typedef for stream support */
typedef long mp_off_t;
