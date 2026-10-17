# CamOS fruit-fly trap camera (ESP32-S3)

**English** · [Tiếng Việt](README.vi.md)

![the trap camera on its post (illustration)](docs/img/trap-illustration.png)

A small solar camera that photographs a yellow sticky trap in the orchard several times a day and sends the photos to
[FlyBot](https://flybot.agrhub.com), which counts the fruit flies for you. *(Illustration; photos of the assembled trap
will follow in `docs/img/trap-assembled.jpg`.)*

- Runs on Wi-Fi, or on 4G with a SIM card in the field.
- Solar panel + battery: sleeps between photos, wakes on time.
- GPS: FlyBot puts the trap on the map by itself.
- A web page on the camera for live view, settings and the photos on its memory card.
- Live view in FlyBot from anywhere (Freenove board): a small H.264 picture over WebRTC, see section 5.

## 1. What you need

- The camera board **Freenove ESP32-S3 CAM (FNK0085)**, a microSD card and a waterproof box: about 430.000 ₫ + the board
  (US$19.99) for a Wi-Fi camera.
- For the field: a 4G module, a GPS module, a LiPo cell, a solar panel and its charger: about 1.130.000 ₫ + the board.
- The full list with shop links and the prices read on 17 Oct 2026: [docs/BOM.md](docs/BOM.md).
- Wiring diagrams for the field camera: [docs/WIRING.md](docs/WIRING.md). Putting it in the box and on the post:
  [docs/ASSEMBLY.md](docs/ASSEMBLY.md).

## 2. Put the firmware on the board (3 steps, nothing to install)

1. Open **https://agrhub.github.io/camos-esp32-trap/flash/** in **Chrome or Edge** on a computer.
2. Plug the board into the computer with a USB-C cable (the port marked **CH340** / UART).
3. Click **Install** next to your board, pick the COM port, confirm. It takes about a minute.

Use **Install** for a new camera. For a camera that already works, use **Update**: it keeps its settings and its
registration with FlyBot. A running camera can also update itself from its web page (Maintenance & Upgrade).

