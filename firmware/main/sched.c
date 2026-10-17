/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Capture schedule and the sleep cycle.
 *
 * Slots: protocol 8 `detections[]` of type "app" with a `trigger {everyMin, from, to}` (slots `from`, `from + everyMin`,
 * ... up to and including `to`, local time of the shadow `timezone`; `to` before `from` crosses midnight), or, when
 * no detections[] schedule is set, the all-day interval `capture.intervalMin` (shadow) / portal / Kconfig default.
 * The `nextWakeAt` hint of the FlyBot snapshot response moves the next slot when it is sooner.
 * Each slot: capture, POST /api/device/snapshot, then the protocol 5 `capture` event with image.key = snapshotId
 * (image: null and imageError when the upload failed).
 *
 * Battery mode (effective power.mode battery, CAMOS_DEEP_SLEEP): between slots the chip is in deep sleep with the
 * network off. At wake-up the JPEG is captured first (no network needed), then the network, time and MQTT start, the
 * image is uploaded, the event is published, and the camera stays awake CAMOS_AWAKE_WINDOW_S so queued RPCs and a new
 * shadow can arrive. A GPS fix is taken when due (once a day) before the sleep.
 * Powered mode: no deep sleep, MQTT stays connected, captures at the slots, live view on demand.
 */
#include <string.h>
#include <time.h>
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "camos.h"

static const char *TAG = "sched";
static SemaphoreHandle_t cam_mutex;
static void stop_preview_locked(void);
static TaskHandle_t sched_task_handle;
static volatile int64_t last_activity_s; /* uptime second of the last capture end */
static volatile int64_t keep_awake_until_s;
static volatile time_t wake_hint;        /* nextWakeAt from FlyBot */
static volatile time_t next_capture;

#define CONNECT_GIVEUP_S 120  /* no MQTT after this long awake: sleep anyway (a flat network must not drain the cell) */
#define CRITICAL_SLEEP_S (6 * 3600)

void sched_keep_awake(int seconds)
{
	int64_t until = esp_timer_get_time() / 1000000 + seconds;
	if (until > keep_awake_until_s) {
		keep_awake_until_s = until;
	}
}

void sched_wake_hint(time_t at)
{
	wake_hint = at;
	sched_changed();
}

time_t sched_next_capture(void)
{
	return next_capture;
}

/*
 * The camera and the frame buffers are used by one capture at a time (RPC or schedule).
 * Order: JPEG first, then the network. In battery mode the network is off until here; the JPEG waits in PSRAM while
 * the network comes up. The battery is read before Wi-Fi starts (ESP32 ADC2 cannot be read with Wi-Fi on).
 * A JPEG over the 2 MB limit of the snapshot API is retaken at a lower quality (OV5640 at QSXGA can exceed it).
 */
