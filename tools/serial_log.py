#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Non-interactive serial helper (pyserial): capture the boot log, or send console commands and capture the answers.

    python tools/serial_log.py COM7 --seconds 60 --reset            # pull DTR/RTS to reset, log 60 s
    python tools/serial_log.py COM7 --cmd info --cmd batt --wait 5   # send commands, wait N s after each
    python tools/serial_log.py COM7 --seconds 20 --out boot.log

The port is closed when the script ends, so idf.py flash / monitor can use it afterwards.
"""
import argparse
import sys
import time

import serial


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # QR block characters on a cp1252 console
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('port')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--seconds', type=float, default=30)
    ap.add_argument('--reset', action='store_true', help='toggle DTR/RTS like esptool (EN low) before reading')
    ap.add_argument('--cmd', action='append', default=[], help='console command to send (repeatable)')
    ap.add_argument('--wait', type=float, default=5, help='seconds to wait after each command')
    ap.add_argument('--out', help='also write the log to this file')
    a = ap.parse_args()
    out = open(a.out, 'w', encoding='utf-8', errors='replace') if a.out else None

    def emit(line):
        print(line, flush=True)
        if out:
            out.write(line + '\n')

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, a.baud, 0.2
    s.dtr = False  # both lines inactive before the port opens: no reset pulse on the CH343 auto-reset circuit
    s.rts = False
    s.open()
    with s:
        if a.reset:
            # CH343 / CP210x auto-reset circuit: EN follows RTS/DTR; this is what esptool does for a hard reset
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
            s.dtr = False
        else:
            s.dtr = False
            s.rts = False
        t0 = time.time()
        buf = b''

        def pump(until):
            nonlocal buf
            while time.time() < until:
                data = s.read(4096)
                if data:
                    buf += data
                    while b'\n' in buf:
                        line, buf = buf.split(b'\n', 1)
                        emit('%7.2f %s' % (time.time() - t0, line.decode('utf-8', 'replace').rstrip('\r')))

        if a.cmd:
            pump(time.time() + 1.0)
            s.write(b'\n')  # wakes the chip from light sleep (that first byte may be lost)
            pump(time.time() + 0.5)
            for c in a.cmd:
                emit('>>> ' + c)
                s.write((c + '\r\n').encode())
                pump(time.time() + a.wait)
        else:
            pump(time.time() + a.seconds)
        if buf:
            emit('%7.2f %s' % (time.time() - t0, buf.decode('utf-8', 'replace')))
    if out:
        out.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
