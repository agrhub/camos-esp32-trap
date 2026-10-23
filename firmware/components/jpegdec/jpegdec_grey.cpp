#include <new>
#include <string.h>
#include "JPEGDEC.h"
#include "jpegdec_grey.h"

namespace {

struct target {
    uint8_t *out;
    int w, h;
};

// JPEGDEC hands over the picture in blocks (an MCU row at most): copy each into the grey image
int draw(JPEGDRAW *d)
{
    target *t = static_cast<target *>(d->pUser);
    const uint8_t *src = reinterpret_cast<const uint8_t *>(d->pPixels);
    for (int y = 0; y < d->iHeight && d->y + y < t->h; y++) {
        int n = d->iWidthUsed;
        if (d->x + n > t->w) {
            n = t->w - d->x;
        }
        if (n > 0) {
            memcpy(t->out + (d->y + y) * t->w + d->x, src + y * d->iWidth, n);
        }
    }
    return 1;
}

} // namespace

extern "C" bool jpegdec_grey(const uint8_t *jpg, size_t len, int scale_div, uint8_t *out, size_t out_cap, int *w, int *h)
{
    int opt = scale_div == 8 ? JPEG_SCALE_EIGHTH : scale_div == 4 ? JPEG_SCALE_QUARTER : scale_div == 2 ? JPEG_SCALE_HALF : 0;
    int div = opt ? scale_div : 1;
    JPEGDEC *dec = new (std::nothrow) JPEGDEC; // about 20 KB: PSRAM, as any allocation of 256 bytes and more
    if (!dec) {
        return false;
    }
    bool ok = false;
    if (dec->openRAM(const_cast<uint8_t *>(jpg), (int)len, draw)) {
        target t = { out, (dec->getWidth() + div - 1) / div, (dec->getHeight() + div - 1) / div };
        if ((size_t)t.w * t.h <= out_cap) {
            dec->setPixelType(EIGHT_BIT_GRAYSCALE);
            dec->setUserPointer(&t);
            ok = dec->decode(0, 0, opt) == 1;
            *w = t.w;
            *h = t.h;
        }
        dec->close();
    }
    delete dec;
    return ok;
}
