# Giao thức thiết bị CamOS AI Camera

> Phiên bản giao thức `1` · bản nháp 0.1 · 2026-10-12
> Phạm vi: camera ↔ CamOS. Ứng dụng (FlyBot…) không dùng giao thức này; ứng dụng dùng [Open API](open-api.md).

## 1. Kết nối

| | |
|---|---|
| Giao thức | MQTT 3.1.1 trên TLS 1.2+, cổng 8883 |
| Xác thực | Mutual TLS: mỗi camera có chứng chỉ client riêng, nạp lúc sản xuất. CN của chứng chỉ = `serial` |
| Client ID | `serial` |
| Keepalive | 60 s |
| Clean session | `false` (giữ subscription và tin QoS 1 khi mất mạng ngắn) |
| Last Will | topic `camos/{serial}/state`, retained, payload `{"v":1,"online":false}` |
| Quyền | Broker chỉ cho camera publish/subscribe các topic bắt đầu bằng `camos/{serial}/` |

Mất kết nối thì kết nối lại với thời gian chờ tăng dần (1, 2, 4… tối đa 300 s, có ngẫu nhiên ±20%). Sau khi đổi mạng (Wi-Fi ⇄ 4G) thì kết nối lại ngay.

## 2. Topic

| Topic | Chiều | QoS | Retained | Nội dung |
|---|---|---|---|---|
| `camos/{serial}/state` | camera → cloud | 1 | có | Trạng thái hiện tại (mục 4) |
| `camos/{serial}/event` | camera → cloud | 1 | không | Sự kiện (mục 5) |
| `camos/{serial}/ai` | camera → cloud | 0 | không | Kết quả nhận dạng theo khung hình khi đang live (mục 6) |
| `camos/{serial}/telemetry` | camera → cloud | 0 | không | Số đo định kỳ (mục 7) |
| `camos/{serial}/shadow/desired` | cloud → camera | 1 | có | Cấu hình mong muốn (mục 8) |
| `camos/{serial}/shadow/reported` | camera → cloud | 1 | có | Cấu hình đã áp dụng |
| `camos/{serial}/rpc/req` | cloud → camera | 1 | không | Lệnh từ cloud (mục 9) |
| `camos/{serial}/rpc/res` | camera → cloud | 1 | không | Kết quả lệnh |
| `camos/{serial}/up/req` | camera → cloud | 1 | không | Camera hỏi cloud (mục 10) |
| `camos/{serial}/up/res` | cloud → camera | 1 | không | Cloud trả lời |

Camera subscribe: `shadow/desired`, `rpc/req`, `up/res`.

## 3. Quy ước chung

- Payload là JSON UTF-8. Mọi tin có `"v": 1` (phiên bản giao thức).
- `ts`: thời điểm theo mili giây Unix, UTC. Camera đồng bộ giờ bằng NTP, khi không có NTP thì dùng giờ GPS.
- `id`: ULID do bên gửi tạo. Dùng để ghép lệnh với kết quả, và để bên nhận bỏ qua tin trùng.
- Tọa độ khung nhận dạng và vùng ROI là số thực **chuẩn hóa 0–1** theo khung ảnh gốc, nên không phụ thuộc độ phân giải.
- Trường không biết thì bỏ qua. Không đổi nghĩa trường cũ; thay đổi không tương thích thì tăng `v`.

## 4. State

Camera publish khi kết nối xong và mỗi khi có thay đổi đáng kể (đổi mạng, SD, app, firmware).

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

- `boxes`: `[x1, y1, x2, y2, classIndex, confidence]`. Camera gửi tối đa 200 box. `n` luôn là số đếm thật, `truncated: true` nếu có box bị bỏ bớt.
- `image.key` do Device API trả về khi camera upload ảnh (mục 11.4). Camera upload ảnh trước rồi mới gửi sự kiện, kèm `eventId` để backend gắn ảnh với sự kiện. Nếu camera upload ảnh lỗi thì gửi `image: null` kèm `"imageError": "..."`; cloud vẫn nhận sự kiện.
- Camera không có mạng thì xếp sự kiện vào hàng đợi trên SD (tối đa 10.000 tin hoặc 7 ngày), gửi lại theo thứ tự khi có mạng. Cloud bỏ tin trùng theo `id`.

| `type` | Trường riêng |
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

## 6. AI (luồng khi đang live)

