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
| `GET /api/config` | every setting with its effective value; secrets (`password`, `simPin`, `provisionToken`, `token`, `apPassword`) as `true`/`false`; `defaults` (keys that are build defaults), `cert`, `key`, `paired`, `registered`, `claimed`, `hasModem`, `hasGps`, `hasLive`, `serial`, `verifyCode`, `labelQr`, `claimUrl`, `apSsid`, `wifiError`, `regError`, `timezoneAuto`, `timezoneSource`, `uiLang`, `model`, `fw`, `hostname` (read-only: the local name without `.local`), `usbMode` (`auto` default, `storage`, `webcam`, `off`; applied at boot) |
| `GET /api/wifi/scan[?refresh=1]` | `[{ssid, rssi, secure, channel}]`, deduplicated by SSID, strongest first; cached from the scan at portal start, `refresh=1` scans again |
| `POST /api/connect` | `{ssid, password, ...any keys}`: save the other keys, then join that network with the AP still up: `{ok, checking: true, ssid, timeoutS: 30}`; the network is stored only once it gave an IP (an empty password = the stored one of the same network). With 4G on: save and restart (`{ok}`). `400 CHOOSE_NETWORK`, `400 SHORT_PASSWORD`, `400 BAD_VALUE`, `409 BUSY` |
| `GET /api/connect` | `{state: idle\|joining\|ok\|failed, ssid, elapsedS, timeoutS, reason: auth\|not_found\|timeout, ip, hostname}`; `ok`: the camera restarts onto the network 5 s later; `failed`: the portal stays (the 10 minutes start again) |
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
| `GET /api/status` | device, time, network, cloud, live (cloud live view: supported, window, state, viewer, fps, kbit/s), image, stream (fps, frame size), motion, USB port (`usb`: mode, setting, connected, idle, cardLent, streaming, width, height), SD card, power, battery, location, LED |
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
| `connect [ssid [password]]` | in the portal: the page's Connect check (AP up, stored once it gets an IP); no arguments = the stored network |
| `net` | bring the network up and print IP, RSSI, cell info |
| `reg [start]`, `csr` | registration / claim state; the CSR registration sends (the device key is made once) |
| `qr [label]` | the claim URL (or label) as a QR on the console |
| `pair [token]` | owner path: pair with a provisioning token |
| `time [flybot]` | clock state; `time flybot` asks `GET /api/device/time` |
| `snap`, `frame [w h [q]]`, `meta` | capture + upload; one JPEG at a size with timings; the snapshot meta JSON |
| `batt`, `mode auto\|battery\|powered` | battery / solar, power decision; power mode |
| `usb`, `usb auto\|storage\|webcam\|off` | native USB port state (mode, connected, idle, cardLent, streaming); set `usbMode` for the next boot |
| `gps [timeout]`, `at <AT command>` | GPS fix; talk to the modem |
| `led [state]` | show an LED state (`portal`, `connecting`, `online`, `error`, `ota`, `off`, `capture`, `uploaderr`, `lowbatt`) |
| `live`, `live start [min]`, `live stop` | cloud live view (ESP32-S3, `CAMOS_LIVE`): status JSON (state, viewer, fps, kbit/s, encode / camera ms, last error); open a window; drop the viewer and end the window. With `CAMOS_LIVE_BENCH_CREDS`: `live key id\|secret\|token <value>` / `live key clear` (bench keys in RAM, see [LIVE_VIEW.md](LIVE_VIEW.md)) |
| `ota`, `awake [s]`, `sleep [s]`, `portal`, `factory`, `restart` | |

The commands run in the console task, so `snap` or `pair` block the prompt until they finish. With automatic light
sleep on (station mode), the first byte typed after a quiet period wakes the chip and may be lost; after a command the
chip stays awake 5 minutes (`tools/serial_log.py` sends a newline first for that reason).

## 5. NVS keys (namespace `camos`)

Strings: `serial`, `vcode`, `wifi_ssid`, `wifi_pass`, `apn`, `sim_pin`, `api_base`, `token`, `prov_token`, `mqtt_host`,
`thing`, `kvs_chan`, `kvs_region`, `role_alias`, `cred_host`, `claim_url`, `tz`, `tz_auto`, `tz_posix`, `tz_src`,
`ui_lang`, `ap_pass`, `wifi_err`, `reg_err`, `cap_size`, `admin_salt`, `admin_hash`, `mo_roi`, `st_ip`, `st_mask`, `st_gw`,
`st_dns`, `cert`, `key` (PEM). Integers: `interval`, `power_mode` (0 auto, 1 battery, 2 powered), `cellular`, `gps`,
`led`, `batt_mon`, `claimed`, `sd_save`, `sched_from`, `sched_to`, `motion`, `mo_sens`, `mo_cool`, `mo_upload`, `mo_sd`,
`ip_mode`, `wifi_new`, `modem_absent`, `gps_absent`, `portal`, `boots`, `cam_pid` (the detected sensor), `reg_retry` / `reg_taken_s` (the next
registration question after a 409, epoch s, and its back-off), `rereg_at` (the last registration again after removal). Blobs: `settings` (the persisted shadow), `gps_fix`.
The factory partition `fctry` (0x14000, 0x6000, read-only) uses the same namespace and keys (`serial`, `vcode`, `cert`,
`key`, `thing`, `mqtt_host`, `claimed`). `idf.py flash` never touches either partition; `idf.py erase-flash` and the
browser "install" image wipe both; `factory` (console, web console, RPC) keeps the identity keys.

