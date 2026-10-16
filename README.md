# camos-esp32-trap

Open firmware for an ESP32-S3 (or plain ESP32) fruit-fly trap camera. It speaks the CamOS device protocol v1
([docs/protocol-v1.md](docs/protocol-v1.md), Vietnamese) over AWS IoT Core, uploads photos to FlyBot
([docs/PROTOCOL_ADDITIONS.md](docs/PROTOCOL_ADDITIONS.md)), runs on Wi-Fi or 4G (SIMCom A7680C), from solar + LiPo with
deep sleep, reports GPS (ATGM336H), and (ESP32-S3, work in progress) streams live view through Kinesis Video Streams
WebRTC. No AI on the device: the cloud counts the insects (OpenCV 5).

Built for the OpenCV AI Competition 2026 entry "FlyBot on AWS" (https://github.com/agrhub/flybot-aws). License: Apache-2.0.

## Zero-touch demo in 3 steps

The public build carries working defaults (FlyBot at https://flybot.agrhub.com, the demo IoT Core endpoint, timezone
automatic, 60-minute captures, power mode auto, 4G with the APN taken from the SIM, GPS and LED on) and no secrets. Each
camera makes its own key, asks FlyBot for a certificate and waits to be claimed (flybot-aws CONTRACTS.md 2a).

1. **Flash from the browser**: open the flasher page (GitHub Pages of this repo, `docs/flash/`) in Chrome or Edge, plug
   the board in by USB, press "Install". No ESP-IDF needed. (Images also on GitHub Releases.)
2. **Connect**: insert a 4G SIM, or join the `CAM-xxxx` Wi-Fi with a phone (password `camos1234`), pick your network in
   the list, type its password, press Connect. The camera registers itself with FlyBot.
3. **Claim**: sign in at flybot.agrhub.com, "Add camera", scan the QR shown on the camera's page (or type its serial and
   verify code). Until then the camera is "Unclaimed" and uploads at most one photo an hour.

The owner's own fleet can skip registration with a pre-provisioned certificate: `tools/factory.sh` (below).

## Features

- **Setup portal** (SoftAP `CAM-xxxx`, captive page, [docs/PORTAL.md](docs/PORTAL.md)), same look as the CamOS AMB82
  camera's setup page: a tappable Wi-Fi list from a scan made with the SoftAP up (signal bars, lock, rescan, hidden
  network), show-password toggle, "Connect"; a wrong password brings the portal back in about a minute with the error.
  Every other setting sits under "Advanced settings", pre-filled with its effective value and a "default" badge. English
  by default, Vietnamese one tap away (remembered on the camera and in the browser). Protocol 12.3 `GET /info` and
  `POST /provision` kept.
- **Web console** on Wi-Fi (`http://<camera IP>/`), the AMB82 console layout: admin activation / sign-in, Live View
  (JPEG preview, snapshot to FlyBot), Configuration (device, time, maintenance, log, Wi-Fi, 4G, cloud, image, power &
  schedule, GPS, account).
- **Self-registration**: ECDSA P-256 key and CSR made on the device, `POST /api/device/register`, back-off on 429 / 5xx,
  409 shown on the pages with a red LED; unclaimed mode follows FlyBot's `nextWakeAt`. Claim QR rendered on the device.
- **Network**: 4G first when on (esp_modem PPP, A76xx AT set with the SIM7600 profile, CMUX when the module accepts it,
  APN auto from the IMSI, [docs/APN.md](docs/APN.md)), else Wi-Fi. A board without a modem is remembered and skipped.
  MQTT to IoT Core (mutual TLS, SNI) and HTTPS to FlyBot on either. Modem powered down through `AT+CPOF` / PWRKEY before
  deep sleep.
- **Time, timezone, IP: automatic**: clock from SNTP, then the modem's network time (`AT+CCLK?` with NITZ), then GPS,
  then `GET /api/device/time`; the timezone (IANA + POSIX) and the public IP from `GET /api/device/time`; a manual
  override stays possible.
