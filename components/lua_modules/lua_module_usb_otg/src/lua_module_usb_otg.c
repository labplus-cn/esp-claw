/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_usb_otg.h"

#include <string.h>

#include "cap_lua.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "usb_otg.h"

#define USB_OTG_LUA_MODULE_NAME  "usb_otg"

static const char *TAG = "lua_usb_otg";

static const char *role_to_string(usb_otg_role_t role)
{
    switch (role) {
    case USB_OTG_ROLE_NONE:   return "none";
    case USB_OTG_ROLE_HOST:   return "host";
    case USB_OTG_ROLE_DEVICE: return "device";
    default:                  return "unknown";
    }
}

/* ---- Lua API functions ---- */

/* otg.status() -> string */
static int lua_usb_otg_status(lua_State *L)
{
    usb_otg_role_t role = usb_otg_get_role();
    lua_pushstring(L, role_to_string(role));
    return 1;
}

/* otg.start_host() -> nil (throws on error) */
static int lua_usb_otg_start_host(lua_State *L)
{
    esp_err_t err = usb_otg_init();
    if (err != ESP_OK) {
        return luaL_error(L, "usb_otg: init failed: %s", esp_err_to_name(err));
    }

    err = usb_otg_switch_role(USB_OTG_ROLE_HOST);
    if (err != ESP_OK) {
        return luaL_error(L, "usb_otg: switch to host failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Lua: switched to HOST mode");
    return 0;
}

/* otg.start_device() -> nil (throws on error) */
static int lua_usb_otg_start_device(lua_State *L)
{
    esp_err_t err = usb_otg_init();
    if (err != ESP_OK) {
        return luaL_error(L, "usb_otg: init failed: %s", esp_err_to_name(err));
    }

    err = usb_otg_switch_role(USB_OTG_ROLE_DEVICE);
    if (err != ESP_OK) {
        return luaL_error(L, "usb_otg: switch to device failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Lua: switched to DEVICE mode");
    return 0;
}

/* otg.stop() -> nil */
static int lua_usb_otg_stop(lua_State *L)
{
    esp_err_t err = usb_otg_switch_role(USB_OTG_ROLE_NONE);
    if (err != ESP_OK) {
        return luaL_error(L, "usb_otg: stop failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Lua: USB OTG stopped");
    return 0;
}

/* ---- Module registration ---- */

static const luaL_Reg usb_otg_funcs[] = {
    {"status",       lua_usb_otg_status},
    {"start_host",   lua_usb_otg_start_host},
    {"start_device", lua_usb_otg_start_device},
    {"stop",         lua_usb_otg_stop},
    {NULL, NULL},
};

static int luaopen_usb_otg(lua_State *L)
{
    lua_newtable(L);
    for (const luaL_Reg *f = usb_otg_funcs; f->name; f++) {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    return 1;
}

esp_err_t lua_module_usb_otg_register(void)
{
    return cap_lua_register_module(USB_OTG_LUA_MODULE_NAME, luaopen_usb_otg);
}
