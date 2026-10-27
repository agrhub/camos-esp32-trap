# Live view

There are two ways to watch the camera live.

## 1. On the same network: MJPEG in the web console

The web console streams `GET /api/stream` (multipart/x-mixed-replace JPEG) to one browser at a time on the same
network. Measured on the Freenove board on 16 Oct 2026: about 11 fps at 640x480 and 12.7 fps at 1280x720
(table and setup: [../WEB_CONSOLE.md](../WEB_CONSOLE.md)). While a cloud live session (below) runs, the LAN stream
answers `409 BUSY`.

## 2. Through the cloud: Kinesis Video Streams WebRTC (the `freenove` image)

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
| Video | `espressif/esp_h264` 1.4.1 | software H.264 (openh264, constrained baseline) from YUV422 camera frames, 320x240, target 10 fps and 300 kbit/s, GOP 2 s (the software encoder cannot force a key frame, so a PLI waits for the next one). Only while a viewer is connected; no audio is sent (the answer keeps a send-only PCMA line because viewers offer audio). |
| When | `live.c` | Powered mode (`CAMOS_LIVE_STANDBY`): the camera stays on the channel all the time, so a viewer connects at once. Battery mode: only for `live.start {minutes}` / `live.requested` (5 min), which also keep it awake; a sleeping camera picks them up at its next wake: a photo slot, or (0.9.2) a check-in wake on Wi-Fi every 2 min (cell at 60 % or more) or 10 min, 06:00-18:00 local time (`sched.c`). On 4G there are no check-ins. The window's end drops the viewer; `live.stop` drops the viewer and ends the window. |
| Camera | `cam.c`, `sched.c` | The encoder takes the camera lock (`camera_take`) when the offer arrives, so the camera's DMA buffer and the encoder are allocated before the DTLS session; a running capture finishes first, the web console preview and the LAN MJPEG stream stop. The sensor runs raw YUV422 at `CAMOS_LIVE_XCLK_MHZ` (12 MHz; JPEG captures keep `CAMOS_XCLK_MHZ`). A scheduled capture or `snapshot.now` during a session pauses the stream (`live_pause`): the encoder stops and gives the camera lock back, the capture takes the full-size photo it always takes, the JPEG is copied to PSRAM and the stream starts again in the mode and quality it had (H.264 or data channel JPEG; the default H.264 if that fails), while the upload runs from the copy. The viewer sees the picture stop for the sensor's switch to JPEG, the exposure settling and the switch back; the console's `live status` shows the last pause as `lastPauseMs`. |
| Shadow | `shadow.c` | `live.supported` = built in, paired (channel + region) and a credential source (credential provider settings, or bench keys); `live.active` = a live window is open or a viewer watches. |

Serial console: `live` (status JSON: state `off` / `waiting` / `joining` / `joined` / `viewer` / `retry`, viewer,
fps, kbit/s, encode and camera times, last error), `live start [min]`, `live stop`. The web console's `/api/status`
has the same object under `live`.

### Data channel "camos": JPEG pictures instead of H.264

The H.264 track is limited by the software encoder (320x240, 5-8 fps). The OV5640 also makes JPEG itself, at any size,
at no CPU cost; browsers do not take MJPEG on a video track, so the camera can send those JPEG pictures over a WebRTC
data channel instead (`live_dc.c` for the protocol, host-tested in `tests/test_live_dc.c`; `live_kvs.c` for the rest).

- **Opening.** The VIEWER creates a data channel labelled `camos` before its offer (so the SDP has an application
  line). FlyBot opens it unordered with `maxPacketLifeTime: 300` (see below). The camera accepts it (esp_peer
  `enable_data_channel` with `manual_ch_create`: the camera never creates a channel itself) and greets with `hello`.
  An offer without an application line gets an answer without one, so older viewers keep H.264; a camera on older
  firmware never opens the channel, so the viewer keeps H.264 too.