- **GPS**: NMEA RMC/GGA parser (host-tested), one fix a day or on RPC `gps.fix`, module power-gated, shadow `location`.
- **Power**: `power.mode` auto | battery | powered with hysteresis; telemetry carries `mv, pct, charging, solarMv`.
- **Snapshot**: `POST /api/device/snapshot` multipart with the contract `meta`, back-off retries, `nextWakeAt` honoured;
  triggers schedule, RPC `snapshot.now` / `capture`, boot.
- **Protocol v1** + additions (shadow `capture.intervalMin`, `power.*`, `live.requested`, `kvs.*`, `claimed`; RPC
  `snapshot.now`, `live.start`, `live.stop`, `gps.fix`, `reboot`, `ota.apply`, `net.set`, `factory_reset`; goodbye with
  `sleeping` / `wakeAt`). OTA with a SHA-256 check before the slot is made bootable.
- **Status LED** (WS2812 on GPIO48 of the Freenove board): blue breathe = setup, yellow blink = connecting, green blip =
  online, white flash = capture, red double blink = error, orange = low battery, purple = OTA ([docs/LED.md](docs/LED.md)).
- **Bench console** on UART0 (English): `info`, `scan`, `snap`, `frame`, `led`, `qr`, `csr`, `reg`, `net`, `at`, `gps` ...

## Boards

