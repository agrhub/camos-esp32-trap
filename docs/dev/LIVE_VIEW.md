# Live view over KVS WebRTC (ESP32-S3): state and plan

Contract: flybot-aws/docs/CONTRACTS.md section 4. FlyBot creates one KVS signaling channel per camera (`cam-<serial>`)
and writes it into the shadow (`kvs.channelName`, `kvs.region`; also stored by pairing). The camera joins as MASTER when
`power.mode` is powered, or for `live.start {minutes}` / `live.requested` otherwise, and encodes only while a VIEWER is
connected. Timebox: 2 days of firmware work (21-22 Oct); fallback: RPC `snapshot.now`.

## Live picture on the LAN (MJPEG, firmware 0.4.0)

Independent of KVS: the web console streams `GET /api/stream` (multipart/x-mixed-replace JPEG) to one browser on the
same network, about 11 fps at VGA and 12.7 fps at 1280x720 measured on the Freenove board. Details and measurements:
[WEB_CONSOLE.md](WEB_CONSOLE.md). A KVS live session takes the camera over: the LAN stream ends (and answers `BUSY`)
while it runs.

## What the firmware has today (16 Oct)

- `live.c`: the session logic (`live.start` / `live.stop` / `live.requested`, the awake timer, the shadow `live` block
  with `supported`, `requested`, `active`), calling `live_kvs_start()` / `live_kvs_stop()` that a `live_kvs.c` must provide.
- `cam.c`: `cam_stream_start(w, h, q)` keeps the sensor up at QVGA / VGA JPEG for the encoder, `cam_stream_stop()`.
- `CAMOS_LIVE` is **off**: every `live.start` answers `NOT_SUPPORTED` with a message, FlyBot falls back to `snapshot.now`.
- The ESP32-CAM never gets live view (4 MB flash, no H.264, 4 MB quad PSRAM).

## What espressif/esp-webrtc-solution offers (read 16 Oct, repo main)

`solutions/kvs_master` is a KVS WebRTC MASTER demo: it answers VIEWER offers, with bidirectional audio and video.
Findings from its files:

- It **builds for esp32s3** (`sdkconfig.defaults.esp32s3` exists: octal PSRAM at 120 MHz, 240 MHz CPU, QIO flash, 4 MB
  flash size in the defaults) and for esp32p4; the README only names the ESP32-P4 Function EV board.
- `main/idf_component.yml` pulls `espressif/esp_h264 ~1.4` for `[esp32p4, esp32s3]` (the **software H.264 encoder** on
  the S3), `espressif/esp_peer` (the PeerConnection, registry + `override_path`), `kvs_signaling` (path inside the repo:
  `components/esp_webrtc/impl/kvs_signaling`), `tempotian/av_render`, `tempotian/media_lib_utils`, `esp_webrtc`
  and `webrtc_utils` (paths inside the repo), and `espressif/esp_board_manager ~0.7` for the board / camera setup.
- `main/settings.h`: on anything but the P4 the video is **320x240 at 10 fps** (`VIDEO_WIDTH 320`), Opus audio on.
- Credentials: menuconfig "KVS Signaling Configuration" takes a static **Access Key ID / Secret Access Key** and an
  optional **Session Token** (temporary credentials). There is no IoT credential provider in the demo.

- `components/esp_webrtc/impl/kvs_signaling/kvs_signaling.c` reads the credentials, region and channel name from
  **Kconfig constants** (`CONFIG_KVS_AWS_ACCESS_KEY_ID`, `..._SECRET_ACCESS_KEY`, `..._SESSION_TOKEN`, `CONFIG_KVS_AWS_REGION`,
  `CONFIG_KVS_CHANNEL_NAME`; `const char *channel_name = CONFIG_KVS_CHANNEL_NAME;`) and does SigV4 itself (`sigv4/`,
  `https_client.c`). Run-time values from NVS and the credential provider therefore need a patched copy of that one
  file (about 600 lines) under `firmware/components/kvs_signaling/`. The repository is 31 MB (GitHub `size`).

So on the ESP32-S3 the stack is: camera (esp_board_manager / esp_capture) -> esp_h264 software encoder at QVGA 10 fps
-> esp_peer (DTLS-SRTP) -> KVS signaling (WebSocket, SigV4). MJPEG is not a WebRTC video codec browsers accept, so
software H.264 is the only path on the S3; QVGA at 10 fps is what Espressif ships, VGA is to be tried.

## Plan for the timebox (21-22 Oct)

1. Vendor the needed components under `firmware/components/` as a git submodule of esp-webrtc-solution (the path
   dependencies `esp_webrtc`, `kvs_signaling`, `webrtc_utils`, `av_render`, `media_lib_utils`) and add
   `espressif/esp_peer`, `espressif/esp_h264` to `idf_component.yml` under `rules: if target == esp32s3`. Disk: the repo
   clone is to be checked (keep it under 200 MB; the D: drive has about 3 GB).
2. `live_kvs.c`: `esp_webrtc` with the `kvs_signaling` implementation, our own capture source feeding JPEG frames from
   `cam_stream_start()` through `esp_jpeg` decode -> `esp_h264` encode (or `esp_capture` with the camera if
   `esp_board_manager` has the Freenove board; it has the ESP32-S3-EYE map which is the same), audio off (`DATA_CHANNEL`
   off). Encode only on the `esp_webrtc` "viewer connected" event.
3. Credentials: the **IoT Credential Provider** (role alias) is the right source: the camera already has the IoT
   certificate, `GET https://<cred_host>/role-aliases/<alias>/credentials` with mTLS returns temporary keys + session
   token, which the demo's "Session Token" field proves `kvs_signaling` accepts. `flybot.c` stores `credHost` and
   `roleAlias` from pairing for this. If the SigV4 code in `kvs_signaling` cannot take a session token at run time,
   document it and fall back to static keys set through the portal (`kvsAccessKey` / `kvsSecret`, not in the contract).
4. Memory: the kvs_master defaults put lwIP and mbedTLS in PSRAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`,
   `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`); our defaults already do the second. DTLS-SRTP needs
   `CONFIG_MBEDTLS_SSL_PROTO_DTLS=y`, `CONFIG_MBEDTLS_SSL_DTLS_SRTP=y`, `CONFIG_MBEDTLS_X509_CREATE_C=y`, WebSocket
   transport `CONFIG_WS_TRANSPORT=y`: to be added to the Freenove defaults when `CAMOS_LIVE` is on.
5. If it does not fit the flash (3 MB slot) or does not build in the timebox: stop, keep `CAMOS_LIVE=n`, record here why,
   and FlyBot uses `snapshot.now` for "live".

## Open questions to verify on hardware

- Whether `esp_h264` software encoding at QVGA 10 fps leaves enough CPU for TLS + Wi-Fi on the S3 (Espressif ships it,
  so presumably yes) and what the frame latency is.
- Whether the A7680C's Cat.1 uplink (datasheet up to 5 Mbit/s) carries 300-500 kbit/s of H.264 plus the STUN/TURN
  overhead; TURN is likely behind a carrier NAT, which KVS provides.