esp_err_t capture_now(const char *trigger, upload_t *out)
{
	memset(out, 0, sizeof(*out));
	if (live_streaming()) {
		strlcpy(out->error, "live view running", sizeof(out->error));
		return ESP_ERR_INVALID_STATE;
	}
	sched_init_lock();
	xSemaphoreTake(cam_mutex, portMAX_DELAY);
	stop_preview_locked(); /* a web preview gives way to a real capture */
	led_flash(LED_FLASH_CAPTURE);
	settings_t s;
	shadow_get(&s);
	camera_fb_t *fb = NULL;
	esp_err_t err = ESP_FAIL;
	for (int attempt = 0; attempt < 3; attempt++) {
		err = cam_capture(&s, &fb);
		if (err != ESP_OK) {
			snprintf(out->error, sizeof(out->error), "camera 0x%x", err);
			break;
		}
		if (fb->len <= 2000000) {
			break;
		}
		ESP_LOGW(TAG, "JPEG %u bytes over the 2 MB limit, retaking at lower quality", (unsigned)fb->len);
		cam_release(fb);
		fb = NULL;
		s.jpeg_quality = s.jpeg_quality > 30 ? s.jpeg_quality - 20 : 10;
	}
	if (err == ESP_OK) {
		if (jpeg_dims(fb->buf, fb->len, &out->w, &out->h) != 0) {
			out->w = fb->width;
			out->h = fb->height;
		}
		out->bytes = fb->len;
		ESP_LOGI(TAG, "JPEG %dx%d, %u bytes, free heap %u, free PSRAM %u", out->w, out->h, (unsigned)fb->len,
				 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
		if (g_cfg.sd_save && sd_present()) {
			sd_save_jpeg(fb->buf, fb->len, trigger, out->sd_file, sizeof(out->sd_file)); /* kept even when the upload fails */
		}
		/* CONTRACTS section 2 knows schedule | rpc | boot: a motion photo is an on-demand one for FlyBot */
		const char *meta_trigger = strcmp(trigger, "motion") ? trigger : "rpc";
		battery_sample();
		if (!g_cfg.api_base[0]) {
			strlcpy(out->error, "no api base (set apiBase)", sizeof(out->error));
			err = ESP_ERR_INVALID_STATE;
		} else if (!(g_cfg.wifi_ssid[0] || modem_available())) {
			strlcpy(out->error, "no network configured", sizeof(out->error));
			err = ESP_ERR_INVALID_STATE;
		} else if (net_ensure(), !net_wait_up(90)) { /* the upload needs a network and a valid clock (TLS dates) */
			strlcpy(out->error, "no network", sizeof(out->error));
			err = ESP_FAIL;
		} else {
			err = flybot_snapshot(fb, meta_trigger, out);
			if (err == ESP_OK && out->next_wake_at > time(NULL)) {
				wake_hint = out->next_wake_at;
			}
		}
		cam_release(fb);
	}
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		led_flash(LED_FLASH_ERROR); /* camera or upload failed */
	}
	last_activity_s = esp_timer_get_time() / 1000000;
	xSemaphoreGive(cam_mutex);
	sched_changed();
	return err;
}

/* ------------------------------------------------------------------ web console preview (same camera lock) */

static bool preview_on;
static int pv_w, pv_h;
static volatile int64_t pv_last_us;
static esp_timer_handle_t pv_timer;

static void stop_preview_locked(void)
{
	if (preview_on) {
		cam_stream_stop();
		preview_on = false;
		ESP_LOGI(TAG, "preview stopped");
	}
}

/* Nobody asked for a frame for 15 s: power the sensor down (try-lock: a capture in progress owns the camera) */
static void pv_idle_cb(void *arg)
{
	if (preview_on && esp_timer_get_time() - pv_last_us > 15LL * 1000000 && xSemaphoreTake(cam_mutex, 0) == pdTRUE) {
		stop_preview_locked();
		xSemaphoreGive(cam_mutex);
	}
	if (!preview_on && pv_timer) {
		esp_timer_stop(pv_timer);
	}
}

bool preview_active(void) { return preview_on; }

esp_err_t preview_frame_any(camera_fb_t **fb, int *w, int *h)
{
	int pw = preview_on ? pv_w : 320, ph = preview_on ? pv_h : 240;
	*w = pw, *h = ph;
	return preview_frame(pw, ph, 12, fb);
}

void camera_settings_changed(void)
{
	sched_init_lock();
	if (preview_on && xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
		if (preview_on) {
			settings_t s;
			shadow_get(&s);
			cam_apply_settings(&s);
		}
		xSemaphoreGive(cam_mutex);
	}
}

esp_err_t preview_frame(int w, int h, int quality, camera_fb_t **fb)
{
	*fb = NULL;
	if (live_streaming()) {
		return ESP_ERR_INVALID_STATE;
	}
	sched_init_lock();
	if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
		return ESP_ERR_TIMEOUT; /* a capture + upload is running */
	}
	if (preview_on && (w != pv_w || h != pv_h)) {
		stop_preview_locked();
	}
	if (!preview_on) {
		esp_err_t e = cam_stream_start(w, h, quality);
		if (e != ESP_OK) {
			xSemaphoreGive(cam_mutex);
			return e;
		}
		preview_on = true;
		pv_w = w, pv_h = h;
		for (int i = 0; i < 2; i++) { /* exposure settles */
			camera_fb_t *f = esp_camera_fb_get();
			if (f) {
				esp_camera_fb_return(f);
			}
		}
		if (!pv_timer) {
			const esp_timer_create_args_t a = { .callback = pv_idle_cb, .name = "preview-idle" };
			esp_timer_create(&a, &pv_timer);
		}
		if (pv_timer) {
			esp_timer_stop(pv_timer);
			esp_timer_start_periodic(pv_timer, 5000000);
		}
		ESP_LOGI(TAG, "preview %dx%d started", w, h);
	}
	pv_last_us = esp_timer_get_time();
	*fb = esp_camera_fb_get();
	if (!*fb) {
		xSemaphoreGive(cam_mutex);
		return ESP_FAIL;
	}
	return ESP_OK; /* the lock is held until preview_release */
}