- **Default.** JPEG over the data channel: once the channel is open and the viewer has not asked for something else,
  the camera switches to JPEG 640x480, `q` 12, 15 fps (the web console stream's standard quality); on 4G it stays on
  H.264 until the viewer asks (a round trip through the TURN relay took 1.5-1.8 s, 22 Oct 2026). H.264 320x240 on the
  video track is the fallback: before the channel opens, for a viewer without the channel, when the channel closes,
  and when the viewer asks for `h264`. While JPEG flows, the video track carries nothing.
- **Weak link.** When no JPEG ack has come back for 6 s while frames went out (or four round trips, when those are
  longer, up to 15 s), the camera steps one video tier down: 2K, FullHD, HD, 480p by picture area as FlyBot names them
  (`live_dc_step_down`; within a tier the offered size closest to the tier's own, e.g. 2048x1536 -> 1920x1080 ->
  1280x720 -> 640x480), and below 480p H.264 320x240. Each step sends `mode` with `"reason":"weak_link"` and waits
  another 6 s before the next. A request with `"fixed":true` (a size the person picked) is never stepped down: the ack
  window paces it at whatever frame rate the link carries.
- **Sizes** follow the camera module: `sizes` in `hello` is `camera.liveSizes` of the shadow (the sensor's frame sizes
  up to 2048x1536 within its maximum), the same list the web console's own stream offers. Above 1600x1200 the sensor
  runs at the photo XCLK (10 MHz), as the web console's stream does. With `power.level` `eco` (0.9.2: a cell under
  `highMv` that is not charging at full input) the list stops at the 480p tier.
- **Text messages** are JSON control messages:

  | From | Message | Meaning |
  |---|---|---|
  | camera | `{"t":"hello","v":1,"mode":"h264","w":320,"h":240,"q":12,"fps":10,"modes":["h264","jpeg"],"sizes":[[320,240],[640,480],[800,600],[1024,768],[1280,720],[1280,1024],[1600,1200],[1920,1080],[2048,1536]],"qRange":[8,40],"chunk":16000}` | the channel is open: what runs (`mode`, `w`, `h`, `q`, `fps`) and what can be asked for (`modes`, `sizes`: here an OV5640, `qRange`) |
  | viewer | `{"t":"quality","mode":"jpeg","w":640,"h":480,"q":12,"fps":10,"fixed":true}` | switch, at any time in the session. `mode` is `h264` or `jpeg`; fields left out keep their value; `h264` always uses the build's 320x240; `w`x`h` must be one of `sizes`, `q` within `qRange` (esp32-camera scale, lower is better), `fps` 1..25; `fixed` (optional): no step down on a weak link |
  | camera | `{"t":"mode","mode":"jpeg","w":640,"h":480,"q":12,"fps":15}` | the setting now running, also the answer to a request that changes nothing; `"reason":"weak_link"` when the camera stepped down by itself. A change restarts the sensor: about 1 s without pictures |
  | camera | `{"t":"stats","mode":...,"rate":14.9,"kbps":790,"bytes":6640,"drops":0,"waits":17,"ackMs":45,"pushMs":9}` | every 2 s: frames per second sent, kbit/s, mean frame size, chunks the channel refused, times the camera waited for acks or RAM, send-to-ack time of the last frame, time to hand a frame to the channel |
  | camera | `{"t":"error","msg":"size not offered"}` | a request was refused, or the sensor did not start at that size (the camera keeps what ran) |
  | viewer | `{"t":"ack","seq":42}` | a complete JPEG frame arrived. Cumulative: a lost ack is covered by the next one |

- **Binary messages** carry one chunk of a JPEG frame (at most 16,000 JPEG bytes) behind a 20-byte header, little
  endian:

  | Bytes | Field |
  |---|---|
  | 0-1 | `'C'` `'J'` |
  | 2 | version, 1 |
  | 3 | header length, 20 (a viewer skips a longer header) |
  | 4-7 | frame sequence number (counts up through the session) |
  | 8-11 | total JPEG bytes of the frame |
  | 12-15 | offset of this chunk in the frame |
  | 16-19 | capture time, ms since the session started |

  The viewer puts a frame together from its chunks (in any order), shows it once every byte arrived, acknowledges it,
  and drops a frame that a newer sequence number overtakes before it is complete (FlyBot:
  `client/src/utils/camosLive.js`).
- **Flow control.** The camera keeps at most 2 frames unacknowledged; a frame not acknowledged within 500 ms is taken
  as lost. While the window is full, or while less than 16 KB of internal RAM is free, it takes no new picture: the
  newest picture goes next, nothing queues behind the channel, and latency stays at about one frame. A chunk the
  channel refuses drops the rest of that frame.
