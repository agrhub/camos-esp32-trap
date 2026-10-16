#!/usr/bin/env python3
"""Print the label QR of a camera (protocol 12.1): CAMOS:1;SN:<serial>;VC:<verify code>;M:<model>;;

    python tools/label.py ESP000000000002 K7P4QX [AIC-FREENOVE-S3CAM] [-o label.png]

The serial and verify code are in the boot log ("serial ..., verify code ..."), on the portal page and in the console
`info`. Needs `pip install qrcode[pil]` for the PNG; without it the QR is printed as text in the terminal (and the plain
string, which FlyBot's "add camera" screen also accepts typed by hand).
"""
import sys


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('-')]
    out = None
    if '-o' in argv:
        out = argv[argv.index('-o') + 1]
        args = [a for a in args if a != out]
    if len(args) < 2:
        print(__doc__)
        return 2
    serial, code = args[0], args[1]
    model = args[2] if len(args) > 2 else 'AIC-FREENOVE-S3CAM'
    text = f'CAMOS:1;SN:{serial};VC:{code};M:{model};;'
    print(text)
    try:
        import qrcode
    except ImportError:
        print('pip install qrcode[pil] for a QR image; the text above is the label content')
        return 0
    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_M, border=2)
    qr.add_data(text)
    qr.make(fit=True)
    if out:
        qr.make_image().save(out)
        print(f'wrote {out}')
    else:
        qr.print_ascii(invert=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
