-- Test script for STM8S001 slave MCU (C-backed module)
-- Usage: lua --run --path {CUR_SKILL_DIR}/test/stm8_test.lua
-- Or from CLI: test mcu battery / test mcu motor1 50

local stm8 = require("stm8")
local delay = require("delay")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local MOTOR_SPEED = int_arg("speed", 50)
local RUN_TIME_MS = int_arg("run_time_ms", 3000)

local mcu

local function cleanup()
    if mcu then
        pcall(function()
            mcu:stop_all()
            mcu:close()
        end)
        mcu = nil
    end
end

local function run()
    -- Prefer board-manager config; fall back to manual pins
    local device_name = a.device or "stm8s001"
    local ok, err

    ok, mcu = pcall(stm8.new, { device = device_name })
    if not ok then
        print(string.format("[stm8] board config '%s' failed: %s, trying manual pins",
                              device_name, tostring(mcu)))
        local port = int_arg("port", 0)
        local sda  = int_arg("sda", 21)
        local scl  = int_arg("scl", 22)
        local addr = int_arg("addr", 0x11)
        mcu = stm8.new({
            port = port,
            sda  = sda,
            scl  = scl,
            i2c_addr = addr,
        })
    end

    print(string.format("[stm8] opened addr=0x%02X", mcu:address()))

    -- Scan I2C bus for diagnostics
    local addrs = mcu:scan_bus()
    print(string.format("[stm8] bus scan: %d device(s) found", #addrs))
    for i, addr in ipairs(addrs) do
        print(string.format("[stm8]   [%d] 0x%02X", i, addr))
    end

    -- Read battery voltage
    local mv = mcu:read_battery_mv()
    print(string.format("[stm8] Battery: %d mV", mv))

    -- Test motor 1
    print(string.format("[stm8] Motor 1: speed=%d", MOTOR_SPEED))
    mcu:set_motor1(MOTOR_SPEED)
    delay.delay_ms(RUN_TIME_MS)

    -- Test motor 2
    print(string.format("[stm8] Motor 2: speed=%d", MOTOR_SPEED))
    mcu:set_motor2(MOTOR_SPEED)
    delay.delay_ms(RUN_TIME_MS)

    -- Stop all
    print("[stm8] Stopping all motors")
    mcu:stop_all()

    -- Final battery read
    mv = mcu:read_battery_mv()
    print(string.format("[stm8] Final Battery: %d mV", mv))

    print("[stm8] Test complete")
end

local ok2, err2 = xpcall(run, debug.traceback)
cleanup()
if not ok2 then
    error(err2)
end