**Without the web flasher:** download `camos-esp32-trap-freenove-v<version>-install.bin` from
[Releases](https://github.com/agrhub/camos-esp32-trap/releases) and write it at address 0:

```
python -m esptool --chip esp32s3 --port COM7 write_flash 0x0 camos-esp32-trap-freenove-v0.5.0-install.bin
```

(`esptool` comes with `pip install esptool`; on Linux the port is `/dev/ttyUSB0`.)

## 3. Connect it to Wi-Fi (or 4G)

With a SIM card in the 4G module, the camera goes online by itself (it picks the APN from the SIM). For Wi-Fi:

| | |
|---|---|
| ![setup page: Wi-Fi list](docs/img/portal-wifi.png) | 1. Power the camera. The LED **breathes blue**: the camera opens its own Wi-Fi `CAM-xxxx`. <br>2. On your phone, join **`CAM-xxxx`**, password **`camos1234`**. The setup page opens by itself (else open `http://192.168.4.1`). <br>3. Tap your Wi-Fi, type its password, tap **Connect**. <br>4. The LED blinks yellow, then a green blip: the camera is online. If the password was wrong, the `CAM-xxxx` network comes back after about a minute with a message. |

"Advanced settings" on the same page has the 4G APN, the photo interval, the power mode, the timezone and the FlyBot
address. Every field already has a working default: change nothing unless you need to.
[More about the setup page](docs/PORTAL.md).

![advanced settings](docs/img/portal-advanced.png)

## 4. Claim it on FlyBot

| | |
|---|---|
| ![unclaimed camera with its QR code](docs/img/portal-unclaimed.png) | A new camera registers itself with FlyBot and waits for its owner. Until it is claimed it is "Unclaimed" and sends at most one photo an hour. <br><br>1. Sign in at **https://flybot.agrhub.com**. <br>2. Choose **Add camera** and scan the QR code shown on the camera's setup page or web page, or type the **serial number** and the **verify code** printed next to it. <br>3. The camera now follows the schedule set in FlyBot. |

## 5. The camera's web page

When the camera is on your Wi-Fi, open **`http://flybot-xxxxxx.local/`** in a browser on the same Wi-Fi, where
`xxxxxx` is the last 6 characters of the serial number in lower case (serial `ESP32S3-0A1B2C` gives
`flybot-0a1b2c.local`). The setup page and the serial `info` command show the exact name. Windows, macOS, iPhone and
Linux open it; many Android browsers do not resolve `.local` names, so there use `http://<camera IP>/` (the IP is in
your router's list, where the camera appears as `flybot-xxxxxx`, or on the camera's Wi-Fi page). The first visit asks you to set the **admin password** (8-64 characters, letters and digits).

| Live View | Playback | Image settings |
|---|---|---|
| ![live view](docs/img/console-live.png) | ![playback](docs/img/console-playback.png) | ![image](docs/img/console-image.png) |
| **Motion detection** | **Capture schedule** | **Storage** |
| ![motion](docs/img/console-motion.png) | ![schedule](docs/img/console-schedule.png) | ![storage](docs/img/console-storage.png) |
| **Network** | **System** | **Tiếng Việt** |
| ![network](docs/img/console-network.png) | ![system](docs/img/console-system.png) | ![vietnamese](docs/img/console-live-vi.png) |

*The pictures in these screenshots are drawings of a sticky card, not camera photos.* Every page:
[docs/WEB_CONSOLE.md](docs/WEB_CONSOLE.md).

**Live view in FlyBot (away from home).** The Freenove camera can also send its live picture to FlyBot's camera page
over the Internet (Amazon Kinesis Video Streams WebRTC): 320x240 H.264, about 5-8 frames a second on the board we
measured, one viewer at a time. On mains power it is ready all the time; on battery it joins when FlyBot asks (at its
next wake-up). While someone watches from FlyBot, the camera's own web page shows "busy" instead of its live picture,
and a scheduled photo due in that time is skipped. FlyBot hands the camera its live view keys by itself
(flybot.agrhub.com does); with older firmware FlyBot offers a photo and the camera's web page instead.
Details: [docs/dev/LIVE_VIEW.md](docs/dev/LIVE_VIEW.md).

## 6. What the LED says

| LED (top of the board) | Meaning |
|---|---|
| blue, slow breathing | setup mode: join `CAM-xxxx` with your phone |
| yellow, fast blinking | connecting to Wi-Fi / 4G / FlyBot |
| one green blip every 5 s (first minute) | online |
| one white flash | taking a photo |
| red double blink | a problem: no network, or FlyBot refused the camera (see the setup page) |
| orange, 3 blinks | battery low, going to sleep |
| purple | installing new firmware: do not unplug |
| off | sleeping between photos, or online for more than a minute |

Details: [docs/LED.md](docs/LED.md).

## 7. Troubleshooting

- **It does not join my Wi-Fi.** After about a minute it opens `CAM-xxxx` again and the setup page says why. Check the
  password; the camera needs a 2.4 GHz network (not 5 GHz only).
- **No 4G.** Check that the SIM has data and no PIN (or type the PIN in Advanced settings), that the antenna is screwed
  on, and that the 4G module has its own thick supply wires from the battery (it draws short peaks of up to 2 A). The
  web page's 4G section shows the operator and the signal.
- **The picture is black.** At night that is normal: the camera has no infrared light. Set a capture window for the
  daylight hours (Capture Schedule). In daylight, check that nothing covers the lens and that the ribbon cable of the
  camera is pushed in fully.
- **The BOOT button.** While the camera runs, **hold it 3 seconds and release when the LED turns blue**: the camera
  opens setup mode (`CAM-xxxx`) and keeps every setting and the web page password. **Hold it 10 seconds, until the LED
  turns red**, for a factory reset: settings and the web page password go back to the defaults, then setup mode opens.
  Your FlyBot registration stays either way.
- **I forgot the web page password.** Do the 10-second factory reset above, join your Wi-Fi again, and the next visit
  to the web page asks for a new password.
- **I want to start over.** Web page > Maintenance & Upgrade > Factory reset (keeps the registration), or flash the
  **Install** image again (erases everything; the camera then needs to be reset in FlyBot before it can register again).
- **The camera is "Unclaimed" in FlyBot.** Claim it (section 4). Until then it sends at most one photo an hour.

## 8. For developers

Build from source, settings and build options, the device protocol, factory provisioning, host tests, power-budget
calculations and the mock camera used for the screenshots:

- [docs/dev/BUILD.md](docs/dev/BUILD.md): ESP-IDF build, presets, Kconfig, defaults, tests, releases, the mock camera
- [docs/dev/protocol-v1.md](docs/dev/protocol-v1.md) and [docs/dev/PROTOCOL_ADDITIONS.md](docs/dev/PROTOCOL_ADDITIONS.md): MQTT / HTTPS protocol
- [docs/dev/FACTORY.md](docs/dev/FACTORY.md): pre-provisioning a fleet with certificates
- [docs/dev/POWER_BUDGET.md](docs/dev/POWER_BUDGET.md), [docs/dev/LIVE_VIEW.md](docs/dev/LIVE_VIEW.md), [docs/dev/PINPROBE.md](docs/dev/PINPROBE.md)
- [CONTRIBUTING.md](CONTRIBUTING.md)

## Licence

GNU Affero General Public License v3.0 only (AGPL-3.0-only), Copyright (C) 2026 AGRHUB: see [LICENSE](LICENSE). Third-party components in the firmware image keep their own licences: see [NOTICE](NOTICE). If you run a modified version for others over a network, the AGPL asks you to offer them its source. Linking Espressif's binary-only libraries (Wi-Fi, PHY, esp_peer, esp_h264) is allowed by an additional permission under AGPL section 7, written in [NOTICE](NOTICE).
