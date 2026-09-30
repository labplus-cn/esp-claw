# sht20 — SHT20 temperature and humidity sensor

Pure-Lua SHT20 driver backed by the built-in `i2c` and `delay` modules. The
sensor uses its fixed 7-bit I2C address `0x40`.

## Open and read

```lua
local sht20 = require("sht20")

local sensor = sht20.new({
    port = 0,
    sda = 44,
    scl = 43,
    frequency = 100000,
})

local sample = sensor:read()
print(string.format("temperature: %.2f C", sample.temperature))
print(string.format("humidity: %.2f %%", sample.humidity))
sensor:close()
```

`new(opts)` accepts an existing `opts.bus`, or `port`, `sda`, `scl`, and an
optional `frequency`. `address`/`addr`, when supplied, must be `0x40`.
`reset=false` skips the normal soft reset. When the driver creates the bus it
closes its Lua bus handle by default; set `close_bus=false` to keep it open.

Methods:

- `read()` returns `{ temperature = degrees_celsius, humidity = percent }`.
- `read_temperature()` returns degrees Celsius.
- `read_humidity()` returns relative humidity clamped to 0–100 percent.
- `name()` returns `"sht20"`.
- `address()` returns `0x40`.
- `close()` releases the device and any owned bus handle.

Measurements are blocking and include the conversion delay required by the
sensor. Responses are checked using the SHT20 CRC-8 polynomial.
