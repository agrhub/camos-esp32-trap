/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Live view on the LAN: GET /api/stream?w=640&h=480&q=12 answers multipart/x-mixed-replace (MJPEG) from a task of its
 * own (httpd async request), so the web console keeps answering while a viewer watches. One viewer at a time: a second
 * one gets 409 BUSY. The stream stops when the browser closes the connection (the page clears the <img> when it is
 * hidden), on live view over KVS, or after STREAM_MAX_S. Frames come from the shared camera preview (sched.c), so a
 * scheduled capture still runs: it takes the camera for a few seconds and the stream carries on afterwards.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "camos.h"

static const char *TAG = "stream";
#define BOUNDARY "camosframe"
#define STREAM_MAX_S (30 * 60)

static volatile bool busy;
static volatile bool stop_req;
static stream_stats_t stats;

void stream_get_stats(stream_stats_t *out) { *out = stats; }
void stream_stop(void) { stop_req = true; }
bool stream_active(void) { return busy; }

/* The frame sizes the page offers; anything else falls back to VGA */
bool frame_size_ok(int w, int h)
{
	static const int sizes[][2] = { { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 1200 } };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (sizes[i][0] == w && sizes[i][1] == h) {
			return true;
		}
	}
	return false;
}

typedef struct {
	httpd_req_t *req;
	int w, h, q;
} stream_job_t;

static void stream_task(void *arg)
{
	stream_job_t *job = arg;
	httpd_req_t *req = job->req;
	httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
	uint8_t *copy = NULL;
	size_t cap = 0;
	int64_t t0 = esp_timer_get_time(), last_log = t0, win_t = t0;
	uint32_t win_frames = 0, fails = 0;
	uint64_t win_bytes = 0;
	memset(&stats, 0, sizeof(stats));
	stats.w = job->w, stats.h = job->h, stats.quality = job->q;
	ESP_LOGI(TAG, "viewer started: %dx%d q%d", job->w, job->h, job->q);
	bool wifi = net_type() == NET_WIFI;
	if (wifi) {
		esp_wifi_set_ps(WIFI_PS_NONE); /* modem sleep between beacons halves the frame rate */
	}
	while (!stop_req && esp_timer_get_time() - t0 < (int64_t)STREAM_MAX_S * 1000000) {
		camera_fb_t *fb = NULL;
		esp_err_t e = preview_frame(job->w, job->h, job->q, &fb);
		if (e != ESP_OK) {
			/* a capture or live view owns the camera for a moment: wait and try again */
			if (++fails > 50) {
				ESP_LOGW(TAG, "no frames for a while (%s): ending the stream", esp_err_to_name(e));
				break;
			}
			vTaskDelay(pdMS_TO_TICKS(200));
			continue;
		}
		fails = 0;
		/* copy and give the camera back before the (slower) network send */
		size_t len = fb->len;
		if (len > cap) {
			free(copy);
			cap = len + len / 4;
			copy = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
		}
		if (copy) {
			memcpy(copy, fb->buf, len);
		}
		preview_release(fb);
		if (!copy) {
			break;
		}
		char head[128];
		int n = snprintf(head, sizeof(head), "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)len);
		if (httpd_resp_send_chunk(req, head, n) != ESP_OK || httpd_resp_send_chunk(req, (const char *)copy, len) != ESP_OK) {
			break; /* the viewer went away */
		}
		stats.frames++;
		stats.bytes += len;
		win_frames++;
		win_bytes += len;
		int64_t now = esp_timer_get_time();
		if (now - win_t >= 2000000) {
			stats.fps = win_frames * 1000000.0f / (float)(now - win_t);
			stats.avg_bytes = (uint32_t)(win_bytes / win_frames);
			win_t = now, win_frames = 0, win_bytes = 0;
		}
		if (now - last_log >= 10000000) {
			ESP_LOGI(TAG, "%dx%d q%d: %.1f fps, %u bytes per frame", job->w, job->h, job->q, stats.fps, (unsigned)stats.avg_bytes);
			last_log = now;
			sched_keep_awake(30);
		}
	}
	if (wifi) {
		esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	}
	httpd_resp_send_chunk(req, NULL, 0);
	httpd_req_async_handler_complete(req);
	free(copy);
	ESP_LOGI(TAG, "viewer gone after %u frames (%.1f fps)", (unsigned)stats.frames, stats.fps);
	free(job);
	busy = false;
	stop_req = false;
	vTaskDelete(NULL);
}

esp_err_t web_stream_get(httpd_req_t *req)
{
	if (live_active()) {
		return web_send_error(req, "409 Conflict", "BUSY", "live view over KVS is running");
	}
	if (busy) {
		return web_send_error(req, "409 Conflict", "BUSY", "another viewer is watching: one live picture at a time");
	}
	char q[64] = "", v[8];
	int w = 640, h = 480, quality = 12;
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		if (httpd_query_key_value(q, "w", v, sizeof(v)) == ESP_OK) {
			w = atoi(v);
		}
		if (httpd_query_key_value(q, "h", v, sizeof(v)) == ESP_OK) {
			h = atoi(v);
		}
		if (httpd_query_key_value(q, "q", v, sizeof(v)) == ESP_OK) {
			quality = atoi(v);
		}
	}
	if (!frame_size_ok(w, h)) {
		w = 640, h = 480;
	}
	if (quality < 8 || quality > 40) {
		quality = 12;
	}
	stream_job_t *job = calloc(1, sizeof(*job));
	if (!job || httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
		free(job);
		return web_send_error(req, "500 Internal Server Error", "INTERNAL", "async");
	}
	job->w = w, job->h = h, job->q = quality;
	busy = true;
	stop_req = false;
	if (xTaskCreate(stream_task, "mjpeg", 6144, job, 4, NULL) != pdPASS) {
		busy = false;
		httpd_req_async_handler_complete(job->req);
		free(job);
		return ESP_FAIL;
	}
	return ESP_OK;
}