void preview_release(camera_fb_t *fb)
{
	if (fb) {
		esp_camera_fb_return(fb);
	}
	xSemaphoreGive(cam_mutex);
}

/* The cloud live view takes the camera for as long as a viewer watches: after a capture in progress, and the web
 * console preview gives way */
bool camera_take(int timeout_ms)
{
	sched_init_lock();
	if (xSemaphoreTake(cam_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
		return false;
	}
	stop_preview_locked();
	return true;
}

void camera_give(void)
{
	xSemaphoreGive(cam_mutex);
}

void preview_stop(void)
{
	sched_init_lock();
	xSemaphoreTake(cam_mutex, portMAX_DELAY);
	stop_preview_locked();
	xSemaphoreGive(cam_mutex);
}

static void scheduled_capture(const char *detection_id, const char *trigger)
{
	char id[27];
	cJSON *e = evt_new("capture", id); /* ts = the moment of the slot */
	cJSON_AddStringToObject(e, "trigger", trigger);
	cJSON_AddStringToObject(e, "detectionId", detection_id);
	upload_t up;
	if (capture_now(trigger, &up) == ESP_OK) {
		cJSON *img = cJSON_AddObjectToObject(e, "image");
		cJSON_AddStringToObject(img, "key", up.key);
		cJSON_AddNumberToObject(img, "w", up.w);
		cJSON_AddNumberToObject(img, "h", up.h);
		cJSON_AddNumberToObject(img, "bytes", (double)up.bytes);
		cJSON_AddStringToObject(e, "snapshotId", up.key);
	} else {
		cJSON_AddNullToObject(e, "image");
		cJSON_AddStringToObject(e, "imageError", up.error[0] ? up.error : "failed");
	}
	proto_publish_event(e);
	ESP_LOGI(TAG, "capture %s (%s): %s", detection_id, id, up.error[0] ? up.error : up.key);
}

/* ------------------------------------------------------------------ next slot */

/* First slot strictly after `after`, 0 when none */
static time_t next_fire(const sched_t *s, time_t after)
{
	int window = s->to_min - s->from_min;
	if (window < 0) {
		window += 24 * 60;
	}
	struct tm today;
	localtime_r(&after, &today);
	today.tm_hour = today.tm_min = today.tm_sec = 0;
	for (int d = -1; d <= 1; d++) { /* yesterday's window can reach past midnight */
		struct tm day = today;
		day.tm_mday += d;
		day.tm_isdst = -1;
		time_t base = mktime(&day); /* also normalizes `day` */
		int since_from = (int)((after - base) / 60) - s->from_min;
		int k = since_from > 0 ? since_from / s->every_min : 0;
		for (; k * s->every_min <= window; k++) {
			struct tm slot = day;
			slot.tm_min = s->from_min + k * s->every_min; /* mktime carries into hours */
			slot.tm_isdst = -1;
			time_t t = mktime(&slot);
			if (t > after) {
				return t;
			}
		}
	}
	return 0;
}

/* The effective schedule list: detections[] when present, else the all-day interval */
static int schedules(const settings_t *s, sched_t out[MAX_SCHED])
{
	if (s->n_sched > 0) {
		memcpy(out, s->sched, sizeof(sched_t) * s->n_sched);
		return s->n_sched;
	}
	int every = shadow_interval_min();
	if (!shadow_claimed() && every < 60) {
		every = 60; /* CONTRACTS 2a.3: an unclaimed camera uploads at most once an hour */
	}
	/* the local window from the console's Schedule page (all day by default) */
	sched_t t = { .every_min = every, .from_min = g_cfg.sched_from, .to_min = g_cfg.sched_to };
	if (t.from_min == t.to_min) {
		t.from_min = 0, t.to_min = 24 * 60 - 1;
	}
	strcpy(t.id, "interval");
	out[0] = t;
	return 1;
}

cJSON *sched_json(int n_next)
{
	settings_t s;
	shadow_get(&s);
	sched_t list[MAX_SCHED];
	int n = schedules(&s, list);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "source", s.n_sched > 0 ? "cloud" : "local");
	cJSON_AddBoolToObject(o, "claimed", shadow_claimed());
	cJSON_AddBoolToObject(o, "clockSynced", clock_synced());
	cJSON *items = cJSON_AddArrayToObject(o, "items");
	for (int i = 0; i < n; i++) {
		cJSON *it = cJSON_CreateObject();
		cJSON_AddStringToObject(it, "id", list[i].id);
		cJSON_AddNumberToObject(it, "everyMin", list[i].every_min);
		char hm[16];
		snprintf(hm, sizeof(hm), "%02d:%02d", list[i].from_min / 60, list[i].from_min % 60);
		cJSON_AddStringToObject(it, "from", hm);
		snprintf(hm, sizeof(hm), "%02d:%02d", list[i].to_min / 60, list[i].to_min % 60);
		cJSON_AddStringToObject(it, "to", hm);
		cJSON_AddItemToArray(items, it);
	}
	cJSON *next = cJSON_AddArrayToObject(o, "next");
	if (clock_synced()) {
		time_t after[MAX_SCHED];
		time_t now = time(NULL);
		for (int i = 0; i < n; i++) {
			after[i] = now;
		}
		for (int k = 0; k < n_next; k++) {
			time_t best = 0;
			int bi = -1;
			for (int i = 0; i < n; i++) {
				time_t t = next_fire(&list[i], after[i]);
				if (t && (!best || t < best)) {
					best = t, bi = i;
				}
			}
			if (bi < 0) {
				break;
			}
			after[bi] = best;
			struct tm lt;
			localtime_r(&best, &lt);
			char ts[24];
			strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &lt);
			cJSON *x = cJSON_CreateObject();
			cJSON_AddStringToObject(x, "at", ts);
			cJSON_AddStringToObject(x, "id", list[bi].id);
			cJSON_AddItemToArray(next, x);
		}
	}
	if (wake_hint > time(NULL)) {
		char ts[25];
		iso8601(wake_hint, ts);
		cJSON_AddStringToObject(o, "nextWakeAt", ts); /* FlyBot's hint, can come sooner */
	}
	return o;
}

