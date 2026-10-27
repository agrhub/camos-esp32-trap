# What to buy (bill of materials)

Two builds:

- **Wi-Fi camera**: the camera board, a microSD card and a trap housing with its lure (or a box and a sticky card). Powered from a USB charger near a Wi-Fi network.
- **Field camera (4G + GPS + solar)**: adds the 4G modem, the GPS, a LiPo cell and a solar charger (the hanging trap's lid has its own solar panel; the box needs one).

Prices are the prices shown on the shop pages on **17 Oct 2026**, in VND unless marked; the Shopee rows (items 1, 1a,
6c, 12, 24, 25) were read on **24 Oct 2026**, for the variant named in the row, and item 15 on **25 Oct 2026**. They change often, and some items
were out of stock that day (marked *out of stock*). "Not read" means the page gave no price; nothing here is
estimated. banlinhkien.com prices exclude VAT.

Wiring: [WIRING.md](WIRING.md). Assembly: [ASSEMBLY.md](ASSEMBLY.md).

## 1. Every camera

| # | Part | Qty | Shop | Price read | Notes |
|---|---|---|---|---|---|
| 1 | **ESP32-S3 N16R8 CAM board, the version without a camera** (16 MB flash, 8 MB PSRAM, USB, microSD) | 1 | [Shopee: ESP32-S3 N16R8 CAM](https://shopee.vn/ESP32-S3-N16R8-CAM-OV5640-5MP-OV3660-3MP-%C4%90%C3%A3-QC-16MB-Flash-8MB-PSRAM-WiFi-BLE-USB-OTG-microSD-i.484901669.57065429758) | 180.000 (variant "s3n16r8 no cam") | buy it without a camera and add item 1a. The page does not give the pin map: flash the Freenove image, and if the camera does not start see [dev/PINPROBE.md](dev/PINPROBE.md) |
| 1a | **OV5640 camera module (DVP) with an adjustable wide-angle lens** | 1 | [Shopee: OV5640 DVP ống kính góc rộng](https://shopee.vn/M%C3%B4-%C4%91un-m%C3%A1y-%E1%BA%A3nh-OV5640-DVP-%E1%BB%90ng-k%C3%ADnh-g%C3%B3c-r%E1%BB%99ng-5MP-cho-ESP32-CAM-i.1090518323.41175318836) | 239.640 (after the shop's voucher, as shown) | field of view 65°, 100°, 120° or 130° to choose; the lens turns, so it is focused on the bottom of the trap (or a card 15-25 cm away) without modifying it, then locked |
| 1b | or **Freenove ESP32-S3-WROOM CAM**, kit FNK0085 (ESP32-S3, 8 MB PSRAM, OV5640) | 1 | [store.freenove.com/products/fnk0085](https://store.freenove.com/products/fnk0085) (international) | **US$19.99** | the board this firmware was first built and tested on; the 8 MB and 16 MB flash variants both work. Its OV5640 has a fixed lens focused far away: sharp only from about 50 cm |
| 1c | or a generic ESP32-S3 N16R8 CAM board | 1 | [icdayroi.com: Kit ESP32-S3 WROOM N16R8 CAM](https://icdayroi.com/kit-esp32-s3-wroom-n16r8-cam-development) | 380.000 | firmware preset `esp32s3cam` (build from source), no status LED; check the pin map first ([dev/PINPROBE.md](dev/PINPROBE.md)); camera sensor not stated on the page |
| 2 | microSD card 32 GB, class 10 | 1 | [thegioiic.com: Micro SDHC 32GB Class10](https://thegioiic.com/the-nho-micro-sdhc-32gb-class10-ghi-10mb-s-doc-48mb-s) | 103.740 (*out of stock*) | optional: keeps every photo on the camera too (Playback page). FAT32 |
| 3 | **Hanging solar insect trap housing** (solar wasp / yellow-jacket trap with LED light) | 1 | [amazon.com/dp/B0GV48Z8BG](https://www.amazon.com/dp/B0GV48Z8BG) or [aliexpress.com/item/1005006892558507](https://www.aliexpress.com/item/1005006892558507.html) (international) | US$9.90 (Amazon) or US$3.00 (AliExpress) for one trap | the housing of the trap in the photos: the board, the modules and their wiring are mounted inside the lid ([ASSEMBLY.md](ASSEMBLY.md)); the Amazon listing also sells packs of 4 |
| 3a | or waterproof box IP65 150 x 110 x 70 mm | 1 | [thegioiic.com: hộp nhựa chống nước 150x110x70](https://thegioiic.com/hop-nhua-chong-nuoc-150x110x70-co-tai) | 100.320 (*out of stock*) | cut a window for the lens and glue in clear acrylic |
| 3b | or IP65 box 160 x 160 x 90 mm with a clear lid | 1 | [thegioiic.com: hộp nắp trong suốt 160x160x90](https://thegioiic.com/hop-nhua-chong-nuoc-nap-trong-suot-160x160x90) | 90.060 (*out of stock*) | the camera looks through the lid |
| 5 | Yellow sticky cards 15 x 25 cm, pack of 50 | 1 | [gumic.vn: combo 50 miếng bẫy ruồi vàng](https://gumic.vn/combo-50-mieng-bay-ruoi-vang-va-cac-loai-con-trung-gay-hai-dp6454.htm) | 155.000 | the card the camera photographs |
| 6 | Fruit-fly trap holder with lure (BA780, cap + 1 bottle) | 1 | [gumic.vn: dụng cụ bẫy ruồi vàng](https://gumic.vn/dung-cu-bay-ruoi-vang-hai-trai-cay-the-he-moi-hieu-qua-tiet-kiem-chi-phi) | 70.000 | the page does not say which option this price is for |
| 6b | Methyl eugenol lure Vizubon-D (2 x 10 ml) | 1 | [nongcutiendam.vn: Vizubon-D](https://nongcutiendam.vn/san-pham/thuoc-dan-du-diet-ruoi-vang-vizubon-d-t58/) | 35.000 (sale; list 40.000) | contains an insecticide (naled): follow the label |
| 6c | Fruit-fly lure, 2 ml tube | 1 | [Shopee: chất dẫn dụ ruồi vàng ống 2ml](https://shopee.vn/Ch%E1%BA%A5t-d%E1%BA%ABn-d%E1%BB%A5-ru%E1%BB%93i-v%C3%A0ng-%C4%91%E1%BB%A5c-qu%E1%BA%A3-c%C3%A2y-tr%E1%BB%93ng-(%E1%BB%91ng-2ml)-i.1405653524.25939583230) | 8.900 | the lure of the hanging trap (item 3); follow the label |
| 7 | USB-C cable and a 5 V 1 A USB charger | 1 | any | not read | Wi-Fi camera only; the board's CH340 port also flashes the firmware |

## 2. Field camera: add these

| # | Part | Qty | Shop | Price read | Notes |
|---|---|---|---|---|---|
| 8 | **4G module SIMCom A7680C, TDM2309 board** (SIM800L-compatible pins) | 1 | [linhkienthuduc.com: A7680C TDM2309](https://linhkienthuduc.com/san-pham/module-4g-simcom-a7680c-tdm2309-giai-phap-thay-the-y-het-chan-cho-2g-sim800-sim800l) | 169.000 ("4G SMS" version; VoLTE version 199.000) | 3.8-4.2 V, 3.3 V / 5 V logic; no antenna mentioned on the page (the module of the hanging trap came with its antenna, so item 9 is not needed there) |
| 9 | 4G antenna, FPC 3 dBi, IPEX | 1 | [hshop.vn: anten 4G FPC IPEX](https://hshop.vn/anten-4g-3dbi-53-24mm-fpc-ipex-tx4g-fpc-5324-antenna-ziisor) | 20.000 | 690-960 and 1700-2700 MHz; stick it inside the lid |
| 9b | or 4G antenna 700-2700 MHz 5 dBi, SMA, 3 m cable | 1 | [nshopvn.com: anten 700-2700MHz](https://nshopvn.com/product/anten-4g-3g-2g-gprs-nb-gsm-bang-tan-700-2700mhz-5dbi/) | 66.000 (*out of stock*) | outside the box; needs an IPEX-to-SMA pigtail if the modem has IPEX |
| 10 | **GPS module ATGM336H** (GPS + BeiDou) | 1 | [thegioiic.com: ATGM336H](https://thegioiic.com/atgm336h-gps-dual-mode-module) | 173.280 (*out of stock*) | also [icdayroi.com](https://icdayroi.com/module-gps-bds-atgm336h) at 250.000; antenna not mentioned (the module of the hanging trap came with its antenna, so item 11 is not needed there) |
| 11 | Active GPS antenna 25 x 25 mm, IPEX | 1 | [linhkienthuduc.com: anten GPS 25x25](https://linhkienthuduc.com/san-pham/anten-gps-25x25mm-active-antenna/) | 65.000 | 3-5 V active patch, for a module mounted deep inside a box. Not needed in the hanging trap: the antenna that came with the module gives a fix in seconds outdoors (7 satellites, 25 Oct 2026; the misses before were a firmware UART clock bug, fixed in 0.9.2, not released yet) |
| 12 | **LiPo pouch cell 3.7 V 2000 mAh 103450** (10 x 34 x 50 mm) | 1 | [Shopee: pin tép 103450 2000mAh](https://shopee.vn/Pin-t%C3%A9p-3.7V-2000mAh-103450-d%C3%B9ng-cho-loa-Bluetooth-%C4%90%E1%BB%8Bnh-v%E1%BB%8B-GPS-Camera-h%C3%A0nh-tr%C3%ACnh-i.853237848.28122587321) | 72.000 (2000 mAh variant) | flat, so it fits the lid of the hanging trap; an old phone battery (3.7-3.85 V Li-ion / LiPo) does the same. Protection not stated on the page: the charger's DW01 protects it. Also [thegioiic.com: Lipo 103450](https://thegioiic.com/pin-sac-lipo-103450-2000mah-3-7v) at 49.875 (*out of stock* on 17 Oct) |
| 12b | or 18650 cell 2000 mAh + 1-cell holder | 1 + 1 | [hshop.vn: 18650 2000mAh](https://hshop.vn/pin-sac-18650-li-ion-rechargeable-battery-3-7v-2000mah-10c-sunpower), [icdayroi.com: holder](https://icdayroi.com/hop-de-pin-18650-loai-1-cell) | 35.000 + 4.000 | bulkier than item 12; the cell page does not mention a protection circuit: the charger's DW01 protects it |
| 13 | Solar panel 6 V 2 W, 136 x 110 mm | 1 | [thegioiic.com: tấm NLMT 2W 6V](https://thegioiic.com/tam-nang-luong-mat-troi-2w-6v) | 83.220 | also [icdayroi.com](https://icdayroi.com/pin-nang-luong-mat-troi-6v-2w) at 120.000 |
| 14 | Charger TP4056 with DW01 protection, USB-C | 1 | [thegioiic.com: TP4056 Type-C có bảo vệ](https://thegioiic.com/tp4056-mach-sac-pin-usb-type-c-1a-co-bao-ve) | 6.270 | works with the 6 V panel (its open-circuit voltage stays under the TP4056's 8 V limit); no MPPT |
| 14b | or CN3791 MPPT solar charger | 1 | [thegioiic.com: CN3791](https://thegioiic.com/cn3791-mach-sac-pin-nang-luong-mat-troi) | 67.260 (*out of stock*) | **the boards found are the 9 V version: use them with a 9 V panel**, not the 6 V one |
| 15 | **5 V boost board, mini DC-DC 5 / 8 / 9 / 12 V** (2.5 V in and up; 5 V 1 A from a 3.7 V cell, per the listing) | 1 | [thegioiic.com: mạch tăng áp mini 5V 8V 9V 12V](https://www.thegioiic.com/mach-tang-ap-dc-dc-mini-dau-ra-5v-8v-9v-12v) | 5.700 | feeds the board's **5V pin** from the cell; the board's own regulator makes the 3.3 V. **It ships set to 12 V: set its A / B pads to 0 / 0 (5 V, table on the listing) and measure 5.0-5.2 V before connecting.** Its LED draws about 1 mA (under 1 mA without it): remove it for a longer deep sleep. Not a 3.3 V regulator on the 3V3 pin: the 0.6 A auto buck-boost sold for it fell to 1.8 V under the board's load (25 Oct 2026), while a 5 V boost ran the same board on the same old cell |
| 16 | **P-MOSFET AO3401A** (or SI2301 / AO3407), SOT-23 | 1 | [thegioiic.com: AO3401A](https://thegioiic.com/ao3401a-mosfet-kenh-p-30v-4a-sot23) | 2.772 each | **the 4G module's power switch** (4 A): with it the module is off whenever the ESP32 is not running, instead of draining the cell ([WIRING.md](WIRING.md), section 2). A second one only for a GPS switched on its supply instead of its ground (`gpsPowerActiveLow`). A SOT-23 to 2.54 mm adapter makes it easy to solder on item 24 |
| 17 | NPN transistor MMBT3904, S8050 or C1815 | 1-2 | [thegioiic.com: MMBT3904](https://thegioiic.com/mmbt3904-1am-transistor-npn-40v-0-2a-3-chan-sot-23) | 399 | drives the gate of the 4G switch (item 16) from GPIO47; in the hanging trap a C1815 also switches the GPS ground. Never the 4G supply itself: 150-200 mA at most, the module peaks at 2 A |
| 18 | Resistor kit 1/4 W 1 %, 600 pieces (30 values) | 1 | [thegioiic.com: bộ 600 điện trở](https://thegioiic.com/bo-600-dien-tro-1-4w-1-5-vong-mau) | 39.900 | needs 100k x4, 33k x1, 10k x2, 1k x2; check the kit's value list (not confirmed on the page) |
| 19 | Capacitor 1000 uF 10 V, pack of 5 | 1 | [banlinhkien.com: tụ 1000uF 10V](https://banlinhkien.com/tu-hoa-1000uf-10v-10x13mm-5c-p23358531.html) | 2.500 | at the modem's VCC, not optional: without it, a cell near 3.8 V let the modem's transmit peaks break the UART link and every connection failed (24 Oct 2026). Prefer a low-ESR type (not stated on this page); two in parallel help |
| 20 | Ceramic capacitor 100 nF, pack of 10 | 1 | [banlinhkien.com: tụ gốm 104](https://banlinhkien.com/tu-gom-104-100nf-50v-10c-p6652092.html) | 2.000 | modem VCC and the two ADC pins |
| 22 | Jumper wires 20 cm male-female, 40 pieces | 1 | [hshop.vn: dây cắm đực-cái](https://hshop.vn/day-camtest-board-duc-coi20cm40soi) | 30.000 | use thicker wire (0.5 mm2) from the cell to the modem |
| 23 | Nano SIM with a data plan (Viettel, MobiFone, Vinaphone) | 1 | carrier, e.g. [4g.viettel.vn/data](https://4g.viettel.vn/data) | not read | the camera picks the APN from the SIM ([APN.md](APN.md)) |
| 24 | Prototype board FR4, double-sided, 7 x 9 cm, 2.54 mm | 1 | [Shopee: PCB đục lỗ 2 mặt](https://shopee.vn/PCB-%C4%90%E1%BB%A5c-L%E1%BB%97-FR4-2.54mm-(5x7)-(7x9)-2-M%E1%BA%B7t-i.35490992.58015909114) | 24.000 (7 x 9) | carries the boost board, the dividers, the capacitors and the terminals ([WIRING.md](WIRING.md)) |
| 25 | Screw terminal KF301-2P, 5.08 mm, pack of 10 | 1 | [Shopee: domino KF301](https://shopee.vn/%C4%90%E1%BA%A7u-Terminal-Domino-2P-3P-4P-2-3-4-Ch%C3%A2n-KF301-2P-KF301-3P-KF301-4P-5.08mm-300V-16A-i.1510124588.29331553149) | 18.000 (2P, 10 pieces) | on the prototype board: one for the LiPo, one for the solar panel |

## 3. Totals (prices read on 17, 24 and 25 Oct 2026)

| Build | Parts priced in VND | Plus |
|---|---|---|
| **Field camera in the hanging solar trap** (items 1, 1a, 6c, 8, 10, 12, 14, 15, 16 x1, 17 x2, 18, 19, 20, 24, 25) | **944.760 ₫** | the trap housing US$3.00 (item 3), so about US$39.27 in all (26.050 ₫/USD); SIM and data plan (not read); optional: microSD, the 6 V 2 W panel and an active GPS antenna (items 2, 13, 11) +251.960 ₫. The 4G and GPS modules came with their antennas; no sticky card or lure holder: the housing is the trap |
| **Wi-Fi camera in the same housing** (items 1, 1a) | **419.640 ₫** | the housing US$3.00; a USB charger and cable (not read); optional microSD (item 2) 103.740 ₫ |
| Reference Wi-Fi camera of [WIRING.md](WIRING.md) (items 1, 1a, 2, 3a, 5, 6) | 848.700 ₫ | a USB charger and cable (not read); with the Freenove board instead of items 1 + 1a: 429.060 ₫ + US$19.99 |
| Reference field camera of [WIRING.md](WIRING.md), IP65 box, 5 V boost board (the Wi-Fi items + 8, 9, 10, 11, 12, 13, 14, 15, 16 x1, 17 x2, 18, 19, 20, 22, 24, 25) | 1.563.140 ₫ (848.700 + 714.440) | SIM and data plan (not read) |

The totals take the first option of each row, one pack where parts come in packs, and the prices as shown, including
items that were out of stock that day. The hanging trap that was first built (the photos) had the Freenove board, an
18650 cell and a 5 V boost board on the board's 5V pin (448.954 ₫ of parts plus the board and the housing); this list
keeps the boost board (item 15) and replaces the cell with a flat LiPo (item 12). A 3.3 V regulator on the 3V3 pin was
tried in its place and could not carry the board (item 15).

## 4. International alternatives (prices read on 17 Oct 2026)

| Part | Where | Price read |
|---|---|---|
| LiPo 3.7 V 2000 mAh with JST-PH (Adafruit 2011) | [adafruit.com/product/2011](https://www.adafruit.com/product/2011) | US$12.50 (out of stock) |
| Solar LiPo charger bq24074, takes a 6-10 V panel (Adafruit 4755) | [adafruit.com/product/4755](https://www.adafruit.com/product/4755) | US$14.95 |
| AO3401A P-MOSFET (LCSC C15127) | [lcsc.com C15127](https://www.lcsc.com/product-detail/C15127.html) | US$0.103 each at 5+ |
| MMBT3904 NPN (LCSC C20526) | [lcsc.com C20526](https://www.lcsc.com/product-detail/C20526.html) | US$0.0118 each at 50+ |

## 5. For the bench

A USB-C cable to the board's CH340 port (flashing and the serial console), a multimeter (check the 5 V and 3V3 rails and the
battery reading before closing the box), and for the AI-Thinker ESP32-CAM a USB-serial adapter (CP2102 / CH340, 5 V).
