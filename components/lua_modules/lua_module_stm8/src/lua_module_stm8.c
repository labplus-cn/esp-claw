/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_stm8.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#define STM8_LUA_MODULE_NAME   "stm8"
#define STM8_METATABLE         "stm8.device"
#define STM8_DEFAULT_ADDR      0x11
#define STM8_DEFAULT_FREQ_HZ   100000
#define STM8_MAX_NAME_LEN      64

#define STM8_REG_MOTOR1        1
#define STM8_REG_MOTOR2        2
#define STM8_REG_BATTERY       3

#define STM8_SPEED_MIN         (-100)
#define STM8_SPEED_MAX         100

static const char *TAG = "lua_stm8";

/*
 * Mirror of the auto-generated board config struct for the stm8s001 device.
 * MUST be byte-for-byte identical to dev_custom_stm8s001_config_t.
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    const char *description;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_stm8_board_cfg_t;

typedef struct {
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t dev;
    char peripheral_name[STM8_MAX_NAME_LEN];
    uint8_t i2c_addr;
    bool peripheral_ref_held;
} lua_stm8_handle_t;

typedef struct {
    lua_stm8_handle_t *handle;
} lua_stm8_ud_t;

/* ------------------------------------------------------------------ */
/*  Internal: I2C bus open / close                                      */
/* ------------------------------------------------------------------ */

