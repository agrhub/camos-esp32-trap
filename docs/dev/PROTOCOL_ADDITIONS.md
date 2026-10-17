# CamOS protocol v1: additions used by camos-esp32-trap (FlyBot on AWS)

Base protocol: [protocol-v1.md](protocol-v1.md) (CamOS device protocol v1, MQTT over AWS IoT Core). Nothing below changes
the meaning of an existing field; everything is additive and a cloud that does not know a field ignores it (protocol 3).
The contract with FlyBot is `flybot-aws/docs/CONTRACTS.md` sections 2, 3 and 4; this file records what the firmware
actually sends and accepts.

## 1. Topics and session (unchanged)

`camos/{serial}/{state,event,telemetry,shadow/desired,shadow/reported,rpc/req,rpc/res}`, client id = serial, keepalive 60 s,
clean session false, Last Will `{"v":1,"online":false}` retained on `state`. Mutual TLS with the device certificate from the
portal (CN = serial); the broker is verified against the embedded Amazon Root CA 1. The serial is `ESP` + 12 hex digits of the
Wi-Fi MAC (`ESP000000000002`).

## 2. HTTPS to FlyBot (CONTRACTS section 2)

Base URL `apiBase` and the device token come from pairing. Headers on every call: `X-Device-Token`, `X-Device-Serial`.
The token is only ever sent to `apiBase` (protocol 11.4).

| Call | Firmware |
|---|---|
| `POST /api/device/pair` `{"serial","verifyCode","provisionToken","model","fw"}` | once, when a provisioning token is stored and no device token: from the portal page ("Save and pair now", over the configured Wi-Fi) or at the next boot over whichever network comes up (4G included). Stores `deviceToken`, `iot.endpoint`, `iot.thingName`, `kvs.channelName`, `kvs.region`; also `iot.credentialsEndpoint` and `iot.roleAlias` when present (live view credentials). The provisioning token is erased after success. `model` and `fw` are additive. |
| `POST /api/device/snapshot` multipart `image` (image/jpeg) + `meta` (application/json) | every capture: schedule, RPC (`capture` / `snapshot.now`), boot. 4 attempts, back-off 2 / 4 / 8 s + jitter; a 4xx stops the retries. A JPEG over 2 MB is retaken at a lower quality (up to twice) before the upload. `nextWakeAt` moves the next capture when it is sooner than the schedule. |
| `GET /api/device/time` | when SNTP gets no answer within 15 s of the network coming up (4G carriers that block UDP 123); retried with SNTP until the clock is set. `epoch` is preferred, `now` parsed otherwise. |
| `GET /api/device/ota` | on RPC `ota.apply` and the console `ota` command. 204 = nothing to do. |

`meta` as sent (CONTRACTS shape plus two additive fields):

```json
{"v":1,"serial":"ESP000000000002","capturedAt":"2026-10-16T08:00:00Z","trigger":"schedule",
 "battery":{"mv":3900,"pct":65,"charging":true,"solarMv":5800},"net":"wifi","rssi":-61,
 "location":{"lat":10.971200,"lng":106.650300,"fixAt":"2026-10-16T05:00:12Z"},
 "fw":"0.2.0","model":"AIC-FREENOVE-S3CAM"}
```

`battery` is absent without a battery monitor, `location` without a GPS fix, `solarMv` without a solar ADC.
`trigger` is `schedule` | `rpc` | `boot`. `rssi` is dBm (Wi-Fi) or `-113 + 2 * CSQ` (4G), 0 when unknown.

## 3. Shadow (protocol 8) additions

`desired` keys the firmware applies (anything else: `NOT_SUPPORTED` in `reported.errors`, as before):

