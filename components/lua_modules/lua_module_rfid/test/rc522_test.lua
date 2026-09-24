-- Test script for RC522 RFID reader
-- Usage: lua --run --path {CUR_SKILL_DIR}/test/rc522_test.lua
-- Or from CLI: test rfid

local rc522 = require("lib_rc522")
local delay = require("delay")
local i2c = require("i2c")

local a = type(args) == "table" and args or {}
local function int_arg(k, default)
    local v = a[k]
    if type(v) == "number" then
        return math.floor(v)
    end
    return default
end

local I2C_ADDR = int_arg("addr", 0x2F)
local FREQ_HZ = int_arg("freq_hz", 100000)
local SCAN_TIMEOUT_MS = int_arg("timeout_ms", 5000)

local rfid
local bus
local owns_bus = false

local function cleanup()
    if rfid then
        pcall(function()
            rfid:close()
        end)
        rfid = nil
    end
    if owns_bus and bus then
        pcall(function()
            bus:close()
        end)
        bus = nil
        owns_bus = false
    end
end

local function uid_to_string(uid)
    local parts = {}
    for _, b in ipairs(uid) do
        parts[#parts + 1] = string.format("%02X", b)
    end
    return table.concat(parts, " ")
end

local function run()
    local port = int_arg("port", 0)
    local sda = int_arg("sda", 21)
    local scl = int_arg("scl", 22)

    if a.bus then
        bus = a.bus
    else
        bus = i2c.new(port, sda, scl, FREQ_HZ)
        owns_bus = true
    end

    rfid = rc522.new({
        bus = bus,
        addr = I2C_ADDR,
    })

    -- Read firmware version
    local ver = rfid:read_version()
    print(string.format("[rc522] opened addr=0x%02X, FW=0x%02X", rfid:address(), ver))

    if ver == 0x00 or ver == 0xFF then
        print("[rc522] WARNING: unexpected firmware version, check wiring")
    end

    -- Scan for cards
    print(string.format("[rc522] Scanning for cards (timeout=%d ms)...", SCAN_TIMEOUT_MS))
    local card = rfid:scan(SCAN_TIMEOUT_MS)

    if card then
        print("[rc522] Card found!")
        print(string.format("[rc522]   Type: %s", card.type))
        print(string.format("[rc522]   SAK:  0x%02X", card.sak))
        print(string.format("[rc522]   UID:  %s", uid_to_string(card.uid)))
        print(string.format("[rc522]   UID len: %d bytes", #card.uid))

        -- Halt the card
        rfid:halt()
        print("[rc522] Card halted")
    else
        print("[rc522] No card detected within timeout")
    end

    print("[rc522] Test complete")
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then
    error(err)
end