static esp_err_t stm8_open_bus(const char *peripheral_name, int frequency,
                                i2c_bus_handle_t *bus, bool *ref_held)
{
    i2c_master_bus_handle_t master = NULL;
    i2c_master_bus_config_t *cfg = NULL;
    *ref_held = false;

    ESP_RETURN_ON_ERROR(
        esp_board_periph_ref_handle(peripheral_name, (void **)&master),
        TAG, "ref bus '%s'", peripheral_name);
    *ref_held = true;

    ESP_RETURN_ON_ERROR(
        esp_board_periph_get_config(peripheral_name, (void **)&cfg),
        TAG, "get bus cfg '%s'", peripheral_name);

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
    if (!*bus) {
        esp_board_periph_unref_handle(peripheral_name);
        *ref_held = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void stm8_destroy(lua_stm8_handle_t *hdl)
{
    if (!hdl) return;
    if (hdl->dev) {
        i2c_bus_device_delete(&hdl->dev);
        hdl->dev = NULL;
    }
    if (hdl->peripheral_ref_held) {
        esp_board_periph_unref_handle(hdl->peripheral_name);
        hdl->peripheral_ref_held = false;
    }
    free(hdl);
}

/* ------------------------------------------------------------------ */
/*  Lua metatable methods                                               */
/* ------------------------------------------------------------------ */

static lua_stm8_handle_t *stm8_check_handle(lua_State *L, int idx)
{
    lua_stm8_ud_t *ud = (lua_stm8_ud_t *)luaL_checkudata(L, idx, STM8_METATABLE);
    if (!ud || !ud->handle || !ud->handle->dev) {
        luaL_error(L, "stm8: invalid or closed handle");
    }
    return ud->handle;
}

static int lua_stm8_read_battery_mv(lua_State *L)
{
    lua_stm8_handle_t *hdl = stm8_check_handle(L, 1);
    uint8_t raw[2] = {0};
    /* Step 1: write register address (STOP released, STM8 prepares data) */
    esp_err_t err = i2c_bus_write_byte(hdl->dev, STM8_REG_BATTERY, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "stm8: battery reg write failed: %s", esp_err_to_name(err));
    }
    /* Step 2: read 2 bytes from the register (no register address prefix) */
    err = i2c_bus_read_bytes(hdl->dev, NULL_I2C_MEM_ADDR, 2, raw);
    if (err != ESP_OK) {
        return luaL_error(L, "stm8: battery read failed: %s", esp_err_to_name(err));
    }
    /* Little-endian: low byte first */
    uint16_t mv = ((uint16_t)raw[1] << 8) | raw[0];
    lua_pushinteger(L, mv);
    return 1;
}

static int lua_stm8_set_motor(lua_State *L, uint8_t reg)
{
    lua_stm8_handle_t *hdl = stm8_check_handle(L, 1);
    int speed = (int)luaL_checkinteger(L, 2);
    if (speed < STM8_SPEED_MIN || speed > STM8_SPEED_MAX) {
        return luaL_error(L, "stm8: speed %d out of range [%d, %d]",
                          speed, STM8_SPEED_MIN, STM8_SPEED_MAX);
    }
    uint8_t val = (uint8_t)(int8_t)speed;
    esp_err_t err = i2c_bus_write_byte(hdl->dev, reg, val);
    if (err != ESP_OK) {
        return luaL_error(L, "stm8: motor write failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_stm8_set_motor1(lua_State *L)
{
    return lua_stm8_set_motor(L, STM8_REG_MOTOR1);
}

static int lua_stm8_set_motor2(lua_State *L)
{
    return lua_stm8_set_motor(L, STM8_REG_MOTOR2);
}

static int lua_stm8_stop_all(lua_State *L)
{
    lua_stm8_handle_t *hdl = stm8_check_handle(L, 1);
    i2c_bus_write_byte(hdl->dev, STM8_REG_MOTOR1, 0);
    i2c_bus_write_byte(hdl->dev, STM8_REG_MOTOR2, 0);
    return 0;
}

static int lua_stm8_address(lua_State *L)
{
    lua_stm8_handle_t *hdl = stm8_check_handle(L, 1);
    lua_pushinteger(L, hdl->i2c_addr);
    return 1;
}

static int lua_stm8_scan_bus(lua_State *L)
{
    lua_stm8_handle_t *hdl = stm8_check_handle(L, 1);
    uint8_t addrs[128];
    uint8_t count = i2c_bus_scan(hdl->bus, addrs, 128);
    lua_createtable(L, count, 0);
    for (uint8_t i = 0; i < count; i++) {
        lua_pushinteger(L, addrs[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_stm8_close(lua_State *L)
{
    lua_stm8_ud_t *ud = (lua_stm8_ud_t *)luaL_checkudata(L, 1, STM8_METATABLE);
    if (ud->handle) {
        stm8_destroy(ud->handle);
        ud->handle = NULL;
    }
    return 0;
}

static int lua_stm8_gc(lua_State *L)
{
    lua_stm8_ud_t *ud = (lua_stm8_ud_t *)luaL_testudata(L, 1, STM8_METATABLE);
    if (ud && ud->handle) {
        stm8_destroy(ud->handle);
        ud->handle = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Constructor: stm8.new(opts)                                         */
/* ------------------------------------------------------------------ */

static int lua_stm8_new(lua_State *L)
{
    /* Parse options table */
    const char *device_name = NULL;
    int i2c_addr = STM8_DEFAULT_ADDR;
    int frequency = STM8_DEFAULT_FREQ_HZ;
    const char *peripheral = NULL;
    int port = -1;
    int sda = -1;
    int scl = -1;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) device_name = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "i2c_addr");
        if (lua_isinteger(L, -1)) i2c_addr = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "frequency");
        if (lua_isinteger(L, -1)) frequency = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "freq_hz");
        if (lua_isinteger(L, -1)) frequency = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "peripheral");
        if (lua_isstring(L, -1)) peripheral = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "port");
        if (lua_isinteger(L, -1)) port = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "sda");
        if (lua_isinteger(L, -1)) sda = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "scl");
        if (lua_isinteger(L, -1)) scl = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    /* If device name given, read config from board manager */
    if (device_name) {
        void *config = NULL;
        esp_err_t err = esp_board_manager_get_device_config(device_name, &config);
        if (err != ESP_OK) {
            return luaL_error(L, "stm8: board config '%s' not found: %s",
                              device_name, esp_err_to_name(err));
        }

        lua_stm8_board_cfg_t *board_cfg = (lua_stm8_board_cfg_t *)config;
        if (sizeof(lua_stm8_board_cfg_t) > 256) {
            return luaL_error(L, "stm8: board config struct too large");
        }

        if (board_cfg->i2c_addr > 0) i2c_addr = board_cfg->i2c_addr;
        if (board_cfg->frequency > 0) frequency = board_cfg->frequency;
        if (board_cfg->peripheral_name) peripheral = board_cfg->peripheral_name;

        ESP_LOGI(TAG, "stm8: board config '%s' -> addr=0x%02X, freq=%d, periph='%s'",
                 device_name, i2c_addr, frequency, peripheral ? peripheral : "?");
    }

    /* Create handle */
    lua_stm8_handle_t *hdl = calloc(1, sizeof(*hdl));
    if (!hdl) return luaL_error(L, "stm8: OOM");

    hdl->i2c_addr = (uint8_t)i2c_addr;

    if (peripheral) {
        /* Board-managed I2C bus */
        snprintf(hdl->peripheral_name, sizeof(hdl->peripheral_name), "%s", peripheral);
        esp_err_t err = stm8_open_bus(peripheral, frequency, &hdl->bus, &hdl->peripheral_ref_held);
        if (err != ESP_OK) {
            free(hdl);
            return luaL_error(L, "stm8: I2C bus '%s' open failed", peripheral);
        }
    } else if (port >= 0 && sda >= 0 && scl >= 0) {
        /* Manual I2C bus creation */
        const i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .sda_pullup_en = true,
            .scl_pullup_en = true,
            .master.clk_speed = (uint32_t)frequency,
        };
        hdl->bus = i2c_bus_create((i2c_port_t)port, &i2c_cfg);
        if (!hdl->bus) {
            free(hdl);
            return luaL_error(L, "stm8: I2C bus create failed on port %d", port);
        }
    } else {
        free(hdl);
        return luaL_error(L, "stm8: need 'device' or 'port'+'sda'+'scl'");
    }

    /* Add I2C device on the bus */
    hdl->dev = i2c_bus_device_create(hdl->bus, hdl->i2c_addr, 0);
    if (!hdl->dev) {
        stm8_destroy(hdl);
        return luaL_error(L, "stm8: I2C device create failed at 0x%02X", hdl->i2c_addr);
    }

    ESP_LOGI(TAG, "stm8: ready @ 0x%02X", hdl->i2c_addr);

    /* Create Lua userdata */
    lua_stm8_ud_t *ud = (lua_stm8_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->handle = hdl;
    luaL_getmetatable(L, STM8_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Module registration                                                 */
/* ------------------------------------------------------------------ */

static const luaL_Reg stm8_methods[] = {
    {"read_battery_mv", lua_stm8_read_battery_mv},
    {"set_motor1",      lua_stm8_set_motor1},
    {"set_motor2",      lua_stm8_set_motor2},
    {"stop_all",        lua_stm8_stop_all},
    {"address",         lua_stm8_address},
    {"scan_bus",        lua_stm8_scan_bus},
    {"close",           lua_stm8_close},
    {NULL, NULL},
};

static int luaopen_stm8(lua_State *L)
{
    /* Metatable for device objects */
    if (luaL_newmetatable(L, STM8_METATABLE)) {
        lua_pushcfunction(L, lua_stm8_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        for (const luaL_Reg *m = stm8_methods; m->name; m++) {
            lua_pushcfunction(L, m->func);
            lua_setfield(L, -2, m->name);
        }
    }
    lua_pop(L, 1);

    /* Module table */
    lua_newtable(L);
    lua_pushcfunction(L, lua_stm8_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_stm8_register(void)
{
    return cap_lua_register_module(STM8_LUA_MODULE_NAME, luaopen_stm8);
}