Chỉ gửi khi đang có live (`live.start`), tối đa 5 tin/giây. CamOS chuyển vào data channel WebRTC của stream.

```json
{ "v": 1, "ts": 1760256012345, "pts": 912345, "detectionId": "person", "n": 2, "truncated": false, "boxes": [[0.41, 0.22, 0.55, 0.81, 0, 0.88]] }
```

`pts` là timestamp video (ms) của khung hình được nhận dạng, để player khớp khung với hình.

## 7. Telemetry

Gửi mỗi 5 phút (mỗi 30 phút khi dùng 4G).

```json
{ "v": 1, "ts": 1760256000000, "uptimeS": 86400, "cpu": 41, "heapFree": 21000000, "fps": { "main": 15, "sub": 15 }, "npuMs": { "person": 38, "fruit-fly-sticky": 2400 }, "dataUsedMB": { "wifi": 812.4, "cell": 35.1 }, "rebootCount": 2 }
```

## 8. Shadow

CamOS là bên giữ bản `desired`; camera là bên giữ bản `reported`. Khi `desired.version` đổi:
1. Camera áp dụng cấu hình mới.
2. Camera publish `reported` với cùng `version` và danh sách lỗi (nếu có).

Những phần chưa áp dụng được thì camera giữ nguyên cấu hình cũ.

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

- `audio`: `{ "enabled": true, "speakerVolume": 80 }`: `enabled` bật hoặc tắt tiếng micro (luồng vẫn có track AAC, chỉ im lặng khi tắt); `speakerVolume` 0-100 là âm lượng loa (0 = tắt loa).
- `video.image` (thêm 0.25): chỉnh ISP, áp dụng ngay (không khởi động lại luồng) và camera lưu vào flash nên giữ qua lần khởi động lại. Mọi trường đều tùy chọn, là số nguyên:

  | Trường | Khoảng | Nghĩa |
  |---|---|---|
  | `brightness` | -64..64 | độ sáng |
  | `contrast`, `saturation`, `sharpness` | 0..100 | tương phản, bão hòa, độ nét |
  | `flicker` | 0..3 | chống nhấp nháy đèn: 0 tắt, 1 50 Hz, 2 60 Hz, 3 tự động |
  | `wdr` | 0..2 | WDR: 0 tắt, 1 thủ công (mức của tuning), 2 tự động |
  | `orientation` | 0..3 | 0 bình thường, 1 lật ngang, 2 lật dọc, 3 cả hai |
  | `ldc` | 0..1 | sửa méo ống kính |

  Giá trị ngoài khoảng hoặc không phải số nguyên: camera giữ giá trị cũ của trường đó và báo `{ "path": "video.image.<trường>", "code": "BAD_VALUE" }`; trường lạ: `NOT_SUPPORTED`. Khoảng giống trang cấu hình web của camera. Quy tắc: shadow thắng; thay đổi từ trang web có hiệu lực tới `version` mới hoặc lần kết nối sau (bản `desired` retained được áp dụng lại mỗi lần camera khởi động).
- `capture.resolution` (thêm 0.25): `"main"` (mặc định) hoặc `"max"`: ảnh của `capture` và ảnh chụp theo sự kiện lấy ở kích thước đầy đủ của sensor. Firmware 0.25: luồng chính luôn chạy ở kích thước đầy đủ của sensor (K306P 2560x1440, F37 1920x1080) và JPEG lấy từ chính luồng đó, nên hai giá trị cho cùng một ảnh; `w`/`h` trong `image` luôn là kích thước thật đọc từ JPEG. `capture.jpegQuality` chưa hỗ trợ (`NOT_SUPPORTED`).
- Action `"siren"` trong `actions` của một tính năng: khi có sự kiện, loa phát còi báo động 5 giây (camera có loa).
- Shadow không bao giờ chứa mật khẩu Wi-Fi. Mật khẩu chỉ đi qua RPC `net.set` và chỉ lưu trên camera.
- `schedule` theo giờ địa phương của `timezone`.
- `type: "app"` cần app tương ứng đã được cài. Nếu chưa có, camera báo lỗi `APP_NOT_INSTALLED` trong `reported.errors`; CamOS tự cài app rồi gửi lại.

`reported`:

```json
{ "v": 1, "version": 15, "appliedAt": 1760256100000, "errors": [{ "path": "detections[2]", "code": "APP_NOT_INSTALLED" }],
  "video": { "image": { "brightness": 40, "contrast": 50, "saturation": 50, "sharpness": 50, "flicker": 1, "wdr": 2, "orientation": 0, "ldc": 0 } },
  "capture": { "resolution": "main", "w": 2560, "h": 1440 } }
```

- `video.image` và `capture` trong `reported` (thêm 0.25): giá trị đang chạy sau khi áp dụng (đã lưu, hoặc của tuning ISP khi chưa ai đặt; `null` khi ISP chưa đọc được). `capture.w`/`h` là kích thước JPEG của luồng chính.
- Lúc kết nối (thêm 0.25): `state` retained (mục 4) cũng có `video.image` và `capture` như trên, nên cloud biết cấu hình ảnh của camera ngay cả khi chưa có `desired` nào.

## 9. RPC: cloud → camera

Yêu cầu:

```json
{ "v": 1, "id": "01JA...", "method": "capture", "params": { "detectionId": "trap-count" }, "timeoutS": 60 }
```

Kết quả:

```json
{ "v": 1, "id": "01JA...", "ok": true, "result": { "image": { "key": "devices/AIC0001/snap/....jpg", "w": 2560, "h": 1440, "bytes": 412345 }, "bytes": 412345, "ai": { "n": 37, "boxes": [] } } }
{ "v": 1, "id": "01JA...", "ok": false, "error": { "code": "BUSY", "message": "OTA in progress" } }
```

| Method | `params` | `result` |
|---|---|---|
| `capture` | `detectionId?` (chạy nhận dạng của tính năng đó trên ảnh) | `image` (`key`, `w`, `h`, `bytes`; `bytes` trong `image` thêm 0.25), `bytes`, `ai?`. `w`/`h` đọc từ JPEG. Ảnh upload lên Device API (mục 11.4). Kích thước theo `capture.resolution` (mục 8) |
| `live.start` | `url` (RTMP), `token`, `stream` (`main`/`sub`), `maxS` | `streamId` |
| `live.stop` | — | — |
| `records.list` | `from`, `to` (ms) | `segments: [{ start, end, bytes, recording? }]`, `more` (tối đa 100 đoạn; `more: true` thì hỏi tiếp từ `end` cuối) |
| `records.fetch` | `start` (ms) | — ngay khi nhận. Camera upload đoạn chứa `start` lên Device API (mục 11.4) rồi gửi sự kiện `record_result` |
| `net.set` | `wifi: { ssid, password }`, `cellular: { apn, user?, password? }` | — |
| `ota.start` | `version`, `url` (HTTPS), `size`, `sha256` (hex), `signature` (base64 DER, ECDSA P-256 trên SHA-256 của ảnh, khoá release của CamOS) | — ngay khi nhận; lỗi tức thì: `BAD_PARAMS`, `BUSY`, `OLD_VERSION`. Sau đó sự kiện `ota_result`: `stage: "verified"` (đúng kích thước, SHA-256 và chữ ký, chưa ghi gì), `stage: "installed"` (đã ghi slot dự phòng, camera khởi động lại), hoặc `ok: false` với `DOWNLOAD_FAILED`, `SIGNATURE_INVALID`, `INSTALL_FAILED`. Ảnh sai không bao giờ được ghi |
| `speaker.play` | `tone` (`"beep"` hoặc `"siren"`) và `seconds` (1-60, mặc định 3); hoặc `url` (http/https, file WAV PCM 16-bit, mono hoặc stereo, tần số bất kỳ, tối đa 3 MB) | — khi đã bắt đầu phát; lỗi: `NOT_SUPPORTED` (không có loa), `BAD_PARAMS`, `BUSY` (đang tải một file khác). Âm mới thay âm đang phát |
| `speaker.stop` | — | — |
| `app.install` | `id`, `version`, `url`, `size`, `sha256`, `signature` | — (kết quả báo bằng sự kiện `app_result`) |
| `app.remove` | `id` | — |
| `reboot` | — | — |
| `format_sd` | — | — |
| `factory_reset` | — | — (camera xóa mạng, cấu hình, app; giữ chứng chỉ) |
| `log.fetch` | `lines?` | `key` (file upload lên Device API) |

