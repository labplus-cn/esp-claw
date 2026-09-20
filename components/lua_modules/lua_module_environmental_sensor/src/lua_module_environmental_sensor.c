/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_environmental_sensor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#include "bme69x.h"
#include "bme69x_defs.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#include "dht.h"
#include "driver/gpio.h"
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_LTR308ALS || CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_SPL06
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#endif
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_MODULE_ENVIRONMENTAL_SENSOR_NAME      "environmental_sensor"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT  "dht"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690 "bme690"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_LTR308ALS "ltr308als"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_SPL06 "spl06"

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#define LUA_MODULE_BME690_METATABLE        "environmental_sensor.device"
#define LUA_MODULE_BME690_DEFAULT_NAME     "environmental_sensor"
#define LUA_MODULE_BME690_LEGACY_NAME      "bme690_sensor"
#define LUA_MODULE_BME690_MAX_NAME_LEN     64
#define LUA_MODULE_BME690_DEFAULT_FREQ_HZ  400000
#define LUA_MODULE_BME690_DEFAULT_HEAT_C   300
#define LUA_MODULE_BME690_DEFAULT_HEAT_MS  100

typedef struct {
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    struct bme69x_dev sensor_handle;
    char peripheral_name[LUA_MODULE_BME690_MAX_NAME_LEN];
    bool peripheral_ref_held;
    bool sensor_initialized;
    uint8_t i2c_addr;
    uint16_t heatr_temp;
    uint16_t heatr_dur;
} lua_module_bme690_handle_t;

typedef struct {
    lua_module_bme690_handle_t *handle;
    char device_name[LUA_MODULE_BME690_MAX_NAME_LEN];
} lua_module_bme690_ud_t;

