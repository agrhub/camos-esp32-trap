# CamOS AI Camera device protocol

> English translation of the CamOS device protocol v1 (the original is in Vietnamese). Same section numbers, field
> names, values, topics and examples; where the translation is unclear, the field names and examples are authoritative.
> Additions used by this firmware are in [PROTOCOL_ADDITIONS.md](PROTOCOL_ADDITIONS.md).

> Protocol version `1` · draft 0.1 · 2026-10-12
> Scope: camera ↔ CamOS. Applications (FlyBot…) do not use this protocol; applications use the CamOS Open API (not part of this repository).

## 1. Connection

| | |
|---|---|
| Protocol | MQTT 3.1.1 over TLS 1.2+, port 8883 |
| Authentication | Mutual TLS: every camera has its own client certificate, loaded at manufacturing. The certificate CN = `serial` |
| Client ID | `serial` |
| Keepalive | 60 s |
| Clean session | `false` (keeps subscriptions and QoS 1 messages through short network losses) |
| Last Will | topic `camos/{serial}/state`, retained, payload `{"v":1,"online":false}` |
| Permissions | The broker only lets the camera publish/subscribe topics that start with `camos/{serial}/` |

After a lost connection, reconnect with an increasing wait (1, 2, 4… up to 300 s, with ±20% randomness). After a
network change (Wi-Fi ⇄ 4G), reconnect at once.

## 2. Topics

| Topic | Direction | QoS | Retained | Content |
|---|---|---|---|---|
| `camos/{serial}/state` | camera → cloud | 1 | yes | Current state (section 4) |
| `camos/{serial}/event` | camera → cloud | 1 | no | Events (section 5) |
| `camos/{serial}/ai` | camera → cloud | 0 | no | Per-frame detection results while live (section 6) |
| `camos/{serial}/telemetry` | camera → cloud | 0 | no | Periodic measurements (section 7) |
| `camos/{serial}/shadow/desired` | cloud → camera | 1 | yes | Desired configuration (section 8) |
| `camos/{serial}/shadow/reported` | camera → cloud | 1 | yes | Applied configuration |
| `camos/{serial}/rpc/req` | cloud → camera | 1 | no | Commands from the cloud (section 9) |
| `camos/{serial}/rpc/res` | camera → cloud | 1 | no | Command results |
| `camos/{serial}/up/req` | camera → cloud | 1 | no | Camera asks the cloud (section 10) |
| `camos/{serial}/up/res` | cloud → camera | 1 | no | Cloud answers |

The camera subscribes to: `shadow/desired`, `rpc/req`, `up/res`.

## 3. General conventions

- Payloads are UTF-8 JSON. Every message has `"v": 1` (the protocol version).
- `ts`: a point in time in Unix milliseconds, UTC. The camera syncs its clock with NTP, and uses GPS time when there is
  no NTP.
- `id`: a ULID made by the sender. Used to match a command with its result, and to let the receiver drop duplicates.
- Detection box coordinates and ROI areas are real numbers **normalised 0–1** to the original frame, so they do not
  depend on the resolution.
- Unknown fields are ignored. The meaning of an existing field never changes; an incompatible change increments `v`.

## 4. State

The camera publishes it once connected and whenever something notable changes (network, SD, app, firmware).

```json
{
  "v": 1,
  "ts": 1760256000000,
  "online": true,
  "model": "AIC-AMB82-K306P",
  "fw": "0.3.1",
  "protocol": 1,
  "apps": [{ "id": "fruit-fly-sticky", "version": "1.2.0", "enabled": true }],
  "net": {
    "type": "wifi",
    "ip": "192.168.1.23",
    "rssi": -61,
    "cell": { "imei": "86...", "iccid": "8984...", "operator": "Viettel", "csq": 18, "registered": true }
  },
  "gps": { "lat": 10.9712, "lon": 106.6503, "fix": true, "ts": 1760255990000 },
  "sd": { "present": true, "totalMB": 59640, "freeMB": 41200, "healthy": true },
  "tempC": 52.5,
  "shadowVersion": 14
}
```

