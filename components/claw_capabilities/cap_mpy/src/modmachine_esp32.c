/*
 * Minimal machine module platform stub for cap_mpy.
 *
 * Provides stubs for symbols referenced by ports/esp32/modmachine.c
 * that we don't implement (machine_rtc, machine_timer, machine_touchpad).
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "esp_mac.h"

// ── FATFS stubs for functions not in ESP-IDF's fatfs ──────────────────
// MicroPython's vfs_fat.c uses f_getcwd/f_chdir which ESP-IDF's fatfs
// doesn't provide. Provide minimal stubs.
#include "ff.h"

FRESULT f_getcwd(char *buff, size_t len) {
    (void)buff;
    (void)len;
    return FR_OK;
}

FRESULT f_chdir(const char *path) {
    (void)path;
    return FR_OK;
}

#if MICROPY_PY_MACHINE

// ── Stubs for symbols referenced by the official port's modmachine.c ──
// These modules are not compiled in our build, but the module globals table
// in ports/esp32/modmachine.c references their types and config structs.

// machine_rtc stubs
#include "driver/rtc_io.h"
typedef struct { int dummy; } machine_rtc_config_t;
machine_rtc_config_t machine_rtc_config;
const mp_obj_type_t machine_rtc_type;

// machine_timer stubs
const mp_obj_type_t machine_timer_type;

// machine_touchpad stubs
const mp_obj_type_t machine_touchpad_type;

// ── Platform-specific machine functions ───────────────────────────────
// Note: mp_machine_reset, mp_machine_reset_cause, mp_machine_get_freq,
// mp_machine_set_freq, mp_machine_idle, mp_machine_unique_id,
// mp_machine_lightsleep, mp_machine_deepsleep are provided by
// ports/esp32/modmachine.c (included via MICROPY_PY_MACHINE_INCLUDEFILE).

#endif // MICROPY_PY_MACHINE