/*
 * Local mirror of the auto-generated `dev_custom_environmental_sensor_config_t`
 * struct (or whatever name the board manager emits for this device).
 *
 * IMPORTANT: This MUST be byte-for-byte identical to the auto-generated
 * struct. `lua_bme690_resolve_board_cfg()` cross-checks the size reported
 * by the board manager descriptor against `sizeof(lua_bme690_board_cfg_t)`
 * and refuses to use the config if they differ -- otherwise a YAML schema
 * mismatch would silently misinterpret bytes (e.g. read the wrong field
 * as `i2c_addr` or `frequency`).
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_bme690_board_cfg_t;

typedef struct {
    char peripheral_name[LUA_MODULE_BME690_MAX_NAME_LEN];
    int i2c_addr;
    int frequency;
    bool has_peripheral;
    bool has_i2c_addr;
    bool has_frequency;
    uint16_t heatr_temp;
    uint16_t heatr_dur;
} lua_bme690_resolved_cfg_t;

static const char *TAG = "lua_module_bme690";

static void lua_module_bme690_destroy_handle(lua_module_bme690_handle_t *handle);

static esp_err_t lua_module_bme690_open_i2c_bus(const char *peripheral_name,
                                                int frequency,
                                                i2c_bus_handle_t *i2c_bus_handle,
                                                bool *peripheral_ref_held)
{
    i2c_master_bus_handle_t i2c_master_handle = NULL;
    i2c_master_bus_config_t *i2c_master_cfg = NULL;

    *peripheral_ref_held = false;

    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(peripheral_name, (void **)&i2c_master_handle),
                        TAG, "Failed to reference board I2C bus '%s'", peripheral_name);
    *peripheral_ref_held = true;

    esp_err_t err = esp_board_periph_get_config(peripheral_name, (void **)&i2c_master_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get board I2C config '%s': %s", peripheral_name, esp_err_to_name(err));
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return err;
    }

    const i2c_config_t i2c_bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .scl_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .master.clk_speed = (uint32_t)frequency,
        .clk_flags = 0,
    };

    (void)i2c_master_handle;
    *i2c_bus_handle = i2c_bus_create(i2c_master_cfg->i2c_port, &i2c_bus_cfg);
    if (*i2c_bus_handle == NULL) {
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_select_addr(lua_module_bme690_handle_t *handle, uint8_t i2c_addr)
{
    if (handle->i2c_dev_handle != NULL && handle->i2c_addr == i2c_addr) {
        return ESP_OK;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
    }

    handle->i2c_dev_handle = i2c_bus_device_create(handle->i2c_bus_handle, i2c_addr, 0);
    if (handle->i2c_dev_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create environmental sensor I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }

    handle->i2c_addr = i2c_addr;
    return ESP_OK;
}

static esp_err_t lua_module_bme690_probe_chip(lua_module_bme690_handle_t *handle)
{
    uint8_t chip_id = 0;
    esp_err_t err = i2c_bus_read_bytes(handle->i2c_dev_handle, BME69X_REG_CHIP_ID, 1, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BME690 chip ID at 0x%02x: %s",
                 handle->i2c_addr, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Environmental sensor probe at 0x%02x -> chip_id=0x%02x",
             handle->i2c_addr, chip_id);
    if (chip_id != BME69X_CHIP_ID) {
        ESP_LOGE(TAG,
                 "Unexpected environmental sensor chip ID 0x%02x at 0x%02x, expected 0x%02x. "
                 "Check whether the BME690 sub-board is inserted.",
                 chip_id, handle->i2c_addr, BME69X_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_read(uint8_t reg_addr,
                                                       uint8_t *reg_data,
                                                       uint32_t len,
                                                       void *intf_ptr)
{
    lua_module_bme690_handle_t *handle = (lua_module_bme690_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BME69X_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_read_bytes(handle->i2c_dev_handle, reg_addr, (size_t)len, reg_data);
    return (err == ESP_OK) ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_write(uint8_t reg_addr,
                                                        const uint8_t *reg_data,
                                                        uint32_t len,
                                                        void *intf_ptr)
{
    lua_module_bme690_handle_t *handle = (lua_module_bme690_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BME69X_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_write_bytes(handle->i2c_dev_handle, reg_addr, (size_t)len, reg_data);
    return (err == ESP_OK) ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static void lua_module_bme690_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

static esp_err_t lua_module_bme690_apply_default_runtime_config(lua_module_bme690_handle_t *handle)
{
    struct bme69x_conf conf = {
        .filter = BME69X_FILTER_OFF,
        .odr = BME69X_ODR_NONE,
        .os_hum = BME69X_OS_16X,
        .os_pres = BME69X_OS_16X,
        .os_temp = BME69X_OS_16X,
    };
    struct bme69x_heatr_conf heatr_conf = {
        .enable = BME69X_ENABLE,
        .heatr_temp = handle->heatr_temp,
        .heatr_dur = handle->heatr_dur,
    };

    int8_t rslt = bme69x_set_conf(&conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Failed to configure BME690 oversampling: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Failed to configure BME690 heater: %d", rslt);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_read_sample(lua_module_bme690_handle_t *handle,
                                               struct bme69x_data *data)
{
    struct bme69x_conf conf = {
        .filter = BME69X_FILTER_OFF,
        .odr = BME69X_ODR_NONE,
        .os_hum = BME69X_OS_16X,
        .os_pres = BME69X_OS_16X,
        .os_temp = BME69X_OS_16X,
    };
    struct bme69x_heatr_conf heatr_conf = {
        .enable = BME69X_ENABLE,
        .heatr_temp = handle->heatr_temp,
        .heatr_dur = handle->heatr_dur,
    };
    uint8_t n_data = 0;

    int8_t rslt = bme69x_set_conf(&conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_heatr_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_op_mode failed: %d", rslt);
        return ESP_FAIL;
    }

    uint32_t delay_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &handle->sensor_handle) +
                        ((uint32_t)heatr_conf.heatr_dur * 1000U);
    handle->sensor_handle.delay_us(delay_us, handle->sensor_handle.intf_ptr);

    rslt = bme69x_get_data(BME69X_FORCED_MODE, data, &n_data, &handle->sensor_handle);
    if (rslt != BME69X_OK || n_data == 0) {
        ESP_LOGW(TAG, "bme69x_get_data failed or empty: rslt=%d n_data=%u", rslt, n_data);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_create_handle(const lua_bme690_resolved_cfg_t *cfg,
                                                 lua_module_bme690_handle_t **out_handle)
{
    lua_module_bme690_handle_t *handle = calloc(1, sizeof(lua_module_bme690_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->heatr_temp = cfg->heatr_temp;
    handle->heatr_dur = cfg->heatr_dur;

    esp_err_t err = lua_module_bme690_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                                                   &handle->i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_bme690_select_addr(handle, (uint8_t)cfg->i2c_addr);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err;
    }

    err = lua_module_bme690_probe_chip(handle);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    memset(&handle->sensor_handle, 0, sizeof(handle->sensor_handle));
    handle->sensor_handle.read = lua_module_bme690_i2c_read;
    handle->sensor_handle.write = lua_module_bme690_i2c_write;
    handle->sensor_handle.delay_us = lua_module_bme690_delay_us;
    handle->sensor_handle.intf = BME69X_I2C_INTF;
    handle->sensor_handle.intf_ptr = handle;

    int8_t rslt = BME69X_OK;
    rslt = bme69x_init(&handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_init failed: %d", rslt);
        lua_module_bme690_destroy_handle(handle);
        return (rslt == BME69X_E_DEV_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    err = lua_module_bme690_apply_default_runtime_config(handle);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err;
    }

    handle->sensor_initialized = true;
    *out_handle = handle;
    ESP_LOGI(TAG, "BME690 initialized on %s, addr 0x%02x, freq %d Hz",
             cfg->peripheral_name, cfg->i2c_addr, cfg->frequency);
    return ESP_OK;
}

static void lua_module_bme690_destroy_handle(lua_module_bme690_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
    }
    if (handle->peripheral_ref_held && handle->peripheral_name[0] != '\0') {
        esp_board_periph_unref_handle(handle->peripheral_name);
    }
    free(handle);
}

static lua_module_bme690_ud_t *lua_module_bme690_get_ud(lua_State *L, int idx)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_BME690_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "environmental_sensor: invalid or closed handle");
    }
    return ud;
}

static int lua_module_bme690_close_impl(lua_State *L, lua_module_bme690_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_module_bme690_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_module_bme690_gc(lua_State *L)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_testudata(L, 1, LUA_MODULE_BME690_METATABLE);
    if (ud && ud->handle) {
        return lua_module_bme690_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_bme690_close(lua_State *L)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_BME690_METATABLE);
    if (ud->handle) {
        return lua_module_bme690_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_bme690_name(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_module_bme690_chip_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, ud->handle->sensor_handle.chip_id);
    return 1;
}

static int lua_module_bme690_variant_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)ud->handle->sensor_handle.variant_id);
    return 1;
}

static int lua_module_bme690_read(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read failed");
    }

    lua_newtable(L);
    lua_pushnumber(L, data.temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, data.pressure);
    lua_setfield(L, -2, "pressure");
    lua_pushnumber(L, data.humidity);
    lua_setfield(L, -2, "humidity");
    lua_pushnumber(L, data.gas_resistance);
    lua_setfield(L, -2, "gas_resistance");
    lua_pushinteger(L, data.status);
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, data.gas_index);
    lua_setfield(L, -2, "gas_index");
    lua_pushinteger(L, data.meas_index);
    lua_setfield(L, -2, "meas_index");
    return 1;
}

static int lua_module_bme690_read_temperature(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_temperature failed");
    }

    lua_pushnumber(L, data.temperature);
    return 1;
}

static int lua_module_bme690_read_pressure(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_pressure failed");
    }

    lua_pushnumber(L, data.pressure);
    return 1;
}

static int lua_module_bme690_read_humidity(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_humidity failed");
    }

    lua_pushnumber(L, data.humidity);
    return 1;
}

static int lua_module_bme690_read_gas(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_gas failed");
    }

    lua_pushnumber(L, data.gas_resistance);
    return 1;
}

/*
 * Walk the board manager descriptor list and verify that the auto-generated
 * config for `device_name` has the layout `lua_bme690_board_cfg_t` expects.
 * Returns ESP_ERR_NOT_FOUND if the board doesn't declare the device, and
 * ESP_ERR_INVALID_SIZE if the schema diverges from this module's mirror.
 */
