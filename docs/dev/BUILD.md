# Building from source

Most users flash a release from the browser (README). Build from source to change pins, options or code.

## Boards (presets)

| Preset | Board | Target | Flash / PSRAM | Sensor | Release image |
|---|---|---|---|---|---|
| `freenove` | Freenove ESP32-S3-WROOM CAM, kit FNK0085 | esp32s3 | 8 or 16 MB (the image declares 8 MB) / 8 MB octal | OV5640 (OV2640 detected too) | yes |
| `esp32cam` | AI-Thinker ESP32-CAM | esp32 | 4 MB / 4 MB quad | OV2640 | yes |
| `esp32s3cam` | generic ESP32-S3 N16R8 CAM (ESP32-S3-EYE pin map) | esp32s3 | 16 MB / 8 MB octal | OV2640 / OV3660 / OV5640 | no: check the pins first ([PINPROBE.md](PINPROBE.md)) |

The ESP32-CAM build has no modem, GPS, SD card or live view (`CAMOS_MODEM=n`, `CAMOS_GPS=n`): Wi-Fi + snapshot + deep
sleep. The cloud live view (`CAMOS_LIVE`, [LIVE_VIEW.md](LIVE_VIEW.md)) is on in the `freenove` preset only; the
`esp32s3cam` preset builds the same components with it off (untested board).

## Build and flash

ESP-IDF v5.3 or newer (releases are built with v5.4.4). The component manager fetches `esp32-camera`, `esp_modem`,
`led_strip`, `qrcode` and `mdns` on the first build (internet needed once), and for the ESP32-S3 also `esp_peer`,
`esp_h264` and `esp_websocket_client` (live view; about 100 MB in `firmware/managed_components`).

Linux / macOS (`. ~/esp/esp-idf/export.sh` first), from `firmware/`:

```bash
idf.py -B build-freenove -DSDKCONFIG=build-freenove/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.freenove_esp32s3_cam" set-target esp32s3 build
idf.py -B build-freenove -p /dev/ttyUSB0 flash monitor
```

Windows PowerShell (the helper sets up a minimal IDF environment; set `IDF_PATH` / `IDF_TOOLS_PATH` if ESP-IDF is not
at `C:\dev\esp-idf` / `C:\dev\espressif`):

```powershell
tools\build.ps1 freenove build               # or esp32cam / esp32s3cam
tools\build.ps1 freenove flash COM7          # the CH340 port of the Freenove board
tools\build.ps1 freenove monitor COM7        # serial console, Ctrl+] to exit
tools\build.ps1 freenove clean               # deletes firmware\build-freenove (about 170 MB)
```

`idf.py flash` never touches the NVS and `fctry` partitions (settings and identity survive re-flashing);
`idf.py erase-flash` and the `-install.bin` release image wipe them (a registered camera then needs a reset in FlyBot).

Same images as a release, by hand (from `firmware/build-freenove/`):

```bash
python -m esptool --chip esp32s3 merge_bin -o install.bin @flash_args     # write at 0x0
cp camos-esp32.bin update.bin; cp ota_data_initial.bin otadata.bin        # write at 0x20000 and 0x11000
```

## Build-time defaults

`firmware/defaults.example.json` holds the public defaults (FlyBot address, IoT endpoint, timezone, interval, power
mode, 4G on with the APN from the SIM, GPS and LED on) and no secrets. Copy it to `firmware/defaults.local.json`
(git-ignored) to build your bench Wi-Fi into your own images. Values set in the setup page or the web console override
the defaults; a factory reset returns to them. Settings layers, the local APIs, the serial console and the NVS keys:
[CONFIG.md](CONFIG.md).

## Options (menuconfig "CamOS trap camera")

`idf.py -B build-freenove menuconfig`. Every pin of [../WIRING.md](../WIRING.md) is an option (`CAMOS_MODEM_TX_GPIO`,
`CAMOS_GPS_POWER_GPIO`, `CAMOS_BATT_ADC_GPIO`, ...), plus the camera pins, XCLK, flips, warm-up frames, deep sleep and
awake window, the auto power-mode thresholds (`CAMOS_POWER_HIGH_MV` 4050 / `CAMOS_POWER_LOW_MV` 3850), battery low and
critical levels, the LED type and brightness, the microSD card (`CAMOS_SD`), the web console and `CAMOS_WEB_BENCH_IP`
(bench tests only, see [CONFIG.md](CONFIG.md)), and `CAMOS_LIVE` (cloud live view, on in the `freenove` preset: size,
frame rate, bitrate, the live view XCLK, powered-mode standby, bench keys: [LIVE_VIEW.md](LIVE_VIEW.md)).

## Tests

```bash
make -C tests          # NMEA, power hysteresis, registration response, APN table, motion detection, SigV4
```

Needs gcc and cJSON (ESP-IDF's copy when `IDF_PATH` is set, else `libcjson-dev`). CI runs them and builds the
`freenove` and `esp32cam` presets on every push (`.github/workflows/ci.yml`).

## Mock camera, screenshots and diagrams

- `node tools/mock/server.js` serves the setup page (`/portal`) and the web console (`/`) with fake data on
  http://localhost:8099/, to work on the pages without a board. Options for screenshots are at the top of the file.
- Screenshots in `docs/img/` were taken from the mock with headless Chrome, for example:
  `chrome --headless=new --window-size=1280,640 --screenshot=console-live.png "http://localhost:8099/#live"`
  (phone width: `http://localhost:8099/phone?src=%2Fportal`).
- Wiring diagram: `docs/img/wiring-overview.svg` is edited as SVG (the drawing of the field-camera wiring sheet); its
  PNG copy comes from the same headless Chrome command on the SVG. Keep its pins in step with the Kconfig defaults.
  `python tools/diagrams/wiring.py` draws `docs/img/trap-illustration.svg`.

## Releases

`git tag v0.4.0 && git push origin v0.4.0` runs `.github/workflows/release.yml`: it checks that the tag matches
`PROJECT_VER` in `firmware/CMakeLists.txt`, builds both presets with the public defaults, refuses an image with
`CAMOS_WEB_BENCH_IP` set, and publishes the GitHub Release (install / update / otadata images, ESP Web Tools manifests,
`SHA256SUMS`, notes from `.github/release-notes.md`) and the flasher page `docs/flash/` on GitHub Pages under `/flash/`.

One-time repository settings: Settings > Pages > Source: **GitHub Actions**; Settings > Environments > `github-pages` >
Deployment branches and tags: add a tag rule `v*`.

## Layout

- `firmware/main/`: `main.c` boot, `store.c` settings, `portal.c` + `portal.html`, `web.c` + `console.html`,
  `stream.c` (MJPEG), `sd.c`, `motion.c` + `motion_core.c`, `events.c`, `register.c` + `reg_parse.c`, `qr.c`, `led.c`,
  `net.c` / `wifi.c` / `modem.c` + `apn.c`, `flybot.c`, `proto.c`, `shadow.c`, `sched.c`, `cam.c`, `power.c` +
  `power_decide.c`, `battery.c`, `gps.c` + `nmea.c`, `ota.c`, `live.c`, `console.c`, `tz.c`, `util.c`.
- `firmware/defaults.example.json`, `sdkconfig.defaults*`, `partitions-{4,8,16}mb.csv` (with the `fctry` partition).
- `tests/`: host unit tests. `tools/`: build helper, factory provisioning, label QR, serial capture, mock camera,
  diagrams ([../../tools/README.md](../../tools/README.md)).
- `docs/`: user guides; `docs/dev/`: this page, the protocol, configuration, factory provisioning, power budget.
