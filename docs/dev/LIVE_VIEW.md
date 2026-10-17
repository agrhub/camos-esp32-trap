# Live view

There are two ways to watch the camera live.

## 1. On the same network: MJPEG in the web console

The web console streams `GET /api/stream` (multipart/x-mixed-replace JPEG) to one browser at a time on the same
network. Measured on the Freenove board on 16 Oct 2026: about 11 fps at 640x480 and 12.7 fps at 1280x720
(table and setup: [../WEB_CONSOLE.md](../WEB_CONSOLE.md)). While a cloud live session (below) runs, the LAN stream
answers `409 BUSY`.

## 2. Through the cloud: Kinesis Video Streams WebRTC (Freenove build)

Contract (FlyBot, `flybot-aws/docs/CONTRACTS.md` section 4): one KVS signaling channel per camera (`cam-<serial>`),
written into the shadow (`kvs.channelName`, `kvs.region`). The browser (FlyBot's `KvsViewer`) joins as VIEWER and
sends an SDP offer; the camera is the MASTER and answers.

### What the firmware does (`CAMOS_LIVE`, on in the `freenove` preset)

| Step | Where | How |
|---|---|---|
| Credentials | `live_kvs.c` | AWS IoT credential provider: `GET https://<credHost>/role-aliases/<roleAlias>/credentials` with the device certificate (mutual TLS) and `x-amzn-iot-thingname`; 1 h keys, renewed 5 min before they expire. `credHost` / `roleAlias` come from pairing (`iot.credentialsEndpoint`, `iot.roleAlias`), registration (same fields) or the shadow (`kvs.credentialsEndpoint`, `kvs.roleAlias`). |
| Channel | `live_kvs.c` | `DescribeSignalingChannel` (ARN), `GetSignalingChannelEndpoint` (role MASTER: WSS + HTTPS), `GetIceServerConfig` (TURN), all SigV4-signed (`sigv4.c`, host-tested in `tests/test_sigv4.c` against the AWS reference vectors). |
| Signaling | `live_kvs.c` | WebSocket to the presigned WSS URL (`X-Amz-ChannelARN`, 299 s); `SDP_OFFER` / `ICE_CANDIDATE` in, `SDP_ANSWER` out (the answer carries the camera's candidates: host, server-reflexive and relay). One viewer at a time; a new offer replaces the current viewer. Browser mDNS (`.local`) candidates are skipped: the viewer's checks reach the camera's host candidate and become peer-reflexive. |
| Peer | `espressif/esp_peer` 1.5.7 | ICE (one KVS TURN server over UDP, which also answers STUN), DTLS-SRTP, RTP with NACK. Opened for each offer, closed when the viewer leaves (DTLS close, ICE keep-alive lost) or when no connection is made within 30 s. |
| Video | `espressif/esp_h264` 1.4.1 | software H.264 (openh264, constrained baseline) from YUV422 camera frames, 320x240, target 10 fps and 300 kbit/s, GOP 2 s, IDR on PLI. Only while a viewer is connected; no audio is sent (the answer keeps a send-only PCMA line because viewers offer audio). |
| When | `live.c` | Powered mode (`CAMOS_LIVE_STANDBY`): the camera stays on the channel all the time, so a viewer connects at once. Battery mode: only for `live.start {minutes}` / `live.requested` (5 min), which also keep it awake. The window's end drops the viewer; `live.stop` drops the viewer and ends the window. |
| Camera | `cam.c`, `sched.c` | The encoder takes the camera lock (`camera_take`) when the offer arrives, so the camera's DMA buffer and the encoder are allocated before the DTLS session; a running capture finishes first, the web console preview and the LAN MJPEG stream stop. The sensor runs raw YUV422 at `CAMOS_LIVE_XCLK_MHZ` (20 MHz; JPEG captures keep `CAMOS_XCLK_MHZ`). Scheduled captures and `snapshot.now` answer `BUSY` while a viewer watches, and run normally while the camera only waits on the channel. |
| Shadow | `shadow.c` | `live.supported` = built in, paired (channel + region) and a credential source (credential provider settings, or bench keys); `live.active` = a live window is open or a viewer watches. |

Serial console: `live` (status JSON: state `off` / `waiting` / `joining` / `joined` / `viewer` / `retry`, viewer,
fps, kbit/s, encode and camera times, last error), `live start [min]`, `live stop`. The web console's `/api/status`
has the same object under `live`.

### Measured on the board (17 Oct 2026)

Freenove FNK0085 (ESP32-S3 N8R8 at 240 MHz, OV5640), Wi-Fi at -43 dBm, channel `cam-<serial>` in ap-southeast-2.
Viewer: a bench page with the KVS WebRTC JS SDK 2.9.0 in Chromium on a PC on the same Wi-Fi, doing what FlyBot's
`KvsViewer` does (recvonly video + audio, trickle ICE). Credentials: bench keys (the AWS side has no role alias yet,
see below). The room was lit by fading daylight, then dark (evening); the encoder's time per frame follows the noise
in the picture, so the numbers are given per light level.

| | Measured |
|---|---|
| Picture | 320x240 H.264 (`video/H264`), decoded by the browser; 0 frames dropped, 0 packets lost, 0 NACKs; colours match a JPEG of the same scene |
| Frame rate, dim room | 7.5-7.8 fps (camera wait 73-77 ms + encode 50-58 ms per frame) |
| Frame rate, dark room (high gain) | 5.1-6.4 fps, 1,420 frames in 221 s (camera wait 39-86 ms + encode 79-154 ms) |
| Sensor clock | XCLK 10 MHz: 3.9 fps (camera wait 180 ms); 24 MHz: camera wait 10 ms but a darker picture and encode 92-117 ms (the raw frames compete with the encoder for PSRAM bandwidth); 20 MHz is the default |
| Bitrate | 4-40 kbit/s for a still indoor scene (the 300 kbit/s target is a ceiling) |
| Session start | camera: offer to answer 1.6-3.3 s (TURN allocation, the sensor starting), viewer connected 3.0-5.0 s after the offer. Browser: offer to first frame 4.6-7.1 s (one run 12 s: ICE took 5 s) |
| Viewer leaves | noticed after 29 s with esp_peer's default keep-alive (5 checks); the firmware now uses 3 checks (about 20 s, not measured) |
| Network | host / host UDP on the LAN; RTT 14-300 ms (mean 100 ms), the browser's jitter buffer settles at 37-41 ms, decode 1-4 ms per frame |
| Latency | not measured glass-to-glass; from the parts: frame age up to one frame period + encode 50-150 ms + half the RTT + jitter buffer 40 ms + decode, about 0.2-0.4 s |
| Memory | free internal RAM 95 KB after boot (88 KB before the 1 KB PSRAM threshold), about 77 KB while waiting on the channel, 16.2-17.5 KB during a session (stable over 4 minutes); free PSRAM 5.99 MB during a session |
| CPU | the encoder runs on core 1 and is busy about 40-70 % of the time (encode time x frame rate); Wi-Fi, lwIP, DTLS-SRTP, the signaling and the camera DMA task run on core 0 |
| Two sessions in a row | the second viewer connected to the same camera without a restart |
| LAN MJPEG after the change | 320x240 and 640x480 at about 10 fps (120 frames in 12 s, connection set-up included); answers `409 BUSY` while the cloud viewer watches |
| Image size | 1,939,344 bytes of the 3 MB app slot (38 % free); 0.4.2 without live view: 1,532,224 bytes. ESP32-CAM preset: 1,432,336 bytes (19 % of its 1.69 MB slot free), `esp32s3cam` preset (live view off): 1,461,648 bytes |
| Credential provider | the call with the device certificate works (mutual TLS) and answers 403 Access Denied: no role alias on the AWS side |

The 10 fps target is not reached: the software encoder needs 50-150 ms for a 320x240 frame on the S3 and the raw
sensor mode adds its own wait. Larger sizes drop the frame rate in proportion (`CAMOS_LIVE_WIDTH` / `_HEIGHT`).

Not measured: the 4G (A7680C) uplink, a remote viewer through a TURN relay (the camera's relay candidate is there,
the bench viewer was on the same LAN), a session on battery, glass-to-glass latency, daylight frame rate.

### What the AWS side still needs (flybot-aws, not deployed by this change)

The camera's credential provider call answers `403 {"message":"Access Denied"}` today: the stack has no IoT role alias
and the device policy does not allow `iot:AssumeRoleWithCertificate`. `infra/template.yaml` needs:

1. An IAM role the IoT credential provider can assume, with the master rights on the camera's own channel only:

   ```yaml
   CameraKvsRole:
     Type: AWS::IAM::Role
     Properties:
       RoleName: !Sub 'flybot-camera-kvs-${Stage}'
       AssumeRolePolicyDocument:
         Version: '2012-10-17'
         Statement:
           - Effect: Allow
             Principal: {Service: credentials.iot.amazonaws.com}
             Action: sts:AssumeRole
       Policies:
         - PolicyName: kvs-master
           PolicyDocument:
             Version: '2012-10-17'
             Statement:
               - Effect: Allow
                 Action: [kinesisvideo:ConnectAsMaster, kinesisvideo:DescribeSignalingChannel,
                          kinesisvideo:GetSignalingChannelEndpoint, kinesisvideo:GetIceServerConfig]
                 Resource: !Sub 'arn:${AWS::Partition}:kinesisvideo:${AWS::Region}:${AWS::AccountId}:channel/cam-${!credentials-iot:ThingName}/*'
   CameraKvsRoleAlias:
     Type: AWS::IoT::RoleAlias
     Properties:
       RoleAlias: !Sub 'flybot-camera-kvs-${Stage}'
       RoleArn: !GetAtt CameraKvsRole.Arn
       CredentialDurationSeconds: 3600
   ```

2. One more statement in `CameraPolicy` (`camos-camera-<stage>`; the policy is close to the 2,048-character cap):

   ```yaml
   - Effect: Allow
     Action: iot:AssumeRoleWithCertificate
     Resource: !GetAtt CameraKvsRoleAlias.RoleAliasArn
   ```

3. FlyBot must tell the camera where to ask: the stack's credential provider endpoint
   (`aws iot describe-endpoint --endpoint-type iot:CredentialProvider`) and the role alias name, as
   `iot.credentialsEndpoint` / `iot.roleAlias` in the `/api/device/register` and `/api/device/pair` answers, and as
   `kvs.credentialsEndpoint` / `kvs.roleAlias` in the shadow `desired` for cameras that registered before (the
   firmware takes all three). The endpoint can go into `.env` like `IOT_ENDPOINT`.

The thing name must stay the serial: the role policy's `${credentials-iot:ThingName}` is the `x-amzn-iot-thingname`
header, which the credential provider checks against the thing attached to the certificate.

### Bench test without the AWS side

`CAMOS_LIVE_BENCH_CREDS` (off in every preset) adds `live key id|secret|token <value>` to the serial console: temporary
keys held in RAM only, never stored or printed (`token` appends; the console line is 256 characters). Use short-lived
keys. With them loaded the camera joins its channel as it would with the credential provider.

### Licences

`esp_h264` (Apache-2.0, openh264 BSD-2-Clause inside), `esp_websocket_client` 1.7.0 (Apache-2.0) and `esp_libsrtp`
(libsrtp, BSD-3-Clause) are free software. `esp_peer` ships its ICE / DTLS / RTP core as a binary library
(`libpeer_default.a`) under the "Espressif Modified MIT" licence, which allows use only with Espressif chips. The
AGPL-3.0 does not allow such a restriction by itself, so a release image with `CAMOS_LIVE` on needs an additional
permission from the copyright holder (AGPL section 7) for linking these Espressif components, the same open point as
the Wi-Fi libraries (see `NOTICE`). The repository itself holds none of their code (the component manager fetches it).

### Why esp_peer and not another WebRTC stack

- `stephanrotolante/esp32-webrtc-example`: ESP32-S3 + ESP-IDF 5, `esp_h264` and libpeer (MIT), but it is a WHIP
  publisher (the camera makes the offer to a Pion server), has no KVS signaling, and the repository has no licence,
  so none of its code can be used here.
- `espressif/esp-webrtc-solution` `solutions/kvs_master`: the same esp_peer + esp_h264 pair, with a KVS signaling
  component that takes the keys, region and channel from Kconfig. The firmware uses esp_peer and esp_h264 from the
  component registry with its own signaling and SigV4 (AGPL), which take the run-time values from pairing and the
  credential provider; none of the demo's code is copied.