static esp_err_t lua_bme690_resolve_board_cfg(const char *device_name,
                                              const lua_bme690_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *desc = g_esp_board_devices;
    while (desc != NULL && desc->name != NULL) {
        if (strcmp(desc->name, device_name) == 0) {
            if (desc->cfg == NULL) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(lua_bme690_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u differs from expected %u; "
                         "board_devices.yaml schema is out of sync with lua_bme690_board_cfg_t. "
                         "Every field listed in lua_bme690_board_cfg_t MUST be present in YAML "
                         "(use -1 for unused GPIOs).",
                         device_name,
                         (unsigned)desc->cfg_size,
                         (unsigned)sizeof(lua_bme690_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const lua_bme690_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lua_module_bme690_load_board_defaults(const char *device_name,
                                                       lua_bme690_resolved_cfg_t *out)
{
    const lua_bme690_board_cfg_t *board = NULL;
    esp_err_t err = lua_bme690_resolve_board_cfg(device_name, &board);
    if (err != ESP_OK) {
        return err;
    }

    if (board->chip != NULL && strcmp(board->chip, LUA_MODULE_ENVIRONMENTAL_SENSOR_SELECTED_CHIP_NAME) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, LUA_MODULE_ENVIRONMENTAL_SENSOR_SELECTED_CHIP_NAME);
    }

    if (board->peripheral_name != NULL && board->peripheral_name[0] != '\0') {
        snprintf(out->peripheral_name, sizeof(out->peripheral_name), "%s", board->peripheral_name);
        out->has_peripheral = true;
    }
    if (board->i2c_addr != 0) {
        out->i2c_addr = board->i2c_addr;
        out->has_i2c_addr = true;
    }
    if (board->frequency > 0) {
        out->frequency = board->frequency;
        out->has_frequency = true;
    }

    return ESP_OK;
}

static void lua_module_bme690_apply_lua_overrides(lua_State *L, int opts_idx,
                                                  lua_bme690_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "peripheral");
    if (lua_isstring(L, -1)) {
        const char *p = lua_tostring(L, -1);
        snprintf(cfg->peripheral_name, sizeof(cfg->peripheral_name), "%s", p);
        cfg->has_peripheral = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "i2c_addr");
    if (lua_isnumber(L, -1)) {
        cfg->i2c_addr = (int)lua_tointeger(L, -1);
        cfg->has_i2c_addr = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "frequency");
    if (lua_isnumber(L, -1)) {
        cfg->frequency = (int)lua_tointeger(L, -1);
        cfg->has_frequency = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "heater_temp");
    if (lua_isnumber(L, -1)) {
        cfg->heatr_temp = (uint16_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "heater_duration");
    if (lua_isnumber(L, -1)) {
        cfg->heatr_dur = (uint16_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
}

static int lua_module_bme690_new(lua_State *L)
{
    const char *device_name = LUA_MODULE_BME690_DEFAULT_NAME;
    int opts_idx = 0;

    if (lua_isstring(L, 1)) {
        device_name = lua_tostring(L, 1);
        if (lua_istable(L, 2)) {
            opts_idx = 2;
        }
    } else if (lua_istable(L, 1)) {
        opts_idx = 1;
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) {
            device_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (strlen(device_name) >= LUA_MODULE_BME690_MAX_NAME_LEN) {
        return luaL_error(L, "environmental_sensor device name too long");
    }

    lua_bme690_resolved_cfg_t cfg = {
        .heatr_temp = LUA_MODULE_BME690_DEFAULT_HEAT_C,
        .heatr_dur = LUA_MODULE_BME690_DEFAULT_HEAT_MS,
    };

    esp_err_t err = lua_module_bme690_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L,
                          "environmental_sensor.new: board device '%s' config schema mismatch "
                          "(see error log above for details)", device_name);
    }
    if (err != ESP_OK && strcmp(device_name, LUA_MODULE_BME690_DEFAULT_NAME) == 0) {
        esp_err_t legacy_err = lua_module_bme690_load_board_defaults(LUA_MODULE_BME690_LEGACY_NAME, &cfg);
        if (legacy_err == ESP_OK) {
            opened_device_name = LUA_MODULE_BME690_LEGACY_NAME;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_MODULE_BME690_DEFAULT_NAME, LUA_MODULE_BME690_LEGACY_NAME);
        } else if (legacy_err == ESP_ERR_INVALID_SIZE) {
            return luaL_error(L,
                              "environmental_sensor.new: legacy board device '%s' config schema mismatch",
                              LUA_MODULE_BME690_LEGACY_NAME);
        }
    }

    lua_module_bme690_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "environmental_sensor.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
        cfg.i2c_addr = BME69X_I2C_ADDR_LOW;
        cfg.has_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = LUA_MODULE_BME690_DEFAULT_FREQ_HZ;
        cfg.has_frequency = true;
    }
    if (cfg.i2c_addr != BME69X_I2C_ADDR_LOW && cfg.i2c_addr != BME69X_I2C_ADDR_HIGH) {
        return luaL_error(L, "environmental_sensor.new: unsupported BME690 I2C address 0x%d (expected 0x76 or 0x77)",
                          cfg.i2c_addr);
    }

    lua_module_bme690_handle_t *handle = NULL;
    err = lua_module_bme690_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "environmental_sensor.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);

    luaL_getmetatable(L, LUA_MODULE_BME690_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}
#endif

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#define LUA_MODULE_DHT_METATABLE           "environmental_sensor.dht_device"
#define LUA_MODULE_DHT_DEFAULT_TYPE        DHT_TYPE_DHT11
#define LUA_MODULE_DHT_PRE_READ_DELAY_US   (200 * 1000)

typedef struct {
    gpio_num_t pin;
    dht_sensor_type_t sensor_type;
    bool closed;
} lua_module_dht_ud_t;

static dht_sensor_type_t lua_module_dht_sensor_type_from_string(const char *sensor_type_str)
{
    if (!sensor_type_str || strcmp(sensor_type_str, "dht11") == 0) {
        return DHT_TYPE_DHT11;
    }
    if (strcmp(sensor_type_str, "dht22") == 0 ||
        strcmp(sensor_type_str, "am2301") == 0 ||
        strcmp(sensor_type_str, "am2302") == 0 ||
        strcmp(sensor_type_str, "am2321") == 0 ||
        strcmp(sensor_type_str, "dht21") == 0) {
        return DHT_TYPE_AM2301;
    }
    if (strcmp(sensor_type_str, "si7021") == 0) {
        return DHT_TYPE_SI7021;
    }
    return (dht_sensor_type_t)-1;
}

static lua_module_dht_ud_t *lua_module_dht_get_ud(lua_State *L, int idx)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_DHT_METATABLE);
    if (ud == NULL || ud->closed) {
        luaL_error(L, "environmental_sensor: invalid or closed dht handle");
    }
    return ud;
}

static esp_err_t lua_module_dht_read_float(dht_sensor_type_t sensor_type, gpio_num_t pin,
                                           float *temperature, float *humidity)
{
    esp_rom_delay_us(LUA_MODULE_DHT_PRE_READ_DELAY_US);
    return dht_read_float_data(sensor_type, pin, humidity, temperature);
}

static int lua_module_dht_close(lua_State *L)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_DHT_METATABLE);
    ud->closed = true;
    return 0;
}

static int lua_module_dht_gc(lua_State *L)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_testudata(L, 1, LUA_MODULE_DHT_METATABLE);
    if (ud != NULL) {
        ud->closed = true;
    }
    return 0;
}

static int lua_module_dht_name(lua_State *L)
{
    lua_module_dht_get_ud(L, 1);
    lua_pushstring(L, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT);
    return 1;
}

static int lua_module_dht_read(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushnumber(L, temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, humidity);
    lua_setfield(L, -2, "humidity");
    return 1;
}

static int lua_module_dht_read_temperature(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_temperature failed: %s", esp_err_to_name(err));
    }

    lua_pushnumber(L, temperature);
    return 1;
}

static int lua_module_dht_read_humidity(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_humidity failed: %s", esp_err_to_name(err));
    }

    lua_pushnumber(L, humidity);
    return 1;
}