## 5. Event

```json
{
  "v": 1,
  "id": "01JA2Z8Q4M7Y3V6N8K2D5F1H9C",
  "ts": 1760256012345,
  "type": "app_detection",
  "detectionId": "trap-count",
  "app": "fruit-fly-sticky",
  "model": "yolo11n-fruitfly-v3",
  "n": 37,
  "truncated": false,
  "boxes": [[0.112, 0.204, 0.131, 0.229, 0, 0.91]],
  "labels": ["fruit_fly"],
  "image": { "key": "devices/AIC0001/snap/01JA2Z8Q....jpg", "w": 2560, "h": 1440 },
  "clip": null
}
```

- `boxes`: `[x1, y1, x2, y2, classIndex, confidence]`. The camera sends at most 200 boxes. `n` is always the real
  count; `truncated: true` when boxes were left out.
- `image.key` is returned by the Device API when the camera uploads the image (section 11.4). The camera uploads the
  image first and then sends the event, with `eventId` so the backend links the image to the event. If the image upload
  fails, the camera sends `image: null` with `"imageError": "..."`; the cloud still takes the event.
- Without a network, the camera queues events on the SD card (at most 10,000 messages or 7 days) and sends them in
  order when the network is back. The cloud drops duplicates by `id`.

| `type` | Specific fields |
|---|---|
| `motion` | `detectionId`, `score`, `roi` |
| `person` | `detectionId`, `n`, `boxes` |
| `object` | `detectionId`, `n`, `boxes`, `labels` |
| `app_detection` | `detectionId`, `app`, `model`, `n`, `truncated`, `boxes`, `labels`, `image` |
| `capture` | `image`, `trigger` (`schedule` / `rpc`) |
| `sd_error`, `sd_full` | `detail` |
| `tamper` | `reason` (`covered` / `moved` / `defocus`) |
| `network_switch` | `from`, `to` |
| `location_changed` | `lat`, `lon`, `distanceM` |
| `ota_result`, `app_result` | `target`, `version`, `ok`, `error` |
| `low_power` | `voltage` |
| `boot` | `reason` (`power_on` / `watchdog` / `ota` / `rpc` / `crash`), `fw` |

## 6. AI (stream while live)

Sent only while live (`live.start`), at most 5 messages per second. CamOS forwards it into the WebRTC data channel of
the stream.

```json
{ "v": 1, "ts": 1760256012345, "pts": 912345, "detectionId": "person", "n": 2, "truncated": false, "boxes": [[0.41, 0.22, 0.55, 0.81, 0, 0.88]] }
```

`pts` is the video timestamp (ms) of the frame that was analysed, so the player can match the boxes to the picture.

## 7. Telemetry

Sent every 5 minutes (every 30 minutes on 4G).

```json
{ "v": 1, "ts": 1760256000000, "uptimeS": 86400, "cpu": 41, "heapFree": 21000000, "fps": { "main": 15, "sub": 15 }, "npuMs": { "person": 38, "fruit-fly-sticky": 2400 }, "dataUsedMB": { "wifi": 812.4, "cell": 35.1 }, "rebootCount": 2 }
```

## 8. Shadow

CamOS owns the `desired` document; the camera owns the `reported` document. When `desired.version` changes:
1. The camera applies the new configuration.
2. The camera publishes `reported` with the same `version` and the list of errors (if any).

Parts that cannot be applied keep their previous configuration.