void sched_changed(void)
{
	if (sched_task_handle) {
		xTaskNotifyGive(sched_task_handle);
	}
}

/* ------------------------------------------------------------------ power: deep sleep between slots */

#if CONFIG_CAMOS_DEEP_SLEEP
/* Sleep is allowed once the MQTT session has been up for the awake window (queued RPCs and the retained shadow have
 * arrived and been handled), or when MQTT cannot be reached after CONNECT_GIVEUP_S. Not while an RPC runs, the
 * console or live view asked to stay awake, or a pairing is pending. */
static bool may_sleep(void)
{
	int64_t up = esp_timer_get_time() / 1000000;
	if (!proto_idle() || up - last_activity_s < 5 || up < keep_awake_until_s || live_active()) {
		return false;
	}
	int connected_s = proto_connected_for_s();
	if (connected_s >= CONFIG_CAMOS_AWAKE_WINDOW_S) {
		return true;
	}
	if (connected_s < 0 && up >= CONNECT_GIVEUP_S) {
		ESP_LOGW(TAG, "no MQTT after %d s awake: giving up until the next wake", CONNECT_GIVEUP_S);
		led_set(LED_ERROR);
		vTaskDelay(pdMS_TO_TICKS(2500)); /* one red double blink is visible before the sleep */
		return true;
	}
	return false;
}

/* `until`: wall-clock second to be awake for. The wake-up is early by `lead` (boot, camera warm-up, and the drift of
 * the RTC clock, taken as 2 %): the slot is then waited for awake, with the network still off. */
static void deep_sleep_until(time_t until, time_t now, bool with_lead)
{
	int64_t total = (int64_t)(until - now);
	int64_t lead = with_lead ? 4 + total / 50 : 0;
	if (lead > 90) {
		lead = 90;
	}
	int64_t sleep_s = total - lead;
	if (sleep_s < 1) {
		sleep_s = 1;
	}
	ESP_LOGI(TAG, "deep sleep %lld s (wake %lld s before the slot)", (long long)sleep_s, (long long)lead);
	proto_goodbye((uint64_t)until * 1000ULL - (uint64_t)lead * 1000ULL);
	if (proto_connected()) {
		proto_flush(5000);
	}
	net_sleep_prepare(); /* modem off through PWRKEY and its rail, Wi-Fi stopped */
	cam_sleep_prepare();
	led_sleep_prepare(battery_mv() >= 0 && battery_mv() < CONFIG_CAMOS_BATT_LOW_MV);
	esp_sleep_enable_timer_wakeup((uint64_t)sleep_s * 1000000ULL);
	esp_deep_sleep_start();
}
#endif