static int lua_module_dht_read_raw_method(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    int16_t humidity = 0;
    int16_t temperature = 0;

    esp_rom_delay_us(LUA_MODULE_DHT_PRE_READ_DELAY_US);
    esp_err_t err = dht_read_data(ud->sensor_type, ud->pin, &humidity, &temperature);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_raw failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, temperature);
    lua_pushinteger(L, humidity);
    return 2;
}

static int lua_module_dht_new(lua_State *L)
{
    int opts_idx = 0;

    if (lua_istable(L, 1)) {
        opts_idx = 1;
    } else if (lua_istable(L, 2)) {
        opts_idx = 2;
    }

    if (opts_idx == 0) {
        return luaL_error(L, "environmental_sensor.new({ type = \"dht\", pin = <gpio> ... }) expects an options table");
    }

    lua_getfield(L, opts_idx, "pin");
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "sensor_type");
    const char *sensor_type_str = lua_isnoneornil(L, -1) ? NULL : luaL_checkstring(L, -1);
    dht_sensor_type_t sensor_type = lua_module_dht_sensor_type_from_string(sensor_type_str);
    lua_pop(L, 1);

    if ((int)sensor_type < 0) {
        return luaL_error(L, "environmental_sensor.new: invalid dht sensor_type");
    }

    lua_module_dht_ud_t *ud = (lua_module_dht_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->pin = pin;
    ud->sensor_type = sensor_type;

    luaL_getmetatable(L, LUA_MODULE_DHT_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_module_dht_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_DHT_METATABLE)) {
        lua_pushcfunction(L, lua_module_dht_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_dht_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_dht_read_raw_method);
        lua_setfield(L, -2, "read_raw");
        lua_pushcfunction(L, lua_module_dht_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_dht_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, lua_module_dht_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_dht_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif

/* ===================================================================
 *  LTR-308ALS — Ambient Light Sensor backend
 * =================================================================== */
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_LTR308ALS

#define LTR308ALS_METATABLE        "environmental_sensor.ltr308als_device"
#define LTR308ALS_DEFAULT_NAME     "ltr308als"
#define LTR308ALS_MAX_NAME_LEN     64
#define LTR308ALS_DEFAULT_FREQ_HZ  400000
#define LTR308ALS_I2C_ADDR         0x53  /* 7-bit address (ADDR pin = VCC) */

/* LTR-308ALS registers */
#define LTR_REG_CTRL               0x00
#define LTR_REG_MEAS_RATE          0x04
#define LTR_REG_GAIN               0x05
#define LTR_REG_PART_ID            0x06
#define LTR_REG_STATUS             0x07
#define LTR_REG_ALS_DATA0          0x0D
#define LTR_CTRL_ACTIVE            0x02
#define LTR_CTRL_RESET             0x10
#define LTR_MEAS_100MS             0x22
#define LTR_STATUS_DRDY            0x08
#define LTR_PART_ID_VAL            0xB1

typedef struct {
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    char peripheral_name[LTR308ALS_MAX_NAME_LEN];
    bool peripheral_ref_held;
    bool sensor_initialized;
    uint8_t i2c_addr;
    uint8_t gain_idx;
} lua_module_ltr308als_handle_t;

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_ltr308als_board_cfg_t;

typedef struct {
    lua_module_ltr308als_handle_t *handle;
    char device_name[LTR308ALS_MAX_NAME_LEN];
} lua_module_ltr308als_ud_t;

static const float ltr_gains[] = { 1.0f, 3.0f, 6.0f, 9.0f, 18.0f };
static const char *TAG_LTR = "lua_module_ltr308als";

static esp_err_t lua_ltr308als_open_bus(const char *peripheral_name, int frequency,
                                         i2c_bus_handle_t *bus, bool *ref_held)
{
    i2c_master_bus_handle_t master = NULL;
    i2c_master_bus_config_t *cfg = NULL;
    *ref_held = false;
    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(peripheral_name, (void **)&master),
                        TAG_LTR, "ref bus '%s'", peripheral_name);
    *ref_held = true;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_config(peripheral_name, (void **)&cfg),
                        TAG_LTR, "get bus cfg '%s'", peripheral_name);
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda_io_num,
        .scl_io_num = cfg->scl_io_num,
        .sda_pullup_en = cfg->flags.enable_internal_pullup,
        .scl_pullup_en = cfg->flags.enable_internal_pullup,
        .master.clk_speed = (uint32_t)frequency,
        .clk_flags = 0,
    };
    *bus = i2c_bus_create(cfg->i2c_port, &i2c_cfg);
    if (!*bus) { esp_board_periph_unref_handle(peripheral_name); *ref_held = false; return ESP_FAIL; }
    return ESP_OK;
}

static esp_err_t lua_ltr308als_probe(lua_module_ltr308als_handle_t *hdl)
{
    uint8_t pid = 0;
    esp_err_t err = i2c_bus_read_bytes(hdl->i2c_dev_handle, LTR_REG_PART_ID, 1, &pid);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG_LTR, "LTR-308ALS Part ID = 0x%02X (expect 0x%02X)", pid, LTR_PART_ID_VAL);
    /* Reset */
    ESP_RETURN_ON_ERROR(i2c_bus_write_bytes(hdl->i2c_dev_handle, LTR_REG_CTRL, 1, &(uint8_t){LTR_CTRL_RESET}),
                        TAG_LTR, "LTR reset");
    vTaskDelay(pdMS_TO_TICKS(100));
    /* Configure: gain=1 (3x), meas rate=100ms */
    hdl->gain_idx = 1;
    ESP_RETURN_ON_ERROR(i2c_bus_write_bytes(hdl->i2c_dev_handle, LTR_REG_GAIN, 1, &hdl->gain_idx),
                        TAG_LTR, "LTR gain");
    ESP_RETURN_ON_ERROR(i2c_bus_write_bytes(hdl->i2c_dev_handle, LTR_REG_MEAS_RATE, 1, &(uint8_t){LTR_MEAS_100MS}),
                        TAG_LTR, "LTR meas rate");
    ESP_RETURN_ON_ERROR(i2c_bus_write_bytes(hdl->i2c_dev_handle, LTR_REG_CTRL, 1, &(uint8_t){LTR_CTRL_ACTIVE}),
                        TAG_LTR, "LTR active");
    vTaskDelay(pdMS_TO_TICKS(10));
    hdl->sensor_initialized = true;
    return ESP_OK;
}

static void lua_ltr308als_destroy(lua_module_ltr308als_handle_t *hdl)
{
    if (!hdl) return;
    if (hdl->i2c_dev_handle) { i2c_bus_device_delete(&hdl->i2c_dev_handle); }
    if (hdl->peripheral_ref_held && hdl->peripheral_name[0]) {
        esp_board_periph_unref_handle(hdl->peripheral_name);
    }
    free(hdl);
}

