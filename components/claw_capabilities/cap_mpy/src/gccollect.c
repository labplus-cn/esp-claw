/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython garbage collection support for ESP-Claw cap_mpy component.
 * Follows the official ESP32 port pattern for RISC-V.
 */
#include <stdio.h>

#include "py/mpconfig.h"
#include "py/mpstate.h"
#include "py/gc.h"
#include "shared/runtime/gchelper.h"

#if CONFIG_IDF_TARGET_ARCH_RISCV

void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

#elif CONFIG_IDF_TARGET_ARCH_XTENSA

#include "xtensa/hal.h"
#include "xtensa/config/core-isa.h"
#include "esp_cpu.h"

static void gc_collect_inner(volatile unsigned int level)
{
    if (level < XCHAL_NUM_AREGS / 8) {
        gc_collect_inner(level + 1);
    } else {
        volatile uint32_t sp = (uint32_t)esp_cpu_get_sp();
        gc_collect_root((void **)sp, ((mp_uint_t)MP_STATE_THREAD(stack_top) - sp) / sizeof(uint32_t));
    }
}

void gc_collect(void)
{
    gc_collect_start();
    gc_collect_inner(0);
    gc_collect_end();
}

#else
#error unknown CONFIG_IDF_TARGET_ARCH
#endif

#if MICROPY_GC_SPLIT_HEAP_AUTO
#include "esp_heap_caps.h"

size_t gc_get_max_new_split(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
}
#endif
