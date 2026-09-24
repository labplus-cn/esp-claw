# lib_rc522.lua

Reusable pure-Lua driver for the RC522 RFID reader over I2C. It uses the builtin `i2c` module and exports `require("lib_rc522")`.

## When to use

Use this library when a script needs to detect ISO14443A cards (MIFARE Classic, Ultralight, Plus, etc.) via an RC522 reader connected over I2C at address 0x2F.

## Loading

```lua
local rc522 = require("lib_rc522")
```

The script must also have access to the `i2c` module. You can either pass an existing I2C bus handle or let the library create one from GPIO options.

## Constructor

```lua
local rfid = rc522.new(opts)
```

`opts` is a table:

- `bus`: existing I2C bus userdata. This is recommended when the script already owns a bus.
- `port`: I2C port number. Required if `bus` is not provided.
- `sda`: SDA GPIO. Required if `bus` is not provided.
- `scl`: SCL GPIO. Required if `bus` is not provided.
- `freq_hz`: I2C frequency in Hz. Defaults to `100000`.
- `frequency`: alias of `freq_hz`.
- `addr`: RC522 7-bit I2C address. Defaults to `0x2F`.
- `close_bus`: if `true`, close a bus created from `port`/`sda`/`scl` when `rfid:close()` is called.

If `bus` is omitted, the library creates an I2C bus from `port`/`sda`/`scl`.

## Methods

- `rfid:address()`: returns the configured 7-bit I2C address.
- `rfid:read_version()`: returns the RC522 firmware version byte.
- `rfid:scan(timeout_ms)`: poll for a card with the given timeout (default 2000 ms). Returns a card table or `nil`.
- `rfid:halt()`: send HLTA to the currently selected card.
- `rfid:close()`: disables the antenna, closes the I2C device, and optionally the bus.

## Card detection result

```lua
{
    uid = { 0x12, 0x34, 0x56, 0x78 },  -- UID bytes (4, 7, or 10 bytes)
    sak = 0x08,                          -- Select Acknowledge byte
    type = "MIFARE 1K",                  -- Human-readable card type
}
```

## Supported card types

| SAK | UID length | Card type |
|-----|-----------|-----------|
| 0x08 / 0x09 | 4 bytes | MIFARE Classic 1K |
| 0x18 | 4 bytes | MIFARE Classic 4K |
| 0x00 | 4 bytes | MIFARE Ultralight |
| 0x10 / 0x11 | 4 bytes | MIFARE Plus |
| 0x01 | 4 bytes | MIFARE TNP3XXX |
| 0x00 | 7 bytes | MIFARE Ultralight C |
| 0x08 / 0x09 | 7 bytes | MIFARE Classic 1K (7-byte UID) |

## Example

```lua
local rc522 = require("lib_rc522")
local i2c = require("i2c")

local bus = i2c.new(0, 21, 22, 100000)
local rfid = rc522.new({ bus = bus })

print("FW version:", string.format("0x%02X", rfid:read_version()))

local card = rfid:scan(3000)
if card then
    print("Type:", card.type)
    local uid_str = {}
    for _, b in ipairs(card.uid) do
        uid_str[#uid_str + 1] = string.format("%02X", b)
    end
    print("UID:", table.concat(uid_str, " "))
else
    print("No card detected")
end

rfid:close()
bus:close()
```
