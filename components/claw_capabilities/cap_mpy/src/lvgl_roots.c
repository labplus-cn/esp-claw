/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Root pointer registrations for LVGL MicroPython bindings.
 * These must be scanned by the QSTR/root-pointer generation pipeline
 * so that lv_mp.c can compile with proper mp_state_vm_t fields.
 */
#include "py/runtime.h"

/* LVGL binding root pointers (mirrors what gen_mpy.py emits in lv_mp.c) */
MP_REGISTER_ROOT_POINTER(void *mp_lv_roots);
MP_REGISTER_ROOT_POINTER(void *mp_lv_user_data);
MP_REGISTER_ROOT_POINTER(int mp_lv_roots_initialized);
MP_REGISTER_ROOT_POINTER(int lvgl_mod_initialized);