| Preset | Board | Target | Flash / PSRAM | Sensor | Build dir |
|---|---|---|---|---|---|
| `freenove` (G-S2 board) | Freenove ESP32-S3-WROOM CAM, kit FNK0085 | esp32s3 | 16 MB (owner's board; 8 MB variants: see the preset file) / 8 MB octal | OV5640 (OV2640 detected too) | `firmware/build-freenove` |
| `esp32cam` | AI-Thinker ESP32-CAM | esp32 | 4 MB / 4 MB quad | OV2640 | `firmware/build-esp32cam` |
| `esp32s3cam` | generic ESP32-S3 N16R8 CAM (ESP32-S3-EYE map) | esp32s3 | 16 MB / 8 MB octal | OV2640 / OV3660 / OV5640 | `firmware/build-esp32s3cam` |

The ESP32-CAM build has no modem, GPS or live view (`CAMOS_MODEM=n`, `CAMOS_GPS=n`): Wi-Fi + snapshot + deep sleep.
Pins, rails and the modem / GPS / solar wiring: [docs/WIRING.md](docs/WIRING.md). Parts: [docs/BOM.md](docs/BOM.md).
Consumption: [docs/POWER_BUDGET.md](docs/POWER_BUDGET.md).

## Build and flash

ESP-IDF v5.3 or newer (built with v5.4.4). The component manager fetches `esp32-camera`, `esp_modem`, `led_strip` and
`qrcode` on the first build (internet needed once).

Defaults: `firmware/defaults.example.json` (public demo values, no secrets). Copy it to `firmware/defaults.local.json`
(git-ignored) to bake your bench Wi-Fi into your own builds. Values set in the portal / console override the defaults;
factory reset returns to them.

Windows PowerShell (the helper sets up a minimal IDF environment; set `IDF_PATH` / `IDF_TOOLS_PATH` if ESP-IDF is not
at `C:\dev\esp-idf` / `C:\dev\espressif`):

```powershell
tools\build.ps1 freenove build               # or esp32cam / esp32s3cam
tools\build.ps1 freenove flash COM7          # the CH343 / CH340 port of the Freenove board
tools\build.ps1 freenove monitor COM7        # console, Ctrl+] to exit
tools\build.ps1 freenove clean               # frees ~170 MB
```

Linux / macOS (`. ~/esp/esp-idf/export.sh` first), from `firmware/`:

```bash
idf.py -B build-freenove -DSDKCONFIG=build-freenove/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.freenove_esp32s3_cam" set-target esp32s3 build
idf.py -B build-freenove -p /dev/ttyUSB0 flash monitor
```

`idf.py flash` never touches the NVS partition (configuration and identity survive re-flashing); `idf.py erase-flash`
and the browser "install" image wipe it (a registered camera then needs a reset in FlyBot: use the "update" image).
Releases: push a tag `v*` and `.github/workflows/release.yml` builds both boards, attaches the images and publishes the
flasher page (Settings > Pages > Source: GitHub Actions).

## First run (portal walkthrough)

Screenshots to be taken on a phone (placeholders below). The page is in English; "Tiếng Việt" at the top right switches it.

1. Flash, open the monitor: `serial ESP..., verify code ...`. With no default Wi-Fi and no SIM, the camera tries 4G,
   finds no modem (about 30 s, remembered) and opens `setup portal: Wi-Fi CAM-xxxx` (LED blue, breathing).
2. Join `CAM-xxxx` (password `camos1234`); the page `http://192.168.4.1/` opens by itself.
   ![portal: network list](docs/img/portal-1.png) *(placeholder: "Connect the camera to Wi-Fi", the Unclaimed box with the
   claim QR, serial and verify code, the list of networks with bars and locks)*
3. Tap your network (the password field gets the focus, "Show" reveals it), or "Other network (hidden)…".
   ![portal: password](docs/img/portal-2.png) *(placeholder: the chosen network highlighted, password, Connect)*
4. Optional: "Advanced settings" (4G, schedule & power, timezone override, LED, GPS, FlyBot address, provisioning token,
   IoT Core endpoint, certificate + key). Every field shows its value; "default" marks the build defaults.
   ![portal: advanced](docs/img/portal-3.png) *(placeholder)*
5. "Connect": "Connecting…"; the setup network closes. The monitor shows `got IP`, `clock set from sntp`, `timezone ...
   from ...`, `register: ...`, and once FlyBot answers `registered: ... unclaimed`, `connected to mqtts://...`.
   ![portal: connecting](docs/img/portal-4.png) *(placeholder)*
6. Claim the camera in FlyBot with the QR. Then `http://<camera IP>/` opens the web console (first visit sets the admin
   password).

## Layout

- `firmware/main/`: `main.c` boot, `store.c` settings (defaults / factory / NVS), `portal.c` + `portal.html`, `web.c` +
  `console.html`, `register.c` + `reg_parse.c`, `qr.c`, `led.c`, `net.c` / `wifi.c` / `modem.c` + `apn.c`, `flybot.c`,
  `proto.c`, `shadow.c`, `sched.c`, `cam.c`, `power.c` + `power_decide.c`, `battery.c`, `gps.c` + `nmea.c`, `ota.c`,
  `live.c`, `console.c`, `tz.c`, `util.c`, `Kconfig.projbuild`, `camos_defaults.h.in`.
- `firmware/defaults.example.json`, `sdkconfig.defaults*`, `partitions-{4,8,16}mb.csv` (with the `fctry` partition).
- `tests/`: host unit tests (`make -C tests`: NMEA, power hysteresis, registration response, APN table, CCLK).
- `tools/`: `build.ps1`, `factory.sh` / `factory.ps1`, `provision.sh`, `label.py`, `serial_log.py`, `PINPROBE.md`.
- `docs/`: protocol v1, PROTOCOL_ADDITIONS, PORTAL, LED, APN, WIRING, BOM, POWER_BUDGET, LIVE_VIEW, `flash/` (browser flasher).

## Factory provisioning (owner's alternative)

`bash tools/factory.sh COM7` reads the MAC, makes a key and a self-signed certificate (CN = serial) or takes
`--cert/--key`, builds the read-only `fctry` NVS image (serial, verify code, certificate, key, thing, claimed) and
flashes it at 0x14000. It prints the flybot-aws command that registers the certificate in AWS IoT Core (run it with the
owner's AWS profile: it creates resources). A camera with a factory certificate never self-registers.

## Status (16 Oct 2026)

Builds for the Freenove ESP32-S3, the generic ESP32-S3 CAM and the ESP32-CAM presets. Verified on the Freenove board:
boot, 8 MB PSRAM, 16 MB flash, OV5640 capture, portal with the Wi-Fi scan, LED states (from the log), CSR (checked with
openssl), claim QR, console commands. Registration needs FlyBot's `/api/device/register` (not deployed yet). The modem,
GPS and solar parts are on order. Host tests pass. Bench checklist: flybot-aws docs/plan/STATUS_E.md.