static lua_module_ltr308als_ud_t *lua_ltr308als_get_ud(lua_State *L, int idx)
{
    lua_module_ltr308als_ud_t *ud =
        (lua_module_ltr308als_ud_t *)luaL_checkudata(L, idx, LTR308ALS_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "environmental_sensor: invalid ltr308als handle");
    }
    return ud;
}

static int lua_ltr308als_read(lua_State *L)
{
    lua_module_ltr308als_ud_t *ud = lua_ltr308als_get_ud(L, 1);
    lua_module_ltr308als_handle_t *hdl = ud->handle;
    uint8_t status = 0;
    if (i2c_bus_read_bytes(hdl->i2c_dev_handle, LTR_REG_STATUS, 1, &status) != ESP_OK) {
        return luaL_error(L, "ltr308als status read failed");
    }
    if (!(status & LTR_STATUS_DRDY)) {
        lua_newtable(L);
        lua_pushnumber(L, 0);
        lua_setfield(L, -2, "lux");
        return 1;
    }
    uint8_t raw[3] = { 0 };
    if (i2c_bus_read_bytes(hdl->i2c_dev_handle, LTR_REG_ALS_DATA0, 3, raw) != ESP_OK) {
        return luaL_error(L, "ltr308als data read failed");
    }
    uint32_t als_raw = raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16);
    float lux = als_raw * 0.6f / ltr_gains[hdl->gain_idx];
    lua_newtable(L);
    lua_pushnumber(L, lux);
    lua_setfield(L, -2, "lux");
    return 1;
}

static int lua_ltr308als_read_lux(lua_State *L)
{
    lua_module_ltr308als_ud_t *ud = lua_ltr308als_get_ud(L, 1);
    lua_module_ltr308als_handle_t *hdl = ud->handle;
    uint8_t status = 0;
    i2c_bus_read_bytes(hdl->i2c_dev_handle, LTR_REG_STATUS, 1, &status);
    if (!(status & LTR_STATUS_DRDY)) { lua_pushnumber(L, 0); return 1; }
    uint8_t raw[3] = { 0 };
    if (i2c_bus_read_bytes(hdl->i2c_dev_handle, LTR_REG_ALS_DATA0, 3, raw) != ESP_OK) {
        return luaL_error(L, "ltr308als read_lux failed");
    }
    uint32_t als_raw = raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16);
    lua_pushnumber(L, als_raw * 0.6f / ltr_gains[hdl->gain_idx]);
    return 1;
}

static int lua_ltr308als_name(lua_State *L) { lua_ltr308als_get_ud(L, 1); lua_pushstring(L, "ltr308als"); return 1; }
static int lua_ltr308als_close(lua_State *L)
{
    lua_module_ltr308als_ud_t *ud = lua_ltr308als_get_ud(L, 1);
    if (ud->handle) { lua_ltr308als_destroy(ud->handle); ud->handle = NULL; }
    return 0;
}
static int lua_ltr308als_gc(lua_State *L)
{
    lua_module_ltr308als_ud_t *ud = (lua_module_ltr308als_ud_t *)luaL_testudata(L, 1, LTR308ALS_METATABLE);
    if (ud && ud->handle) { lua_ltr308als_destroy(ud->handle); ud->handle = NULL; }
    return 0;
}

