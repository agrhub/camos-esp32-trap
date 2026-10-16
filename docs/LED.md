# Status LED

The Freenove ESP32-S3 CAM has a WS2812 RGB LED on GPIO48 (Freenove's `ws2812.h`: `WS2812_PIN 48`). The firmware drives
it through the RMT peripheral (`espressif/led_strip` 3.x) at low brightness (`CAMOS_LED_BRIGHTNESS` = 25 of 255, about
10 %). The AI-Thinker ESP32-CAM preset uses its red LED on GPIO33 (active low): the same patterns, without colours.
The generic ESP32-S3 CAM preset has no LED configured (`CAMOS_LED_NONE`): the RGB pin of those boards is not documented.

The AMB82 camera defines one LED meaning (its blue LED blinks during Wi-Fi setup, otherwise off); the setup state keeps
that colour here, the rest is new.

| State | Pattern | When |
|---|---|---|
| Setup portal | blue, slow breathe (2 s period) | the `CAM-xxxx` setup network is open |
| Connecting | yellow, fast blink (4 Hz) | Wi-Fi joining, 4G registering, MQTT not up yet |
| Online | green 60 ms blip every 5 s for the first 60 s, then dark | MQTT connected (dark afterwards to save power) |
| Capturing / uploading | one white 150 ms flash | each capture (schedule, RPC, console, web console) |
| Error | red double blink every 2 s | no network after the give-up time, certificate missing, registration refused (409) |
| Upload failed | red double blink, 3 times | a capture or upload failed |
| Low battery | orange blink, 3 times | just before a deep sleep with the cell below `CAMOS_BATT_LOW_MV` |
| OTA | purple, steady | firmware update downloading and writing |
| Deep sleep | off | the data line is held low through the sleep |

Switch it off: portal "Cài đặt nâng cao > Đèn trạng thái", web console "Nguồn điện & lịch chụp", serial console
`set ledEnabled 0` (NVS key `led`, default from `defaults.*.json` `ledEnabled`). Kconfig: "CamOS trap camera > Status LED"
(type, GPIO, count, brightness).

Every state change is written to the log (`I (...) led: portal (blue breathe)`), so a serial log shows what the LED
showed. The console command `led <state>` (`portal`, `connecting`, `online`, `error`, `ota`, `off`, `capture`,
`uploaderr`, `lowbatt`) shows one state on demand to check the colours on a board.

Power: a WS2812 draws about 1 mA even when dark (its own controller; datasheet value, not measured here). The firmware
cannot switch it off; on a battery build that matters (docs/POWER_BUDGET.md). Cutting the LED's 5 V / 3V3 feed is the only
way to remove it.
