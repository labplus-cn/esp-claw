-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0

-- Pure-Lua SHT20 temperature/humidity driver using the builtin I2C module.

local i2c = require("i2c")
local delay = require("delay")

local M = {}
local mt = {}
mt.__index = mt

local DEFAULT_ADDRESS = 0x40
local DEFAULT_FREQUENCY_HZ = 100000
local CMD_SOFT_RESET = 0xFE
local CMD_TEMP_NO_HOLD = 0xF3
local CMD_HUMIDITY_NO_HOLD = 0xF5
local TEMP_WAIT_MS = 90
local HUMIDITY_WAIT_MS = 35
local RESET_WAIT_MS = 20

local function crc8(msb, lsb)
    local crc = 0
    for _, byte in ipairs({msb, lsb}) do
        crc = crc ~ byte
        for _ = 1, 8 do
            if (crc & 0x80) ~= 0 then
                crc = ((crc << 1) ~ 0x31) & 0xFF
            else
                crc = (crc << 1) & 0xFF
            end
        end
    end
    return crc
end

local function read_raw(self, command, wait_ms)
    assert(self._dev, "sht20: device is closed")
    self._dev:write_byte(command)
    delay.delay_ms(wait_ms)

    local data = self._dev:read(3)
    local msb, lsb, received_crc = string.byte(data, 1, 3)
    if not received_crc then
        error("sht20: short measurement response")
    end
    local expected_crc = crc8(msb, lsb)
    if received_crc ~= expected_crc then
        error(string.format(
            "sht20: CRC mismatch (received 0x%02X, expected 0x%02X)",
            received_crc, expected_crc))
    end
    return (((msb << 8) | lsb) & 0xFFFC)
end

function M.new(opts)
    opts = type(opts) == "table" and opts or {}
    local address = opts.address or opts.addr or DEFAULT_ADDRESS
    if address ~= DEFAULT_ADDRESS then
        error(string.format("sht20: address must be 0x%02X", DEFAULT_ADDRESS))
    end

    local bus = opts.bus
    local owns_bus = false
    if bus == nil then
        bus = i2c.new(
            assert(opts.port, "sht20.new: missing 'port'"),
            assert(opts.sda, "sht20.new: missing 'sda'"),
            assert(opts.scl, "sht20.new: missing 'scl'"),
            opts.frequency or opts.freq_hz or DEFAULT_FREQUENCY_HZ
        )
        owns_bus = opts.close_bus ~= false
    end

    local dev = bus:device(address, opts.frequency or opts.freq_hz or DEFAULT_FREQUENCY_HZ)
    local sensor = setmetatable({
        _bus = bus,
        _dev = dev,
        _owns_bus = owns_bus,
        _address = address,
    }, mt)

    if opts.reset ~= false then
        local ok, err = pcall(function()
            dev:write_byte(CMD_SOFT_RESET)
            delay.delay_ms(RESET_WAIT_MS)
        end)
        if not ok then
            sensor:close()
            error(err)
        end
    end
    return sensor
end

function mt:read_temperature()
    local raw = read_raw(self, CMD_TEMP_NO_HOLD, TEMP_WAIT_MS)
    return -46.85 + 175.72 * raw / 65536.0
end

function mt:read_humidity()
    local raw = read_raw(self, CMD_HUMIDITY_NO_HOLD, HUMIDITY_WAIT_MS)
    local humidity = -6.0 + 125.0 * raw / 65536.0
    return math.max(0.0, math.min(100.0, humidity))
end

function mt:read()
    return {
        temperature = self:read_temperature(),
        humidity = self:read_humidity(),
    }
end

function mt:name()
    return "sht20"
end

function mt:address()
    return self._address
end

function mt:close()
    if self._dev then
        self._dev:close()
        self._dev = nil
    end
    if self._owns_bus and self._bus then
        self._bus:close()
    end
    self._bus = nil
end

return M
