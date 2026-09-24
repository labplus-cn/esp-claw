# Lua STM8S001 Slave MCU (C-backed module)

This module provides a C-backed Lua driver for the STM8S001 slave MCU connected via I2C. It reads I2C bus configuration from the board manager YAML, ensuring consistent driver management with other board peripherals.

## How to call
- Import it with `local stm8 = require("stm8")`
- Create an instance with `local mcu = stm8.new({ device = "stm8s001" })`
- Read battery voltage with `mcu:read_battery_mv()`
- Control motors with `mcu:set_motor1(speed)` and `mcu:set_motor2(speed)`
- Stop all motors with `mcu:stop_all()`
- Scan the I2C bus with `mcu:scan_bus()`
- Call `mcu:close()` when done

## Options table
| Field      | Type    | Meaning                                              |
|------------|---------|------------------------------------------------------|
| `device`   | string  | Board manager YAML device name (recommended)         |
| `port`     | integer | I2C port number (manual mode)                        |
| `sda`      | integer | SDA GPIO number (manual mode)                        |
| `scl`      | integer | SCL GPIO number (manual mode)                        |
| `frequency`| integer | I2C clock in Hz (default `100000`)                   |
| `freq_hz`  | integer | Alias of `frequency`                                 |
| `i2c_addr` | integer | 7-bit I2C address (default `0x11`)                   |

## Methods
- `mcu:address()` — returns the configured I2C address
- `mcu:read_battery_mv()` — returns battery voltage in millivolts
- `mcu:set_motor1(speed)` — set motor 1 speed (-100 to +100, negative = reverse)
- `mcu:set_motor2(speed)` — set motor 2 speed (-100 to +100, negative = reverse)
- `mcu:stop_all()` — stop both motors
- `mcu:scan_bus()` — scan I2C bus, returns array of ACKed addresses
- `mcu:close()` — releases the I2C device and peripheral reference

## Example
```lua
local stm8 = require("stm8")

-- Board-managed mode (recommended)
local mcu = stm8.new({ device = "stm8s001" })

-- Read battery
print("Battery:", mcu:read_battery_mv(), "mV")

-- Control motors
mcu:set_motor1(50)    -- Motor 1 forward 50%
mcu:set_motor2(-80)   -- Motor 2 reverse 80%

-- Stop all
mcu:stop_all()

mcu:close()
```
