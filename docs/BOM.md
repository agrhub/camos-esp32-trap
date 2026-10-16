# What to buy (bill of materials)

Two builds:

- **Wi-Fi camera**: the camera board, a microSD card, a box and the trap. Powered from a USB charger near a Wi-Fi network.
- **Field camera (4G + GPS + solar)**: adds the 4G modem, the GPS, a LiPo cell, a solar panel and its charger.

Prices are the prices shown on the shop pages on **17 Oct 2026**, in VND unless marked. They change often, and some
items were out of stock that day (marked *out of stock*). "Not read" means the page gave no price; nothing here is
estimated. banlinhkien.com prices exclude VAT.

Wiring: [WIRING.md](WIRING.md). Assembly: [ASSEMBLY.md](ASSEMBLY.md).

## 1. Every camera

| # | Part | Qty | Shop | Price read | Notes |
|---|---|---|---|---|---|
| 1 | **Freenove ESP32-S3-WROOM CAM**, kit FNK0085 (ESP32-S3, 8 MB PSRAM, OV5640) | 1 | [store.freenove.com/products/fnk0085](https://store.freenove.com/products/fnk0085) (international) | **US$19.99** | the board this firmware is built and tested on; the 8 MB and 16 MB flash variants both work |
| 1b | or a generic ESP32-S3 N16R8 CAM board | 1 | [icdayroi.com: Kit ESP32-S3 WROOM N16R8 CAM](https://icdayroi.com/kit-esp32-s3-wroom-n16r8-cam-development) | 380.000 | firmware preset `esp32s3cam` (build from source), no status LED; check the pin map first ([dev/PINPROBE.md](dev/PINPROBE.md)); camera sensor not stated on the page |
| 2 | microSD card 32 GB, class 10 | 1 | [thegioiic.com: Micro SDHC 32GB Class10](https://thegioiic.com/the-nho-micro-sdhc-32gb-class10-ghi-10mb-s-doc-48mb-s) | 103.740 (*out of stock*) | optional: keeps every photo on the camera too (Playback page). FAT32 |
| 3 | Waterproof box IP65 150 x 110 x 70 mm | 1 | [thegioiic.com: hộp nhựa chống nước 150x110x70](https://thegioiic.com/hop-nhua-chong-nuoc-150x110x70-co-tai) | 100.320 (*out of stock*) | cut a window for the lens and glue in clear acrylic |
| 3b | or IP65 box 160 x 160 x 90 mm with a clear lid | 1 | [thegioiic.com: hộp nắp trong suốt 160x160x90](https://thegioiic.com/hop-nhua-chong-nuoc-nap-trong-suot-160x160x90) | 90.060 (*out of stock*) | the camera looks through the lid |
| 4 | Cable gland PG7 (IP68) | 1-3 | [thegioiic.com: PG7](https://thegioiic.com/pg7-oc-siet-cap-nhua-chong-nuoc-ip68-co-dai-oc-mau-den) | 1.368 each | power cable, solar cable, antenna cable |
| 5 | Yellow sticky cards 15 x 25 cm, pack of 50 | 1 | [gumic.vn: combo 50 miếng bẫy ruồi vàng](https://gumic.vn/combo-50-mieng-bay-ruoi-vang-va-cac-loai-con-trung-gay-hai-dp6454.htm) | 155.000 | the card the camera photographs |
| 6 | Fruit-fly trap holder with lure (BA780, cap + 1 bottle) | 1 | [gumic.vn: dụng cụ bẫy ruồi vàng](https://gumic.vn/dung-cu-bay-ruoi-vang-hai-trai-cay-the-he-moi-hieu-qua-tiet-kiem-chi-phi) | 70.000 | the page does not say which option this price is for |
| 6b | Methyl eugenol lure Vizubon-D (2 x 10 ml) | 1 | [nongcutiendam.vn: Vizubon-D](https://nongcutiendam.vn/san-pham/thuoc-dan-du-diet-ruoi-vang-vizubon-d-t58/) | 35.000 (sale; list 40.000) | contains an insecticide (naled): follow the label |
| 7 | USB-C cable and a 5 V 1 A USB charger | 1 | any | not read | Wi-Fi camera only; the board's CH340 port also flashes the firmware |

## 2. Field camera: add these

| # | Part | Qty | Shop | Price read | Notes |
|---|---|---|---|---|---|
| 8 | **4G module SIMCom A7680C, TDM2309 board** (SIM800L-compatible pins) | 1 | [linhkienthuduc.com: A7680C TDM2309](https://linhkienthuduc.com/san-pham/module-4g-simcom-a7680c-tdm2309-giai-phap-thay-the-y-het-chan-cho-2g-sim800-sim800l) | 169.000 ("4G SMS" version; VoLTE version 199.000) | 3.8-4.2 V, 3.3 V / 5 V logic; no antenna mentioned on the page |
| 9 | 4G antenna, FPC 3 dBi, IPEX | 1 | [hshop.vn: anten 4G FPC IPEX](https://hshop.vn/anten-4g-3dbi-53-24mm-fpc-ipex-tx4g-fpc-5324-antenna-ziisor) | 20.000 | 690-960 and 1700-2700 MHz; stick it inside the lid |
| 9b | or 4G antenna 700-2700 MHz 5 dBi, SMA, 3 m cable | 1 | [nshopvn.com: anten 700-2700MHz](https://nshopvn.com/product/anten-4g-3g-2g-gprs-nb-gsm-bang-tan-700-2700mhz-5dbi/) | 66.000 (*out of stock*) | outside the box; needs an IPEX-to-SMA pigtail if the modem has IPEX |
| 10 | **GPS module ATGM336H** (GPS + BeiDou) | 1 | [thegioiic.com: ATGM336H](https://thegioiic.com/atgm336h-gps-dual-mode-module) | 173.280 (*out of stock*) | also [icdayroi.com](https://icdayroi.com/module-gps-bds-atgm336h) at 250.000; antenna not mentioned |
| 11 | Active GPS antenna 25 x 25 mm, IPEX | 1 | [linhkienthuduc.com: anten GPS 25x25](https://linhkienthuduc.com/san-pham/anten-gps-25x25mm-active-antenna/) | 65.000 | 3-5 V active patch |
| 12 | LiPo cell 3.7 V 2000 mAh 103450, with protection | 1 | [thegioiic.com: Lipo 103450 2000mAh](https://thegioiic.com/pin-sac-lipo-103450-2000mah-3-7v) | 49.875 (*out of stock*) | connector not stated |
| 12b | or 18650 cell 2000 mAh + 1-cell holder | 1 + 1 | [hshop.vn: 18650 2000mAh](https://hshop.vn/pin-sac-18650-li-ion-rechargeable-battery-3-7v-2000mah-10c-sunpower), [icdayroi.com: holder](https://icdayroi.com/hop-de-pin-18650-loai-1-cell) | 35.000 + 4.000 | the cell page does not mention a protection circuit: the charger's DW01 protects it |
| 13 | Solar panel 6 V 2 W, 136 x 110 mm | 1 | [thegioiic.com: tấm NLMT 2W 6V](https://thegioiic.com/tam-nang-luong-mat-troi-2w-6v) | 83.220 | also [icdayroi.com](https://icdayroi.com/pin-nang-luong-mat-troi-6v-2w) at 120.000 |
| 14 | Charger TP4056 with DW01 protection, USB-C | 1 | [thegioiic.com: TP4056 Type-C có bảo vệ](https://thegioiic.com/tp4056-mach-sac-pin-usb-type-c-1a-co-bao-ve) | 6.270 | works with the 6 V panel (its open-circuit voltage stays under the TP4056's 8 V limit); no MPPT |
| 14b | or CN3791 MPPT solar charger | 1 | [thegioiic.com: CN3791](https://thegioiic.com/cn3791-mach-sac-pin-nang-luong-mat-troi) | 67.260 (*out of stock*) | **the boards found are the 9 V version: use them with a 9 V panel**, not the 6 V one |
| 15 | 3.3 V buck-boost TPS63020 (1.8-5.5 V in, 1.3 A) | 1 | [thegioiic.com: TPS63020 3.3V](https://thegioiic.com/tps63020-mach-on-ap-dc-dc-3-3v-1-3a) | 53.000 (*out of stock*) | feeds the board's 3V3 pin from the cell |
| 16 | P-MOSFET AO3401A, SOT-23 | 1-2 | [thegioiic.com: AO3401A](https://thegioiic.com/ao3401a-mosfet-kenh-p-30v-4a-sot23) | 2.772 each | GPS power switch (a second one for an optional modem rail switch) |
| 17 | NPN transistor MMBT3904 (or S8050), SOT-23 | 1 | [thegioiic.com: MMBT3904](https://thegioiic.com/mmbt3904-1am-transistor-npn-40v-0-2a-3-chan-sot-23) | 399 | drives the modem's PWRKEY |
| 18 | Resistor kit 1/4 W 1 %, 600 pieces (30 values) | 1 | [thegioiic.com: bộ 600 điện trở](https://thegioiic.com/bo-600-dien-tro-1-4w-1-5-vong-mau) | 39.900 | needs 100k x4, 33k x1, 1k x2; check the kit's value list (not confirmed on the page) |
| 19 | Capacitor 1000 uF 10 V, pack of 5 | 1 | [banlinhkien.com: tụ 1000uF 10V](https://banlinhkien.com/tu-hoa-1000uf-10v-10x13mm-5c-p23358531.html) | 2.500 | at the modem's VCC; prefer a low-ESR type (not stated on this page) |
| 20 | Ceramic capacitor 100 nF, pack of 10 | 1 | [banlinhkien.com: tụ gốm 104](https://banlinhkien.com/tu-gom-104-100nf-50v-10c-p6652092.html) | 2.000 | modem VCC and the two ADC pins |
| 21 | Cable gland PG9 | 1 | [thegioiic.com: PG9](https://thegioiic.com/pg9-oc-siet-cap-nhua-chong-nuoc-ip68-co-dai-oc-mau-trang) | 1.710 | solar cable |
| 22 | Jumper wires 20 cm male-female, 40 pieces | 1 | [hshop.vn: dây cắm đực-cái](https://hshop.vn/day-camtest-board-duc-coi20cm40soi) | 30.000 | use thicker wire (0.5 mm2) from the cell to the modem |
| 23 | Nano SIM with a data plan (Viettel, MobiFone, Vinaphone) | 1 | carrier, e.g. [4g.viettel.vn/data](https://4g.viettel.vn/data) | not read | the camera picks the APN from the SIM ([APN.md](APN.md)) |

## 3. Totals (prices read on 17 Oct 2026)

| Build | Parts priced in VND | Plus |
|---|---|---|
| **Wi-Fi camera** (items 2, 3, 4 x1, 5, 6) | **430.428 ₫** | the board: US$19.99 (Freenove), or 380.000 ₫ for the generic board (item 1b; total 810.428 ₫); a USB charger and cable (not read) |
| **Field camera, 4G + GPS + solar** (the Wi-Fi items + 8, 9, 10, 11, 12, 13, 14, 15, 16 x1, 17, 18, 19, 20, 21, 22 and 2 more PG7) | **1.132.090 ₫** (430.428 + 701.662) | the board as above (with the generic board: 1.512.090 ₫); SIM and data plan (not read) |

The totals take the first option of each row, one pack where parts come in packs, and the prices as shown, including
items that were out of stock that day.

## 4. International alternatives (prices read on 17 Oct 2026)

| Part | Where | Price read |
|---|---|---|
| LiPo 3.7 V 2000 mAh with JST-PH (Adafruit 2011) | [adafruit.com/product/2011](https://www.adafruit.com/product/2011) | US$12.50 (out of stock) |
| Solar LiPo charger bq24074, takes a 6-10 V panel (Adafruit 4755) | [adafruit.com/product/4755](https://www.adafruit.com/product/4755) | US$14.95 |
| AO3401A P-MOSFET (LCSC C15127) | [lcsc.com C15127](https://www.lcsc.com/product-detail/C15127.html) | US$0.103 each at 5+ |
| MMBT3904 NPN (LCSC C20526) | [lcsc.com C20526](https://www.lcsc.com/product-detail/C20526.html) | US$0.0118 each at 50+ |

## 5. For the bench

A USB-C cable to the board's CH340 port (flashing and the serial console), a multimeter (check the 3V3 rail and the
battery reading before closing the box), and for the AI-Thinker ESP32-CAM a USB-serial adapter (CP2102 / CH340, 5 V).
