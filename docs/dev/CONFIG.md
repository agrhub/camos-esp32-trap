# Settings, local APIs and the serial console

## 1. Where settings come from

Nothing but the board pin preset and the Amazon Root CA is fixed at build time. Settings come in three layers: build
defaults (`firmware/defaults.example.json`, or a git-ignored `defaults.local.json`), the factory partition written by
`tools/factory.*`, and the runtime NVS set by the portal, the web console, the serial console, pairing and registration.
The pages show every value pre-filled and mark build defaults with a "default" badge. Both pages and the flasher page
are English by default; the language link switches to Vietnamese and the choice is remembered on the camera (NVS
`ui_lang`, key `uiLang`) and in the browser (`localStorage`).

## 2. Setup portal API (http://192.168.4.1)

| Call | Body / result |
|---|---|
| `GET /api/config` | every setting with its effective value; secrets (`password`, `simPin`, `provisionToken`, `token`, `apPassword`) as `true`/`false`; `defaults` (keys that are build defaults), `cert`, `key`, `paired`, `registered`, `claimed`, `hasModem`, `hasGps`, `hasLive`, `serial`, `verifyCode`, `labelQr`, `claimUrl`, `apSsid`, `wifiError`, `regError`, `timezoneAuto`, `timezoneSource`, `uiLang`, `model`, `fw`, `hostname` (read-only: the local name without `.local`) |
| `GET /api/wifi/scan[?refresh=1]` | `[{ssid, rssi, secure, channel}]`, deduplicated by SSID, strongest first; cached from the scan at portal start, `refresh=1` scans again |
| `POST /api/connect` | `{ssid, password, ...any keys}`: save and restart onto that network. `400 CHOOSE_NETWORK`, `400 SHORT_PASSWORD`, `400 BAD_VALUE` |
| `POST /api/config` | `{key: value, ...}` with any of `ssid password apn simPin cellular apiBase provisionToken token iotHost thingName kvsChannel kvsRegion roleAlias credHost timezone intervalMin powerMode ledEnabled gps batteryMonitor apPassword uiLang` (an empty string = back to the default) |
| `POST /api/cert` | `{"cert": "<PEM>", "key": "<PEM>"}` (each under 6000 bytes) |
| `POST /api/pair` | pre-provisioned path: joins the Wi-Fi with the AP up and calls `POST /api/device/pair` with the provisioning token |
| `POST /api/restart` | restart |
| `GET /api/qr.svg?what=claim\|label` | the claim URL QR or the label QR, SVG made on the device |
| `GET /info` | protocol 12.3: `{serial, model, fw, wifiScan: [{ssid, rssi, secure}]}` |
| `POST /provision` | protocol 12.3: `{ssid, password, provisionToken?, endpoint?, apn?, apiBase?}` -> `{"ok":true}`, restarts |
| `POST /api/config` (new keys in 0.4.0) | `sdSave`, `schedFrom`, `schedTo` (minutes after midnight), `motion`, `motionSens` (1-100), `motionCooldown` (s), `motionRoi` (`"x,y,w,h"` percent), `motionSd`, `motionUpload`, `ipMode` (0 DHCP, 1 static), `staticIp`, `staticMask`, `staticGateway`, `staticDns` (also accepted by the web console) |

## 3. Web console API (http://<camera IP>/)

Every call needs a session cookie from `POST /api/activate` (first visit) or `POST /api/login`; every POST needs the
header `X-Requested-With: camos`. Details at the top of `firmware/main/web.c`.

| Call | What it does |
|---|---|
| `GET /api/session` | `{activated, signedIn, model, serial, fw, uiLang}` (no sign-in needed) |
| `POST /api/activate {password}`, `/api/login {password}`, `/api/logout`, `/api/password {current, new}`, `/api/lang {uiLang}` | admin password and session |
| `GET /api/status` | device, time, network, cloud, image, stream (fps, frame size), motion, SD card, power, battery, location, LED |
| `GET|POST /api/config`, `POST /api/cert`, `GET /api/wifi/scan` | as in the portal |
| `GET /api/image`, `POST /api/image {image: {...}, jpegQuality, save}` | image settings (shadow `video.image` keys); `save: false` previews only; `{restore: true}` |
| `GET /api/stream?w=&h=&q=` | MJPEG live picture, one viewer at a time (409 `BUSY` for the next) |
| `GET /api/frame.jpg?w=&h=` | one preview frame |
| `POST /api/snapshot` | full-size capture + upload (`sdFile` when also saved on the card) |
| `GET /api/schedule`, `GET /api/events` | effective schedule and next captures; the last 64 events |
| `GET /api/sd`, `GET /api/sd/files?day=`, `GET /api/sd/file?day=&name=[&dl=1]`, `POST /api/sd/delete {day[, name]}`, `POST /api/sd/format` | microSD photos |
| `POST /api/firmware` | app image as `application/octet-stream`, optional `X-Fw-Sha256: <hex>`; installs and restarts |
| `POST /api/ota`, `/api/gps`, `/api/pair`, `/api/reboot`, `/api/factory-reset`; `GET /api/logs?since=`, `GET /api/qr.svg` | |