- **Partial reliability.** With a fully reliable channel, esp_peer's SCTP stalled for seconds after losses on a busy
  Wi-Fi (acks 0.4-1 s late, ICE round trips up to 5 s, DTLS writes failing, 1-4 fps at 640x480). With the viewer's
  channel unordered and `maxPacketLifeTime: 300`, a lost chunk costs one frame instead: on the same board and Wi-Fi,
  640x480 went to 15 fps with acks after 30-50 ms. Control messages share the channel, so FlyBot repeats a quality
  request every 2 s (3 times) until a `mode` message confirms it.
- **Memory.** The data channel caches are in PSRAM (send 512 KB, receive 16 KB). The Freenove preset now keeps only
  allocations under 256 bytes in internal RAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=256`, as Espressif's kvs_master
  does): with 1 KB, a JPEG session ran the internal heap down to 23 bytes and Wi-Fi lost packets. Camera DMA during a
  session: esp32-camera's JPEG mode takes a fixed 16 KB internal block (whatever `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX`
  says), and on the heap a DTLS session had fragmented the JPEG start right after the data channel opened failed now
  and then (largest free block 15,360 bytes: the YUV422 pipeline's). So the H.264 pipeline's YUV422 frames go by DMA
  straight into PSRAM (esp32-camera's PSRAM DMA mode; for JPEG that mode lost every end marker), and the session holds a
  16 KB internal block from its start, freed just before each JPEG start (live JPEG, or a photo during the session) so
  the camera's buffer goes into that very hole, and taken again when the camera powers down (cam.c).
- **Sensor clock.** H.264 and JPEG run at `CAMOS_LIVE_XCLK_MHZ`, 12 MHz (snapshots keep 10 MHz): 16 and 20 MHz cost the
  board's own Wi-Fi 4-100x of its throughput ([../WEB_CONSOLE.md](../WEB_CONSOLE.md), "Camera clock and Wi-Fi"). In a dim
  room the streams use the sensor's night mode ([../WEB_CONSOLE.md](../WEB_CONSOLE.md), "Exposure in dim light").

FlyBot's quality choices (`client/src/utils/camosLive.js`): **Auto** = JPEG 640x480 q12 at 15 fps (or the offered
size closest to it), **240p** = H.264 320x240 at 10 fps, then one JPEG size per tier the camera offers in `hello`
(480p, HD, FullHD, 2K; q12, 15 fps up to 640x480 and 10 fps above), asked with `"fixed":true`. Without a data channel
the viewer gets H.264.

Measured on 17 Oct 2026, in the evening in a dark room (the pictures are near black, so the frames are small; a lit or
noisy scene makes them 2-3x larger, see the LAN table in [../WEB_CONSOLE.md](../WEB_CONSOLE.md)). Same board, bench
viewer in Chromium on the same Wi-Fi, channel unordered with `maxPacketLifeTime: 300`:

| Asked | Sent / shown | Frame size | Bitrate | Ack after | Notes |
|---|---|---|---|---|---|
| JPEG 640x480 q12, 15 fps | 14.8-15.3 / 14.9-15.5 fps | 6.6 KB | 790-813 kbit/s | 30-95 ms | ICE round trip 21-39 ms; 0 frames dropped by the viewer |
| JPEG 640x480 q12, 20 fps | 4.4-12.2 / 5-13.6 fps | 6.6 KB | 230-650 kbit/s | 29-460 ms | the link set the pace |
| JPEG 800x600 q12, 15 fps | 7.6-13.2 / 6.5-12 fps | 10 KB | 615-1,060 kbit/s | 60-690 ms | |
| JPEG 1024x768 q12, 12 fps | 3-10.3 / 6.8-11 fps | 16 KB | 390-1,320 kbit/s | 40-545 ms | two chunks per frame |
| JPEG 1280x720 q12, 10 fps | 3.2-8.5 / 5-9.1 fps | 18.7 KB | 490-1,280 kbit/s | 27-374 ms | |
| H.264 320x240, 10 fps | 7.4 fps | | | | back from JPEG mid-session, every time |
| reliable channel, 640x480 | 1-4 fps | 6.6-11.5 KB | 70-400 kbit/s | 0.2-1 s | before the switch to partial reliability |

- A first run that evening (reliable channel, quiet Wi-Fi) also reached 15-18 fps at 640x480 and 13-16 fps at
  800x600. Later the camera's Wi-Fi lost 25-70 % of pings during every session, even H.264 at 1 kbit/s (ICE round
  trips 0.8-4.5 s, DTLS writes failing), while it answered pings well when idle; the camera task on core 1 instead of
  core 0 changed nothing. The numbers are what the link allowed, not a limit of the camera: it hands a 16 KB frame to
  the channel in 25-35 ms, and its sensor gives 640x480 JPEG at 15 fps and more. To be measured again on a quiet
  network and in daylight.
- A switch takes about 1 s (the sensor restarts); the first JPEG arrives 1.1-1.9 s after the request. On the poor link
  a request was lost now and then (the bench page did not repeat it; FlyBot does).
- Free internal RAM during a JPEG session: 44-45 KB (H.264 session: about 45 KB with the 256-byte threshold); free
  PSRAM 6.7-7.4 MB depending on the size (two frame buffers of up to 393 KB).
- Latency feel: with acks at 30-100 ms and at most two frames in flight, a JPEG picture is about 0.1-0.2 s old when
  shown (not measured glass-to-glass); on the poor link the window kept it under about 0.7 s instead of letting it
  grow.

Measured again on 18 Oct 2026, late morning (a lit room, so frames 1.7-3.5x the evening sizes), same board and bench
viewer on the same Wi-Fi, ICE round trip 15-43 ms, every size asked at 25 fps so the link sets the pace:

| Asked | Sent / shown | Frame size | Bitrate | Ack after | Notes |
|---|---|---|---|---|---|
| JPEG 640x480 q12 (the default, 15 fps) | 8.6-11.1 / 9.5 fps | 11 KB | 860 kbit/s | 31 ms | |
| JPEG 640x480 q12, 25 fps | 3.4-10.1 / 8.3 fps | 11 KB | 750 kbit/s | 92 ms | |
| JPEG 800x600 q12, 25 fps | 6.8-9.3 / 9.3 fps | 16 KB | 1,210 kbit/s | 11 ms | |
| JPEG 1024x768 q12, 25 fps | 2.9-5.4 / 4.4 fps | 24 KB | 870 kbit/s | 78 ms | |
| JPEG 1280x720 q12, 25 fps | 3.7-6.3 / 5.5 fps | 27 KB | 1,220 kbit/s | 107 ms | |
| JPEG 1600x1200 q12, 25 fps | 2.6-3.6 / 3.3 fps | 56 KB | 1,520 kbit/s | 26 ms | four chunks per frame |
| no acks from the viewer | H.264 320x240 at 7 fps | | | | `mode` with `reason: weak_link` 7.0 s after the last ack |

The camera waited for acks most of the time (`waits` 54-118 per run), so these are again the link's numbers, about
1-1.5 Mbit/s that morning, not the sensor's.

After the DMA change (H.264 frames in PSRAM, the JPEG block held for the session), 18 Oct afternoon, another Wi-Fi, each
size asked at 25 fps for 12 s: 640x480 11.0 fps (10 KB), 800x600 10.9 fps (12 KB), 1024x768 7.5 fps (20 KB), 1280x720
7.7-10.5 fps (22-23 KB), 1600x1200 3.5-4.1 fps (51-53 KB, 4 fps held for 3.3 minutes). The automatic JPEG start worked
in each of four sessions (first JPEG 1.1-1.2 s after the channel opened; two other attempts failed at ICE, before any
video). A photo during the session paused the stream for about 7 s (the switch to the full-size photo and back) and the
stream came back in the mode it had, also at 1600x1200; the 300 KB upload then ran beside it, and the weak-link check
waits meanwhile (acks get lost while the upload fills the uplink).

Not measured: a remote viewer through TURN with JPEG, 4G.

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
| Sensor clock | XCLK 10 MHz: 3.9 fps (camera wait 180 ms); 24 MHz: camera wait 10 ms but a darker picture and encode 92-117 ms (the raw frames compete with the encoder for PSRAM bandwidth); 20 MHz was the default then (now 12 MHz with a faster OV5640 PLL: `CAMOS_LIVE_XCLK_MHZ`, `CAMOS_LIVE_YUV_PLL_MUL`) |
| Bitrate | 4-40 kbit/s for a still indoor scene (the 300 kbit/s target is a ceiling) |
| Session start | camera: offer to answer 1.6-3.3 s (TURN allocation, the sensor starting), viewer connected 3.0-5.0 s after the offer. Browser: offer to first frame 4.6-7.1 s (one run 12 s: ICE took 5 s) |
| Viewer leaves | noticed after 29 s with esp_peer's default keep-alive (5 checks); the firmware now uses 3 checks (about 20 s, not measured) |
| Network | host / host UDP on the LAN; RTT 14-300 ms (mean 100 ms), the browser's jitter buffer settles at 37-41 ms, decode 1-4 ms per frame |
| Latency | not measured glass-to-glass; from the parts: frame age up to one frame period + encode 50-150 ms + half the RTT + jitter buffer 40 ms + decode, about 0.2-0.4 s |
| Memory | free internal RAM 95 KB after boot (88 KB before the 1 KB PSRAM threshold), about 77 KB while waiting on the channel, 16.2-17.5 KB during a session (stable over 4 minutes); free PSRAM 5.99 MB during a session. With the 256-byte threshold of the data channel change: about 89 KB after boot, about 45 KB during an H.264 session |
| CPU | the encoder runs on core 1 and is busy about 40-70 % of the time (encode time x frame rate); Wi-Fi, lwIP, DTLS-SRTP, the signaling and the camera DMA task run on core 0 |
| Two sessions in a row | the second viewer connected to the same camera without a restart |
| LAN MJPEG after the change | 320x240 and 640x480 at about 10 fps (120 frames in 12 s, connection set-up included); answers `409 BUSY` while the cloud viewer watches |
| Image size | 1,939,344 bytes of the 3 MB app slot (38 % free); with the JPEG data channel: 1,943,200 bytes (38 % free); 0.4.2 without live view: 1,532,224 bytes. ESP32-CAM preset: 1,432,336 bytes (19 % of its 1.69 MB slot free), `esp32s3cam` preset (live view off): 1,461,648 bytes |
| Credential provider | with the flybot-aws role alias (17 Oct 2026): the camera gets its own keys with its certificate and joins `cam-<serial>`; a viewer then played 320x240 H.264 at 8 fps, 0 frames dropped, first frame about 6 s after the viewer's offer |

The 10 fps target is not reached: the software encoder needs 50-150 ms for a 320x240 frame on the S3 and the raw
sensor mode adds its own wait. Larger sizes drop the frame rate in proportion (`CAMOS_LIVE_WIDTH` / `_HEIGHT`).

Not measured: the 4G (A7680C) uplink, a remote viewer through a TURN relay (the camera's relay candidate is there,
the bench viewer was on the same LAN), a session on battery, glass-to-glass latency, daylight frame rate.

### The AWS side (flybot-aws, deployed on 17 Oct 2026)

flybot-aws (`infra/template.yaml`, deployed on 17 Oct 2026) has the three pieces below; FlyBot sends the endpoint and
the alias to the camera (`IOT_CREDENTIALS_ENDPOINT`, `IOT_ROLE_ALIAS`), and publishes the desired settings, retained,
on `camos/<serial>/shadow/desired`, the topic the firmware reads:

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

`esp_h264` (Apache-2.0, openh264 BSD-2-Clause inside, the encoder shipped as a binary library), `esp_websocket_client`
1.7.0 (Apache-2.0) and `esp_libsrtp` (libsrtp, BSD-3-Clause) are free software. `esp_peer` ships its ICE / DTLS / RTP
core as a binary library (`libpeer_default.a`) under the "Espressif Modified MIT" licence, which allows use only with
Espressif chips. The copyright holder grants an additional permission under AGPL section 7 for linking these Espressif
System Libraries, like the Wi-Fi and PHY libraries: see `NOTICE`. Release images are built with `CAMOS_LIVE` on for
the Freenove board. The repository itself holds none of their code (the component manager fetches it).

### Why esp_peer and not another WebRTC stack

- `stephanrotolante/esp32-webrtc-example`: ESP32-S3 + ESP-IDF 5, `esp_h264` and libpeer (MIT), but it is a WHIP
  publisher (the camera makes the offer to a Pion server), has no KVS signaling, and the repository has no licence,
  so none of its code can be used here.
- `espressif/esp-webrtc-solution` `solutions/kvs_master`: the same esp_peer + esp_h264 pair, with a KVS signaling
  component that takes the keys, region and channel from Kconfig. The firmware uses esp_peer and esp_h264 from the
  component registry with its own signaling and SigV4 (AGPL), which take the run-time values from pairing and the
  credential provider; none of the demo's code is copied.