Mã lỗi: `BAD_PARAMS`, `BUSY`, `NOT_SUPPORTED`, `NO_SD`, `SD_ERROR`, `APP_NOT_INSTALLED`, `SIGNATURE_INVALID`, `DOWNLOAD_FAILED`, `UPLOAD_FAILED`, `TIMEOUT`, `INTERNAL`.

Camera xử lý RPC theo thứ tự nhận. Lệnh nặng (`ota.start`, `app.install`, `records.fetch`) trả `ok` ngay khi nhận và báo kết quả cuối bằng sự kiện. Lệnh đến khi camera đang mất mạng sẽ được broker giữ lại (QoS 1, clean session false); camera bỏ qua lệnh đã quá `timeoutS` tính từ lúc nhận.

## 10. Camera hỏi cloud: camera → cloud

Cùng dạng với RPC, nhưng chiều ngược lại (`up/req` → `up/res`).

| Method | `params` | `result` |
|---|---|---|
| `hello` | `fw`, `model`, `provisionToken?`, `bootReason` | `serverTime`, `shadowVersion` |
| `ota.check` | `channel`, `fw` | `available`, `version`, `url`, `size`, `sha256`, `signature` |
| `time` | — | `serverTime` |

## 11. Video, ảnh và file lên CamOS

### 11.1 Video

- RTMP (sau này RTMPS) tới `url` do `live.start` cấp, dạng `rtmp://{host}/{app}/{streamId}?token={token}`. Token dùng một lần, hết hạn sau 60 s nếu camera chưa kết nối.
- Video H.264, keyframe mỗi 2 s; âm thanh AAC nếu bật. Camera tự dừng khi quá `maxS` hoặc khi nhận `live.stop`.

### 11.2 Ghi hình trên thẻ SD

- Camera ghi luồng chính (H.264) thành các đoạn MP4 dài 300 s, tên theo giờ UTC lúc bắt đầu: `rec/YYYYMMDD-HHMMSS-<giây>.mp4` khi đã đóng; đoạn đang ghi chưa có `-<giây>`.
- Đoạn dở dang do mất điện không có index MP4 nên không phát được; camera xóa khi khởi động. Trước khi tự khởi động lại (OTA, `reboot`), camera đóng đoạn đang ghi.
- Thẻ còn dưới 1 GB trống thì camera xóa đoạn cũ nhất.
- `records.fetch` kết thúc bằng sự kiện:

```json
{ "v": 1, "id": "rr<rpcId>", "ts": 1760256012345, "clock": "utc", "type": "record_result", "rpcId": "<id của records.fetch>",
  "ok": true, "key": "devices/AIC.../records/....mp4", "start": 1760255700000, "end": 1760256000000, "bytes": 27342122 }
```

Lỗi: `"ok": false, "error": { "code": "NOT_FOUND" | "IN_PROGRESS" | "SD_ERROR" | "UPLOAD_FAILED" }`. `IN_PROGRESS`: `start` nằm trong đoạn đang ghi, hỏi lại sau khi đoạn đóng.

### 11.3 Clip theo sự kiện

Tính năng có `actions` chứa `"clip"`: camera giữ ~60 s video gần nhất trong RAM; khi có sự kiện, nó lấy từ keyframe trước mốc −5 s tới +15 s, đóng gói MP4 ngay trên camera (chỉ video, `moov` ở đầu), upload lên `/device/v1/clips?eventId=&start=&end=` rồi gửi:

```json
{ "v": 1, "id": "cr<eventId>", "ts": 1760256030000, "clock": "utc", "type": "clip_ready", "eventId": "<id sự kiện>",
  "key": "devices/AIC.../clips/....mp4", "start": 1760256007000, "end": 1760256028000, "bytes": 2233000 }
```

Lỗi: `"ok": false, "error": { "code": "NO_VIDEO" | "UPLOAD_FAILED" | "INTERNAL" }`. Mỗi lúc chỉ làm một clip; cooldown của tính năng giữ nhịp.

### 11.4 Ảnh và file: Device API

Camera upload thẳng lên API của CamOS; backend tự chọn nơi lưu (S3, GCS, đĩa) và trả `key`. Camera không cần biết nơi lưu, không dùng URL ký sẵn.

```
POST {apiBase}/device/v1/images?kind=snapshot&w=1920&h=1080&eventId={id}    (hoặc &rpcId={id} khi chụp theo lệnh capture)
Content-Type: image/jpeg
Content-Length: ...
<JPEG>

201 {"key": "devices/AIC54F29F5167FA/snap/20261013/....jpg"}
```

