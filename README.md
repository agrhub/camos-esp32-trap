# CamOS fruit-fly trap camera (ESP32-S3)

**English** · [Tiếng Việt](README.vi.md)

![the real build: the boards wired on a prototype board in the trap lid, the finished trap, the solar lid](docs/img/trap-build.jpg)

A small solar camera that lives in the lid of a hanging fruit-fly trap. It looks down into the trap several times a
day and sends the photos to [FlyBot](https://flybot.agrhub.com), which counts the fruit flies for you. The lure in the
trap draws the flies in; there is no glue card to change.

- Runs on Wi-Fi, or on 4G with a SIM card in the field.
- The solar panel on the lid charges a flat LiPo cell: the camera sleeps between photos and wakes on time.
- GPS: FlyBot puts the trap on the map by itself.
- A web page on the camera for live view, settings and the photos on its memory card.
- Live view in FlyBot from anywhere (both ESP32-S3 boards): H.264 or the camera's own JPEG pictures over WebRTC, see
  section 7.

![the trap camera live in FlyBot, looking down into the trap (serial masked)](docs/img/esp32-live.jpg)

## 1. What you need

- The camera board: an **ESP32-S3 N16R8 CAM** bought without a camera plus an **OV5640 module with an adjustable lens**
  (419.640 ₫, about US$16), or the **Freenove ESP32-S3 CAM (FNK0085)** (US$19.99), and a trap housing: the hanging solar
  trap in the photos costs US$3.00. That is a Wi-Fi camera, about US$19.
- For the field: a 4G module and a GPS module (both with their antennas), a flat LiPo cell, a charger, a 5 V
  boost board, a power switch for the 4G module, a prototype board with screw terminals and the lure: 944.760 ₫ of
  parts, about US$39 with the housing
  (26.050 ₫/USD, the Vietcombank rate of 21 Oct 2026). The housing's own solar panel charges the cell.
- Another way: an IP65 box on a post that looks at a yellow sticky card, with a 6 V 2 W panel and an active GPS
  antenna, about US$60 ([ASSEMBLY.md](docs/ASSEMBLY.md), section 4).
- The full list with shop links and the prices read on 17, 24 and 25 Oct 2026: [docs/BOM.md](docs/BOM.md).
- A computer with Chrome or Edge (to flash the firmware), a phone (to set up the Wi-Fi), a microSD card (FAT32,
  optional but recommended) and an account on [FlyBot](https://flybot.agrhub.com).

![the parts of the trap camera and their prices: about US$39](docs/img/trap-parts.jpg)

**Setting up a camera, in short:**

| Step | What | Guide | Time |
|---|---|---|---|
| 2 | Wire the field camera and put it in the trap lid (skip for a Wi-Fi camera on USB) | [WIRING.md](docs/WIRING.md), [ASSEMBLY.md](docs/ASSEMBLY.md) | an afternoon |
| 3 | Put the firmware on the board | [FLASHING.md](docs/FLASHING.md) | 5 min |
| 4 | Connect it to Wi-Fi (a 4G SIM needs nothing) | [PORTAL.md](docs/PORTAL.md) | 2 min |
| 5 | Claim it in FlyBot | section 5 below | 1 min |
| 6 | Set the admin password and check the picture on its web page | [WEB_CONSOLE.md](docs/WEB_CONSOLE.md) | 5 min |
| 7 | Hang it in the orchard, then follow the flies in FlyBot | section 7 below | |

## 2. Build the trap

**A Wi-Fi camera** needs no wiring: the board, its camera and a USB power supply (any 5 V phone charger, 1 A or more).

**The field camera** (4G + GPS + battery + solar) is wired on a small prototype board inside the trap lid. Wired as
drawn, firmware 0.9.2 (the current release) needs no settings; only tick "Battery / solar divider fitted" once the
dividers are in (setup page or web console). 0.9.1 and older use GPIO47 as a PWRKEY pulse and cannot keep the 4G power
switch on.

![wiring overview](docs/img/wiring-overview.png)

- Every wire, pin by pin, with the 4G power switch and the battery dividers: [docs/WIRING.md](docs/WIRING.md).
- The parts with shop links and prices: [docs/BOM.md](docs/BOM.md).
- Putting it in the lid, the camera's view, the antennas and hanging the trap: [docs/ASSEMBLY.md](docs/ASSEMBLY.md).

Flash the board (step 3) before it goes into the lid: it is easier to reach the USB port and the buttons.

## 3. Put the firmware on the board (3 steps, nothing to install)

![the web flasher](docs/img/flash-page.png)

1. Open **https://agrhub.github.io/camos-esp32-trap/flash/** in **Chrome or Edge** on a computer.
2. Plug the board into the computer with a USB-C data cable (the port marked **CH340** / UART).
3. Click **Install** next to your board, pick the COM port, confirm. It takes about a minute.

Use **Install** for a new camera. For a camera that already works, use **Update**: it keeps its settings and its
registration with FlyBot. A running camera can also update itself from its web page (Maintenance & Upgrade) or from
FlyBot.

The full guide, with the port that does not show up, download mode, flashing without the web page
(`esptool` + the release file) and building from source with `tools\build.ps1`: **[docs/FLASHING.md](docs/FLASHING.md)**.

## 4. Connect it to Wi-Fi (or 4G)

With a SIM card in the 4G module, the camera goes online by itself (it picks the APN from the SIM): skip to step 5.
For Wi-Fi, use your phone:

| 1. Join `CAM-xxxx` | 2. Pick your Wi-Fi | 3. It connects | 4. Online |
|---|---|---|---|
| ![setup page](docs/img/portal-wifi.png) | ![network picked](docs/img/portal-pick.png) | ![connecting](docs/img/portal-joining.png) | ![connected](docs/img/portal-joined.png) |

1. Power the camera. The LED **breathes blue**: the camera opens its own Wi-Fi `CAM-xxxx`. On your phone, join
   **`CAM-xxxx`**, password **`camos1234`**. The setup page opens by itself (else open `http://192.168.4.1`).
2. Tap your Wi-Fi (2.4 GHz), type its password, tap **Connect**.
3. The camera tries your network and the page counts the seconds.
4. **Connected**: the camera restarts onto your Wi-Fi. The LED blinks yellow, then a green blip: it is online. Note the
   local name on the page, `http://flybot-xxxxxx.local/`: that is the camera's web page (step 6).

A wrong password: the page says so and you type it again. "Advanced settings" on the same page has the 4G APN, the photo
interval, the power mode, the timezone and the FlyBot address; every field already has a working default. Every step,
every advanced setting and what to do when it fails: **[docs/PORTAL.md](docs/PORTAL.md)**.

To open the setup page again later (a new router, a new password): **hold the BOOT button 3 s** while the camera runs and
release it when the LED turns blue. Every setting stays.

## 5. Claim it on FlyBot

| | |
|---|---|
| ![unclaimed camera with its QR code](docs/img/portal-unclaimed.png) | A new camera registers itself with FlyBot and waits for its owner. Until it is claimed it is "Unclaimed" and sends at most one photo an hour. <br><br>1. Sign in at **https://flybot.agrhub.com** (on a computer or a phone). <br>2. On the Cameras page choose **Add Camera** and scan the QR code shown on the camera's setup page or web page, or type the **serial number** and the **verify code** printed next to it (also on the web page's Device Information). <br>3. Give it a name and pick the **trap type**: **Lure trap (no glue)** for the hanging trap, **Sticky trap (glue card)** for a sticky card. Its GPS position (or a drag on the **Field Map**) puts it on the map. <br>4. The camera now follows the photo schedule set in FlyBot. |

## 6. The camera's web page

When the camera is on your Wi-Fi, open **`http://flybot-xxxxxx.local/`** in a browser on the same Wi-Fi, where
`xxxxxx` is the last 6 characters of the serial number in lower case (serial `ESP000000000001` gives
`flybot-000001.local`). Many Android browsers do not resolve `.local` names: there use `http://<camera IP>/` (the IP is
in your router's list, where the camera appears as `flybot-xxxxxx`).

![first visit: set the admin password](docs/img/console-activate.png)

The first visit asks you to set the **admin password** (8-64 characters, letters and digits). Then:

| Page | What you do there | |
|---|---|---|
| **Live View** | watch the live picture, focus the lens, save a picture, send a photo to FlyBot now | ![live](docs/img/console-live.png) |
| **Playback** | the photos on the memory card, by day and on a time line; download or delete | ![playback](docs/img/console-playback.png) |
| System > **Device Information** | serial, verify code, firmware, network, cloud, battery at a glance | ![device](docs/img/console-system.png) |
| System > **Time Settings** | the camera's clock and timezone (automatic) | ![time](docs/img/console-time.png) |
| System > **Maintenance & Upgrade** | update the firmware (from FlyBot or a file), restart, factory reset | ![maintenance](docs/img/console-maintenance.png) |
| System > **Events**, **Log** | what the camera did lately; the live log to send along with a problem report | ![events](docs/img/console-events.png) |
| Network > **Wi-Fi / TCP/IP** | the connection, a static IP, change the Wi-Fi | ![wi-fi](docs/img/console-network.png) |
| Network > **4G** | operator and signal, APN, SIM PIN | ![4g](docs/img/console-4g.png) |
| Network > **Cloud Platform** | the FlyBot connection, the claim QR | ![cloud](docs/img/console-cloud.png) |
| Image > **Display** | brightness, contrast, orientation, white balance, exposure, photo quality and size, with the live picture beside them | ![image](docs/img/console-image.png) |
| Event > **Motion Detection** | motion detection with an area drawn on the picture (mains-powered cameras) | ![motion](docs/img/console-motion.png) |
| Event > **Capture Schedule** | who sets the schedule, the interval and the daily window, the next photos | ![schedule](docs/img/console-schedule.png) |
| Storage > **Storage Management** | the memory card, format, and the native USB port (USB drive, card reader, webcam) | ![storage](docs/img/console-storage.png) |
| Power & location > **Power & Schedule** | battery and solar, power mode (auto / battery / powered), LED, battery dividers | ![power](docs/img/console-power.png) |
| Power & location > **GPS** | the position, the satellite systems (0.9.2; automatic: GPS + BeiDou in Vietnam), get a position now | ![gps](docs/img/console-gps.png) |
| Security > **Account** | change the admin password | ![account](docs/img/console-account.png) |
| Language menu | English or **Tiếng Việt** | ![vietnamese](docs/img/console-live-vi.png) |

*The pictures in these screenshots are drawings of a sticky card from a mock camera, not camera photos.* Every page and
every setting, explained: **[docs/WEB_CONSOLE.md](docs/WEB_CONSOLE.md)**.

**Before you hang the trap**, close the lid and open Live View, then check three things: the bottom of the trap fills
most of the picture, it is sharp (turn the lens of the OV5640 module gently to focus), and it is upright
(Image > Display > orientation).

## 7. Daily use

**What the camera does by itself.** It takes a photo of the trap on the schedule set in FlyBot (a camera running its own
schedule takes one an hour, all day, until you set a window), keeps a copy on the memory card, and uploads it. FlyBot
counts the fruit flies on every photo. On battery it sleeps between photos and wakes on time; on mains or a full,
charging battery it stays online. Once a day it takes a GPS position, so FlyBot puts the trap on the map by itself.

**What you do in FlyBot** (https://flybot.agrhub.com, on a computer or a phone), from anywhere:

- follow the fly counts of each trap; set an alert threshold per camera and get an email or a Telegram message when a trap reaches it, or when a camera goes offline;
- look at the photos with their counts, and take a new one with **Capture now**;
- watch the live picture (see below);
- change a camera's photo schedule and power mode (battery and solar, or mains), and with 0.9.2 its network (Wi-Fi,
  4G or automatic) and GPS satellite systems; a sleeping camera takes a change at its next wake;
- update the firmware of a camera;
- see every trap on the **Field Map**.

**What you do on the camera's web page** (on the same Wi-Fi only): set it up, check the picture and the focus, change the
Wi-Fi or a 4G setting, browse the photos on the card, read the log when something is wrong.

**Emptying the trap and changing the lure.** Empty the trap and refill the lure as the lure's label or your
pest-control plan says, and wipe the lens and the inside of the clear body when they get dirty. Put the lid back the
same way round so the picture stays framed. Log it under **Trap maintenance** on the camera's page in FlyBot, and take
a photo on demand (Live View > Send to FlyBot, or **Capture now** in FlyBot) to start the new count. With a sticky
card (the other way, [ASSEMBLY.md](docs/ASSEMBLY.md) section 4), replace the card when it is full and put the new one
in the same place.

**Live view in FlyBot (away from home).** The camera (both ESP32-S3 boards) can also send its live picture to FlyBot's
camera page over the Internet (Amazon Kinesis Video Streams WebRTC), one viewer at a time. The quality menu under the picture
picks 240p (320x240 H.264, about 5-8 frames a second) or one of the sizes the camera streams (480p, HD, FullHD, 2K):
these send the camera's own JPEG pictures, up to 15 frames a second at 640x480 when the Wi-Fi uplink is good. Auto
takes 640x480, and the camera steps down by itself when the link cannot carry it. On mains power (or power mode
powered) it is ready all the time. On battery it joins when FlyBot asks, at its next wake: with 0.9.2, by day on
Wi-Fi, a short check-in every 2-10 minutes; otherwise (and always on 4G) at its next photo. While someone watches from
FlyBot, the camera's own web page shows "busy" instead of its live picture, and a scheduled photo pauses the live
picture for a few seconds. FlyBot hands the camera its live view keys by itself
(flybot.agrhub.com does); with older firmware FlyBot offers a photo and the camera's web page instead.
Details: [docs/dev/LIVE_VIEW.md](docs/dev/LIVE_VIEW.md).

**The photos on the card.** Playback on the web page, or take the card out (power off first), or plug the board's native
USB port into a computer: the card shows up as a USB drive and the camera as a webcam
([WEB_CONSOLE.md](docs/WEB_CONSOLE.md#native-usb-port-card-reader-webcam-or-usb-drive)).

**Firmware updates.** FlyBot offers new firmware to the camera; or use Maintenance & Upgrade on the web page. The
camera keeps its settings and registration, and goes back to the previous firmware by itself if a new one cannot connect.

## 8. What the LED says

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

## 9. Troubleshooting

- **It does not join my Wi-Fi.** After about a minute it opens `CAM-xxxx` again and the setup page says why. Check the
  password; the camera needs a 2.4 GHz network (not 5 GHz only).
- **No 4G.** Check that the SIM has data and no PIN (or type the PIN in Advanced settings), that the antenna is screwed
  on, and that the 4G module has its own thick supply wires from the battery and its 1000 uF capacitor (it draws short
  peaks of up to 2 A). The
  web page's 4G section shows the operator and the signal.
- **The picture is black.** At night that is normal: the camera has no infrared light. Set a capture window for the
  daylight hours (Capture Schedule). In daylight, check that nothing covers the lens and that the ribbon cable of the
  camera is pushed in fully.
- **The picture is blurred.** Wipe the lens and the clear body, then focus the OV5640's lens on the bottom of the trap
  (turn it gently, watching Live View).
- **No GPS position.** The GPS needs a clear view of the sky; indoors there is usually none. The first position can take
  a few minutes. GPS > Get position now tries at once.
- **The BOOT button.** While the camera runs, **hold it 3 seconds and release when the LED turns blue**: the camera
  opens setup mode (`CAM-xxxx`) and keeps every setting and the web page password. **Hold it 10 seconds, until the LED
  turns red**, for a factory reset: settings and the web page password go back to the defaults, then setup mode opens.
  Your FlyBot registration stays either way. **A short press wakes a sleeping camera** (night sleep, or between
  photos on battery) for 10 minutes, for the live view or the settings.
- **I forgot the web page password.** Do the 10-second factory reset above, join your Wi-Fi again, and the next visit
  to the web page asks for a new password.
- **I want to start over.** Web page > Maintenance & Upgrade > Factory reset (keeps the registration), or flash the
  **Install** image again (erases everything; the camera then needs to be reset in FlyBot before it can register again).
- **The camera is "Unclaimed" in FlyBot.** Claim it (section 5). Until then it sends at most one photo an hour.
- **Still stuck?** Web page > System > Log > Download, and send the file with your question.

## 10. For developers

Build from source, settings and build options, the device protocol, factory provisioning, host tests, power-budget
calculations and the mock camera used for the screenshots:

- [docs/FLASHING.md](docs/FLASHING.md) (section C) and [docs/dev/BUILD.md](docs/dev/BUILD.md): ESP-IDF build, presets, Kconfig, defaults, tests, releases, the mock camera
- [docs/dev/CONFIG.md](docs/dev/CONFIG.md): settings layers, the local JSON API, the serial console, NVS keys
- [docs/dev/protocol-v1.md](docs/dev/protocol-v1.md) and [docs/dev/PROTOCOL_ADDITIONS.md](docs/dev/PROTOCOL_ADDITIONS.md): MQTT / HTTPS protocol
- [docs/dev/FACTORY.md](docs/dev/FACTORY.md) and [tools/README.md](tools/README.md): pre-provisioning a fleet with certificates
- [docs/dev/POWER_BUDGET.md](docs/dev/POWER_BUDGET.md), [docs/dev/LIVE_VIEW.md](docs/dev/LIVE_VIEW.md), [docs/dev/PINPROBE.md](docs/dev/PINPROBE.md)
- [CONTRIBUTING.md](CONTRIBUTING.md)

## Licence

GNU Affero General Public License v3.0 only (AGPL-3.0-only), Copyright (C) 2026 AGRHUB: see [LICENSE](LICENSE). Third-party components in the firmware image keep their own licences: see [NOTICE](NOTICE). If you run a modified version for others over a network, the AGPL asks you to offer them its source. Linking Espressif's binary-only libraries (Wi-Fi, PHY, esp_peer, esp_h264) is allowed by an additional permission under AGPL section 7, written in [NOTICE](NOTICE).