| Key | Values | Effect |
|---|---|---|
| `capture.intervalMin` | 1..1440, or `null` | all-day capture interval. Used when no `detections[]` schedule is set. `null` = back to the portal value / Kconfig default (60). |
| `power.mode` | `"auto"` \| `"battery"` \| `"powered"`, or `null` | see section 6. `null` = the portal value. |
| `power.highMv`, `power.lowMv` | 3000..4400 | auto-mode hysteresis thresholds (defaults from Kconfig: 4050 / 3850). |
| `live.requested` | bool | `true`: the camera starts a live session (5 min) at the next wake / shadow delivery, then clears it. |
| `kvs.channelName`, `kvs.region` | string | stored in NVS (normally set by pairing). |
| `kvs.credentialsEndpoint`, `kvs.roleAlias` | string | the AWS IoT credential provider host and role alias of the live view credentials, stored in NVS (for cameras that registered or paired without them; see LIVE_VIEW.md). |

Priority for the interval and the power mode: shadow > portal > Kconfig default.

`video.image`: the protocol 8 keys `brightness` (-64..64), `contrast`, `saturation`, `sharpness` (0..100) and
`orientation` (0 normal, 1 mirror, 2 flip, 3 both) are applied, plus these ESP32 additions (the web console sets the
same keys locally; a later shadow wins):

| Key | Values | OV5640 mapping |
|---|---|---|
| `whiteBalance` | `"auto"` \| `"sunny"` \| `"cloudy"` \| `"office"` \| `"home"` | AWB mode |
| `effect` | `"none"` \| `"negative"` \| `"grayscale"` \| `"red"` \| `"green"` \| `"blue"` \| `"sepia"` | special effect |
| `aeLevel` | -5..5 (other sensors -2..2) | exposure compensation |
| `exposureAuto`, `exposure` | bool, 0..1200 | AEC on, or the manual exposure value |
| `gainAuto`, `gain` | bool, 0..30 | AGC on, or the manual gain |
| `gainCeiling` | 0..6 (2x..128x) | AGC ceiling |
| `denoise` | 0..8 | noise reduction |

`flicker`, `wdr` and `ldc` have no control in esp32-camera: `NOT_SUPPORTED`. `reported.video.image` carries every key.

`claimed` (bool) is also accepted (section 8).

`reported` (and the retained `state`) carry, next to the existing `version`, `appliedAt`, `errors`, `video.image`, `capture`:

```json
{"capture":{"resolution":"max","jpegQuality":85,"intervalMin":60,"w":2560,"h":1920},
 "power":{"mode":"auto","effective":"battery","highMv":4050,"lowMv":3850},
 "live":{"requested":false,"supported":false,"active":false},
 "kvs":{"channelName":"cam-ESP000000000002","region":"ap-southeast-2"},
 "location":{"lat":10.971200,"lng":106.650300,"fixAt":"2026-10-16T05:00:12Z"},
 "net":"wifi",
 "battery":{"voltage":3.9,"percent":65,"mv":3900,"pct":65,"charging":true,"solarMv":5800},
 "nextCaptureAt":"2026-10-16T09:00:00Z"}
```

`battery.voltage` / `percent` are the fields of the reference firmware (kept); `mv`, `pct`, `charging`, `solarMv` are the
CONTRACTS fields. `state` also has `thingName`, `net.cell {imei, iccid, operator, csq, registered}` on 4G (protocol 4 shape)
and `gps {lat, lon, fix, ts}` (protocol 4) when the build has GPS.

`live.supported` is true when the build has the cloud live view (`CAMOS_LIVE`), the camera is paired (KVS channel and
region) and it has a way to get KVS credentials (credential provider host and role alias); `live.active` is true while
a `live.start` / `live.requested` window is open or a viewer is connected. A powered camera stays on its signaling
channel without a window (LIVE_VIEW.md).

## 4. RPC (protocol 9) additions

