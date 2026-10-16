#!/usr/bin/env python3
"""
Draws the wiring diagrams in docs/img (SVG). The pin numbers are the firmware's Kconfig defaults for the `freenove`
preset (firmware/main/Kconfig.projbuild, menu "CamOS trap camera"); change both together.

    python tools/diagrams/wiring.py            writes docs/img/wiring-*.svg
PNG copies: open each SVG in Chrome / Edge headless (see docs/dev/BUILD.md, "Diagrams and screenshots").
"""
import os

OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'docs', 'img')
FONT = "Segoe UI, Roboto, Helvetica, Arial, sans-serif"
C = {  # wire colours
    'bat': '#d62828', '3v3': '#f08c00', 'gnd': '#222222', 'uart': '#1c7ed6', 'ctl': '#7048e8', 'adc': '#2b8a3e',
    'sol': '#e8590c', 'ant': '#868e96',
}


class Svg:
    def __init__(self, w, h, title, sub):
        self.w, self.h, self.items = w, h, []
        self.text(24, 36, title, 22, weight=700)
        self.text(24, 60, sub, 13, fill='#5b6672')

    def add(self, s):
        self.items.append(s)

    def text(self, x, y, s, size=13, anchor='start', weight=400, fill='#1f2833', family=FONT):
        s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        self.add(f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{s}</text>')

    def box(self, x, y, w, h, title, sub='', fill='#f8f9fa', stroke='#495057', dash=False):
        d = ' stroke-dasharray="6 4"' if dash else ''
        self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{fill}" stroke="{stroke}" stroke-width="1.6"{d}/>')
        self.text(x + w / 2, y + 22, title, 14, 'middle', 700)
        if sub:
            for i, line in enumerate(sub.split('\n')):
                self.text(x + w / 2, y + 40 + 15 * i, line, 11.5, 'middle', fill='#5b6672')

    def pin(self, x, y, label, side='left', color='#495057'):
        """a pin dot on a box edge, label inside the box"""
        self.add(f'<circle cx="{x}" cy="{y}" r="4" fill="#fff" stroke="{color}" stroke-width="2"/>')
        if side == 'left':
            self.text(x + 9, y + 4, label, 12, 'start', 600)
        else:
            self.text(x - 9, y + 4, label, 12, 'end', 600)

    def wire(self, pts, kind, label=None, at=0, dy=-6, dash=False):
        d = ' stroke-dasharray="7 5"' if dash else ''
        p = ' '.join(f'{x},{y}' for x, y in pts)
        self.add(f'<polyline points="{p}" fill="none" stroke="{C[kind]}" stroke-width="2.6" stroke-linejoin="round" stroke-linecap="round"{d}/>')
        if label:
            (x1, y1), (x2, y2) = pts[at], pts[at + 1]
            self.text((x1 + x2) / 2, (y1 + y2) / 2 + dy, label, 11, 'middle', 600, C[kind])

    def dot(self, x, y, kind):
        self.add(f'<circle cx="{x}" cy="{y}" r="4" fill="{C[kind]}"/>')

    def resistor(self, x, y, label, vertical=True):
        if vertical:
            self.add(f'<rect x="{x - 7}" y="{y - 18}" width="14" height="36" rx="2" fill="#fff" stroke="#495057" stroke-width="1.6"/>')
            self.text(x + 12, y + 4, label, 11.5, 'start', 600)
        else:
            self.add(f'<rect x="{x - 18}" y="{y - 7}" width="36" height="14" rx="2" fill="#fff" stroke="#495057" stroke-width="1.6"/>')
            self.text(x, y - 12, label, 11.5, 'middle', 600)

    def cap(self, x, y, label):
        self.add(f'<line x1="{x - 14}" y1="{y - 4}" x2="{x + 14}" y2="{y - 4}" stroke="#495057" stroke-width="2.4"/>'
                 f'<line x1="{x - 14}" y1="{y + 4}" x2="{x + 14}" y2="{y + 4}" stroke="#495057" stroke-width="2.4"/>')
        self.text(x + 20, y + 4, label, 11.5, 'start', 600)

    def gnd(self, x, y):
        self.add(f'<line x1="{x}" y1="{y}" x2="{x}" y2="{y + 8}" stroke="{C["gnd"]}" stroke-width="2.4"/>'
                 f'<line x1="{x - 10}" y1="{y + 8}" x2="{x + 10}" y2="{y + 8}" stroke="{C["gnd"]}" stroke-width="2.4"/>'
                 f'<line x1="{x - 6}" y1="{y + 12}" x2="{x + 6}" y2="{y + 12}" stroke="{C["gnd"]}" stroke-width="2.4"/>'
                 f'<line x1="{x - 2}" y1="{y + 16}" x2="{x + 2}" y2="{y + 16}" stroke="{C["gnd"]}" stroke-width="2.4"/>')

    def antenna(self, x, y, label):
        self.add(f'<polyline points="{x},{y} {x},{y - 34}" stroke="{C["ant"]}" stroke-width="2.4" fill="none"/>'
                 f'<polyline points="{x - 12},{y - 46} {x},{y - 34} {x + 12},{y - 46}" stroke="{C["ant"]}" stroke-width="2.4" fill="none"/>')
        self.text(x - 18, y - 30, label, 11.5, 'end', 600, '#5b6672')

    def mosfet(self, x, y, label):
        """P-MOSFET symbol box: S on top, D at the bottom, G on the left"""
        self.add(f'<rect x="{x - 26}" y="{y - 26}" width="52" height="52" rx="6" fill="#fff4e6" stroke="#495057" stroke-width="1.6"/>')
        self.text(x, y - 4, 'P-FET', 11, 'middle', 700)
        self.text(x, y + 11, label, 10.5, 'middle', 400, '#5b6672')

    def legend(self, x, y, kinds):
        names = {'bat': 'BAT+ (3.4-4.2 V)', '3v3': '3.3 V', 'gnd': 'GND', 'uart': 'UART data', 'ctl': 'control',
                 'adc': 'ADC sense', 'sol': 'solar input', 'ant': 'antenna'}
        for i, k in enumerate(kinds):
            xx = x + i * 150
            self.add(f'<line x1="{xx}" y1="{y}" x2="{xx + 28}" y2="{y}" stroke="{C[k]}" stroke-width="3"/>')
            self.text(xx + 34, y + 4, names[k], 12)

    def note(self, x, y, lines, size=12):
        for i, s in enumerate(lines):
            self.text(x, y + i * (size + 5), s, size, fill='#495057')

    def save(self, name):
        body = '\n'.join(self.items)
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}">\n'
               f'<rect width="100%" height="100%" fill="#ffffff"/>\n{body}\n</svg>\n')
        with open(os.path.join(OUT, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(svg)
        print('wrote', name)


def board(s, x, y, w, h, left, right, extra):
    s.box(x, y, w, h, 'Freenove ESP32-S3 CAM (FNK0085)', extra, fill='#e6fcf5', stroke='#0ca678')
    pins = {}
    for i, (key, label) in enumerate(left):
        py = y + 110 + i * 40
        s.pin(x, py, label, 'left')
        pins[key] = (x, py)
    for i, (key, label) in enumerate(right):
        py = y + 110 + i * 40
        s.pin(x + w, py, label, 'right')
        pins[key] = (x + w, py)
    return pins


# ------------------------------------------------------------------ overview
def overview():
    s = Svg(1440, 980, 'Trap camera wiring: overview',
            'Freenove ESP32-S3 CAM + A7680C 4G (TDM2309) + ATGM336H GPS + LiPo + solar charger. Pins = firmware defaults (freenove preset).')
    s.legend(24, 92, ['bat', '3v3', 'gnd', 'uart', 'ctl', 'adc', 'sol', 'ant'])
    # power column
    s.box(40, 140, 210, 90, 'Solar panel 6 V 2 W', '136 x 110 mm, tilted to the sun', fill='#fff4e6', stroke='#e8590c')
    s.box(40, 300, 210, 160, 'Charger board', 'TP4056 + DW01 (protected)\nor CN3791 MPPT matched to\nthe panel voltage', fill='#fff9db', stroke='#f08c00')
    s.pin(250, 420, 'OUT+ / BAT+', 'right')
    s.box(40, 580, 210, 110, 'LiPo 1S 2000 mAh', '3.7 V nominal, protected\n(4.2 V full, 3.3 V empty)', fill='#fff5f5', stroke='#d62828')
    s.wire([(145, 230), (145, 300)], 'sol', 'panel + / -', dy=4)
    s.wire([(145, 460), (145, 580)], 'bat', 'B+ / B-', dy=4)
    # BAT+ bus
    s.wire([(250, 420), (290, 420)], 'bat')
    s.dot(290, 420, 'bat')
    s.wire([(290, 500), (290, 120), (1100, 120), (1100, 250), (1150, 250)], 'bat', 'BAT+ to the modem VCC (short, thick wires)', at=1, dy=-6)
    # 3.3 V regulator
    s.box(320, 330, 170, 90, '3.3 V buck-boost', 'TPS63020 module\n1.8-5.5 V in, 3.3 V 1.3 A', fill='#fff9db', stroke='#f08c00')
    s.wire([(290, 375), (320, 375)], 'bat')
    s.dot(290, 375, 'bat')
    # board
    left = [('3v3', '3V3'), ('gnd', 'GND'), ('io1', 'GPIO1  (ADC1_CH0)'), ('io2', 'GPIO2  (ADC1_CH1)')]
    right = [('io41', 'GPIO41  U1 TX'), ('io42', 'GPIO42  U1 RX'), ('io47', 'GPIO47  PWRKEY'), ('gnd2', 'GND'),
             ('io14', 'GPIO14  U2 RX'), ('io21', 'GPIO21  GPS power')]
    p = board(s, 590, 240, 330, 400, left, right,
              'ESP32-S3 N8R8 / N16R8, OV5640 on the FPC\non board: WS2812 LED GPIO48, BOOT button GPIO0,\nmicroSD (CLK 39, CMD 38, D0 40), USB-C CH340')
    s.wire([(490, 375), (545, 375), (545, p['3v3'][1]), p['3v3']], '3v3', '3.3 V', at=0)
    s.dot(545, p['3v3'][1], '3v3')
    s.wire([(405, 420), (405, 470), (560, 470), (560, p['gnd'][1]), p['gnd']], 'gnd')
    s.gnd(405, 480)
    # battery divider -> GPIO1
    s.resistor(330, 540, '100k')
    s.resistor(330, 620, '100k')
    s.wire([(290, 500), (330, 500), (330, 522)], 'bat')
    s.wire([(330, 558), (330, 602)], 'adc')
    s.wire([(330, 638), (330, 660)], 'gnd')
    s.gnd(330, 660)
    s.wire([(330, 580), (575, 580), (575, p['io1'][1]), p['io1']], 'adc', 'battery sense', at=0)
    s.dot(330, 580, 'adc')
    s.cap(400, 605, '100 nF')
    s.wire([(400, 580), (400, 601)], 'adc')
    s.dot(400, 580, 'adc')
    s.wire([(400, 609), (400, 625)], 'gnd')
    s.gnd(400, 625)
    # solar divider -> GPIO2
    s.resistor(470, 200, '100k')
    s.resistor(470, 270, '33k')
    s.wire([(250, 185), (470, 185), (470, 182)], 'sol', 'panel +', at=0)
    s.wire([(470, 218), (470, 252)], 'adc')
    s.wire([(470, 288), (470, 300)], 'gnd')
    s.gnd(470, 300)
    s.wire([(470, 235), (580, 235), (580, p['io2'][1]), p['io2']], 'adc', 'solar sense', at=0)
    s.dot(470, 235, 'adc')
    # modem
    mx, my = 1150, 170
    s.box(mx, my, 250, 230, 'A7680C 4G (TDM2309)', 'SIM800L-compatible pinout\n3.8-4.2 V, 3.3 / 5 V logic', fill='#e7f5ff', stroke='#1c7ed6')
    mp = {'vcc': (mx, 250), 'gnd': (mx, 280), 'rxd': (mx, 310), 'txd': (mx, 340), 'rst': (mx, 370)}
    for k, lab in [('vcc', 'VCC'), ('gnd', 'GND'), ('rxd', 'RXD'), ('txd', 'TXD'), ('rst', 'PWRKEY / RST *')]:
        s.pin(*mp[k], lab, 'left')
    s.antenna(mx + 220, my, 'LTE antenna (IPEX / SMA)')
    s.cap(1050, 215, '')
    s.text(1040, 200, '1000 uF + 100 nF', 11.5, 'end', 600)
    s.wire([(1100, 160), (1050, 160), (1050, 211)], 'bat')
    s.dot(1100, 160, 'bat')
    s.wire([(1050, 219), (1050, 232)], 'gnd')
    s.gnd(1050, 232)
    s.wire([p['io41'], (1000, p['io41'][1]), (1000, mp['rxd'][1]), mp['rxd']], 'uart', 'TX -> RXD', at=0)
    s.wire([p['io42'], (1015, p['io42'][1]), (1015, mp['txd'][1]), mp['txd']], 'uart', 'RX <- TXD', at=0)
    # common ground bus
    s.wire([(1085, p['gnd2'][1]), (1085, mp['gnd'][1]), mp['gnd']], 'gnd')
    s.wire([p['gnd2'], (1085, p['gnd2'][1])], 'gnd')
    # PWRKEY NPN
    y47 = p['io47'][1]
    s.resistor(970, y47, '1k', vertical=False)
    s.wire([p['io47'], (952, y47)], 'ctl')
    s.add(f'<rect x="1030" y="{y47 - 22}" width="44" height="44" rx="6" fill="#f3f0ff" stroke="#495057" stroke-width="1.6"/>')
    s.text(1052, y47 + 4, 'NPN', 11, 'middle', 700)
    s.wire([(988, y47), (1030, y47)], 'ctl')
    s.wire([(1052, y47 - 22), (1052, 388), (1130, 388), (1130, mp['rst'][1]), mp['rst']], 'ctl', 'collector', at=1, dy=-5)
    s.wire([(1052, y47 + 22), (1052, p['gnd2'][1])], 'gnd')
    s.dot(1052, p['gnd2'][1], 'gnd')
    # GPS
    gx, gy = 1150, 600
    s.box(gx, gy, 250, 220, 'ATGM336H GPS', 'GPS + BeiDou, NMEA 9600 baud\n3.3 V, about 25 mA', fill='#ebfbee', stroke='#2b8a3e')
    gp = {'tx': (gx, 690), 'rx': (gx, 720), 'gnd': (gx, 750), 'vcc': (gx, 780)}
    for k, lab in [('tx', 'TX'), ('rx', 'RX (leave open)'), ('gnd', 'GND'), ('vcc', 'VCC')]:
        s.pin(*gp[k], lab, 'left')
    s.antenna(gx + 220, gy, 'active GPS antenna (IPEX)')
    s.wire([p['io14'], (990, p['io14'][1]), (990, gp['tx'][1]), gp['tx']], 'uart', 'RX <- TX', at=0)
    s.wire([(1085, p['gnd2'][1]), (1085, gp['gnd'][1]), gp['gnd']], 'gnd')
    s.dot(1085, p['gnd2'][1], 'gnd')
    # GPS switch: source left, drain right, gate on top
    fx, fy = 1040, 880
    s.add(f'<rect x="{fx - 30}" y="{fy - 24}" width="60" height="48" rx="6" fill="#fff4e6" stroke="#495057" stroke-width="1.6"/>')
    s.text(fx, fy - 2, 'P-FET', 11, 'middle', 700)
    s.text(fx, fy + 13, 'AO3401', 10.5, 'middle', 400, '#5b6672')
    s.text(fx - 38, fy + 18, 'S', 11, 'middle', 700)
    s.text(fx + 38, fy + 18, 'D', 11, 'middle', 700)
    s.text(fx + 8, fy - 30, 'G', 11, 'start', 700)
    s.wire([(545, p['3v3'][1]), (545, fy), (fx - 30, fy)], '3v3', '3.3 V to the switch (source)', at=1, dy=-6)
    s.wire([(fx + 30, fy), (1120, fy), (1120, gp['vcc'][1]), gp['vcc']], '3v3', 'drain', at=0, dy=-6)
    y21 = p['io21'][1]
    s.resistor(970, y21, '1k', vertical=False)
    s.wire([p['io21'], (952, y21)], 'ctl')
    s.wire([(988, y21), (1040, y21), (1040, fy - 24)], 'ctl', 'gate', at=1, dy=70)
    s.resistor(960, 820, '')
    s.text(948, 824, '100k', 11.5, 'end', 600)
    s.wire([(960, 802), (960, 790), (1040, 790)], 'ctl')
    s.dot(1040, 790, 'ctl')
    s.wire([(960, 838), (960, fy)], '3v3')
    s.dot(960, fy, '3v3')
    s.note(40, 730, ['* The TDM2309 keeps the SIM800L pinout: wire the NPN collector to',
                     '  its PWRKEY pin (or RST if that is all it has). Without the NPN,',
                     '  set CAMOS_MODEM_PWRKEY_ACTIVE_HIGH=n.',
                     'Common ground for every board. Feed the camera board at 3V3 from',
                     'the buck-boost, not 5 V. Optional: charger CHRG pin to GPIO46.'])
    s.save('wiring-overview.svg')


# ------------------------------------------------------------------ 4G
def modem():
    s = Svg(1100, 600, 'A7680C 4G (TDM2309) to the Freenove board',
            'UART1 at 115200 8N1. Kconfig: CAMOS_MODEM_TX_GPIO=41, RX=42, PWRKEY=47 (active high through an NPN), POWER_GPIO=-1.')
    s.legend(24, 92, ['bat', 'gnd', 'uart', 'ctl', 'ant'])
    left = [('io41', 'GPIO41  TX'), ('io42', 'GPIO42  RX'), ('io47', 'GPIO47'), ('gnd', 'GND')]
    s.box(60, 140, 300, 320, 'Freenove ESP32-S3 CAM', 'header pins', fill='#e6fcf5', stroke='#0ca678')
    bp = {}
    for i, (k, lab) in enumerate(left):
        y = 250 + i * 50
        s.pin(360, y, lab, 'right')
        bp[k] = (360, y)
    mx, my = 720, 140
    s.box(mx, my, 300, 320, 'A7680C TDM2309', '3.8-4.2 V, 3.3 V / 5 V logic\n(no level shifter needed)', fill='#e7f5ff', stroke='#1c7ed6')
    mp = {}
    for i, (k, lab) in enumerate([('rxd', 'RXD'), ('txd', 'TXD'), ('rst', 'PWRKEY / RST *'), ('gnd', 'GND'), ('vcc', 'VCC')]):
        y = 250 + i * 50
        s.pin(mx, y, lab, 'left')
        mp[k] = (mx, y)
    s.antenna(mx + 250, my, 'LTE 700-2700 MHz (IPEX or SMA)')
    s.wire([bp['io41'], mp['rxd']], 'uart', 'ESP TX -> modem RXD')
    s.wire([bp['io42'], mp['txd']], 'uart', 'ESP RX <- modem TXD')
    s.resistor(430, bp['io47'][1], '1k', vertical=False)
    s.wire([bp['io47'], (412, bp['io47'][1])], 'ctl')
    s.add(f'<rect x="520" y="{bp["io47"][1] - 24}" width="48" height="48" rx="6" fill="#f3f0ff" stroke="#495057" stroke-width="1.6"/>')
    s.text(544, bp['io47'][1] + 4, 'NPN', 11, 'middle', 700)
    s.text(544, bp['io47'][1] - 32, 'MMBT3904 / S8050', 10.5, 'middle', 400, '#5b6672')
    s.wire([(448, bp['io47'][1]), (520, bp['io47'][1])], 'ctl', 'base')
    s.wire([(568, bp['io47'][1]), mp['rst']], 'ctl', 'collector')
    s.wire([bp['gnd'], (544, bp['gnd'][1]), mp['gnd']], 'gnd', 'GND (also the NPN emitter)')
    s.wire([(544, bp['io47'][1] + 24), (544, bp['gnd'][1])], 'gnd')
    s.dot(544, bp['gnd'][1], 'gnd')
    s.wire([(140, 560), (640, 560), (640, mp['vcc'][1]), mp['vcc']], 'bat', 'BAT+ from the LiPo / charger output (not 3.3 V)', at=0)
    s.cap(600, 520, '1000 uF low-ESR + 100 nF')
    s.wire([(640, 505), (600, 505), (600, 516)], 'bat')
    s.wire([(600, 524), (600, 538)], 'gnd')
    s.gnd(600, 538)
    s.note(60, 500, ['* SIM800L-compatible boards often have only RST (reset).',
                     '  The firmware pulses 1.5 s to start and uses AT+CPOF to stop.'], 11.5)
    s.save('wiring-4g.svg')


# ------------------------------------------------------------------ GPS
def gps():
    s = Svg(1100, 580, 'ATGM336H GPS to the Freenove board',
            'UART2 RX only at 9600 8N1. Kconfig: CAMOS_GPS_RX_GPIO=14, TX=-1, POWER_GPIO=21 (P-MOSFET, low = on).')
    s.legend(24, 92, ['3v3', 'gnd', 'uart', 'ctl', 'ant'])
    s.box(60, 140, 300, 330, 'Freenove ESP32-S3 CAM', 'header pins', fill='#e6fcf5', stroke='#0ca678')
    bp = {}
    for k, lab, y in [('3v3', '3V3', 240), ('io21', 'GPIO21', 300), ('io14', 'GPIO14  RX', 360), ('gnd', 'GND', 420)]:
        s.pin(360, y, lab, 'right')
        bp[k] = (360, y)
    gx = 760
    s.box(gx, 140, 280, 330, 'ATGM336H module', 'VCC 3.3 V (many boards take 3.3-5 V)\nabout 25 mA while searching', fill='#ebfbee', stroke='#2b8a3e')
    gp = {}
    for k, lab, y in [('vcc', 'VCC', 240), ('tx', 'TX', 300), ('rx', 'RX (leave open)', 340), ('gnd', 'GND', 380), ('pps', 'PPS (not used)', 420)]:
        s.pin(gx, y, lab, 'left')
        gp[k] = (gx, y)
    s.antenna(gx + 230, 140, 'active patch antenna, IPEX')
    s.mosfet(600, 260, 'AO3401')
    s.text(612, 228, 'S', 11, 'start', 700)
    s.text(612, 300, 'D', 11, 'start', 700)
    s.text(566, 254, 'G', 11, 'end', 700)
    s.wire([bp['3v3'], (420, 240), (420, 190), (600, 190), (600, 234)], '3v3', 'source', at=2)
    s.resistor(520, 225, '')
    s.text(508, 229, '100k', 11.5, 'end', 600)
    s.wire([(520, 190), (520, 207)], '3v3')
    s.dot(520, 190, '3v3')
    s.wire([(520, 243), (520, 260)], 'ctl')
    s.resistor(440, 300, '1k', vertical=False)
    s.wire([bp['io21'], (422, 300)], 'ctl')
    s.wire([(458, 300), (520, 300), (520, 260), (574, 260)], 'ctl', 'gate', at=0, dy=-6)
    s.dot(520, 260, 'ctl')
    s.wire([(600, 286), (600, 320), (700, 320), (700, 240), gp['vcc']], '3v3', 'drain', at=1, dy=14)
    s.wire([bp['io14'], (720, 360), (720, 300), gp['tx']], 'uart', 'ESP RX <- GPS TX', at=0)
    s.wire([bp['gnd'], (740, 420), (740, 380), gp['gnd']], 'gnd', 'GND', at=0)
    s.note(60, 505, ['The 100k from gate to source keeps the GPS off while the ESP32 resets; GPIO21 low switches it on.',
                     'Keep the antenna face up, 10 cm from the LTE antenna and the panel. No backup cell on V_BCKP: every fix is a cold start',
                     '(about 35 s in the open, datasheet value).'])
    s.save('wiring-gps.svg')


# ------------------------------------------------------------------ power
def power():
    s = Svg(1200, 640, 'Power: solar -> charger -> LiPo -> rails',
            'BAT+ feeds the modem directly and the 3.3 V buck-boost for the camera board and the GPS. Kconfig: BATT_ADC_GPIO=1 (100k/100k), SOLAR_ADC_GPIO=2 (100k/33k).')
    s.legend(24, 92, ['sol', 'bat', '3v3', 'gnd', 'adc'])
    s.box(40, 150, 200, 90, 'Solar panel', '6 V 2 W (136 x 110 mm)', fill='#fff4e6', stroke='#e8590c')
    s.box(320, 140, 230, 110, 'Charger', 'TP4056 + DW01 (protected)\nor CN3791 MPPT for the panel', fill='#fff9db', stroke='#f08c00')
    s.box(320, 380, 230, 110, 'LiPo 1S 2000 mAh', '103450 pouch or 18650\n3.3-4.2 V', fill='#fff5f5', stroke='#d62828')
    s.box(680, 140, 220, 100, '3.3 V buck-boost', 'TPS63020: 1.8-5.5 V in\n3.3 V up to 1.3 A', fill='#fff9db', stroke='#f08c00')
    s.box(980, 120, 190, 70, 'Camera board 3V3', 'ESP32-S3 + OV5640', fill='#e6fcf5', stroke='#0ca678')
    s.box(980, 220, 190, 70, 'GPS VCC', 'through the AO3401 (GPIO21)', fill='#ebfbee', stroke='#2b8a3e')
    s.box(980, 360, 190, 90, 'A7680C VCC', '3.8-4.2 V, peaks 2 A\n1000 uF + 100 nF at the pins', fill='#e7f5ff', stroke='#1c7ed6')
    s.wire([(240, 195), (320, 195)], 'sol', 'IN+')
    s.wire([(435, 250), (435, 380)], 'bat', 'BAT+ / B-', dy=4)
    s.wire([(550, 195), (680, 195)], 'bat', 'OUT+ (= BAT+)')
    s.wire([(615, 195), (615, 405), (980, 405)], 'bat', 'BAT+ (short, thick: 0.5 mm2)', at=1)
    s.dot(615, 195, 'bat')
    s.wire([(900, 190), (940, 190), (940, 155), (980, 155)], '3v3', '3.3 V', at=0)
    s.wire([(940, 190), (940, 255), (980, 255)], '3v3')
    s.dot(940, 190, '3v3')
    # dividers
    s.resistor(160, 330, '100k')
    s.resistor(160, 410, '33k')
    s.wire([(140, 240), (140, 290), (160, 290), (160, 312)], 'sol')
    s.wire([(160, 348), (160, 392)], 'adc')
    s.wire([(160, 428), (160, 445)], 'gnd')
    s.gnd(160, 445)
    s.wire([(160, 370), (60, 370)], 'adc', 'to GPIO2', at=0)
    s.dot(160, 370, 'adc')
    s.resistor(760, 470, '100k')
    s.resistor(760, 550, '100k')
    s.wire([(615, 405), (615, 430), (760, 430), (760, 452)], 'bat')
    s.wire([(760, 488), (760, 532)], 'adc')
    s.wire([(760, 568), (760, 585)], 'gnd')
    s.gnd(760, 585)
    s.wire([(760, 510), (900, 510)], 'adc', 'to GPIO1 (+100 nF to GND)', at=0)
    s.dot(760, 510, 'adc')
    s.note(40, 520, ['GPIO2 sees 6.5 V x 33/133 = 1.6 V; GPIO1 sees 4.2 V / 2 = 2.1 V',
                     '(both inside the 12 dB ADC range). Dividers draw about 21 uA and 49 uA.',
                     'Turn on "Battery / solar divider fitted" in the portal once they are wired.',
                     'The TDM2309 is specified from 3.8 V: below that the cell is near empty anyway.'], 12)
    s.save('wiring-power.svg')


# ------------------------------------------------------------------ illustration of the mounted trap (not a photo)
def trap():
    s = Svg(1100, 700, 'Illustration: the trap camera on its post (side view)',
            'Drawing, not a photo. The camera looks at the sticky card from 15-25 cm, square to the card, shaded from the sun.')
    a = s.add
    a('<rect x="0" y="640" width="1100" height="60" fill="#d8e8c8"/>')
    a('<rect x="520" y="130" width="22" height="520" fill="#a5734a"/>')          # post
    a('<rect x="380" y="150" width="300" height="14" rx="3" fill="#868e96"/>')  # rain roof bar
    a('<polygon points="360,150 700,150 690,132 370,132" fill="#adb5bd"/>')     # roof
    # solar panel tilted on the roof
    a('<polygon points="560,128 700,70 712,92 572,150" fill="#1c3d6e" stroke="#0b1e3a" stroke-width="2"/>')
    s.text(720, 74, 'solar panel 6 V 2 W, tilted ~15 deg', 12.5, 'start', 600)
    s.text(720, 92, 'facing the equator (south in Vietnam)', 12, 'start', 400, '#5b6672')
    # camera box on an arm, left of the post
    a('<rect x="430" y="300" width="90" height="12" fill="#868e96"/>')          # arm
    a('<rect x="300" y="250" width="130" height="110" rx="10" fill="#e9ecef" stroke="#495057" stroke-width="2"/>')
    a('<rect x="296" y="285" width="10" height="40" rx="2" fill="#a5d8ff" stroke="#1c7ed6" stroke-width="1.5"/>')  # window
    a('<rect x="290" y="270" width="20" height="8" fill="#868e96"/>')           # small hood over the window
    s.text(365, 300, 'IP65 box', 12, 'middle', 700)
    s.text(365, 316, 'camera board,', 11, 'middle', 400, '#5b6672')
    s.text(365, 330, '4G, GPS, LiPo', 11, 'middle', 400, '#5b6672')
    a('<line x1="410" y1="250" x2="410" y2="205" stroke="#495057" stroke-width="3"/>')  # LTE antenna stub
    s.text(415, 200, 'LTE antenna', 11.5, 'start', 600, '#5b6672')
    a('<line x1="365" y1="360" x2="365" y2="420" stroke="#e8590c" stroke-width="2.5" stroke-dasharray="5 4"/>')
    s.text(358, 432, 'solar cable through a gland (bottom)', 11.5, 'end', 400, '#5b6672')
    # sticky card + lure holder hanging left
    a('<line x1="140" y1="150" x2="380" y2="150" stroke="#495057" stroke-width="3"/>')  # card arm
    a('<line x1="150" y1="150" x2="150" y2="210" stroke="#495057" stroke-width="1.5"/>')
    a('<rect x="140" y="210" width="22" height="200" fill="#f4dc12" stroke="#c9a800" stroke-width="2"/>')  # card edge-on
    s.text(60, 440, 'yellow sticky card 15 x 25 cm', 12.5, 'start', 600)
    s.text(60, 458, '(trap holder + lure below or beside it)', 12, 'start', 400, '#5b6672')
    # field of view
    a('<polygon points="296,305 162,215 162,405" fill="#a5d8ff" fill-opacity=".35" stroke="#1c7ed6" stroke-dasharray="6 4"/>')
    a('<line x1="162" y1="520" x2="296" y2="520" stroke="#495057" stroke-width="1.5"/>')
    a('<line x1="162" y1="512" x2="162" y2="528" stroke="#495057" stroke-width="1.5"/><line x1="296" y1="512" x2="296" y2="528" stroke="#495057" stroke-width="1.5"/>')
    s.text(229, 545, '15-25 cm', 12.5, 'middle', 700)
    s.text(229, 563, 'lens centred on the card', 12, 'middle', 400, '#5b6672')
    s.text(531, 670, 'post 1.2-1.5 m, in the canopy shade', 12.5, 'middle', 600)
    s.text(1080, 690, 'Illustration', 13, 'end', 700, '#d62828')
    s.save('trap-illustration.svg')


if __name__ == '__main__':
    overview()
    modem()
    gps()
    power()
    trap()
