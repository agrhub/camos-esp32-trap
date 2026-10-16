/* Frame-difference motion detection, pure C (no ESP-IDF) so it is unit-tested on the host (tests/test_motion.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MOTION_MAX_PIXELS (200 * 150) /* 1600x1200 decoded at 1/8 */

typedef struct {
	int w, h;
	bool have_bg;
	uint8_t bg[MOTION_MAX_PIXELS]; /* running average of the scene (grey) */
} motion_model_t;

typedef struct {
	int level;      /* changed pixels inside the region, per mille */
	int threshold;  /* per mille needed for motion at this sensitivity */
	bool motion;
	bool lighting;  /* most of the picture changed at once (exposure, lights): ignored, background reset */
} motion_result_t;

/* Pixel and area thresholds for a sensitivity 1..100 (100 = most sensitive) */
int motion_pixel_threshold(int sensitivity);
int motion_area_threshold(int sensitivity);

/* Compare a grey picture (w x h, w*h <= MOTION_MAX_PIXELS) with the background inside roi = {x, y, w, h} in percent,
 * then blend it into the background. The first picture (or a new size) only sets the background. */
motion_result_t motion_compare(motion_model_t *m, const uint8_t *grey, int w, int h, const int roi[4], int sensitivity);