```json
{
  "v": 1,
  "version": 15,
  "video": {
    "main": { "width": 2560, "height": 1440, "fps": 15, "bitrateKbps": 2500, "codec": "h264" },
    "sub":  { "width": 640,  "height": 360,  "fps": 15, "bitrateKbps": 400,  "codec": "h264" },
    "sdCodec": "h265",
    "osd": { "time": true, "name": "Lô A - bẫy 3" },
    "flip": false, "nightMode": "auto"
  },
  "detections": [
    { "id": "motion", "type": "motion", "enabled": true, "sensitivity": 60, "roi": [], "schedule": "always", "cooldownS": 60, "actions": ["event"] },
    { "id": "person", "type": "person", "enabled": true, "minConfidence": 0.5, "minCount": 1, "roi": [[[0.1,0.1],[0.9,0.1],[0.9,0.9],[0.1,0.9]]], "schedule": { "days": [1,2,3,4,5,6,7], "from": "18:00", "to": "06:00" }, "cooldownS": 300, "actions": ["event", "snapshot", "clip"] },
    { "id": "object", "type": "object", "enabled": false, "classes": ["dog", "cat", "bird"], "minConfidence": 0.5, "roi": [], "schedule": "always", "cooldownS": 300, "actions": ["event"] },
    { "id": "trap-count", "type": "app", "app": "fruit-fly-sticky", "enabled": true, "trigger": { "everyMin": 30, "from": "06:00", "to": "18:00" }, "params": { "minConfidence": 0.35, "tile": 640, "overlap": 0.2 }, "actions": ["event", "snapshot"] }
  ],
  "capture": { "jpegQuality": 85, "resolution": "max" },
  "record": { "sd": "continuous", "segmentS": 300, "cloud": "event", "preRollS": 5, "postRollS": 15 },
  "net": { "prefer": "wifi", "wifi": [{ "ssid": "Vuon-A" }], "cellular": { "apn": "v-internet", "dataLimitMB": 3000, "liveStream": "sub" } },
  "ota": { "channel": "stable", "wifiOnly": false },
  "timezone": "Asia/Ho_Chi_Minh"
}
```

(The OSD `name` in the example is sample data: "Plot A - trap 3".)

- `audio`: `{ "enabled": true, "speakerVolume": 80 }`: `enabled` turns the microphone sound on or off (the stream keeps
  its AAC track, it is only silent when off); `speakerVolume` 0-100 is the speaker volume (0 = speaker off).
