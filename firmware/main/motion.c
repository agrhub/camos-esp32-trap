/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Motion detection on the camera (web console Event > Motion, docs/WEB_CONSOLE.md): frame difference against a running
 * average of the scene, on a small grey copy of the preview picture (JPEG decoded at 1/4 or 1/8 scale), inside a region
 * of interest drawn on the live picture. Runs only while enabled and the effective power mode is powered (the camera
 * then stays on at QVGA, or at the size a live viewer picked). On motion, at most once per cooldown: the picture goes to
 * the microSD card (motionSd), a protocol event `motion` is published, and with motionUpload a full-size photo is
 * captured and uploaded to FlyBot (trigger rpc in the CONTRACTS section 2 meta; the event says motion).
 * No person / object classification: the ESP32-S3 has no NPU; the counting stays in the cloud.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "camos.h"
#include "motion_core.h"

static const char *TAG = "motion";
#define PERIOD_MS 400

static TaskHandle_t task;
static motion_model_t *model;
static uint8_t *rgb, *grey;
static volatile bool running;
static volatile int last_level, last_threshold;
static volatile uint32_t count;
static volatile int64_t last_trigger_us;
static volatile time_t last_at;
static int roi[4] = { 0, 0, 100, 100 };

static bool wanted(void)
{
	return g_cfg.motion && power_effective() == POWER_POWERED && !live_streaming() && !ota_running();
}

void motion_config_changed(void)
{
	int r[4];
	if (roi_parse(g_cfg.motion_roi, r)) {
		memcpy(roi, r, sizeof(roi));
	}
	if (model) {
		model->have_bg = false; /* new region or sensitivity: learn the scene again */
	}
	if (task) {
		xTaskNotifyGive(task);
	}
}

/* JPEG -> grey at a scale that gives 80..200 pixels across */
static bool decode_grey(const camera_fb_t *fb, int fw, int *gw, int *gh)
{
	esp_jpeg_image_scale_t sc = fw <= 320 ? JPEG_IMAGE_SCALE_1_4 : JPEG_IMAGE_SCALE_1_8;
	esp_jpeg_image_cfg_t cfg = {
		.indata = fb->buf,
		.indata_size = fb->len,
		.outbuf = rgb,
		.outbuf_size = MOTION_MAX_PIXELS * 3,
		.out_format = JPEG_IMAGE_FORMAT_RGB888,
		.out_scale = sc,
	};
	esp_jpeg_image_output_t out = { 0 };
	if (esp_jpeg_decode(&cfg, &out) != ESP_OK || out.width * out.height > MOTION_MAX_PIXELS) {
		return false;
	}
	int n = out.width * out.height;
	for (int i = 0; i < n; i++) {
		const uint8_t *p = rgb + 3 * i;
		grey[i] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
	}
	*gw = out.width, *gh = out.height;
	return true;
}

/* Capture + upload of a motion trigger (motion.upload): a TLS upload needs more stack than the motion task has
 * (6 KB overflowed and restarted the camera), and a 12 KB task of its own often found no internal block that large
 * on Wi-Fi ("no memory for the motion photo upload", 23 Oct 2026). The scheduler's task, which has that stack and
 * waits most of the time, runs it (sched_run), one at a time. */
static volatile bool uploading;

static void upload_job(void)
{
	upload_t up;
	char cid[27];
	cJSON *c = evt_new("capture", cid);
	cJSON_AddStringToObject(c, "trigger", "motion");
	if (capture_now("motion", &up) == ESP_OK) {
		cJSON *img = cJSON_AddObjectToObject(c, "image");
		cJSON_AddStringToObject(img, "key", up.key);
		cJSON_AddNumberToObject(img, "w", up.w);
		cJSON_AddNumberToObject(img, "h", up.h);
		cJSON_AddNumberToObject(img, "bytes", (double)up.bytes);
		cJSON_AddStringToObject(c, "snapshotId", up.key);
	} else {
		cJSON_AddNullToObject(c, "image");
		cJSON_AddStringToObject(c, "imageError", up.error[0] ? up.error : "failed");
	}
	proto_publish_event(c);
	uploading = false;
}

