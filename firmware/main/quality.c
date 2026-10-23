/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Image quality of each photo, measured on the camera before the upload and sent in the snapshot meta as `quality`
 * (docs/dev/PROTOCOL_ADDITIONS.md). The numbers are FlyBot's (server/src/services/imageQuality.js): the photo as a
 * grey copy about 320 wide, then
 *   brightness  mean luminance, 0-255
 *   contrast    standard deviation of the luminance
 *   sharpness   variance of the Laplacian (edges); blur flattens it
 *   edgeRatio   sharpness / contrast^2: edges per unit of contrast (focused traps 0.19 and up, a blurry lens 0.01-0.04)
 *   clipped     share of pure-white pixels
 * and `change`, the per mille of a 40x30 thumbnail that differs from the previous photo's after the brightness shift
 * is taken out: a new insect, a moved or replaced card. The thumbnail lives in RTC memory, so deep sleep keeps it;
 * after a power-on there is nothing to compare with and `change` is left out.
 *
 * A JPEG decoded at 1/8 is the average of each 8x8 block (its DC term), the same box average the server takes. The
 * decoder is JPEGDEC (components/jpegdec), straight to grey: 2560x1920 took 800 ms with esp_jpeg (to RGB888, then grey).
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "jpegdec_grey.h"
#include "camos.h"

static const char *TAG = "quality";

#define MAX_W 360
#define MAX_H 360
#define THUMB_W 40
#define THUMB_H 30
#define CHANGE_LEVEL 16 /* a thumbnail cell counts as changed past this many grey levels */
#define THUMB_MAGIC 0x51544831

static RTC_DATA_ATTR uint32_t thumb_magic;
static RTC_DATA_ATTR uint8_t thumb[THUMB_W * THUMB_H];

static int scale_for(int w)
{
	return w > 1280 ? 8 : w > 640 ? 4 : w > 320 ? 2 : 1;
}

bool photo_quality(const uint8_t *jpg, size_t len, int w, photo_quality_t *q)
{
	memset(q, 0, sizeof(*q));
	int64_t t0 = esp_timer_get_time();
	uint8_t *g = heap_caps_malloc(MAX_W * MAX_H, MALLOC_CAP_SPIRAM);
	if (!g) {
		return false;
	}
	int gw = 0, gh = 0;
	bool ok = jpegdec_grey(jpg, len, scale_for(w), g, MAX_W * MAX_H, &gw, &gh);
	int n = gw * gh;
	if (!ok || n < 64) {
		ESP_LOGW(TAG, "decode failed (%dx%d)", gw, gh);
		free(g);
		return false;
	}
	int64_t t1 = esp_timer_get_time();
	uint64_t sum = 0, sum2 = 0;
	int clipped = 0;
	for (int i = 0; i < n; i++) {
		uint8_t y = g[i];
		sum += y;
		sum2 += (uint32_t)y * y;
		clipped += y >= 250;
	}
	double mean = (double)sum / n;
	double var = (double)sum2 / n - mean * mean;
	int64_t lsum = 0, lsum2 = 0;
	int lcount = 0;
	for (int y = 1; y < gh - 1; y++) {
		const uint8_t *r = g + y * gw;
		for (int x = 1; x < gw - 1; x++) {
			int lap = r[x - 1] + r[x + 1] + r[x - gw] + r[x + gw] - 4 * r[x];
			lsum += lap;
			lsum2 += lap * lap;
			lcount++;
		}
	}
	double lmean = lcount ? (double)lsum / lcount : 0;
	q->brightness = (float)mean;
	q->contrast = (float)sqrt(var > 0 ? var : 0);
	q->sharpness = (float)(lcount ? (double)lsum2 / lcount - lmean * lmean : 0);
	q->edge_ratio = q->contrast > 0 ? q->sharpness / (q->contrast * q->contrast) : 0;
	q->clipped = (float)clipped / n;

	/* 40x30 thumbnail (box average) and its change from the last photo, brightness shift removed */
	uint8_t now[THUMB_W * THUMB_H];
	int now_sum = 0, old_sum = 0;
	for (int ty = 0; ty < THUMB_H; ty++) {
		for (int tx = 0; tx < THUMB_W; tx++) {
			int x0 = tx * gw / THUMB_W, x1 = (tx + 1) * gw / THUMB_W, y0 = ty * gh / THUMB_H, y1 = (ty + 1) * gh / THUMB_H;
			int s = 0, c = 0;
			for (int y = y0; y < y1; y++) {
				for (int x = x0; x < x1; x++) {
					s += g[y * gw + x];
					c++;
				}
			}
			now[ty * THUMB_W + tx] = c ? s / c : 0;
			now_sum += now[ty * THUMB_W + tx];
			old_sum += thumb[ty * THUMB_W + tx];
		}
	}
	q->change = -1;
	if (thumb_magic == THUMB_MAGIC) {
		int shift = (now_sum - old_sum) / (THUMB_W * THUMB_H), changed = 0;
		for (int i = 0; i < THUMB_W * THUMB_H; i++) {
			changed += abs(now[i] - thumb[i] - shift) > CHANGE_LEVEL;
		}
		q->change = changed * 1000 / (THUMB_W * THUMB_H);
	}
	memcpy(thumb, now, sizeof(thumb));
	thumb_magic = THUMB_MAGIC;
	free(g);
	q->decode_ms = (int)((t1 - t0) / 1000);
	q->ms = (int)((esp_timer_get_time() - t0) / 1000);
	ESP_LOGI(TAG, "%dx%d: brightness %.0f, contrast %.1f, sharpness %.0f, edge ratio %.3f, clipped %.3f, change %d, "
			 "%d ms (decode %d)", gw, gh, q->brightness, q->contrast, q->sharpness, q->edge_ratio, q->clipped, q->change,
			 q->ms, q->decode_ms);
	return true;
}

void photo_quality_json(cJSON *parent, const photo_quality_t *q)
{
	cJSON *o = cJSON_AddObjectToObject(parent, "quality");
	cJSON_AddNumberToObject(o, "brightness", roundf(q->brightness * 10) / 10);
	cJSON_AddNumberToObject(o, "contrast", roundf(q->contrast * 10) / 10);
	cJSON_AddNumberToObject(o, "sharpness", roundf(q->sharpness * 10) / 10);
	cJSON_AddNumberToObject(o, "edgeRatio", roundf(q->edge_ratio * 1000) / 1000);
	cJSON_AddNumberToObject(o, "clipped", roundf(q->clipped * 1000) / 1000);
	if (q->change >= 0) {
		cJSON_AddNumberToObject(o, "change", q->change);
	}
	cJSON_AddNumberToObject(o, "ms", q->ms);
}
