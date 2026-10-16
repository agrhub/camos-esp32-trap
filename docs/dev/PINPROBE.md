# Confirm the camera pin map before flashing a generic ESP32-S3 CAM board

The firmware ships two pin presets (menuconfig "CamOS trap camera" > "Board pins"):

| Preset | Status |
|---|---|
| `ai-thinker-esp32cam` | Standard AI-Thinker map. Not yet run on the owner's board by us. |
| `esp32s3-eye-compatible` (Keyestudio MB0184 / generic N16R8 CAM) | **Unverified for the board you bought.** The map below is what the Keyestudio MB0184 documents (Arduino define `CAMERA_MODEL_ESP32S3_EYE`) and what the Freenove ESP32-S3 WROOM CAM uses; the generic boards that copy it should match, but the seller's pages give no pin text. |

Preset 2 (also in `sdkconfig.defaults.esp32s3cam`): PWDN -1, RESET -1, XCLK 15, SIOD 4, SIOC 5, Y9 16, Y8 17, Y7 18, Y6 12,
Y5 10, Y4 8, Y3 9, Y2 11, VSYNC 6, HREF 7, PCLK 13. microSD (SD_MMC 1-bit): CLK 39, CMD 38, D0 40 (the firmware does not use
the card). The RGB LED pin is undocumented, so no LED is configured. GPIO35-37 are taken by the octal PSRAM and never used.

## Before you flash

1. Get the seller's `CameraWebServer` example (Arduino, ESP32 board package) or `camera_pins.h` from the board's wiki / the
   seller's download link. Boards sold by "TL Quantum" and most other vendors ship one. Open `camera_pins.h` and find the
   block for `CAMERA_MODEL_ESP32S3_EYE` (or the block the vendor says to select).
2. Compare every line with preset 2 above (`XCLK_GPIO_NUM`, `SIOD_GPIO_NUM`, `SIOC_GPIO_NUM`, `Y2..Y9`, `VSYNC`, `HREF`,
   `PCLK`, `PWDN`, `RESET`).
3. If any pin differs, edit it in `idf.py -B build-esp32s3cam menuconfig` > "CamOS trap camera" > "Board pins"
   (they are plain integers, `-1` = not connected) and rebuild. Do not flash a wrong map: a wrong pin only makes camera init fail,
   but two outputs driving each other can heat a pin.
4. Cheap check without our firmware: flash the vendor's `CameraWebServer` once with Wi-Fi credentials and confirm a live picture.
   It also tells you the sensor (OV3660 or OV2640) and that the board's PSRAM works.

## What the firmware tells you at the first capture

- `camera init failed 0x105` (sensor not found): wrong SIOD/SIOC/XCLK/PWDN, no power, or the ribbon cable. A timeout (`0x107`) or `no usable frame`: the data pins, VSYNC, HREF or PCLK are wrong.
- `sensor PID 0x26...` = OV2640 (UXGA 1600x1200), `0x3660` = OV3660 (QXGA 2048x1536), `0x5640` = OV5640 (QSXGA 2560x1920).
- Boot line `flash 16 MB, PSRAM 8192 KB`: if PSRAM shows `NOT initialized`, or the board resets in a loop with a PSRAM error,
  the module has quad PSRAM (N16R2 style) instead of octal: set `CONFIG_SPIRAM_MODE_QUAD=y` instead of `CONFIG_SPIRAM_MODE_OCT=y`
  in `sdkconfig.defaults.esp32s3cam`. If flash shows 8 MB, the board is not an N16: fix `CONFIG_ESPTOOLPY_FLASHSIZE_*`.
- Two USB-C ports: the one marked UART/COM goes to the USB-serial chip (log + flashing), the other is the native USB port.
  The log is mirrored on both (`CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG`).