static void trigger(const motion_result_t *r, const char *sd_name)
{
	count++;
	last_trigger_us = esp_timer_get_time();
	last_at = clock_synced() ? time(NULL) : 0;
	ESP_LOGI(TAG, "motion: %d per mille changed (threshold %d)%s%s", r->level, r->threshold, sd_name[0] ? ", saved " : "", sd_name);
	char id[27];
	cJSON *e = evt_new("motion", id);
	cJSON_AddNumberToObject(e, "level", r->level);
	cJSON_AddNumberToObject(e, "sensitivity", g_cfg.motion_sens);
	cJSON *rr = cJSON_AddArrayToObject(e, "roi");
	for (int i = 0; i < 4; i++) {
		cJSON_AddItemToArray(rr, cJSON_CreateNumber(roi[i]));
	}
	if (sd_name[0]) {
		cJSON_AddStringToObject(e, "sdFile", sd_name);
	}
	char detail[48];
	snprintf(detail, sizeof(detail), "%d per mille changed%s", r->level, sd_name[0] ? ", on SD" : "");
	cJSON_AddStringToObject(e, "detail", detail);
	proto_publish_event(e);
	if (g_cfg.motion_upload && !uploading) {
		uploading = true;
		if (!sched_run(upload_job)) {
			uploading = false;
			ESP_LOGW(TAG, "the scheduler has a job already: no motion photo");
		}
	}
}

static void motion_task(void *arg)
{
	uint8_t *copy = NULL;
	size_t copy_cap = 0;
	for (;;) {
		if (!wanted()) {
			if (running) {
				running = false;
				ESP_LOGI(TAG, "stopped");
			}
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
			continue;
		}
		if (!running) {
			running = true;
			model->have_bg = false;
			motion_config_changed();
			ESP_LOGI(TAG, "watching: sensitivity %d, region %d,%d %dx%d %%, cooldown %d s", g_cfg.motion_sens, roi[0], roi[1], roi[2],
					 roi[3], g_cfg.motion_cool);
		}
		camera_fb_t *fb = NULL;
		int fw, fh, gw, gh;
		if (preview_frame_any(&fb, &fw, &fh) != ESP_OK) {
			vTaskDelay(pdMS_TO_TICKS(1000)); /* a capture owns the camera */
			continue;
		}
		/* the JPEG is copied and the camera given back before the (slow) decode: the web console stream waited for it */
		if (fb->len > copy_cap) {
			free(copy);
			copy_cap = fb->len + fb->len / 4;
			copy = heap_caps_malloc(copy_cap, MALLOC_CAP_SPIRAM);
			if (!copy) {
				copy_cap = 0;
			}
		}
		if (!copy) {
			preview_release(fb);
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}
		camera_fb_t local = *fb;
		memcpy(copy, fb->buf, fb->len);
		local.buf = copy;
		preview_release(fb);
		fb = &local;
		motion_result_t r = { 0 };
		bool ok = decode_grey(fb, fw, &gw, &gh);
		if (ok) {
			r = motion_compare(model, grey, gw, gh, roi, g_cfg.motion_sens);
			last_level = r.level, last_threshold = r.threshold;
		}
		char sd_name[32] = "";
		bool fire = ok && r.motion && esp_timer_get_time() - last_trigger_us >= (int64_t)g_cfg.motion_cool * 1000000;
		if (fire && g_cfg.motion_sd && sd_present()) {
			sd_save_jpeg(fb->buf, fb->len, "motion", sd_name, sizeof(sd_name));
		}
		if (fire) {
			trigger(&r, sd_name);
		}
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
	}
}

void motion_start(void)
{
	if (task) {
		return;
	}
	model = heap_caps_calloc(1, sizeof(*model), MALLOC_CAP_SPIRAM);
	rgb = heap_caps_malloc(MOTION_MAX_PIXELS * 3, MALLOC_CAP_SPIRAM);
	grey = heap_caps_malloc(MOTION_MAX_PIXELS, MALLOC_CAP_SPIRAM);
	if (!model || !rgb || !grey) {
		ESP_LOGE(TAG, "no memory for motion detection");
		return;
	}
	motion_config_changed();
	xTaskCreate(motion_task, "motion", 6144, NULL, 3, &task);
}

void motion_json(cJSON *parent)
{
	cJSON *m = cJSON_AddObjectToObject(parent, "motion");
	cJSON_AddBoolToObject(m, "enabled", g_cfg.motion);
	cJSON_AddBoolToObject(m, "running", running);
	cJSON_AddBoolToObject(m, "active", count && esp_timer_get_time() - last_trigger_us < 5000000);
	cJSON_AddNumberToObject(m, "count", count);
	cJSON_AddNumberToObject(m, "level", last_level);
	cJSON_AddNumberToObject(m, "threshold", last_threshold);
	if (last_at) {
		char s[25];
		iso8601(last_at, s);
		cJSON_AddStringToObject(m, "lastAt", s);
	}
	if (g_cfg.motion && !running) {
		cJSON_AddStringToObject(m, "why", live_streaming() ? "live" : "battery"); /* waiting for powered mode */
	}
}
