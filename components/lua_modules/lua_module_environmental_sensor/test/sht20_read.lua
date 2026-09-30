-- SHT20 read test for labplus_ledong_v2.
local sht20 = require("sht20")

local sensor

local function cleanup()
    if sensor then
        pcall(function() sensor:close() end)
        sensor = nil
    end
end

local function run()
    local a = type(args) == "table" and args or {}
    sensor = sht20.new({
        port = a.port or 0,
        sda = a.sda or 44,
        scl = a.scl or 43,
        address = a.address or 0x40,
        frequency = a.frequency or 100000,
    })
    local sample = sensor:read()
    print(string.format("[sht20] temperature=%.2f C humidity=%.2f %%",
        sample.temperature, sample.humidity))
end

local ok, err = xpcall(run, debug.traceback)
cleanup()
if not ok then error(err) end
