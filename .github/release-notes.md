Firmware {{VERSION}} for the CamOS fruit-fly trap camera.

**Flash it from the browser** (Chrome or Edge on a computer, nothing to install): {{FLASHER}}

Or download the image for your board below and flash it yourself:

| Board | First install (erases the camera) | Update (keeps settings and identity) |
|---|---|---|
| Freenove ESP32-S3 CAM (FNK0085) | `camos-esp32-trap-freenove-v{{VERSION}}-install.bin` at `0x0` | `-otadata.bin` at `0x11000` + `-update.bin` at `0x20000` |
| AI-Thinker ESP32-CAM | `camos-esp32-trap-esp32cam-v{{VERSION}}-install.bin` at `0x0` | same offsets |

```
python -m esptool --chip esp32s3 --port COM7 write_flash 0x0 camos-esp32-trap-freenove-v{{VERSION}}-install.bin
python -m esptool --chip esp32s3 --port COM7 write_flash 0x11000 camos-esp32-trap-freenove-v{{VERSION}}-otadata.bin 0x20000 camos-esp32-trap-freenove-v{{VERSION}}-update.bin
```
(`--chip esp32` for the AI-Thinker board.) A camera that is already running can also take the `-update.bin` from its web
console: Configuration > Maintenance & Upgrade > Upload a firmware file.

Set-up guide: https://github.com/{{REPO}}#readme · Licence: GNU AGPL-3.0-only (source: https://github.com/{{REPO}}); third-party components: NOTICE.
