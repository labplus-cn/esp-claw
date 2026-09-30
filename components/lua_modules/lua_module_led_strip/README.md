# Lua LED Strip

This module describes how to correctly use led_strip when writing Lua scripts.
When a request mentions `ws2812`, use this `led_strip` module by default.

## How to call
- Import it with `local led_strip = require("led_strip")`
- Prefer `local strip = led_strip.open("board_led_strip")` when the strip is
  declared in `board_devices.yaml`; this reuses the Board Manager handle.
- Call `local strip = led_strip.new(gpio, max_leds)` to create a strip handle
  only for hardware that is not already declared in Board Manager.
- Call `strip:set_pixel(index, r, g, b)` to set one pixel
- Call `strip:set_pixel_hsv(index, h, s, v)` to set one pixel using HSV
- Call `strip:refresh()` to apply changes
- Call `strip:clear()` or `strip:close()` when needed

Pixel indexes are 0-based. `set_pixel`, `set_pixel_hsv`, `refresh`, `clear`, and `close` return no values on success and raise a Lua error on failure.

`set_pixel_hsv` uses:
- `h`: `0-359`
- `s`: `0-255`
- `v`: `0-255`

## Example
```lua
local led_strip = require("led_strip")

local strip = led_strip.open("board_led_strip")
strip:set_pixel(0, 255, 0, 0)
strip:set_pixel_hsv(0, 120, 255, 64)
strip:refresh()
strip:close()
```

`open()` returns a borrowed handle. `close()` releases only the Lua wrapper;
Board Manager remains the owner of the physical strip.