static esp_err_t lua_ltr308als_resolve_board_cfg(const char *device_name, lua_ltr308als_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *d = g_esp_board_devices;
    while (d && d->name) {
        if (strcmp(d->name, device_name) == 0) {
            if (!d->cfg) return ESP_ERR_NOT_FOUND;
            if (d->cfg_size != sizeof(lua_ltr308als_board_cfg_t)) return ESP_ERR_INVALID_SIZE;
            *out = (lua_ltr308als_board_cfg_t *)d->cfg;
            return ESP_OK;
        }
        d = d->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static int lua_ltr308als_new(lua_State *L)
{
    const char *dev_name = LTR308ALS_DEFAULT_NAME;
    if (lua_isstring(L, 1)) dev_name = lua_tostring(L, 1);
    lua_ltr308als_board_cfg_t *board = NULL;
    esp_err_t err = lua_ltr308als_resolve_board_cfg(dev_name, &board);
    if (err == ESP_ERR_INVALID_SIZE) return luaL_error(L, "ltr308als cfg_size mismatch for '%s'", dev_name);
    const char *periph = NULL;
    int i2c_addr = LTR308ALS_I2C_ADDR;
    int freq = LTR308ALS_DEFAULT_FREQ_HZ;
    if (board) {
        if (board->peripheral_name && board->peripheral_name[0]) periph = board->peripheral_name;
        if (board->i2c_addr) i2c_addr = board->i2c_addr;
        if (board->frequency > 0) freq = board->frequency;
    }
    /* Lua overrides */
    int opts_idx = lua_istable(L, 2) ? 2 : (lua_istable(L, 1) ? 1 : 0);
    if (opts_idx) {
        lua_getfield(L, opts_idx, "peripheral");
        if (lua_isstring(L, -1)) periph = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, opts_idx, "i2c_addr");
        if (lua_isnumber(L, -1)) i2c_addr = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (!periph) return luaL_error(L, "ltr308als.new: missing 'peripheral'");
    lua_module_ltr308als_handle_t *hdl = calloc(1, sizeof(*hdl));
    if (!hdl) return luaL_error(L, "ltr308als: OOM");
    snprintf(hdl->peripheral_name, sizeof(hdl->peripheral_name), "%s", periph);
    err = lua_ltr308als_open_bus(periph, freq, &hdl->i2c_bus_handle, &hdl->peripheral_ref_held);
    if (err != ESP_OK) { free(hdl); return luaL_error(L, "ltr308als: bus open failed"); }
    hdl->i2c_dev_handle = i2c_bus_device_create(hdl->i2c_bus_handle, (uint8_t)i2c_addr, 0);
    if (!hdl->i2c_dev_handle) { lua_ltr308als_destroy(hdl); return luaL_error(L, "ltr308als: dev create failed"); }
    hdl->i2c_addr = (uint8_t)i2c_addr;
    err = lua_ltr308als_probe(hdl);
    if (err != ESP_OK) { lua_ltr308als_destroy(hdl); return luaL_error(L, "ltr308als: probe failed"); }
    lua_module_ltr308als_ud_t *ud = (lua_module_ltr308als_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = hdl;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", dev_name);
    luaL_getmetatable(L, LTR308ALS_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_ltr308als_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LTR308ALS_METATABLE)) {
        lua_pushcfunction(L, lua_ltr308als_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_ltr308als_read); lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_ltr308als_read_lux); lua_setfield(L, -2, "read_lux");
        lua_pushcfunction(L, lua_ltr308als_name); lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_ltr308als_close); lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif /* CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_LTR308ALS */

/* ===================================================================
 *  SPL06-001 — Barometric Pressure Sensor backend
 * =================================================================== */
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_SPL06

#define SPL06_METATABLE        "environmental_sensor.spl06_device"
#define SPL06_DEFAULT_NAME     "spl06"
#define SPL06_MAX_NAME_LEN     64
#define SPL06_DEFAULT_FREQ_HZ  400000
#define SPL06_I2C_ADDR_LOW     0x76
#define SPL06_I2C_ADDR_HIGH    0x77

/* SPL06-001 registers */
#define SPL06_REG_PSR_B2       0x00  /* Pressure MSB */
#define SPL06_REG_PSR_B1       0x01
#define SPL06_REG_PSR_B0       0x02  /* Pressure LSB */
#define SPL06_REG_TMP_B2       0x03  /* Temperature MSB */
#define SPL06_REG_TMP_B1       0x04
#define SPL06_REG_TMP_B0       0x05  /* Temperature LSB */
#define SPL06_REG_PRS_CFG      0x06
#define SPL06_REG_TMP_CFG      0x07
#define SPL06_REG_MEAS_CFG     0x08
#define SPL06_REG_CFG_REG      0x09
#define SPL06_REG_INT_STS      0x0A
#define SPL06_REG_FIFO_STS     0x0B
#define SPL06_REG_RESET        0x0C
#define SPL06_REG_ID           0x0D
#define SPL06_REG_COEF_BASE    0x10  /* Calibration coefficients start */
#define SPL06_REG_COEF_SR      0x28  /* Source register for temp (c00/c10 sign) */

#define SPL06_CHIP_ID          0x10  /* SPL06-001 may report 0x10 or 0x11 */
#define SPL06_CHIP_ID_ALT      0x11
#define SPL06_RESET_VAL        0x89

/* Scale factor for 8x oversampling (datasheet Table: scaling factors) */
#define SPL06_SCALE_8X         7864320.0f

/* MEAS_CFG(0x08) status bits */
#define SPL06_MEAS_COEF_RDY    0x80  /* bit7: calibration coefficients ready */
#define SPL06_MEAS_SENSOR_RDY  0x40  /* bit6: sensor initialization complete */

/*
 * Config values per datasheet: PM_PRC/TMP_PRC = 0011 -> 8x oversampling,
 * which requires NO P_SHIFT/T_SHIFT. TMP_EXT(bit7)=1 (MEMS on-chip temp sensor).
 * PM_PRC=0101 would be 32x (needs shift + scale 516096) — intentionally avoided.
 */
#define SPL06_PRS_CFG_8X       0x33  /* PM_RATE=011(8/s) | PM_PRC=0011(8x) */
#define SPL06_TMP_CFG_8X       0xB3  /* TMP_EXT=1 | TMP_RATE=011(8/s) | TMP_PRC=011(8x) */
#define SPL06_CFG_REG_NOSHIFT  0x00
#define SPL06_MEAS_CTRL_BG_PT  0x07  /* continuous pressure + temperature */

typedef struct {
    int16_t c0, c1;
    int32_t c00, c10;
    int16_t c01, c11, c20, c21, c30;
} spl06_calib_t;

typedef struct {
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    char peripheral_name[SPL06_MAX_NAME_LEN];
    bool peripheral_ref_held;
    bool sensor_initialized;
    uint8_t i2c_addr;
    spl06_calib_t calib;
} lua_module_spl06_handle_t;

typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_spl06_board_cfg_t;

typedef struct {
    lua_module_spl06_handle_t *handle;
    char device_name[SPL06_MAX_NAME_LEN];
} lua_module_spl06_ud_t;

static const char *TAG_SPL06 = "lua_module_spl06";

static esp_err_t spl06_write(lua_module_spl06_handle_t *hdl, uint8_t reg, uint8_t val)
{
    return i2c_bus_write_bytes(hdl->i2c_dev_handle, reg, 1, &val);
}

static esp_err_t spl06_read(lua_module_spl06_handle_t *hdl, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_bus_read_bytes(hdl->i2c_dev_handle, reg, len, buf);
}

static esp_err_t spl06_read_calib(lua_module_spl06_handle_t *hdl)
{
    /* Coefficients occupy registers 0x10..0x21 = 18 bytes */
    uint8_t coef[18] = { 0 };
    ESP_RETURN_ON_ERROR(spl06_read(hdl, SPL06_REG_COEF_BASE, coef, 18), TAG_SPL06, "read coef");
    spl06_calib_t *c = &hdl->calib;
    /* c0: 12-bit signed */
    c->c0 = (int16_t)(((uint16_t)coef[0] << 4) | ((coef[1] >> 4) & 0x0F));
    if (c->c0 & 0x800) c->c0 |= 0xF000;
    /* c1: 12-bit signed */
    c->c1 = (int16_t)(((uint16_t)(coef[1] & 0x0F) << 8) | coef[2]);
    if (c->c1 & 0x800) c->c1 |= 0xF000;
    /* c00: 20-bit signed (coef[3] << 12 | coef[4] << 4 | coef[5] >> 4) */
    c->c00 = (int32_t)(((uint32_t)coef[3] << 12) | ((uint32_t)coef[4] << 4) | ((coef[5] >> 4) & 0x0F));
    if (c->c00 & 0x80000) c->c00 |= 0xFFF00000;
    /* c10: 20-bit signed (coef[5] low 4 bits << 16 | coef[6] << 8 | coef[7]) */
    c->c10 = (int32_t)(((uint32_t)(coef[5] & 0x0F) << 16) | ((uint32_t)coef[6] << 8) | coef[7]);
    if (c->c10 & 0x80000) c->c10 |= 0xFFF00000;
    /* c01..c30: 16-bit signed */
    c->c01 = (int16_t)(((uint16_t)coef[8] << 8) | coef[9]);
    c->c11 = (int16_t)(((uint16_t)coef[10] << 8) | coef[11]);
    c->c20 = (int16_t)(((uint16_t)coef[12] << 8) | coef[13]);
    c->c21 = (int16_t)(((uint16_t)coef[14] << 8) | coef[15]);
    c->c30 = (int16_t)(((uint16_t)coef[16] << 8) | coef[17]);
    return ESP_OK;
}

static esp_err_t spl06_open_bus(const char *periph, int freq, i2c_bus_handle_t *bus, bool *ref)
{
    i2c_master_bus_handle_t master = NULL;
    i2c_master_bus_config_t *cfg = NULL;
    *ref = false;
    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(periph, (void **)&master), TAG_SPL06, "ref");
    *ref = true;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_config(periph, (void **)&cfg), TAG_SPL06, "cfg");
    const i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER, .sda_io_num = cfg->sda_io_num, .scl_io_num = cfg->scl_io_num,
        .sda_pullup_en = cfg->flags.enable_internal_pullup, .scl_pullup_en = cfg->flags.enable_internal_pullup,
        .master.clk_speed = (uint32_t)freq, .clk_flags = 0,
    };
    *bus = i2c_bus_create(cfg->i2c_port, &i2c_cfg);
    if (!*bus) { esp_board_periph_unref_handle(periph); *ref = false; return ESP_FAIL; }
    return ESP_OK;
}