- **Xác thực:** HTTPS với chứng chỉ của camera (mTLS), cùng chứng chỉ dùng cho MQTT; serial lấy từ chứng chỉ. Bản dev dùng HTTP và header `X-Device-Serial`.
- **Token (thêm 0.25):** khi API không dùng mTLS (AWS API Gateway), camera gửi header `X-Device-Token: <token>` trong mọi request tới `apiBase` (ảnh, đoạn ghi, clip, và tải OTA/âm thanh nếu URL nằm dưới `apiBase`; không bao giờ gửi tới host khác), cùng với `X-Device-Serial`. Token đặt bằng `CAMOS=APITOKEN <token>` (tối đa 127 ký tự in được, không có khoảng trắng), lưu trên camera.
- **TLS (thêm 0.25):** `https://` kiểm chứng chứng chỉ server theo CA lưu bằng `CAMOS=CA` (CA dev, hoặc Amazon Root CA 1 trên AWS; có thể nạp nhiều CA nối nhau, tổng ≤ 2048 byte PEM), gửi tên host trong SNI. Camera trình chứng chỉ của mình chỉ khi server yêu cầu. `CAMOS=APIVERIFY OFF` bỏ kiểm chứng (chỉ cho dev).
- **`apiBase`:** CamOS cấp khi kích hoạt (cùng lúc với địa chỉ broker). Bản dev mặc định là `http://{broker}:8090` (`https://{broker}:8443` khi camera có chứng chỉ), đổi được bằng `CAMOS=API <url>`, dạng `http(s)://host[:port][/stage]`; từ 0.25 giá trị này được lưu (`CAMOS=API OFF` về mặc định).
- **Lỗi:** `401` thiết bị lạ, `413` file quá lớn, `415` sai loại, `5xx` lỗi lưu trữ. Upload lỗi thì sự kiện vẫn được gửi với `image: null` và `imageError`; lệnh `capture` trả `UPLOAD_FAILED`.
- **Đoạn ghi SD:** `POST {apiBase}/device/v1/records?start=&end=&rpcId=`, `Content-Type: video/mp4`, trả `201 {"key"}` như ảnh.
- **Clip sự kiện:** `POST {apiBase}/device/v1/clips?eventId=&start=&end=`, `video/mp4`.
- **Sau này:** `/device/v1/logs` (`log.fetch`), cùng cách xác thực.

## 12. Đưa camera lên mạng

### 12.1 QR trên nhãn (chủ vườn quét bằng FlyBot)

```
CAMOS:1;SN:AIC0001A2B3;VC:K7P4QX;M:AIC-AMB82-K306P;;
```

### 12.2 QR cấu hình mạng (FlyBot hiện, camera quét)

```
CAMOS:1;T:WPA;S:<ssid>;P:<password>;K:<provisionToken>;E:<mqtt-host>;A:<apn>;;
```

- Ký tự `;`, `:`, `\` trong giá trị được thoát bằng `\`, giống chuẩn QR Wi-Fi.
- `A` (APN) không bắt buộc. `E` không bắt buộc; mặc định dùng endpoint nạp lúc sản xuất.
- Camera báo đã đọc mã bằng LED/âm thanh, rồi kết nối và gửi `hello` kèm `provisionToken`.

### 12.3 SoftAP (dự phòng)

- Giữ nút pair 5 s. Camera phát Wi-Fi `CAM-{4 ký tự cuối serial}` (WPA2, mật khẩu = mã xác minh trên nhãn) và chạy HTTP tại `http://192.168.4.1`.
- `GET /info` → `{ serial, model, fw, wifiScan: [{ ssid, rssi, secure }] }`
- `POST /provision` với `{ ssid, password, provisionToken, endpoint?, apn? }` → `{ ok }`; camera tắt SoftAP rồi kết nối mạng.
- SoftAP tự tắt sau 10 phút. Trang này chỉ nhận thông tin mạng.

## 13. Việc còn mở

- Dạng `roi` cho motion (lưới ô hay đa giác) chốt sau P0, khi biết `MotionDetection` của SDK hỗ trợ gì.
- Gom nhiều tin `ai` vào một gói để tiết kiệm dữ liệu 4G.
- Ký và mã hóa gói `.camapp`: định dạng chữ ký (Ed25519 đề xuất).
