/* SPDX-License-Identifier: AGPL-3.0-only */
/* Frame-difference motion detection (see motion_core.h, motion.c) */
#include <string.h>
#include <stdlib.h>
#include "motion_core.h"

int motion_pixel_threshold(int s)
{
	if (s < 1) {
		s = 1;
	} else if (s > 100) {
		s = 100;
	}
	return 12 + (100 - s) * 40 / 100; /* grey levels: 12 (most sensitive) .. 51 */
}

int motion_area_threshold(int s)
{
	if (s < 1) {
		s = 1;
	} else if (s > 100) {
		s = 100;
	}
	return 1 + (100 - s) * 30 / 100; /* per mille of the region: 1 .. 30 */
}

#define LIGHTING_PERMILLE 700

motion_result_t motion_compare(motion_model_t *m, const uint8_t *grey, int w, int h, const int roi[4], int sensitivity)
{
	motion_result_t r = { 0 };
	r.threshold = motion_area_threshold(sensitivity);
	if (w <= 0 || h <= 0 || w * h > MOTION_MAX_PIXELS) {
		return r;
	}
	if (!m->have_bg || m->w != w || m->h != h) {
		memcpy(m->bg, grey, (size_t)w * h);
		m->w = w, m->h = h, m->have_bg = true;
		return r;
	}
	int x0 = roi[0] * w / 100, y0 = roi[1] * h / 100;
	int x1 = (roi[0] + roi[2]) * w / 100, y1 = (roi[1] + roi[3]) * h / 100;
	if (x1 <= x0) {
		x1 = x0 + 1;
	}
	if (y1 <= y0) {
		y1 = y0 + 1;
	}
	int thr = motion_pixel_threshold(sensitivity);
	long changed_roi = 0, total_roi = 0, changed_all = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int i = y * w + x;
			bool c = abs((int)grey[i] - (int)m->bg[i]) > thr;
			changed_all += c;
			if (x >= x0 && x < x1 && y >= y0 && y < y1) {
				total_roi++;
				changed_roi += c;
			}
		}
	}
	r.level = total_roi ? (int)(changed_roi * 1000 / total_roi) : 0;
	if (changed_all * 1000 / ((long)w * h) >= LIGHTING_PERMILLE) {
		r.lighting = true;
		memcpy(m->bg, grey, (size_t)w * h); /* a new scene */
		return r;
	}
	r.motion = r.level >= r.threshold;
	for (int i = 0; i < w * h; i++) {
		m->bg[i] = (uint8_t)((m->bg[i] * 7 + grey[i] + 4) / 8);
	}
	return r;
}