| Method | `params` | `result` / error |
|---|---|---|
| `snapshot.now` | — | `{"snapshotId","image":{"key","w","h","bytes"},"bytes"}` (`image.key` = `snapshotId`, so the protocol 9 `capture` shape still holds). Errors `UPLOAD_FAILED`, `INTERNAL`, `BUSY` (live view running). `capture` is an alias. |
| `live.start` | `{"minutes": 1..60}` (default 5) | `{"channelName","seconds"}`; `NOT_SUPPORTED` when the build has no live view (ESP32-CAM, or CAMOS_LIVE off) or the camera has no KVS channel or credential provider settings (see LIVE_VIEW.md), `BAD_PARAMS`, `BUSY`. Repeating it extends the session. The camera joins the channel within a few seconds of the answer (credentials, endpoints, WebSocket); a viewer offer sent before that is lost and the viewer offers again. |
| `live.stop` | — | — (drops the viewer and ends the window; a powered camera stays on the channel) |
| `gps.fix` | `{"timeoutS"?}` (default 120) | `{"location":{lat,lng,fixAt},"sats"}`; `TIMEOUT` when no fix, `NOT_SUPPORTED` without GPS. Blocks the RPC queue for up to `timeoutS`. |
| `ota.apply` | — | `{"version"}` at once, then the event `ota_result` (`stage: "installed"` + restart, or `ok:false` with `DOWNLOAD_FAILED` / `SIGNATURE_INVALID` for a SHA-256 mismatch). `NOT_FOUND` when `GET /api/device/ota` gives 204, `OLD_VERSION` when it offers the running version. |
| `reboot` | — | as before |
| `net.set` | `wifi {ssid, password}`, `cellular {apn}` | stored, effective at the next boot (protocol 9 method, now supported) |
| `factory_reset` | — | erases everything but the serial and verify code (protocol 9 method, now supported) |

Unchanged: `ping`, `state`, `capture`. Everything else answers `NOT_SUPPORTED`.

## 5. Events (protocol 5)

- `capture` (schedule and boot): `trigger` is `"schedule"` or `"boot"` (addition), `detectionId` is the schedule id or
  `"interval"` for the all-day interval, `image.key` = `snapshotId`, plus a top-level `snapshotId`. A capture after
  motion (web console option) has `trigger: "motion"`; its CONTRACTS section 2 `meta.trigger` is `"rpc"`, the closest
  value the contract has, until FlyBot accepts `"motion"`.
- `motion` (addition) `{level, sensitivity, roi: [x, y, w, h], sdFile?, detail}`: frame-difference motion inside the
  area (percent of the picture), `level` in per mille of the area, `sdFile` = `YYYYMMDD/HHMMSSM.JPG` when the picture
  was saved on the microSD card. At most one per cooldown (web console setting, default 60 s).
- `location_changed` `{lat, lon, distanceM}` when a new fix is more than 50 m from the previous one.
- `ota_result` `{target:"firmware", version, ok, stage, error?, detail?}`.
- `boot` `{reason, fw}`: `reason` now also `"ota"` after a successful update.
- `low_power` `{voltage}` as before.

## 6. Sleep and the goodbye message (CONTRACTS section 3)

Before a deep sleep the camera publishes, retained on `state`:

```json
{"v":1,"online":false,"sleeping":true,"wakeAt":1792396800000}
```

`wakeAt` is the ms epoch of the next wake-up (the slot minus the lead time). FlyBot shows "sleeping, next wake HH:MM".
A clean MQTT close sends no Last Will, which is why the camera says it itself.

Effective `power.mode`:
- `powered`: MQTT always connected, no deep sleep, captures at the slots, `live.start` / `snapshot.now` any time.
- `battery`: capture at the slot -> network -> upload -> awake window (30 s) for queued RPCs -> goodbye -> deep sleep
  (modem powered off through PWRKEY, Wi-Fi stopped, camera in PWDN). RPCs sent while asleep wait in the broker (QoS 1,
  clean session false) and run at the next wake.
- `auto`: without a battery divider (`batteryMonitor` off) always `powered` (the USB / mains Wi-Fi camera). With one:
  `powered` when the cell is at or above `highMv` (or the charger reports charging and the cell is above `lowMv`), back
  to `battery` below `lowMv`; a divider that gives no reading counts as `battery`. The state survives deep sleep.

## 7. Pairing material (protocol 12)

