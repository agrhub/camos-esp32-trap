# Power budget (estimates; timings measured, currents not)

## 0. Measured on the Freenove board, 16 Oct 2026 (firmware 0.2.0, USB power, no battery, no divider)

Currents: **not measured** (the board ran from the laptop's USB port; no meter in series). Timings and sizes from the
serial log (`tools/serial_log.py`) and the console `frame` command:

| Item | Value |
|---|---|
| Reset to `app_main` | 0.93 s (boot log timestamp 927 ms; 2nd-stage bootloader + PSRAM memory test 0.42 s of it) |
| `app_main` to SoftAP DHCP up (portal mode) | 1.12 s after reset |
| Console prompt (after the 3.5 s Wi-Fi scan of the portal) | 4.8 s after reset |
| Camera init (OV5640 detect + reconfigure to 2560x1920) | 1.5 s (two inits: UXGA probe, then QSXGA) |
| Full capture path `snap` (init, 6 warm-up frames, JPEG) | 5.2 s |
| JPEG 2560x1920, shadow quality 85 (driver q9) | 371,607 bytes |
| JPEG 2560x1920, driver q12 | 305,080 bytes; 447 ms per frame |
| JPEG 1600x1200 (UXGA), driver q12 | 96,422 bytes; 447 ms per frame (1.8 s for 3 warm-up frames) |
| JPEG 640x480 (VGA), driver q12 | 11,309 bytes; 90 ms per frame |
| JPEG 320x240 (QVGA), driver q12 | 3,972 bytes; 90 ms per frame |
| Free internal heap: idle / with the camera up | 211 KB / 188 KB (minimum seen 187 KB) |
| Free PSRAM: idle / with two 2560x1920 frame buffers | 8,383 KB / 3,463 KB |
| Battery ADC with nothing on GPIO1 | floating: 4318, 4038, 106 mV in three reads (noise, as expected; set `CAMOS_BATT_ADC_GPIO=-1` until the divider is wired) |

XCLK 10 MHz (Freenove's value): the OV5640 PCLK comes out at 6.25 MHz, which is why a QSXGA frame takes 447 ms. 20 MHz
would halve it; to be tried on the bench (`CAMOS_XCLK_MHZ`).

Every current and duration below is **an assumption from datasheets, vendor figures or general experience**: no board of
this project has been on a current meter. The tables exist so that the measurements can replace them row by row. When you
measure, write the value, the date and the instrument next to it; until then any number here is "to be measured on
hardware".

Cell: 1S LiPo 2000 mAh nominal, about 1700 mAh usable between 4.2 V and the 3.3 V cut-off (assumption).

## 1. Battery mode (effective power.mode `battery`), ESP32-S3 + OV5640, Wi-Fi, 30-minute interval

| Phase | Time (assumed) | Current (assumed) | Charge | Basis |
|---|---|---|---|---|
| Deep sleep | rest of the cycle (1757 s) | 3 mA | 5 271 mAs | Freenove board floor: ESP32-S3 deep sleep is about 10 uA (datasheet), the CH340, the LDO quiescent and the idle OV5640 (no PWDN wired) add mA-level drain: **unknown, measure first** |
| Wake, battery read, camera up | 1 s | 60 mA | 60 mAs | CPU 240 MHz |
| Capture (init, 6 warm-up frames, JPEG at 2560x1920) | 3 s | 250 mA | 750 mAs | ESP32-S3 + PSRAM + OV5640 (about 150 mA at full resolution per its datasheet) |
| Wi-Fi join, time, TLS, MQTT | 5 s | 150 mA | 750 mAs | Wi-Fi receive about 100 mA, transmit bursts higher |
| Upload (about 0.6 MB) | 4 s | 200 mA | 800 mAs | Wi-Fi transmit |
| Awake window (modem sleep + light sleep) | 30 s | 40 mA | 1 200 mAs | assumption |
| Goodbye, flush | 1 s | 80 mA | 80 mAs | |
| **Total per 30-minute cycle** | | | **8 911 mAs = 2.48 mAh** | average 4.95 mA |

Endurance without sun: 1700 mAh / 4.95 mA = **about 14 days**. The deep-sleep floor is 59 % of it: a lower floor (camera
power switch, removing the CH340's drain) matters more than anything in the firmware. At a 10-minute interval: 7.1 mA
average, 10 days; at 60 minutes: 4.0 mA, 18 days.

## 2. Battery mode on 4G (A7680C), 30-minute interval

Same as above plus the modem instead of Wi-Fi:

| Phase | Time (assumed) | Current (assumed) | Charge | Basis |
|---|---|---|---|---|
| Modem power-on, AT, SIM, registration | 15 s | 120 mA average | 1 800 mAs | SIMCom A7680C: registration bursts up to 2 A peak, average far lower (vendor figures for Cat.1) |
| PPP, DNS, TLS, MQTT, upload 0.6 MB | 12 s | 250 mA average | 3 000 mAs | Cat.1 transmit about 400-700 mA peak |
| Awake window with the modem in idle | 30 s | 60 mA | 1 800 mAs | modem idle about 20 mA + ESP |
| AT+CPOF and PWRKEY off | 3 s | 60 mA | 180 mAs | |
| Modem rail leakage in deep sleep (no rail switch) | 1757 s | 1 mA | 1 757 mAs | A7680C powered-off state: datasheet says tens of uA, breakouts with LEDs / regulators add more: **measure** |
| Capture, wake, goodbye as in table 1 | | | 890 mAs | |
| Deep sleep floor as in table 1 | | | 5 271 mAs | |
| **Total per 30-minute cycle** | | | **14 698 mAs = 4.08 mAh** | average 8.2 mA |

Endurance without sun: **about 8.6 days**. A GPS fix once a day adds about 25 mA x 40 s = 1 000 mAs/day: negligible.

## 3. Powered mode (effective power.mode `powered`)

MQTT always connected, no deep sleep, light sleep between beacons (Wi-Fi) or the modem in CMUX idle (4G):

| | Wi-Fi (assumed) | 4G (assumed) |
|---|---|---|
| Idle, connected, light sleep | 35 mA | 60 mA |
| Captures (30-min interval) averaged | 1.5 mA | 2.7 mA |
| Live view, 5 min/day averaged (ESP32-S3 encoding + Wi-Fi/4G transmit about 350 mA during the session) | 1.2 mA | 1.2 mA |
| **Average** | **about 38 mA** | **about 64 mA** |

Motion detection (web console) keeps the camera powered at QVGA and decodes about two pictures a second; the LAN live
picture keeps the camera powered and Wi-Fi power save off while someone watches. Neither current is measured yet; both
run only in powered mode (motion) or while a viewer is connected (stream).

A 2000 mAh cell alone lasts 1.9 days (Wi-Fi) or 1.1 days (4G) in powered mode: this mode is for a camera on solar with a
healthy cell, which is exactly what `auto` enforces (powered only above `highMv`, back to battery below `lowMv`).

## 4. Solar sizing (assumption)

A 6 V 2 W panel in southern Vietnam gives about 4 equivalent full-sun hours a day on average over the year, less in the
rainy season (assumption, no local measurement): 2 W x 4 h / 3.7 V x 0.8 (charger and MPPT losses) = about 1.7 Ah/day.
That covers battery mode on 4G (0.2 Ah/day) with a wide margin and powered mode on 4G (1.5 Ah/day) only on good days,
which is why `auto` exists. A 3 W panel gives margin for powered mode.

## 5. What to measure and where to write it

1. Deep-sleep floor of the Freenove board alone (USB disconnected, fed at 3V3): replaces row 1 of table 1.
2. The same with the camera module unplugged (tells the OV5640's idle drain without PWDN).
3. One full battery-mode cycle on Wi-Fi with a logging meter: the average mA replaces the table 1 total.
4. Modem powered-off drain on the breakout (row 5 of table 2) and one 4G cycle.
5. Powered-mode idle on Wi-Fi and on 4G (table 3).
Write the results in this file, with the date and the instrument.
