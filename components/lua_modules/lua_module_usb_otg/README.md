# Lua USB OTG (C-backed module)

This module provides a Lua interface for USB OTG role switching between Host mode (UVC camera) and Device mode (MSC U-disk exposing the SD card).

## How to call
- Import it with `local otg = require("usb_otg")`
- Check current role with `otg.status()` — returns `"none"`, `"host"`, or `"device"`
- Switch to Host mode (USB camera) with `otg.start_host()`
- Switch to Device mode (SD card as U-disk) with `otg.start_device()`
- Stop current role with `otg.stop()`

## Notes
- Host and Device modes are mutually exclusive; switching stops the current role first.
- Entering Device mode unmounts the SD card FATFS; exiting re-mounts it automatically.
- Host mode initializes UVC camera support via `esp_video_init()`.
