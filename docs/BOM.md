# Bill of materials (one orchard trap camera)

Prices not listed (they change weekly); Vietnamese shops the owner uses are named, search the product name on the site.
Quantities for one 4G + GPS + solar camera; the Wi-Fi-only camera needs only the first row and a USB supply.

| # | Part | Qty | Where | Notes |
|---|---|---|---|---|
| 1 | Freenove ESP32-S3-WROOM CAM board, kit FNK0085 (N8R8, OV5640) | 1 | store.freenove.com (on hand) | the G-S2 board; generic ESP32-S3 N16R8 CAM boards with the ESP32-S3-EYE pin map also work (preset `esp32s3cam`) |
| 2 | SIMCom A7680C LTE Cat.1 module on a breakout, "TDM2309" board | 1 | linhkienthuduc.com, search "A7680C TDM2309" | UART AT, PPP, PWRKEY, SIM holder, IPEX/SMA; check the UART logic level of the breakout |
| 3 | LTE antenna 700-2700 MHz, SMA or IPEX to match | 1 | same shop | a 10 cm stick antenna is enough in the open |
| 4 | Nano SIM with a data plan (Viettel / Mobifone / Vinaphone) | 1 | carrier | APN `v-internet` (Viettel), `m-wap` (Mobifone), `m3-world` (Vinaphone): confirm with the carrier |
| 5 | ATGM336H GPS + BeiDou module, 3.3 V, NMEA 9600 | 1 | thegioiic.com, search "ATGM336H" | take the version with an IPEX antenna connector |
| 6 | Active GPS antenna, IPEX, 3.3 V bias | 1 | thegioiic.com | ceramic patch 25 x 25 mm |
| 7 | LiPo / Li-ion cell 1S, 2000-3000 mAh (18650 with holder, or a pouch cell with a JST-PH 2.0) | 1 | any | protected cell, or a protection board |
| 8 | Solar charger board: CN3791 MPPT (6 V panel) preferred, TP4056 with solar input as the fallback | 1 | thegioiic.com / linhkienthuduc.com | with the `CHRG` pin broken out if possible |
| 9 | Solar panel 6 V 2-3 W (about 110 x 136 mm) | 1 | same | see POWER_BUDGET.md for the sizing argument |
| 10 | 3.3 V LDO >= 1 A, low dropout (MCP1826S-3302, or a buck-boost module 3.3 V 1.5 A) | 1 | thegioiic.com | feeds the board's 3V3 pin |
| 11 | P-MOSFET SOT-23 (AO3401, SI2301) | 2-3 | thegioiic.com | GPS switch, optional modem rail switch, optional camera switch |
| 12 | Resistors 100 kOhm (4), 33 kOhm (1), 1 kOhm (3); capacitors 100 nF (4), 1000 uF 6.3 V low-ESR (1), 10 uF (2) | kit | any | dividers, gate pull-ups, modem bulk capacitor |
| 13 | Push button (portal) | 1 | any | only for the AI-Thinker board; the Freenove board uses BOOT |
| 14 | IP65 box about 150 x 100 x 70 mm with a clear window, cable glands, a bracket for the trap | 1 | any | the camera looks at the sticky card from 15-25 cm |
| 15 | Wires, JST connectors, heat-shrink, a small perfboard | kit | any | |

For the bench only: a USB-C cable to the CH340 port of the Freenove board (console + flashing), a multimeter, and for the
AI-Thinker ESP32-CAM a USB-serial adapter (CP2102 / CH340, 5 V) with the GPIO0-to-GND jumper for flashing.

Optional, for current measurements (POWER_BUDGET.md asks for them): a USB power meter with a LiPo adapter, or a Nordic
PPK2 / a 1 Ohm shunt and a logging meter.