The label QR (12.1) is `CAMOS:1;SN:<serial>;VC:<verify code>;M:<model>;;`. These boards have no factory label: the firmware
generates the 6-character verify code at the first boot and shows it in the boot log, on the portal page and in the
console `info`; `tools/label.py` prints the QR for a sticker. The SoftAP page (12.3) keeps `GET /info` and
`POST /provision` (now also accepting `provisionToken`, `endpoint`, `apn`, and the additive `apiBase`) and adds the JSON
API described in [CONFIG.md](CONFIG.md).

## 8. Self-registration and claim (CONTRACTS 2a)

A camera without a certificate (the public build, after flashing) registers itself:

1. Key: ECDSA P-256 made on the device from the hardware RNG, stored in NVS (`key`), never sent anywhere.
2. `POST {apiBase}/api/device/register` (no auth, HTTPS with the ESP-IDF certificate bundle):
   `{"v":1,"serial":"ESP000000000001","verifyCode":"ABC123","model":"AIC-FREENOVE-S3CAM","fw":"0.3.0","csr":"-----BEGIN CERTIFICATE REQUEST-----..."}`.
   The CSR has subject `CN=<serial>` and is signed with ECDSA-SHA256 (checked with `openssl req -verify` on a board).
3. `200` / `201` with `ok`, `certificatePem`, `deviceToken`, `iot.endpoint`: stored (plus `iot.thingName`,
   `kvs.region`, `kvs.channelName`, `claimUrl`, `claimed`, and `iot.credentialsEndpoint` + `iot.roleAlias` for the live
   view when both are present); the MQTT client starts without a restart.
4. `429`, `5xx`, no answer: retried after 30 s, doubling to 1 h, ±20 %. `409 serial_taken`: no more retries, the LED
   goes red, the portal and web console show "registration refused" until the camera is reset in FlyBot and the
   camera is factory-reset.
5. Unclaimed (`claimed: false` from registration, or the shadow): the capture interval is at least 60 minutes and
   `nextWakeAt` from the snapshot response is followed even when it is later than the schedule. The shadow key
   `claimed: true` (addition) returns the camera to its normal schedule. The portal and console show the claim QR
   (`claimUrl`, as SVG made on the device) with the serial and verify code.

The verify code is 6 Crockford base32 characters from the hardware RNG, made once; factory reset keeps it, the serial,
the key, the certificate, the device token and the thing (protocol 9 `factory_reset` "keeps the certificates").

## 9. Time, timezone and IP (CONTRACTS 2a.6)

- Clock, the first that answers: SNTP (`pool.ntp.org`, `time.google.com`) -> the modem's network time (`AT+CTZU=1`,
  `AT+CCLK?`, 4G) -> GPS RMC time -> `GET /api/device/time`. The source is logged (`tz: clock set from sntp: ...`) and
  reported as `clockSource` (`sntp` | `modem` | `gps` | `flybot` | `rtc` | `none`).
- Timezone: `GET /api/device/time` returns `timezone` (IANA), `posix`, `tzSource`, `publicIp`; the POSIX string is
  applied with `setenv("TZ")` / `tzset()` and kept in NVS. Order: a manual override (portal / console `timezone`) >
  the shadow `timezone` (the existing protocol 8 key; `null` returns to automatic) > `/api/device/time` > the build
  default. Reported as `timezone` and `tzSource` (`manual` | `shadow` | `farm` | `gps` | `ip` | `default` | `build`).
- IP: DHCP / PPP. `reported` and `state` gain `localIp` and `publicIp` (additions); the existing `net` key of
  `reported` stays the string `"wifi"` | `"4g"` (CONTRACTS 3), and `state.net.ip` (protocol 4) keeps the local address.

## 10. A note on the CONTRACTS example

The first CONTRACTS section 2 example paired `2026-10-16T08:00:00Z` with epoch 1792396800 (2026-10-19T16:00:00Z); it was
corrected to 1792137600 in flybot-aws 169aa44. The firmware trusts `epoch` when present.
