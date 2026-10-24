#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""
Draws the illustration of the mounted trap, docs/img/trap-illustration.svg. The wiring drawing,
docs/img/wiring-overview.svg, is the drawing of the field-camera wiring sheet and is edited as SVG (its pins are the
firmware's defaults: change both together).

    python tools/diagrams/wiring.py            writes docs/img/trap-illustration.svg
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
    trap()
