# camos-esp32-trap

Open firmware for an ESP32-S3 (or plain ESP32) fruit-fly trap camera. It speaks the CamOS device protocol v1
([docs/protocol-v1.md](docs/protocol-v1.md), Vietnamese) over AWS IoT Core, uploads photos to FlyBot
([docs/PROTOCOL_ADDITIONS.md](docs/PROTOCOL_ADDITIONS.md)), runs on Wi-Fi or 4G (SIMCom A7680C), from solar + LiPo with
deep sleep, reports GPS (ATGM336H), and (ESP32-S3, work in progress) streams live view through Kinesis Video Streams
WebRTC. No AI on the device: the cloud counts the insects (OpenCV 5).

Built for the OpenCV AI Competition 2026 entry "FlyBot on AWS" (https://github.com/agrhub/flybot-aws). License: Apache-2.0.

## Features

- **Setup portal** (SoftAP `CAM-xxxx`, captive page + JSON API, [docs/PORTAL.md](docs/PORTAL.md)): Wi-Fi, APN, FlyBot
  URL, IoT endpoint, certificate + key upload, pairing (`POST /api/device/pair`), capture interval, `power.mode`, timezone.
  Nothing but the board pin preset and the Amazon Root CA is fixed at build time. Protocol 12.3 `GET /info` and
  `POST /provision` kept.
- **Network**: 4G first when an APN is set (esp_modem PPP, A76xx AT set with the SIM7600 profile, CMUX when the module
  accepts it), else Wi-Fi; MQTT to IoT Core (mutual TLS, SNI) and HTTPS to FlyBot on either. Modem powered down through
  `AT+CPOF` / PWRKEY before deep sleep. SNTP with `GET /api/device/time` as the fallback.
- **GPS**: NMEA RMC/GGA parser (host-tested), one fix a day or on RPC `gps.fix`, module power-gated, reported as the
  shadow `location`; sets the clock when NTP is not there yet.
- **Power**: `power.mode` auto | battery | powered. Auto switches by cell voltage with hysteresis (Kconfig and shadow
  thresholds); telemetry carries `mv, pct, charging, solarMv`. Battery mode = scheduled capture + deep sleep; powered =
  MQTT always on, snapshot and live on demand.
- **Snapshot**: `POST /api/device/snapshot` multipart with the contract `meta` JSON, back-off retries, `nextWakeAt`
  honoured; triggers: schedule (`capture.intervalMin` or protocol `detections[]`), RPC `snapshot.now` / `capture`, boot.
- **Protocol v1** + additions: shadow keys `capture.intervalMin`, `power.mode`, `live.requested`, `kvs.channelName`;
  RPC `snapshot.now`, `live.start`, `live.stop`, `gps.fix`, `reboot`, `ota.apply`, `net.set`, `factory_reset`; goodbye
  with `sleeping` / `wakeAt`. OTA from `GET /api/device/ota` with a SHA-256 check before the slot is made bootable.
- **Bench console** on UART0: every module has a command (`snap`, `pair`, `net`, `at`, `gps`, `batt`, `mode`, `sleep` ...).
- **Live view**: see [docs/LIVE_VIEW.md](docs/LIVE_VIEW.md) for the state; `snapshot.now` is the fallback.

## Boards

| Preset | Board | Target | Flash / PSRAM | Sensor | Build dir |
|---|---|---|---|---|---|
| `freenove` (G-S2 board) | Freenove ESP32-S3-WROOM CAM, kit FNK0085 | esp32s3 | 16 MB (owner's board; 8 MB variants: see the preset file) / 8 MB octal | OV5640 (OV2640 detected too) | `firmware/build-freenove` |
| `esp32cam` | AI-Thinker ESP32-CAM | esp32 | 4 MB / 4 MB quad | OV2640 | `firmware/build-esp32cam` |
| `esp32s3cam` | generic ESP32-S3 N16R8 CAM (ESP32-S3-EYE map) | esp32s3 | 16 MB / 8 MB octal | OV2640 / OV3660 / OV5640 | `firmware/build-esp32s3cam` |

The ESP32-CAM build has no modem, GPS or live view (`CAMOS_MODEM=n`, `CAMOS_GPS=n`): Wi-Fi + snapshot + deep sleep.
Pins, rails and the modem / GPS / solar wiring: [docs/WIRING.md](docs/WIRING.md). Parts: [docs/BOM.md](docs/BOM.md).
Consumption estimates: [docs/POWER_BUDGET.md](docs/POWER_BUDGET.md).

## Build and flash

ESP-IDF v5.3 or newer (built with v5.4.4). The component manager fetches `espressif/esp32-camera` and
`espressif/esp_modem` on the first build (internet needed once).

Windows PowerShell (the helper sets up a minimal IDF environment; set `IDF_PATH` / `IDF_TOOLS_PATH` if ESP-IDF is not
at `C:\dev\esp-idf` / `C:\dev\espressif`):

```powershell
tools\build.ps1 freenove build               # or esp32cam / esp32s3cam
tools\build.ps1 freenove flash COM7          # the CH340 port of the Freenove board
tools\build.ps1 freenove monitor COM7        # console, Ctrl+] to exit
tools\build.ps1 freenove clean               # frees ~150 MB
```

Linux / macOS (`. ~/esp/esp-idf/export.sh` first), from `firmware/`:

```bash
idf.py -B build-freenove -DSDKCONFIG=build-freenove/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.freenove_esp32s3_cam" set-target esp32s3 build
idf.py -B build-freenove -p /dev/ttyUSB0 flash monitor
# ESP32-CAM: -B build-esp32cam ... sdkconfig.defaults.esp32cam ... set-target esp32
```

`idf.py menuconfig` > "CamOS trap camera" holds every pin and threshold. `idf.py flash` never touches the NVS
partition (configuration survives re-flashing); `idf.py erase-flash` wipes it.

## First run (portal walkthrough)

1. Flash, open the monitor. The boot log prints `serial ESP..., verify code ...` and `setup portal: Wi-Fi CAM-xxxx`.
2. Join `CAM-xxxx` (password `camos1234`) with a phone or laptop; the page `http://192.168.4.1/` opens.
   ![portal: network](docs/img/portal-1.png) *(screenshot placeholder)*
3. Wi-Fi name + password (or the APN), FlyBot URL, provisioning token (from FlyBot "add camera", which scans the label QR
   printed by `tools/label.py <serial> <verify code>`), interval, power mode, timezone.
   ![portal: FlyBot](docs/img/portal-2.png) *(screenshot placeholder)*
4. Paste or upload the device certificate and private key, "Store certificate + key" (`tools/provision.sh` creates the
   pair and registers it in AWS IoT; FlyBot can also hand them out at pairing in a later version).
   ![portal: certificate](docs/img/portal-3.png) *(screenshot placeholder)*
5. "Save and pair now": the camera joins your Wi-Fi, pairs, shows the thing name and KVS channel. "Restart".
   ![portal: paired](docs/img/portal-4.png) *(screenshot placeholder)*
6. The monitor shows `got IP`, `clock`, `connected to mqtts://...`, the retained `state` on `camos/<serial>/state`, the
   boot snapshot (`trigger: "boot"`), then the schedule. In battery mode: `deep sleep N s`.

Everything the portal does can also be done from the console: `set ssid ...`, `set apiBase ...`, `cert`, `key`,
`pair <token>`, `snap`.

## Layout

- `firmware/main/`: `main.c` boot, `store.c` NVS, `portal.c` + `portal.html`, `net.c` / `wifi.c` / `modem.c`,
  `flybot.c` device API, `proto.c` MQTT, `shadow.c`, `sched.c` capture + sleep, `cam.c`, `power.c` + `power_decide.c`,
  `battery.c`, `gps.c` + `nmea.c`, `ota.c`, `live.c`, `console.c`, `tz.c`, `util.c`, `Kconfig.projbuild`.
- `firmware/sdkconfig.defaults*`, `partitions-{4,8,16}mb.csv`.
- `tests/`: host unit tests (`make -C tests`: NMEA parser, power-mode hysteresis).
- `tools/`: `build.ps1`, `provision.sh` (key + certificate + NVS image), `label.py` (label QR), `PINPROBE.md`.
- `docs/`: protocol v1, PROTOCOL_ADDITIONS, PORTAL, WIRING, BOM, POWER_BUDGET, LIVE_VIEW.

## Status (16 Oct 2026)

Builds for the Freenove ESP32-S3, the generic ESP32-S3 CAM and the ESP32-CAM presets. Verified on the Freenove board on 16 Oct 2026: boot,
8 MB PSRAM, OV5640 capture (2560x1920 JPEG), portal SoftAP, console commands (docs/POWER_BUDGET.md section 0 has the timings). The modem, GPS and solar parts are on order; nothing has talked to FlyBot or IoT Core yet. Host tests pass. See the flybot-aws plan
(docs/plan/STATUS_E.md) for the bench checklist.
