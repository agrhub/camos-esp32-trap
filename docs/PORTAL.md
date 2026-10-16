# Setup portal, web console and bench console

Nothing but the board pin preset and the Amazon Root CA is fixed at build time. Settings come in three layers: build
defaults (`firmware/defaults.example.json`, or a git-ignored `defaults.local.json`), the factory partition written by
`tools/factory.*`, and the runtime NVS set by the portal, the web console, the serial console, pairing and registration.
The pages show every value pre-filled and mark build defaults with a "default" badge. Both pages and the flasher page
are English by default; the language link switches to Vietnamese and the choice is remembered on the camera (NVS
`ui_lang`, key `uiLang`) and in the browser (`localStorage`). The pages follow the CamOS AMB82 camera's design.

## 1. The setup portal (SoftAP)

It opens when the camera has no network to try (no Wi-Fi, and 4G off or no modem / SIM answering), when the button is
held 3 s (Freenove: BOOT; AI-Thinker: a push button on GPIO13), after a new Wi-Fi network could not be joined within
75 s, or after the console command `portal`. Wi-Fi `CAM-xxxx` (last 4 characters of the serial), password `camos1234`
unless changed (printed in the boot log), page at `http://192.168.4.1/` (phones open it by themselves through the
captive DNS). The radio runs APSTA, so the camera scans with the SoftAP up. It closes after 10 minutes or on Connect.
LED: blue, breathing.

Walkthrough (screenshots to be taken on a phone; descriptions of what they show):

1. `portal-1.png`: card "Connect the camera to Wi-Fi", "Camera CAM-877C. Choose a Wi-Fi network and type its password.";
   the Unclaimed box with the claim QR, serial and verify code; the network list, strongest first, each row with the
   name, signal bars (▂▄▆█), a lock for protected networks and the channel; "Other network (hidden)…" last.
2. `portal-2.png`: a network tapped (highlighted), the password field focused, "Show" link, the green Connect button,
   "Network not listed? Scan again".
3. `portal-3.png`: "Advanced settings" opened: 4G (use 4G, APN "automatic from the SIM", SIM PIN), schedule & power
   (interval, power mode, timezone with "Automatic: Asia/Ho_Chi_Minh (source: ...)"), device (status LED, GPS, battery
   divider fitted), FlyBot (address, provisioning token), AWS IoT Core (endpoint, certificate + key paste or file);
   "default" badges on build defaults.
4. `portal-4.png`: "Connecting…" with the note that a wrong password brings setup back in about a minute.
5. After Connect the camera restarts on the chosen network; a wrong password returns to the portal with
   "Could not join "X". Check the password and try again."

Only the Wi-Fi needs a choice when no default Wi-Fi is built in; everything else already has a working default.

### Portal JSON API (http://192.168.4.1)

| Call | Body / result |
|---|---|
| `GET /api/config` | every setting with its effective value; secrets (`password`, `simPin`, `provisionToken`, `token`, `apPassword`) as `true`/`false`; `defaults` (keys that are build defaults), `cert`, `key`, `paired`, `registered`, `claimed`, `hasModem`, `hasGps`, `hasLive`, `serial`, `verifyCode`, `labelQr`, `claimUrl`, `apSsid`, `wifiError`, `regError`, `timezoneAuto`, `timezoneSource`, `uiLang`, `model`, `fw` |
| `GET /api/wifi/scan[?refresh=1]` | `[{ssid, rssi, secure, channel}]`, deduplicated by SSID, strongest first; cached from the scan at portal start, `refresh=1` scans again |
| `POST /api/connect` | `{ssid, password, ...any keys}`: save and restart onto that network. `400 CHOOSE_NETWORK`, `400 SHORT_PASSWORD`, `400 BAD_VALUE` |
| `POST /api/config` | `{key: value, ...}` with any of `ssid password apn simPin cellular apiBase provisionToken token iotHost thingName kvsChannel kvsRegion roleAlias credHost timezone intervalMin powerMode ledEnabled gps batteryMonitor apPassword uiLang` (an empty string = back to the default) |
| `POST /api/cert` | `{"cert": "<PEM>", "key": "<PEM>"}` (each under 6000 bytes) |
| `POST /api/pair` | owner path: joins the Wi-Fi with the AP up and calls `POST /api/device/pair` with the provisioning token |
| `POST /api/restart` | restart |
| `GET /api/qr.svg?what=claim\|label` | the claim URL QR or the label QR, SVG made on the device |
| `GET /info` | protocol 12.3: `{serial, model, fw, wifiScan: [{ssid, rssi, secure}]}` |
| `POST /provision` | protocol 12.3: `{ssid, password, provisionToken?, endpoint?, apn?, apiBase?}` -> `{"ok":true}`, restarts |

## 2. The web console (on Wi-Fi)

`http://<camera IP>/` (the IP is in the boot log and the serial `info`), while the camera is awake on Wi-Fi: always in
powered mode, during the awake window in battery mode (a signed-in visit keeps it awake 2 minutes). Not over 4G. The
same layout as the AMB82 console: a sign-in gate (the first visit sets the admin password, 8-64 characters with letters
and digits; 5 wrong passwords lock sign-in for 5 minutes), "Live View" (JPEG preview at QVGA to HD, snapshot to FlyBot,
status side panel) and "Configuration" with System (device information, time, maintenance & upgrade, log), Network
(Wi-Fi / TCP/IP with a scan table, 4G, cloud platform), Image (display), Power & location (power & schedule, GPS) and
Security (account). Every POST needs `X-Requested-With: camos`; sessions are an `HttpOnly; SameSite=Strict` cookie.

API: `GET /api/session`, `POST /api/activate`, `/api/login`, `/api/logout`, `/api/password`, `/api/lang`,
`GET /api/status`, `GET|POST /api/config`, `POST /api/cert`, `GET /api/wifi/scan`, `POST /api/image`,
`GET /api/frame.jpg?w=&h=`, `POST /api/snapshot`, `/api/pair`, `/api/gps`, `/api/ota`, `/api/reboot`,
`/api/factory-reset`, `GET /api/logs?since=`, `GET /api/qr.svg` (details at the top of `firmware/main/web.c`).

Forgot the admin password: hold BOOT 3 s (portal) or use the serial console `factory`.

## 3. Serial console (UART0, 115200)

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

## 4. NVS keys (namespace `camos`)

Strings: `serial`, `vcode`, `wifi_ssid`, `wifi_pass`, `apn`, `sim_pin`, `api_base`, `token`, `prov_token`, `mqtt_host`,
`thing`, `kvs_chan`, `kvs_region`, `role_alias`, `cred_host`, `claim_url`, `tz`, `tz_auto`, `tz_posix`, `tz_src`,
`ui_lang`, `ap_pass`, `wifi_err`, `reg_err`, `admin_salt`, `admin_hash`, `cert`, `key` (PEM). Integers: `interval`,
`power_mode` (0 auto, 1 battery, 2 powered), `cellular`, `gps`, `led`, `batt_mon`, `claimed`, `wifi_new`,
`modem_absent`, `gps_absent`, `portal`, `boots`. Blobs: `settings` (the persisted shadow), `gps_fix`.
The factory partition `fctry` (0x14000, 0x6000, read-only) uses the same namespace and keys (`serial`, `vcode`, `cert`,
`key`, `thing`, `mqtt_host`, `claimed`). `idf.py flash` never touches either partition; `idf.py erase-flash` and the
browser "install" image wipe both; `factory` (console, web console, RPC) keeps the identity keys.
