# Wiring: ESP32-S3 CAM + SIMCom A7680C + ATGM336H + solar charger

Status 16 Oct 2026: the Freenove ESP32-S3-WROOM CAM board (kit FNK0085) is on the desk; the A7680C, the ATGM336H and the
solar board are being ordered. Everything below about the modem, the GPS and the charger is a design from datasheets and
breakout-board descriptions, **not yet built or measured**. Pin numbers are the firmware defaults (menuconfig
"CamOS trap camera", every pin editable).

## 1. The board: Freenove ESP32-S3-WROOM CAM (FNK0085)

ESP32-S3-WROOM-1 with 8 MB octal PSRAM and 16 MB flash on the owner's unit (the kit is sold as N8R8 too; the boot log says which), OV5640 on the camera connector (OV2640 also detected), micro SD
(SD_MMC 1-bit: CLK 39, CMD 38, D0 40; not used by the firmware), WS2812 RGB LED on GPIO48, plain LED on GPIO2, two USB-C
(CH340 on UART0 for console and flashing; the native USB also carries the log). Camera pins (verified against
Freenove's `Sketch_07.1_CameraWebServer/camera_pins.h`, block `CAMERA_MODEL_ESP32S3_EYE`):

| Signal | GPIO | Signal | GPIO |
|---|---|---|---|
| PWDN / RESET | not wired (-1 / -1) | XCLK | 15 |
| SIOD / SIOC | 4 / 5 | VSYNC / HREF / PCLK | 6 / 7 / 13 |
| Y2 Y3 Y4 Y5 | 11 9 8 10 | Y6 Y7 Y8 Y9 | 12 18 17 16 |

GPIOs taken by the camera, SD, PSRAM (35-37), USB (19, 20) and UART0 (43, 44) leave these on the header for us:
**1, 2, 14, 21, 41, 42, 47, 48** plus the strapping pins 3, 45, 46 (usable as outputs after boot with care; 0 is BOOT).

Firmware pin plan (Freenove preset):

| Function | GPIO | Kconfig | Note |
|---|---|---|---|
| Battery voltage (ADC1_CH0) | 1 | `CAMOS_BATT_ADC_GPIO` | 100k / 100k divider from BAT+, 100 nF to GND |
| Solar / charger input (ADC1_CH1) | 2 | `CAMOS_SOLAR_ADC_GPIO` | 100k / 33k divider from the panel + (also the board's plain LED pin: the LED will glow faintly) |
| GPS TXD -> ESP RX (UART2) | 14 | `CAMOS_GPS_RX_GPIO` | NMEA 9600 8N1, 3.3 V |
| GPS power switch | 21 | `CAMOS_GPS_POWER_GPIO` | P-MOSFET gate, low = on |
| Modem RXD <- ESP TX (UART1) | 41 | `CAMOS_MODEM_TX_GPIO` | 115200 8N1 |
| Modem TXD -> ESP RX (UART1) | 42 | `CAMOS_MODEM_RX_GPIO` | |
| Modem PWRKEY | 47 | `CAMOS_MODEM_PWRKEY_GPIO` | through the breakout's transistor (GPIO high = PWRKEY low); set `CAMOS_MODEM_PWRKEY_ACTIVE_HIGH=n` if wired straight |
| Modem rail switch (optional) | -1 | `CAMOS_MODEM_POWER_GPIO` | no free pin left: use GPIO3 or 45 if wanted, else the modem rail is always on and AT+CPOF / PWRKEY is the off state |
| Charger status CHRG (optional) | -1 | `CAMOS_CHARGING_GPIO` | open-drain, low while charging; GPIO46 (input only) fits |
| Portal button | 0 | `CAMOS_BUTTON_GPIO` | the BOOT button, hold 3 s |

## 2. Power rails

```
 6 V / 2 W panel --+--> solar charger (CN3791 MPPT or TP4056) --> BAT+ (LiPo 1S, 3.7 V nominal)
                   |                                                  |
                   +--> 100k/33k divider --> GPIO2                     +--> 100k/100k divider --> GPIO1
                                                                      |
                                                                      +--> 3.3 V LDO (>= 1 A, low dropout) --> board 3V3 pin
                                                                      |         +--> P-MOSFET (GPIO21) --> ATGM336H VCC
                                                                      |
                                                                      +--> [P-MOSFET, optional] --> A7680C VCC (3.4-4.2 V)
                                                                                 1000 uF + 100 nF at the modem VCC
```

- **Feed the board at 3V3, not 5 V.** The board's own regulator sits behind the USB 5 V input; from a LiPo (3.0-4.2 V) a
  5 V input is below its dropout near empty. A low-dropout 3.3 V regulator rated for at least 1 A (the ESP32-S3 with Wi-Fi
  on peaks around 500 mA, the OV5640 adds about 150 mA at full resolution: datasheet figures, not measured here) straight
  to the 3V3 pin. Candidates: HT7833 (only 500 mA, marginal), MCP1826S-3302 (1 A), AP2112K-3.3 (600 mA, marginal), or a
  small buck-boost module when the cell may go below 3.4 V. Check whether the Freenove board's 3V3 pin feeds the module
  directly (it does on the Freenove schematic as far as its pinout shows; confirm with a meter before the first power-up).
- **The A7680C takes 3.4-4.2 V and up to 2 A peaks** during registration and transmit bursts (SIMCom hardware design
  guide). Run it from the LiPo rail directly, never from the 3.3 V LDO, with at least 1000 uF low-ESR bulk capacitance plus
  100 nF right at the module's VCC pins and short, thick leads (a 20 cm thin jumper drops the voltage under the burst and
  the module resets). Breakout boards (the TDM2309 A7680C board) already carry part of this; add the bulk capacitor anyway.
- The A7680C bare module's UART is 1.8 V logic; breakout boards normally translate to 3.3 V. **Verify on the board you
  receive** (a 1.8 V TXD still reads as high on a 3.3 V ESP input most of the time, but ESP TX at 3.3 V into a 1.8 V RXD
  needs a level shifter or at least a series resistor as the vendor describes).
- **PWRKEY**: pull low >= 1 s to power on, >= 2.5 s to power off (the firmware uses 1.5 s and 3 s; `AT+CPOF` is tried
  first). The breakout usually has an NPN transistor so a GPIO high pulls PWRKEY low; the default assumes that.
- **ATGM336H**: 3.3 V, about 25 mA while acquiring (datasheet), 3.3 V logic NMEA at 9600 baud. Too much for a GPIO:
  switch it with a P-MOSFET (AO3401 / SI2301: source 3V3, drain to the module, 100 kOhm gate to source, GPIO21 to the gate
  through 1 kOhm; GPIO low = on). The module keeps no almanac without power on its backup pin (V_BCKP): every fix is a
  cold start, about 35 s with a clear sky per the datasheet. An active antenna (3.3 V bias from the module) matters under
  trees.
- **Charger**: CN3791 boards (MPPT for a 6 V panel) or TP4056 boards (5 V panel, no MPPT; most have a load-sharing flaw:
  the load shares the cell current and the TP4056 may never terminate: acceptable for a demo). The `CHRG` pin of either
  is open-drain low while charging: wire it to a free input if you want `battery.charging` from the charger instead of
  from the panel voltage.
- Antennas: the A7680C needs an LTE antenna (IPEX on the module, SMA on breakouts; a 700-2700 MHz stick antenna), the
  GPS an active ceramic patch with IPEX. Keep the GPS antenna 10 cm away from the LTE antenna and the panel.

## 3. The plain ESP32-CAM (AI-Thinker) on hand

Wi-Fi + snapshot only (no modem, no GPS in that build: `CAMOS_MODEM=n`, `CAMOS_GPS=n`). Battery divider on GPIO14 (ADC2,
read before Wi-Fi starts), portal button between GPIO13 and GND, flash LED on GPIO4. Power at 5 V in through a USB-serial
adapter for the bench; from a LiPo use the same 3.3 V LDO scheme into the 3V3 pin (the AMS1117 on the board drops too much).
See tools/README.md for the serial adapter wiring.

## 4. Bench order

1. Board alone on USB: flash, portal, pair, snapshot over Wi-Fi (G-S2). Nothing else wired.
2. Add the battery divider (GPIO1) and check `batt` on the console against a meter.
3. Add the GPS on UART2 + its switch: console `gps 120` outdoors.
4. Add the modem on UART1 with its own supply: console `at AT+CPIN?`, `at AT+CSQ`, then `set apn <apn>` and `net`.
5. Only then the solar charger and the outdoor box.
