#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A baseline JPEG to 8-bit grey, scaled down by 1, 2, 4 or 8 (at 8 only the DC term of each block is decoded: the
 * block's average). out holds *w x *h bytes, row after row; false when the image does not decode or does not fit
 * in out_cap bytes. */
bool jpegdec_grey(const uint8_t *jpg, size_t len, int scale_div, uint8_t *out, size_t out_cap, int *w, int *h);

#ifdef __cplusplus
}
#endif
