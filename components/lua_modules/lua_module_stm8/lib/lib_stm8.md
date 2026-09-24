# stm8 — STM8S001 Slave MCU (C-backed module)

C-backed Lua module for the STM8S001 slave MCU. It reads I2C configuration from the board manager YAML and exports `require("stm8")`.

## When to use

Use this module when a script needs to control motors or read battery voltage from an STM8S001 connected over I2C. The module obtains the I2C bus from the board manager, so no hardcoded GPIO pins are needed.

## Loading

```lua
local stm8 = require("stm8")
```

## Constructor

```lua
local mcu = stm8.new(opts)
```

`opts` is a table with two modes:

### Board-managed mode (recommended)

- `device`: device name matching the board manager YAML entry (e.g. `"stm8s001"`). The module reads `i2c_addr`, `frequency`, and the I2C peripheral name from the YAML automatically.

```lua
local mcu = stm8.new({ device = "stm8s001" })
```

### Manual mode

- `port`: I2C port number.
- `sda`: SDA GPIO number.
- `scl`: SCL GPIO number.
- `i2c_addr`: 7-bit I2C address. Defaults to `0x11`.
- `frequency` or `freq_hz`: I2C clock in Hz. Defaults to `100000`.

```lua
local mcu = stm8.new({ port = 0, sda = 21, scl = 22, i2c_addr = 0x11 })
```

## Methods

- `mcu:address()`: returns the configured 7-bit I2C address.
- `mcu:read_battery_mv()`: returns battery voltage in millivolts (2 bytes, little-endian).
- `mcu:set_motor1(speed)`: set motor 1 speed, range -100 to +100, negative = reverse.
- `mcu:set_motor2(speed)`: set motor 2 speed, range -100 to +100, negative = reverse.
- `mcu:stop_all()`: stop both motors (writes 0 to both motor registers).
- `mcu:scan_bus()`: scan the I2C bus, returns an array of ACKed 7-bit addresses.
- `mcu:close()`: releases the I2C device and peripheral reference.

## Register map

| Register | Function | Access | Data format |
|----------|----------|--------|-------------|
| 0x01 | Motor 1 speed | Write | signed byte (-100..+100) |
| 0x02 | Motor 2 speed | Write | signed byte (-100..+100) |
| 0x03 | Battery voltage | Read | 2 bytes, little-endian, mV |

## Board manager YAML

The device must be listed in `board_devices.yaml` so the module can look up its I2C bus:

```yaml
- name: stm8s001
  type: custom
  chip: stm8s001
  version: default
  config:
    i2c_addr: 0x11
    frequency: 100000
    description: "STM8S001 slave MCU (motor control + battery ADC)"
  peripherals:
    - name: i2c_master
```

## Example

```lua
local stm8 = require("stm8")

-- Use board-manager config (recommended)
local mcu = stm8.new({ device = "stm8s001" })

-- Read battery voltage
local mv = mcu:read_battery_mv()
print(string.format("Battery: %d mV", mv))

-- Control motors
mcu:set_motor1(50)    -- Motor 1 forward at 50%
mcu:set_motor2(-80)   -- Motor 2 reverse at 80%

-- Scan bus for diagnostics
local addrs = mcu:scan_bus()
for i, a in ipairs(addrs) do
    print(string.format("  [0x%02X]", a))
end

-- Stop all and release
mcu:stop_all()
mcu:close()
```