`CAMOS_WEB_BENCH_IP` (Kconfig, empty by default): requests from exactly that client address count as signed in, for
automated tests from one bench computer. The boot log warns `BENCH BUILD` when it is set; the release workflow refuses
to publish an image with it set.

## 4. Serial console (UART0, 115200)

Available in normal operation and inside the portal; English. `help` lists the commands:

| Command | What it does |
|---|---|
| `info` | serial, verify code, label QR text, pairing state, network, IP, clock and its source, timezone and its source, public IP, claim state, battery, power mode, next capture, location |
| `cfg` | the configuration as JSON (secrets as set / not set) |
| `set <key> <value>` | the portal keys (`set apn v-internet`, `set powerMode battery`, `set uiLang vi`, `set batteryMonitor 1`) |
| `cert`, `key` | paste a PEM, finish with a line `END` |
| `scan` | Wi-Fi scan (in the portal with the SoftAP up) |
| `net` | bring the network up and print IP, RSSI, cell info |
| `reg [start]`, `csr` | registration / claim state; the CSR registration sends (the device key is made once) |
| `qr [label]` | the claim URL (or label) as a QR on the console |
| `pair [token]` | owner path: pair with a provisioning token |
| `time [flybot]` | clock state; `time flybot` asks `GET /api/device/time` |
| `snap`, `frame [w h [q]]`, `meta` | capture + upload; one JPEG at a size with timings; the snapshot meta JSON |
| `batt`, `mode auto\|battery\|powered` | battery / solar, power decision; power mode |
| `gps [timeout]`, `at <AT command>` | GPS fix; talk to the modem |
| `led [state]` | show an LED state (`portal`, `connecting`, `online`, `error`, `ota`, `off`, `capture`, `uploaderr`, `lowbatt`) |
| `live start [min]`, `live stop`, `ota`, `awake [s]`, `sleep [s]`, `portal`, `factory`, `restart` | |

The commands run in the console task, so `snap` or `pair` block the prompt until they finish. With automatic light
sleep on (station mode), the first byte typed after a quiet period wakes the chip and may be lost; after a command the
chip stays awake 5 minutes (`tools/serial_log.py` sends a newline first for that reason).

## 5. NVS keys (namespace `camos`)

Strings: `serial`, `vcode`, `wifi_ssid`, `wifi_pass`, `apn`, `sim_pin`, `api_base`, `token`, `prov_token`, `mqtt_host`,
`thing`, `kvs_chan`, `kvs_region`, `role_alias`, `cred_host`, `claim_url`, `tz`, `tz_auto`, `tz_posix`, `tz_src`,
`ui_lang`, `ap_pass`, `wifi_err`, `reg_err`, `admin_salt`, `admin_hash`, `mo_roi`, `st_ip`, `st_mask`, `st_gw`,
`st_dns`, `cert`, `key` (PEM). Integers: `interval`, `power_mode` (0 auto, 1 battery, 2 powered), `cellular`, `gps`,
`led`, `batt_mon`, `claimed`, `sd_save`, `sched_from`, `sched_to`, `motion`, `mo_sens`, `mo_cool`, `mo_upload`, `mo_sd`,
`ip_mode`, `wifi_new`, `modem_absent`, `gps_absent`, `portal`, `boots`. Blobs: `settings` (the persisted shadow), `gps_fix`.
The factory partition `fctry` (0x14000, 0x6000, read-only) uses the same namespace and keys (`serial`, `vcode`, `cert`,
`key`, `thing`, `mqtt_host`, `claimed`). `idf.py flash` never touches either partition; `idf.py erase-flash` and the
browser "install" image wipe both; `factory` (console, web console, RPC) keeps the identity keys.