static void spl06_destroy(lua_module_spl06_handle_t *hdl)
{
    if (!hdl) return;
    if (hdl->i2c_dev_handle) i2c_bus_device_delete(&hdl->i2c_dev_handle);
    if (hdl->peripheral_ref_held && hdl->peripheral_name[0]) esp_board_periph_unref_handle(hdl->peripheral_name);
    free(hdl);
}

static esp_err_t spl06_probe(lua_module_spl06_handle_t *hdl)
{
    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(spl06_read(hdl, SPL06_REG_ID, &chip_id, 1), TAG_SPL06, "ID read");
    if (chip_id != SPL06_CHIP_ID && chip_id != SPL06_CHIP_ID_ALT) {
        ESP_LOGE(TAG_SPL06, "SPL06 chip_id=0x%02x (expect 0x%02x or 0x%02x)", chip_id, SPL06_CHIP_ID, SPL06_CHIP_ID_ALT);
        return ESP_ERR_NOT_FOUND;
    }
    /* Configure: 8x oversampling for pressure & temperature (no shift needed) */
    ESP_RETURN_ON_ERROR(spl06_write(hdl, SPL06_REG_PRS_CFG, SPL06_PRS_CFG_8X), TAG_SPL06, "PRS_CFG");
    /* TMP_EXT(bit7)=1 selects the MEMS on-chip temperature sensor (datasheet) */
    ESP_RETURN_ON_ERROR(spl06_write(hdl, SPL06_REG_TMP_CFG, SPL06_TMP_CFG_8X), TAG_SPL06, "TMP_CFG");
    ESP_RETURN_ON_ERROR(spl06_write(hdl, SPL06_REG_CFG_REG, SPL06_CFG_REG_NOSHIFT), TAG_SPL06, "CFG_REG");

    /* Wait for SENSOR_RDY and COEF_RDY before reading calibration coefficients */
    for (int i = 0; i < 100; i++) {
        uint8_t meas = 0;
        if (spl06_read(hdl, SPL06_REG_MEAS_CFG, &meas, 1) == ESP_OK &&
            (meas & (SPL06_MEAS_SENSOR_RDY | SPL06_MEAS_COEF_RDY)) ==
                (SPL06_MEAS_SENSOR_RDY | SPL06_MEAS_COEF_RDY)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    /* Read calibration coefficients */
    ESP_RETURN_ON_ERROR(spl06_read_calib(hdl), TAG_SPL06, "calib");

    /* Start continuous background measurement (pressure + temperature) */
    ESP_RETURN_ON_ERROR(spl06_write(hdl, SPL06_REG_MEAS_CFG, SPL06_MEAS_CTRL_BG_PT), TAG_SPL06, "MEAS_CFG");
    vTaskDelay(pdMS_TO_TICKS(200));
    hdl->sensor_initialized = true;
    ESP_LOGI(TAG_SPL06, "SPL06-001 ready, chip_id=0x%02x", chip_id);
    return ESP_OK;
}

static esp_err_t spl06_read_raw(lua_module_spl06_handle_t *hdl, int32_t *raw_temp, int32_t *raw_pres)
{
    uint8_t buf[3] = { 0 };
    /* Read temperature (24-bit signed) */
    ESP_RETURN_ON_ERROR(spl06_read(hdl, SPL06_REG_TMP_B2, buf, 3), TAG_SPL06, "TMP read");
    *raw_temp = (int32_t)((buf[0] << 16) | (buf[1] << 8) | buf[2]);
    if (*raw_temp & 0x800000) *raw_temp |= 0xFF000000;  /* sign extend 24-bit */
    /* Read pressure (24-bit signed) */
    ESP_RETURN_ON_ERROR(spl06_read(hdl, SPL06_REG_PSR_B2, buf, 3), TAG_SPL06, "PRS read");
    *raw_pres = (int32_t)((buf[0] << 16) | (buf[1] << 8) | buf[2]);
    if (*raw_pres & 0x800000) *raw_pres |= 0xFF000000;
    return ESP_OK;
}

static void spl06_compensate(lua_module_spl06_handle_t *hdl, int32_t raw_t, int32_t raw_p,
                              float *out_temp, float *out_pres)
{
    spl06_calib_t *c = &hdl->calib;
    /* 8x oversampling scale factors (datasheet) */
    float Traw_sc = (float)raw_t / SPL06_SCALE_8X;
    float Praw_sc = (float)raw_p / SPL06_SCALE_8X;
    /* Compensated temperature */
    float Tcomp = c->c0 * 0.5f + c->c1 * Traw_sc;
    /* Compensated pressure */
    float Pcomp = c->c00
                  + Praw_sc * (c->c10 + Praw_sc * (c->c20 + Praw_sc * c->c30))
                  + Traw_sc * c->c01
                  + Traw_sc * Praw_sc * (c->c11 + Praw_sc * c->c21);
    *out_temp = Tcomp;
    *out_pres = Pcomp;  /* in Pa */
}

static lua_module_spl06_ud_t *lua_spl06_get_ud(lua_State *L, int idx)
{
    lua_module_spl06_ud_t *ud = (lua_module_spl06_ud_t *)luaL_checkudata(L, idx, SPL06_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized)
        luaL_error(L, "environmental_sensor: invalid spl06 handle");
    return ud;
}

static int lua_spl06_read(lua_State *L)
{
    lua_module_spl06_ud_t *ud = lua_spl06_get_ud(L, 1);
    int32_t raw_t = 0, raw_p = 0;
    if (spl06_read_raw(ud->handle, &raw_t, &raw_p) != ESP_OK)
        return luaL_error(L, "spl06 read failed");
    float temp = 0, pres = 0;
    spl06_compensate(ud->handle, raw_t, raw_p, &temp, &pres);
    lua_newtable(L);
    lua_pushnumber(L, temp); lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, pres); lua_setfield(L, -2, "pressure");
    return 1;
}

static int lua_spl06_read_temperature(lua_State *L)
{
    lua_module_spl06_ud_t *ud = lua_spl06_get_ud(L, 1);
    int32_t raw_t = 0, raw_p = 0;
    if (spl06_read_raw(ud->handle, &raw_t, &raw_p) != ESP_OK)
        return luaL_error(L, "spl06 read_temperature failed");
    float temp = 0, pres = 0;
    spl06_compensate(ud->handle, raw_t, raw_p, &temp, &pres);
    lua_pushnumber(L, temp);
    return 1;
}

static int lua_spl06_read_pressure(lua_State *L)
{
    lua_module_spl06_ud_t *ud = lua_spl06_get_ud(L, 1);
    int32_t raw_t = 0, raw_p = 0;
    if (spl06_read_raw(ud->handle, &raw_t, &raw_p) != ESP_OK)
        return luaL_error(L, "spl06 read_pressure failed");
    float temp = 0, pres = 0;
    spl06_compensate(ud->handle, raw_t, raw_p, &temp, &pres);
    lua_pushnumber(L, pres);
    return 1;
}

static int lua_spl06_name(lua_State *L) { lua_spl06_get_ud(L, 1); lua_pushstring(L, "spl06"); return 1; }
static int lua_spl06_close(lua_State *L)
{
    lua_module_spl06_ud_t *ud = lua_spl06_get_ud(L, 1);
    if (ud->handle) { spl06_destroy(ud->handle); ud->handle = NULL; }
    return 0;
}
static int lua_spl06_gc(lua_State *L)
{
    lua_module_spl06_ud_t *ud = (lua_module_spl06_ud_t *)luaL_testudata(L, 1, SPL06_METATABLE);
    if (ud && ud->handle) { spl06_destroy(ud->handle); ud->handle = NULL; }
    return 0;
}

static esp_err_t lua_spl06_resolve_board_cfg(const char *device_name, lua_spl06_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *d = g_esp_board_devices;
    while (d && d->name) {
        if (strcmp(d->name, device_name) == 0) {
            if (!d->cfg) return ESP_ERR_NOT_FOUND;
            if (d->cfg_size != sizeof(lua_spl06_board_cfg_t)) return ESP_ERR_INVALID_SIZE;
            *out = (lua_spl06_board_cfg_t *)d->cfg;
            return ESP_OK;
        }
        d = d->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static int lua_spl06_new(lua_State *L)
{
    const char *dev_name = SPL06_DEFAULT_NAME;
    if (lua_isstring(L, 1)) dev_name = lua_tostring(L, 1);
    lua_spl06_board_cfg_t *board = NULL;
    esp_err_t err = lua_spl06_resolve_board_cfg(dev_name, &board);
    if (err == ESP_ERR_INVALID_SIZE) return luaL_error(L, "spl06 cfg_size mismatch for '%s'", dev_name);
    const char *periph = NULL;
    int i2c_addr = SPL06_I2C_ADDR_LOW;
    int freq = SPL06_DEFAULT_FREQ_HZ;
    if (board) {
        if (board->peripheral_name && board->peripheral_name[0]) periph = board->peripheral_name;
        if (board->i2c_addr) i2c_addr = board->i2c_addr;
        if (board->frequency > 0) freq = board->frequency;
    }
    int opts_idx = lua_istable(L, 2) ? 2 : (lua_istable(L, 1) ? 1 : 0);
    if (opts_idx) {
        lua_getfield(L, opts_idx, "peripheral");
        if (lua_isstring(L, -1)) periph = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, opts_idx, "i2c_addr");
        if (lua_isnumber(L, -1)) i2c_addr = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (!periph) return luaL_error(L, "spl06.new: missing 'peripheral'");
    lua_module_spl06_handle_t *hdl = calloc(1, sizeof(*hdl));
    if (!hdl) return luaL_error(L, "spl06: OOM");
    snprintf(hdl->peripheral_name, sizeof(hdl->peripheral_name), "%s", periph);
    err = spl06_open_bus(periph, freq, &hdl->i2c_bus_handle, &hdl->peripheral_ref_held);
    if (err != ESP_OK) { free(hdl); return luaL_error(L, "spl06: bus open failed"); }
    hdl->i2c_dev_handle = i2c_bus_device_create(hdl->i2c_bus_handle, (uint8_t)i2c_addr, 0);
    if (!hdl->i2c_dev_handle) { spl06_destroy(hdl); return luaL_error(L, "spl06: dev create failed"); }
    hdl->i2c_addr = (uint8_t)i2c_addr;
    err = spl06_probe(hdl);
    if (err != ESP_OK) { spl06_destroy(hdl); return luaL_error(L, "spl06: probe failed"); }
    lua_module_spl06_ud_t *ud = (lua_module_spl06_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = hdl;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", dev_name);
    luaL_getmetatable(L, SPL06_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_spl06_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, SPL06_METATABLE)) {
        lua_pushcfunction(L, lua_spl06_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_spl06_read); lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_spl06_read_temperature); lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_spl06_read_pressure); lua_setfield(L, -2, "read_pressure");
        lua_pushcfunction(L, lua_spl06_name); lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_spl06_close); lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif /* CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_SPL06 */

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
static void lua_module_environmental_sensor_create_bme690_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_BME690_METATABLE)) {
        lua_pushcfunction(L, lua_module_bme690_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_bme690_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_bme690_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_bme690_read_pressure);
        lua_setfield(L, -2, "read_pressure");
        lua_pushcfunction(L, lua_module_bme690_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, lua_module_bme690_read_gas);
        lua_setfield(L, -2, "read_gas");
        lua_pushcfunction(L, lua_module_bme690_chip_id);
        lua_setfield(L, -2, "chip_id");
        lua_pushcfunction(L, lua_module_bme690_variant_id);
        lua_setfield(L, -2, "variant_id");
        lua_pushcfunction(L, lua_module_bme690_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_bme690_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif

static bool lua_module_environmental_sensor_table_has_field(lua_State *L, int idx, const char *field)
{
    bool has_field = false;

    if (!lua_istable(L, idx)) {
        return false;
    }

    lua_getfield(L, idx, field);
    has_field = !lua_isnoneornil(L, -1);
    lua_pop(L, 1);
    return has_field;
}

static int lua_module_environmental_sensor_new(lua_State *L)
{
    const char *backend_type = NULL;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "type");
        if (lua_isstring(L, -1)) {
            backend_type = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (backend_type == NULL && lua_istable(L, 1) &&
        lua_module_environmental_sensor_table_has_field(L, 1, "pin")) {
        backend_type = LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT;
    }

    if (backend_type == NULL) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
        return lua_module_bme690_new(L);
#elif CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
        return lua_module_dht_new(L);
#else
        return luaL_error(L, "environmental_sensor has no enabled backend");
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
        return lua_module_bme690_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
        return lua_module_dht_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_LTR308ALS) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_LTR308ALS
        return lua_ltr308als_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_SPL06) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_SPL06
        return lua_spl06_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    return luaL_error(L, "environmental_sensor.new: unsupported type '%s'", backend_type);
}

int luaopen_environmental_sensor(lua_State *L)
{
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
    lua_module_environmental_sensor_create_bme690_metatable(L);
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
    lua_module_dht_create_metatable(L);
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_LTR308ALS
    lua_ltr308als_create_metatable(L);
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_SPL06
    lua_spl06_create_metatable(L);
#endif

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_environmental_sensor_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_environmental_sensor_register(void)
{
    return cap_lua_register_module(LUA_MODULE_ENVIRONMENTAL_SENSOR_NAME,
                                   luaopen_environmental_sensor);
}