/* ------------------------------------------------------------------ task */

static void sched_task(void *arg)
{
	time_t last[MAX_SCHED] = { 0 };
	static bool boot_capture_done;
	for (;;) {
		if (!clock_synced()) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
			continue;
		}
		settings_t s;
		shadow_get(&s);
		sched_t list[MAX_SCHED];
		int n = schedules(&s, list);
		time_t now = time(NULL);
		time_t best = 0;
		int best_i = -1;
		for (int i = 0; i < n; i++) {
			time_t t = next_fire(&list[i], now > last[i] ? now : last[i]);
			if (t && (!best || t < best)) {
				best = t, best_i = i;
			}
		}
		if (wake_hint > now && (!best || wake_hint < best || !shadow_claimed())) {
			/* FlyBot asked for an earlier wake; an unclaimed camera follows nextWakeAt even when it is later */
			best = wake_hint, best_i = 0;
		} else if (wake_hint && wake_hint <= now) {
			wake_hint = 0;
		}
		next_capture = best;

		/* A boot capture (power-on or reset, not a timer wake-up) when the camera is paired: "boot" trigger */
		if (!boot_capture_done && store_paired() && boot_reason_name() && proto_connected()) {
			boot_capture_done = true;
			scheduled_capture("boot", "boot");
			continue;
		}
		boot_capture_done = true;

		bool deep = power_may_deep_sleep();
#if CONFIG_CAMOS_DEEP_SLEEP
		if (deep && battery_critical()) {
			net_ensure(); /* so that low_power can go out before the long sleep */
		}
		if (deep && proto_provisioned() && may_sleep()) {
			if (gps_fix_due() && !battery_critical()) {
				gps_fix(CONFIG_CAMOS_GPS_FIX_TIMEOUT_S, NULL); /* once a day, before the sleep; reported next connect */
				proto_publish_reported_now();
				proto_flush(5000);
			}
			if (battery_critical()) {
				/* Protect the cell: no captures, wake every 6 h to look at the battery (low_power goes out when connected) */
				deep_sleep_until(now + CRITICAL_SLEEP_S, now, false);
			} else if (best_i >= 0 && best - now >= CONFIG_CAMOS_DEEP_SLEEP_MIN_S) {
				deep_sleep_until(best, now, true);
			} else if (best_i < 0) {
				deep_sleep_until(now + CONFIG_CAMOS_NOSCHED_SLEEP_MIN * 60, now, false); /* nothing scheduled: look for a shadow */
			}
		}
#endif
		if (!deep && gps_fix_due() && proto_connected() && !live_active()) {
			gps_fix(CONFIG_CAMOS_GPS_FIX_TIMEOUT_S, NULL);
			proto_publish_reported_now();
		}
		int wait_max = deep ? 5 : 60; /* re-check often when a sleep decision is pending */
		if (best_i < 0) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_max * 1000));
			continue;
		}
		if (best > now) {
			/* Wait in short pieces: SNTP can move the clock, a new shadow can change the plan */
			int64_t wait_s = best - now > wait_max ? wait_max : best - now;
			if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000))) {
				memset(last, 0, sizeof(last)); /* new plan */
				continue;
			}
			if (time(NULL) < best) {
				continue;
			}
		}
		if (battery_critical()) {
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); /* the sleep branch above takes over once it may sleep */
			continue;
		}
		if (best == wake_hint) {
			wake_hint = 0;
		} else {
			last[best_i] = best;
		}
		scheduled_capture(list[best_i].id, "schedule");
	}
}

void sched_init_lock(void)
{
	if (!cam_mutex) {
		cam_mutex = xSemaphoreCreateMutex();
	}
}

void sched_start(void)
{
	sched_init_lock();
	xTaskCreate(sched_task, "sched", 12288, NULL, 5, &sched_task_handle);
}