## 6. Settings from FlyBot (shadow `desired`)

FlyBot publishes `desired` (retained) on `camos/<serial>/shadow/desired`; the camera applies every key it knows,
publishes `reported` with the same `version`, and lists the rest in `reported.errors` as `{"path","code"}`:
`NOT_SUPPORTED` (unknown key) or `BAD_VALUE` (wrong type or out of range; the old value stays). Keys left out keep their
value. A desired value overwrites what was set on the camera itself (web console, serial console), and the retained
document is applied again at every MQTT connect, so FlyBot's form is the source of truth for the keys it sends.

| Key | Type / range | Effect | In `reported` |
|---|---|---|---|
| `timezone` | IANA name (`"Asia/Ho_Chi_Minh"`), or `null` | schedules run in this local time; `null` = automatic (FlyBot's `/api/device/time`, then the IP) | `timezone`, `tzSource` |
| `detections[]` | `{id, type:"app", enabled, trigger:{everyMin 1..1440, from "HH:MM", to "HH:MM"}}`, up to 4 | capture schedules; other types `NOT_SUPPORTED` | `nextCaptureAt` |
| `capture.intervalMin` | 1..1440, or `null` | all-day interval when no `detections[]` is set; `null` = portal / default (60) | `capture.intervalMin` |
| `capture.window` | `{from:"HH:MM", to:"HH:MM"}`, or `null` (all day) | local window of `capture.intervalMin` (`to` before `from` crosses midnight) | `capture.window` |
| `capture.jpegQuality` | 1..100 (higher = better) | photo JPEG quality | `capture.jpegQuality` |
| `capture.resolution` | `"max"`, `"main"` (= max), or `"WxH"` of one entry of `camera.photoSizes` | photo size; any other size is `BAD_VALUE` | `capture.resolution`, `capture.w`, `capture.h` (the size a photo gets) |
| `video.image.brightness` | -64..64 | sensor brightness | `video.image.*` (every key below) |
| `video.image.contrast` | 0..100 | | |
| `video.image.saturation` | 0..100 | | |
| `video.image.sharpness` | 0..100 | | |
| `video.image.orientation` | 0 normal, 1 mirror, 2 flip, 3 both | relative to the board's upright picture | |
| `video.image.vflip` | bool | upside down: orientation bit 2 | `video.image.vflip` |
| `video.image.hmirror` | bool | left-right mirror: orientation bit 1 | `video.image.hmirror` |
| `video.image.whiteBalance` | `"auto"`, `"sunny"`, `"cloudy"`, `"office"`, `"home"` | | |
| `video.image.effect` | `"none"`, `"negative"`, `"grayscale"`, `"red"`, `"green"`, `"blue"`, `"sepia"` | | |
| `video.image.aeLevel` | -5..5 (OV5640; others -2..2) | exposure compensation | |
| `video.image.exposureAuto`, `video.image.exposure` | bool, 0..1200 | automatic exposure, or the manual value | |
| `video.image.gainAuto`, `video.image.gain` | bool, 0..30 | automatic gain, or the manual value | |
| `video.image.gainCeiling` | 0..6 (2x..128x), default 4 (32x) | most gain auto gain may use; OV5640: 64x and 128x are both its 64x maximum | |
| `video.image.denoise` | 0..8 | | |
| `video.image.nightMode` | bool, default false | live view and web console stream only (OV5640): let the auto exposure stretch a frame up to 4x in a very dark place; slower and smeared on movement, so off unless the 32x gain is not enough | |
| `power.mode` | `"auto"`, `"battery"`, `"powered"`, or `null` (portal value) | see PROTOCOL_ADDITIONS.md section 6 | `power.mode`, `power.effective` |
| `power.highMv`, `power.lowMv` | 3000..4400 | auto-mode thresholds | `power.highMv`, `power.lowMv` |
| `motion.enabled` | bool | frame-difference motion detection (powered mode only) | `motion.enabled` |
| `motion.sensitivity` | 1..100 (100 = most sensitive) | | `motion.sensitivity` |
| `motion.cooldownS` | 5..3600 | seconds between two triggers | `motion.cooldownS` |
| `motion.upload` | bool | a trigger also captures and uploads a full photo | `motion.upload` |
| `motion.saveSd` | bool | a trigger saves the picture to the microSD card | `motion.saveSd` |
| `motion.area` | `[x, y, w, h]`, percent 0..100 of the picture | detection area | `motion.area` |
| `storage.sdSave` | bool | every photo also on the microSD card | `storage.sdSave` |
| `led.enabled` | bool | the status LED | `led.enabled` |
| `gps.enabled` | bool | daily GPS fixes when a module is fitted | `gps.enabled` (shadow only; `state` keeps its protocol 4 `gps` fix) |
| `console.language` | `"en"`, `"vi"` | language of the camera's own web pages | `console.language` |
| `claimed` | bool | claim state (CONTRACTS 2a.4) | `claimed` |
| `live.requested` | bool | a live session at the next wake (5 min) | `live.requested`, `live.supported`, `live.active` |
| `kvs.channelName`, `kvs.region`, `kvs.credentialsEndpoint`, `kvs.roleAlias` | string | live view channel and credential provider (FlyBot fills these) | `kvs.channelName`, `kvs.region` |

**Orientation.** `vflip` and `hmirror` are the two bits of `orientation` (`orientation` = `hmirror` + 2 x `vflip`), so
either form works and `reported` carries all three; when one document has both, the key that comes later wins. All
three are relative to the board's upright picture: the build preset sets the sensor's own flip and mirror
(`CONFIG_CAMOS_CAM_VFLIP`, `CONFIG_CAMOS_CAM_HMIRROR`; the Freenove preset turns them on so its OV5640 is upright as
mounted), and `false` / `false` / `0` is that picture. They apply to photos, the web console stream and the live view.

**Camera and sizes (`reported.camera`, also in `state` and the web console's `/api/status`).** The sizes follow the
camera module the board detected (esp32-camera's sensor table; the sensor's PID is kept in NVS `cam_pid`, so they are
known at boot; before the very first power-up of a new board `sensor` is `""`, `maxW` 0 and both lists hold every size):

```json
"camera":{"sensor":"OV5640","maxW":2560,"maxH":1920,
 "photoSizes":[[640,480],[800,600],[1024,768],[1280,720],[1280,1024],[1600,1200],[1920,1080],[2048,1536],[2560,1440],[2560,1600],[2560,1920]],
 "liveSizes":[[320,240],[640,480],[800,600],[1024,768],[1280,720],[1280,1024],[1600,1200]]}
```

- `photoSizes`: from 640x480 to the sensor's maximum (OV2640 1600x1200, OV3660 2048x1536, OV5640 2560x1920: the
  largest esp32-camera drives on that sensor). `capture.resolution` takes `"max"` or one of them.
- `liveSizes`: from 320x240 to 1600x1200, within the maximum: the web console stream (`/api/stream?w=&h=&q=`) and the
  live view data channel (`hello.sizes`, `quality` requests) offer exactly these. JPEG quality of both: 8..40 on the
  esp32-camera scale (lower = better; `hello.qRange`).
- `liveFps` is not reported: the frame rate depends on the link more than on the size (LIVE_VIEW.md has measurements).

**Last reset (`reported.lastReset`, also in `state` and `/api/status`)**, so FlyBot can tell a reset from a sleep:

```json
"lastReset":{"reason":"software","detail":"watchdog","at":"2026-10-18T07:59:21Z","uptimeS":66}
```

- `reason` from the chip: `power_on`, `external`, `software`, `crash`, `int_watchdog`, `task_watchdog`, `watchdog`,
  `brownout`, `usb`, `jtag`, `unknown`. A flash over USB shows as `power_on`.
- `detail`, only for `software`: the firmware's own reason, `ota`, `rpc` (restart from FlyBot or the console),
  `watchdog` (no MQTT for `CONFIG_CAMOS_NET_WATCHDOG_MIN` minutes), `reregister` (removed in FlyBot).
- `at`: when it happened (`null` until the clock is set); `uptimeS`: seconds since then. Deep sleep wakes keep the
  last real reset (RTC memory), so a battery camera still reports it after many wakes.

`NOT_SUPPORTED` (on purpose): the network (`net.*`, Wi-Fi, APN, SIM PIN, static IP: a wrong value would cut the camera
off; use RPC `net.set`), the web console password, `video.main` / `video.sub` / `osd` / `nightMode` / `flicker` /
`wdr` / `ldc` (no such control on these sensors), `record` and `audio` (no recording, no microphone).

Example:

```json
{"version":7,"timezone":"Asia/Ho_Chi_Minh",
 "capture":{"intervalMin":30,"window":{"from":"06:00","to":"18:00"},"jpegQuality":85,"resolution":"1600x1200"},
 "video":{"image":{"brightness":0,"contrast":50,"saturation":60,"vflip":false,"hmirror":false}},
 "power":{"mode":"auto"},
 "motion":{"enabled":true,"sensitivity":60,"cooldownS":120,"upload":false,"saveSd":true,"area":[10,10,80,80]},
 "storage":{"sdSave":true},"led":{"enabled":false},"gps":{"enabled":true},"console":{"language":"vi"}}
```
