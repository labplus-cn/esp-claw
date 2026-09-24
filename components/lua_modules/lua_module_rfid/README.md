# Lua RC522 RFID Reader

This module provides a pure-Lua driver for the RC522 RFID reader connected via I2C.
When a request mentions `RFID`, `RC522`, `NFC card`, `card detection`, or `UID scan`, use this module by default.

## How to call
- Import it with `local rc522 = require("lib_rc522")`
- Create an instance with `local rfid = rc522.new({ bus = bus })`
- Scan for cards with `local card = rfid:scan(timeout_ms)`
- Read firmware version with `rfid:read_version()`
- Halt a card with `rfid:halt()`
- Call `rfid:close()` when done

## Options table
| Field      | Type    | Meaning                                              |
|------------|---------|------------------------------------------------------|
| `port`     | integer | I2C port number                                      |
| `sda`      | integer | SDA GPIO number                                      |
| `scl`      | integer | SCL GPIO number                                      |
| `freq_hz`  | integer | I2C clock in Hz (default `100000`)                   |
| `frequency`| integer | Alias of `freq_hz`                                   |
| `addr`     | integer | 7-bit I2C address (default `0x2F`)                   |
| `bus`      | userdata| Existing `i2c` bus handle, recommended               |
| `close_bus`| boolean | Close a bus created from `port`/`sda`/`scl` when `rfid:close()` runs |

## Methods
- `rfid:address()` — returns the configured I2C address
- `rfid:read_version()` — returns firmware version byte
- `rfid:scan(timeout_ms)` — scan for card, returns `{ uid = {bytes}, sak = number, type = string }` or nil
- `rfid:halt()` — halt the currently selected card
- `rfid:close()` — disables antenna and closes the I2C device

## Card detection result
```lua
{
    uid = { 0x12, 0x34, 0x56, 0x78 },  -- UID bytes
    sak = 0x08,                          -- Select Acknowledge byte
    type = "MIFARE 1K",                  -- Human-readable card type
}
```

## Supported card types
- MIFARE 1K (4-byte and 7-byte UID)
- MIFARE 4K
- MIFARE Ultralight / Ultralight C
- MIFARE Plus
- Other ISO14443A cards

## Example
```lua
local rc522 = require("lib_rc522")
local i2c = require("i2c")

local bus = i2c.new(0, 21, 22, 100000)
local rfid = rc522.new({ bus = bus })

print("Firmware version:", string.format("0x%02X", rfid:read_version()))

-- Scan for a card (2 second timeout)
local card = rfid:scan(2000)
if card then
    print("Card found!")
    print("Type:", card.type)
    local uid_str = {}
    for _, b in ipairs(card.uid) do
        uid_str[#uid_str + 1] = string.format("%02X", b)
    end
    print("UID:", table.concat(uid_str, " "))
else
    print("No card found")
end

rfid:close()
bus:close()
```