- `video.image` (added in 0.25): ISP tuning, applied at once (the stream does not restart) and saved to flash by the
  camera, so it survives restarts. Every field is optional and an integer:

  | Field | Range | Meaning |
  |---|---|---|
  | `brightness` | -64..64 | brightness |
  | `contrast`, `saturation`, `sharpness` | 0..100 | contrast, saturation, sharpness |
  | `flicker` | 0..3 | anti-flicker: 0 off, 1 50 Hz, 2 60 Hz, 3 automatic |
  | `wdr` | 0..2 | WDR: 0 off, 1 manual (the tuning's level), 2 automatic |
  | `orientation` | 0..3 | 0 normal, 1 horizontal mirror, 2 vertical flip, 3 both |
  | `ldc` | 0..1 | lens distortion correction |

  A value out of range or not an integer: the camera keeps the old value of that field and reports
  `{ "path": "video.image.<field>", "code": "BAD_VALUE" }`; an unknown field: `NOT_SUPPORTED`. The ranges are the same as
  on the camera's web configuration page. Rule: the shadow wins; a change made on the web page lasts until a new
  `version` or the next connection (the retained `desired` document is applied again at every camera start).
- `capture.resolution` (added in 0.25): `"main"` (default) or `"max"`: the `capture` picture and event pictures are taken
  at the sensor's full size. Firmware 0.25: the main stream always runs at the sensor's full size (K306P 2560x1440, F37
  1920x1080) and the JPEG is taken from that stream, so both values give the same picture; `w`/`h` in `image` are always
  the real size read from the JPEG. `capture.jpegQuality` is not supported yet (`NOT_SUPPORTED`).
- Action `"siren"` in a feature's `actions`: on an event, the speaker plays a 5-second siren (cameras with a speaker).
- The shadow never contains a Wi-Fi password. Passwords only travel through the RPC `net.set` and are only stored on the
  camera.
- `schedule` is in the local time of `timezone`.
- `type: "app"` needs the matching app to be installed. If it is not, the camera reports `APP_NOT_INSTALLED` in
  `reported.errors`; CamOS installs the app and sends the document again.

`reported`:

```json
{ "v": 1, "version": 15, "appliedAt": 1760256100000, "errors": [{ "path": "detections[2]", "code": "APP_NOT_INSTALLED" }],
  "video": { "image": { "brightness": 40, "contrast": 50, "saturation": 50, "sharpness": 50, "flicker": 1, "wdr": 2, "orientation": 0, "ldc": 0 } },
  "capture": { "resolution": "main", "w": 2560, "h": 1440 } }
```

- `video.image` and `capture` in `reported` (added in 0.25): the running values after applying (the saved ones, or the
  ISP tuning's when nobody set them; `null` when the ISP could not be read yet). `capture.w`/`h` is the JPEG size of the
  main stream.
- At connect time (added in 0.25): the retained `state` (section 4) also carries `video.image` and `capture` as above,
  so the cloud knows the camera's image settings even before any `desired` document exists.

## 9. RPC: cloud → camera

Request:

```json
{ "v": 1, "id": "01JA...", "method": "capture", "params": { "detectionId": "trap-count" }, "timeoutS": 60 }
```

Result:

```json
{ "v": 1, "id": "01JA...", "ok": true, "result": { "image": { "key": "devices/AIC0001/snap/....jpg", "w": 2560, "h": 1440, "bytes": 412345 }, "bytes": 412345, "ai": { "n": 37, "boxes": [] } } }
{ "v": 1, "id": "01JA...", "ok": false, "error": { "code": "BUSY", "message": "OTA in progress" } }
```

| Method | `params` | `result` |
|---|---|---|
| `capture` | `detectionId?` (runs that feature's detection on the picture) | `image` (`key`, `w`, `h`, `bytes`; `bytes` inside `image` added in 0.25), `bytes`, `ai?`. `w`/`h` read from the JPEG. The picture is uploaded to the Device API (section 11.4). The size follows `capture.resolution` (section 8) |
| `live.start` | `url` (RTMP), `token`, `stream` (`main`/`sub`), `maxS` | `streamId` |
| `live.stop` | — | — |
| `records.list` | `from`, `to` (ms) | `segments: [{ start, end, bytes, recording? }]`, `more` (at most 100 segments; with `more: true` ask again from the last `end`) |
| `records.fetch` | `start` (ms) | — as soon as received. The camera uploads the segment containing `start` to the Device API (section 11.4) and then sends the `record_result` event |
| `net.set` | `wifi: { ssid, password }`, `cellular: { apn, user?, password? }` | — |
| `ota.start` | `version`, `url` (HTTPS), `size`, `sha256` (hex), `signature` (base64 DER, ECDSA P-256 over the SHA-256 of the image, CamOS release key) | — as soon as received; immediate errors: `BAD_PARAMS`, `BUSY`, `OLD_VERSION`. Then the `ota_result` event: `stage: "verified"` (size, SHA-256 and signature correct, nothing written yet), `stage: "installed"` (written to the spare slot, the camera restarts), or `ok: false` with `DOWNLOAD_FAILED`, `SIGNATURE_INVALID`, `INSTALL_FAILED`. A wrong image is never written |
| `speaker.play` | `tone` (`"beep"` or `"siren"`) and `seconds` (1-60, default 3); or `url` (http/https, a 16-bit PCM WAV file, mono or stereo, any sample rate, at most 3 MB) | — once playing has started; errors: `NOT_SUPPORTED` (no speaker), `BAD_PARAMS`, `BUSY` (another file is downloading). A new sound replaces the one playing |
| `speaker.stop` | — | — |
| `app.install` | `id`, `version`, `url`, `size`, `sha256`, `signature` | — (the result comes as the `app_result` event) |
| `app.remove` | `id` | — |
| `reboot` | — | — |
| `format_sd` | — | — |
| `factory_reset` | — | — (the camera erases the network, the configuration and apps; it keeps the certificates) |
| `log.fetch` | `lines?` | `key` (file uploaded to the Device API) |

Error codes: `BAD_PARAMS`, `BUSY`, `NOT_SUPPORTED`, `NO_SD`, `SD_ERROR`, `APP_NOT_INSTALLED`, `SIGNATURE_INVALID`,
`DOWNLOAD_FAILED`, `UPLOAD_FAILED`, `TIMEOUT`, `INTERNAL`.

The camera handles RPCs in the order received. Heavy commands (`ota.start`, `app.install`, `records.fetch`) answer `ok`
as soon as received and report the final result with an event. Commands that arrive while the camera is offline are
held by the broker (QoS 1, clean session false); the camera skips commands older than `timeoutS`, counted from
receipt.

## 10. Camera asks the cloud: camera → cloud

Same shape as RPC, in the other direction (`up/req` → `up/res`).

| Method | `params` | `result` |
|---|---|---|
| `hello` | `fw`, `model`, `provisionToken?`, `bootReason` | `serverTime`, `shadowVersion` |
| `ota.check` | `channel`, `fw` | `available`, `version`, `url`, `size`, `sha256`, `signature` |
| `time` | — | `serverTime` |

## 11. Video, pictures and files to CamOS

### 11.1 Video

- RTMP (later RTMPS) to the `url` given by `live.start`, in the form `rtmp://{host}/{app}/{streamId}?token={token}`. The
  token is single-use and expires after 60 s if the camera has not connected.
- H.264 video, a keyframe every 2 s; AAC audio when enabled. The camera stops by itself after `maxS` or on `live.stop`.

### 11.2 Recording on the SD card

- The camera records the main stream (H.264) as 300-second MP4 segments, named by the UTC time at their start:
  `rec/YYYYMMDD-HHMMSS-<seconds>.mp4` once closed; the segment being recorded has no `-<seconds>` yet.
- A segment cut by a power loss has no MP4 index and cannot be played; the camera deletes it at start-up. Before
  restarting by itself (OTA, `reboot`), the camera closes the segment being recorded.
- When the card has less than 1 GB free, the camera deletes the oldest segment.
- `records.fetch` ends with the event:

```json
{ "v": 1, "id": "rr<rpcId>", "ts": 1760256012345, "clock": "utc", "type": "record_result", "rpcId": "<id of records.fetch>",
  "ok": true, "key": "devices/AIC.../records/....mp4", "start": 1760255700000, "end": 1760256000000, "bytes": 27342122 }
```

Errors: `"ok": false, "error": { "code": "NOT_FOUND" | "IN_PROGRESS" | "SD_ERROR" | "UPLOAD_FAILED" }`. `IN_PROGRESS`:
`start` is inside the segment being recorded; ask again after it closes.

### 11.3 Event clips

A feature whose `actions` contain `"clip"`: the camera keeps the last ~60 s of video in RAM; on an event it takes from
the keyframe before −5 s to +15 s, packs an MP4 on the camera (video only, `moov` first), uploads it to
`/device/v1/clips?eventId=&start=&end=` and then sends:

```json
{ "v": 1, "id": "cr<eventId>", "ts": 1760256030000, "clock": "utc", "type": "clip_ready", "eventId": "<event id>",
  "key": "devices/AIC.../clips/....mp4", "start": 1760256007000, "end": 1760256028000, "bytes": 2233000 }
```

Errors: `"ok": false, "error": { "code": "NO_VIDEO" | "UPLOAD_FAILED" | "INTERNAL" }`. One clip at a time; the feature's
cooldown sets the pace.

### 11.4 Pictures and files: Device API

The camera uploads straight to the CamOS API; the backend chooses the storage (S3, GCS, disk) and returns a `key`. The
camera does not need to know where things are stored and does not use pre-signed URLs.

```
POST {apiBase}/device/v1/images?kind=snapshot&w=1920&h=1080&eventId={id}    (or &rpcId={id} for a capture command)
Content-Type: image/jpeg
Content-Length: ...
<JPEG>

201 {"key": "devices/AIC54F29F5167FA/snap/20261013/....jpg"}
```

- **Authentication:** HTTPS with the camera's certificate (mTLS), the same certificate as for MQTT; the serial comes from
  the certificate. The dev build uses HTTP and the `X-Device-Serial` header.
- **Token (added in 0.25):** when the API does not use mTLS (AWS API Gateway), the camera sends the header
  `X-Device-Token: <token>` in every request to `apiBase` (pictures, recordings, clips, and OTA / sound downloads when
  the URL is under `apiBase`; never to another host), together with `X-Device-Serial`. The token is set with
  `CAMOS=APITOKEN <token>` (at most 127 printable characters, no spaces) and stored on the camera.
- **TLS (added in 0.25):** `https://` verifies the server certificate against the CA stored with `CAMOS=CA` (a dev CA, or
  Amazon Root CA 1 on AWS; several CAs may be concatenated, at most 2048 bytes of PEM in total) and sends the host name
  in SNI. The camera presents its own certificate only when the server asks for it. `CAMOS=APIVERIFY OFF` disables
  verification (dev only).
- **`apiBase`:** given by CamOS at activation (together with the broker address). The dev default is
  `http://{broker}:8090` (`https://{broker}:8443` when the camera has a certificate), changed with `CAMOS=API <url>`, in
  the form `http(s)://host[:port][/stage]`; from 0.25 the value is stored (`CAMOS=API OFF` returns to the default).
- **Errors:** `401` unknown device, `413` file too large, `415` wrong type, `5xx` storage error. When an upload fails,
  the event is still sent with `image: null` and `imageError`; the `capture` command returns `UPLOAD_FAILED`.
- **SD recordings:** `POST {apiBase}/device/v1/records?start=&end=&rpcId=`, `Content-Type: video/mp4`, returns
  `201 {"key"}` like pictures.
- **Event clips:** `POST {apiBase}/device/v1/clips?eventId=&start=&end=`, `video/mp4`.
- **Later:** `/device/v1/logs` (`log.fetch`), same authentication.

## 12. Bringing a camera online

### 12.1 Label QR (the grower scans it with FlyBot)

```
CAMOS:1;SN:AIC0001A2B3;VC:K7P4QX;M:AIC-AMB82-K306P;;
```

### 12.2 Network configuration QR (FlyBot shows it, the camera scans it)

```
CAMOS:1;T:WPA;S:<ssid>;P:<password>;K:<provisionToken>;E:<mqtt-host>;A:<apn>;;
```

- The characters `;`, `:`, `\` in values are escaped with `\`, as in the Wi-Fi QR standard.
- `A` (APN) is optional. `E` is optional; by default the endpoint loaded at manufacturing is used.
- The camera signals that it read the code with the LED / a sound, then connects and sends `hello` with
  `provisionToken`.

### 12.3 SoftAP (fallback)

- Hold the pair button for 5 s. The camera opens the Wi-Fi `CAM-{last 4 characters of the serial}` (WPA2, password =
  the verify code on the label) and serves HTTP at `http://192.168.4.1`.
- `GET /info` → `{ serial, model, fw, wifiScan: [{ ssid, rssi, secure }] }`
- `POST /provision` with `{ ssid, password, provisionToken, endpoint?, apn? }` → `{ ok }`; the camera turns the SoftAP off
  and connects to the network.
- The SoftAP turns itself off after 10 minutes. This page only takes network information.

## 13. Open items

- The `roi` format for motion (a grid of cells or polygons) is settled after P0, once it is known what the SDK's
  `MotionDetection` supports.
- Bundle several `ai` messages into one packet to save 4G data.
- Signing and encrypting `.camapp` packages: the signature format (Ed25519 proposed).
