# Live view

There are two ways to watch the camera live.

## 1. On the same network: MJPEG in the web console (available)

The web console streams `GET /api/stream` (multipart/x-mixed-replace JPEG) to one browser at a time on the same
network. Measured on the Freenove board on 16 Oct 2026: about 11 fps at 640x480 and 12.7 fps at 1280x720
(table and setup: [../WEB_CONSOLE.md](../WEB_CONSOLE.md)). While a cloud live session (below) runs, the LAN stream
answers `BUSY`.

## 2. Through the cloud: Kinesis Video Streams WebRTC (not in the release builds)

Contract (FlyBot): one KVS signaling channel per camera (`cam-<serial>`), written into the shadow (`kvs.channelName`,
`kvs.region`). The camera would join as MASTER while `power.mode` is powered, or for `live.start {minutes}` /
`live.requested`, and encode only while a VIEWER is connected.

What the firmware has:

- `live.c`: the session logic (`live.start`, `live.stop`, `live.requested`, the awake timer, the shadow `live` block with
  `supported`, `requested`, `active`), calling `live_kvs_start()` / `live_kvs_stop()`, which a `live_kvs.c` must provide.
- `cam.c`: `cam_stream_start(w, h, q)` keeps the sensor running for an encoder.
- `CAMOS_LIVE` is **off**: `live.start` answers `NOT_SUPPORTED` and FlyBot falls back to `snapshot.now`.
- The ESP32-CAM build never has live view (4 MB flash, no H.264 encoder).

How it can be added on the ESP32-S3 (from reading espressif/esp-webrtc-solution, `solutions/kvs_master`):

- Stack: camera -> software H.264 (`espressif/esp_h264`, the demo uses 320x240 at 10 fps on the S3) -> `esp_peer`
  (DTLS-SRTP) -> KVS signaling (WebSocket, SigV4). Browsers do not accept MJPEG over WebRTC, so software H.264 is the
  only path on the S3.
- The demo's `kvs_signaling.c` takes the credentials, region and channel from Kconfig constants; run-time values need a
  patched copy under `firmware/components/kvs_signaling/`.
- Credentials: the AWS IoT credential provider (role alias) with the device certificate returns temporary keys and a
  session token; `flybot.c` already stores `credHost` and `roleAlias` from pairing.
- sdkconfig additions: `CONFIG_MBEDTLS_SSL_PROTO_DTLS`, `CONFIG_MBEDTLS_SSL_DTLS_SRTP`, `CONFIG_MBEDTLS_X509_CREATE_C`,
  `CONFIG_WS_TRANSPORT`, lwIP in PSRAM.
- Open points: CPU headroom for H.264 + TLS + Wi-Fi on the S3, frame latency, and whether the A7680C's Cat.1 uplink
  carries the stream plus TURN overhead.
