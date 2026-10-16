# Setup portal and bench console

Nothing but the board pin preset and the Amazon Root CA is fixed at build time. Everything else lives in NVS (namespace
`camos`) and is set through the portal page, its JSON API, the serial console, or `tools/provision.sh`.

## 1. Opening the portal

The portal starts when nothing is configured (no Wi-Fi name and no APN), when the button is held 3 s (Freenove: BOOT;
AI-Thinker: a push button on GPIO13), or after the console command `portal`. The camera opens the Wi-Fi network
`CAM-xxxx` (xxxx = last 4 characters of the serial; password `camos1234` unless changed, printed in the boot log) and the
page at `http://192.168.4.1/` (phones open it by themselves through the captive DNS). It closes after 10 minutes or on
"Restart".

Walkthrough (screenshots to be added once the board has run it: `docs/img/portal-1.png` network, `portal-2.png` FlyBot,
`portal-3.png` certificate, `portal-4.png` pairing result):

1. **Network**: Wi-Fi name (picked from the scan list) and password, or the 4G APN (leave the Wi-Fi empty for a 4G-only
   camera; both set = 4G first, Wi-Fi when the modem does not answer).
2. **FlyBot**: the base URL (`https://flybot.agrhub.com`), the provisioning token from the FlyBot "add camera" screen,
   the capture interval, the power mode (`auto` recommended on solar), the timezone.
3. **AWS IoT Core**: paste or upload the device certificate and private key (PEM) and press "Store certificate + key".
   The IoT endpoint, thing name and KVS channel are filled by pairing; edit them only to override.
4. **Save and pair now**: the camera joins your Wi-Fi (the setup network stays up), calls `POST /api/device/pair` with its
   serial, verify code and the provisioning token, and stores the device token, IoT endpoint, thing name and KVS
   channel. Then "Restart". Over 4G, just "Save" and "Restart": the camera pairs by itself at the next boot.

The page header shows the serial, the verify code and the label QR text `CAMOS:1;SN:...;VC:...;M:...;;` (protocol 12.1).
`tools/label.py <serial> <verify code>` prints the QR for a sticker; FlyBot scans it to start the pairing.

## 2. JSON API (http://192.168.4.1)

| Call | Body / result |
|---|---|
| `GET /api/config` | every setting; secrets (`password`, `simPin`, `provisionToken`, `token`, `apPassword`) as `true`/`false`; `cert`, `key`, `paired`, `modem`, `gps`, `live` flags; `serial`, `verifyCode`, `labelQr`, `model`, `fw` |
| `POST /api/config` | `{key: value, ...}` with any of `ssid password apn simPin apiBase provisionToken token iotHost thingName kvsChannel kvsRegion roleAlias credHost timezone intervalMin powerMode apPassword verifyCode`. `{"ok":true}` or `400 {"ok":false,"error":"rejected: key, key"}` (nothing else is rolled back: valid keys are stored) |
| `POST /api/cert` | `{"cert": "<PEM>", "key": "<PEM>"}` (each under 6000 bytes) |
| `POST /api/pair` | `{"provisionToken"?}`; joins the Wi-Fi, pairs; `{"ok":true,"thingName","iotHost","kvsChannel"}` or `502 {"error"}` |
| `POST /api/restart` | `{"ok":true}`, restarts after 1 s |
| `GET /info` | protocol 12.3: `{serial, model, fw, wifiScan: [{ssid, rssi, secure}]}` |
| `POST /provision` | protocol 12.3: `{ssid, password, provisionToken?, endpoint?, apn?, apiBase?}` -> `{"ok":true}`, restarts |

Example from a laptop on the `CAM-xxxx` network:

```bash
curl -s http://192.168.4.1/api/config
curl -s -X POST http://192.168.4.1/api/config -d '{"ssid":"Vuon-A","password":"...","apiBase":"https://flybot.agrhub.com","provisionToken":"...","timezone":"Asia/Ho_Chi_Minh","powerMode":"auto"}'
curl -s -X POST http://192.168.4.1/api/cert -d "$(jq -n --rawfile c cert.pem --rawfile k key.pem '{cert:$c,key:$k}')"
curl -s -X POST http://192.168.4.1/api/pair -d '{}'
curl -s -X POST http://192.168.4.1/api/restart -d '{}'
```

## 3. Serial console (UART0, 115200)

Available in normal operation and inside the portal. Type `help` for the list; the useful ones for the bench:

| Command | What it does |
|---|---|
| `info` | serial, verify code, label QR text, pairing state, network, clock, battery, power mode, next capture, location |
| `cfg` | the configuration as JSON (secrets as set / not set) |
| `set <key> <value>` | same keys as `POST /api/config` (`set apn v-internet`, `set powerMode battery`, `set intervalMin 30`) |
| `cert`, `key` | paste a PEM, finish with a line `END` |
| `net` | bring the network up (4G when an APN is set, else Wi-Fi) and print IP, RSSI, cell info |
| `pair [token]` | pair now |
| `time [flybot]` | clock state; `time flybot` forces `GET /api/device/time` |
| `snap` | capture and upload now (`trigger: "rpc"`); `meta` prints the meta JSON alone |
| `batt` | read the ADCs, print `battery` and `power` |
| `mode auto\|battery\|powered` | power mode |
| `gps [timeout]` | take a fix |
| `at <AT command>` | talk to the modem (command mode or CMUX) |
| `live start [min]`, `live stop` | live view (ESP32-S3 with CAMOS_LIVE) |
| `ota` | check `GET /api/device/ota` and apply |
| `awake [s]` | no deep sleep for a while (default 600 s) |
| `sleep [s]` | deep sleep now |
| `portal`, `factory`, `restart` | |

The console task runs the commands itself, so `snap` or `pair` block the prompt until they finish.

## 4. NVS keys (for tools/provision.sh and nvs_partition_gen)

`serial`, `vcode`, `wifi_ssid`, `wifi_pass`, `apn`, `sim_pin`, `api_base`, `token`, `prov_token`, `mqtt_host`, `thing`,
`kvs_chan`, `kvs_region`, `role_alias`, `cred_host`, `tz`, `ap_pass` (strings); `interval`, `power_mode` (i32; 0 auto,
1 battery, 2 powered); `cert`, `key` (PEM strings); `settings` (blob, the persisted shadow), `gps_fix` (blob),
`portal`, `boots` (i32). `idf.py flash` never touches the NVS partition; `idf.py erase-flash` and the console `factory`
do (`factory` keeps `serial` and `vcode`).
