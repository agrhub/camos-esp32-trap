/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Shadow (protocol 8): the cloud keeps `desired`, the camera applies what it can and publishes `reported` with the same
 * `version` and an `errors` list for the rest. What this firmware supports:
 *
 *   timezone                     IANA name -> POSIX TZ (tz.c); schedules run in local time; null = automatic
 *   detections[] of type "app"   trigger {everyMin, from, to} -> capture schedule (no on-device detection)
 *   capture.jpegQuality 1..100   -> esp32-camera quality 63..4 (see cam.c)
 *   capture.resolution           "main" / "max" = the full sensor size; v1 addition: "WxH" from camera.photoSizes
 *   capture.intervalMin          v1 addition: an all-day capture interval (used when no detections[] schedule is set)
 *   capture.window               v1 addition: {from, to} "HH:MM" local window of that interval; null = all day
 *   video.image.*                brightness, contrast, saturation, sharpness, orientation and the ESP32 additions
 *                                (apply_image: vflip, hmirror, white balance, effect, AE level, exposure, gain, gain
 *                                ceiling, denoise); vflip / hmirror are orientation's bits 2 and 1
 *   power.mode                   v1 addition: "auto" | "battery" | "powered"; power.highMv / power.lowMv thresholds
 *   motion.*                     v1 addition: enabled, sensitivity, cooldownS, upload, saveSd, area (motion.c)
 *   storage.sdSave               v1 addition: every photo also on the microSD card
 *   led.enabled                  v1 addition: the status LED
 *   gps.enabled                  v1 addition: daily GPS fixes when the module is fitted
 *   console.language             v1 addition: "en" | "vi", the camera's own web pages
 *   live.requested               v1 addition: FlyBot asks for a live session at the next wake
 *   kvs.channelName              v1 addition: the KVS signaling channel (stored in NVS)
 *   kvs.credentialsEndpoint      v1 addition: IoT credential provider host and role alias of the live view
 *   kvs.roleAlias                (stored in NVS, like pairing's iot.credentialsEndpoint / iot.roleAlias)
 *   log                          v1 addition: {level, until} turns the remote log on until then (logfwd.c)
 *   disabled                     v1 addition: true while a FlyBot administrator pauses the camera (no captures,
 *                                uploads, motion photos or live view; it wakes every 6 h on battery; OTA still works)
 *
 * capture.resolution / window, motion, storage, led, gps and console are the web console's settings (NVS, store.c):
 * a desired value overwrites what was set on the camera, and both show in `reported`. Key list: docs/dev/CONFIG.md.
 *
 * Everything else is reported as NOT_SUPPORTED, a value out of range as BAD_VALUE (the old value stays).
 * Fields absent from `desired` keep their current value. The applied settings are kept in NVS and used at boot.
 * `reported` (and `state`) also carry location, net and battery (CONTRACTS section 3).
 */
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "camos.h"

static const char *TAG = "shadow";
static settings_t cur;
static SemaphoreHandle_t lock;
static cJSON *last_errors; /* from the last apply, repeated in every reported */
static volatile bool disabled_hint; /* a snapshot upload was answered 403 "disabled" */

static void defaults(settings_t *s)
{
	memset(s, 0, sizeof(*s));
	strcpy(s->tz, "UTC0");
	s->jpeg_quality = 85;
	s->brightness = 0;
	s->contrast = 50;
	s->saturation = 50;
	s->sharpness = 50;
	s->exposure_auto = 1;
	s->exposure = 300;
	s->gain_auto = 1;
	s->gain_ceiling = 4; /* 32x: dim indoor light still reaches the AE target (cam.c) */
	s->night_mode = 0;   /* streams keep their frame rate; the gain brightens a dim scene (cam.c) */
	s->power_mode = -1;
	s->claimed = -1;
}

/* The saved settings: defaults first, so a shorter blob from an older release (fields are only ever added at the end
 * of settings_t) keeps its values and leaves the newer fields at their defaults */
static bool load_saved(settings_t *out)
{
	size_t got = 0;
	defaults(out);
	if (!store_get_blob_upto("settings", out, sizeof(*out), &got) || got < offsetof(settings_t, night_mode)
		|| out->n_sched < 0 || out->n_sched > MAX_SCHED) {
		defaults(out);
		return false;
	}
	return true;
}

/* The shadow's `timezone`, when the cloud set one (tz.c decides between it, a manual override and /api/device/time) */
bool shadow_tz(char *iana, size_t in, char *posix, size_t pn)
{
	if (!lock || !cur.tz_set) {
		return false;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	strlcpy(iana, cur.tz_iana, in);
	strlcpy(posix, cur.tz, pn);
	xSemaphoreGive(lock);
	return true;
}

bool shadow_disabled(void)
{
	return cur.disabled != 0 || disabled_hint;
}

void shadow_note_disabled(bool on)
{
	if (on && !shadow_disabled()) {
		ESP_LOGW(TAG, "FlyBot says this camera is paused: no captures until the shadow says otherwise");
	}
	disabled_hint = on;
	sched_changed();
}

/* claimed in FlyBot (CONTRACTS 2a.4): the shadow's `claimed` once it said so, else the registration result */
bool shadow_claimed(void)
{
	return cur.claimed >= 0 ? cur.claimed != 0 : g_cfg.claimed != 0;
}

void shadow_init(void)
{
	if (lock) {
		return;
	}
	lock = xSemaphoreCreateMutex();
	defaults(&cur);
	settings_t saved;
	if (load_saved(&saved)) {
		cur = saved;
		ESP_LOGI(TAG, "settings from NVS: version %d, %d schedule(s), tz %s, interval %d, power %d", cur.version, cur.n_sched,
				 cur.tz, cur.interval_min, cur.power_mode);
	}
	tz_apply();
}

/* The web console previews image settings without saving them (shadow_apply persist false); Restore comes back here */
void shadow_reload(void)
{
	settings_t saved;
	if (!lock) {
		shadow_init();
	}
	if (!load_saved(&saved)) {
		defaults(&saved);
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	cur = saved;
	xSemaphoreGive(lock);
	camera_settings_changed();
}

void shadow_get(settings_t *out)
{
	if (!lock) {
		shadow_init(); /* the portal / console may ask before app_main got there */
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	*out = cur;
	xSemaphoreGive(lock);
}

int shadow_version(void)
{
	return cur.version;
}

int shadow_interval_min(void)
{
	if (cur.interval_min > 0) {
		return cur.interval_min;
	}
	if (g_cfg.interval_min > 0) {
		return g_cfg.interval_min;
	}
	return CONFIG_CAMOS_CAPTURE_INTERVAL_MIN;
}

power_mode_t shadow_power_mode(void)
{
	if (cur.power_mode >= 0) {
		return (power_mode_t)cur.power_mode;
	}
	return (power_mode_t)g_cfg.power_mode;
}

void shadow_clear_live_request(void)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	cur.live_requested = false;
	xSemaphoreGive(lock);
	settings_t saved; /* only this flag in NVS: not an unsaved web console preview held in cur */
	if (load_saved(&saved)) {
		saved.live_requested = false;
		store_set_blob("settings", &saved, sizeof(saved));
	}
}

/* ------------------------------------------------------------------ apply */

static void add_error(cJSON *errors, const char *path, const char *code)
{
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "path", path);
	cJSON_AddStringToObject(e, "code", code);
	cJSON_AddItemToArray(errors, e);
}

/* A whole number within [lo, hi]; the protocol says anything else is BAD_VALUE */
static bool get_int(const cJSON *item, int lo, int hi, int *out)
{
	if (!cJSON_IsNumber(item) || item->valuedouble != floor(item->valuedouble) || item->valuedouble < lo ||
		item->valuedouble > hi) {
		return false;
	}
	*out = (int)item->valuedouble;
	return true;
}

/* "HH:MM" -> minutes after midnight, or -1 */
static int parse_hhmm(const cJSON *item)
{
	int h, m;
	const char *s = cJSON_GetStringValue(item);
	if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
		return -1;
	}
	return h * 60 + m;
}

static void apply_detections(const cJSON *arr, settings_t *s, cJSON *errors)
{
	char path[64];
	s->n_sched = 0;
	int i = 0;
	const cJSON *d;
	cJSON_ArrayForEach(d, arr)
	{
		snprintf(path, sizeof(path), "detections[%d]", i++);
		if (cJSON_IsFalse(cJSON_GetObjectItem(d, "enabled"))) {
			continue; /* switched off: nothing to do and nothing to complain about */
		}
		const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
		const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(d, "id"));
		if (!type || strcmp(type, "app") != 0) {
			add_error(errors, path, "NOT_SUPPORTED"); /* motion, person, object: no AI on this device */
			continue;
		}
		const cJSON *trig = cJSON_GetObjectItem(d, "trigger");
		sched_t t = { .every_min = 0, .from_min = 0, .to_min = 24 * 60 - 1 };
		if (!id || !id[0] || strlen(id) >= sizeof(t.id) || !cJSON_IsObject(trig) ||
			!get_int(cJSON_GetObjectItem(trig, "everyMin"), 1, 1440, &t.every_min)) {
			add_error(errors, path, "BAD_VALUE");
			continue;
		}
		strcpy(t.id, id);
		const cJSON *from = cJSON_GetObjectItem(trig, "from"), *to = cJSON_GetObjectItem(trig, "to");
		if (from && (t.from_min = parse_hhmm(from)) < 0) {
			add_error(errors, path, "BAD_VALUE");
			continue;
		}
		if (to && (t.to_min = parse_hhmm(to)) < 0) {
			add_error(errors, path, "BAD_VALUE");
			continue;
		}
		if (s->n_sched >= MAX_SCHED) {
			add_error(errors, path, "NOT_SUPPORTED"); /* more schedules than MAX_SCHED */
			continue;
		}
		s->sched[s->n_sched++] = t;
	}
}

/* video.image fields: the protocol ones (brightness, contrast, saturation, sharpness, orientation) and the ESP32
 * additions (white balance, effect, AE level, manual exposure / gain, gain ceiling, denoise, night mode). flicker, wdr and ldc have
 * no OV2640 / OV5640 control in esp32-camera: NOT_SUPPORTED. */
static const char *const wb_names[] = { "auto", "sunny", "cloudy", "office", "home", NULL };
static const char *const fx_names[] = { "none", "negative", "grayscale", "red", "green", "blue", "sepia", NULL };
typedef struct {
	const char *key;
	size_t off;
	int lo, hi;
	const char *const *names; /* string enum instead of a number */
	bool boolean;
	int bit; /* a boolean view of one bit of the field (vflip, hmirror in orientation) */
} img_field_t;
#define F(k, m, lo, hi) { k, offsetof(settings_t, m), lo, hi, NULL, false, 0 }
static const img_field_t img_fields[] = {
	F("brightness", brightness, -64, 64), F("contrast", contrast, 0, 100), F("saturation", saturation, 0, 100),
	F("sharpness", sharpness, 0, 100), F("orientation", orientation, 0, 3),
	/* relative to the board's upright picture (Kconfig CAMOS_CAM_VFLIP / HMIRROR): false, false = as mounted */
	{ "vflip", offsetof(settings_t, orientation), 0, 1, NULL, true, 2 },
	{ "hmirror", offsetof(settings_t, orientation), 0, 1, NULL, true, 1 },
	{ "whiteBalance", offsetof(settings_t, wb_mode), 0, 4, wb_names, false, 0 },
	{ "effect", offsetof(settings_t, effect), 0, 6, fx_names, false, 0 },
	F("aeLevel", ae_level, -5, 5),
	{ "exposureAuto", offsetof(settings_t, exposure_auto), 0, 1, NULL, true, 0 },
	F("exposure", exposure, 0, 1200),
	{ "gainAuto", offsetof(settings_t, gain_auto), 0, 1, NULL, true, 0 },
	F("gain", gain, 0, 30), F("gainCeiling", gain_ceiling, 0, 6), F("denoise", denoise, 0, 8),
	{ "nightMode", offsetof(settings_t, night_mode), 0, 1, NULL, true, 0 },
};
#undef F

static void apply_image(const cJSON *img, settings_t *s, cJSON *errors)
{
	const cJSON *f;
	cJSON_ArrayForEach(f, img)
	{
		char path[48];
		snprintf(path, sizeof(path), "video.image.%s", f->string);
		const img_field_t *fd = NULL;
		for (size_t i = 0; i < sizeof(img_fields) / sizeof(img_fields[0]); i++) {
			if (!strcmp(img_fields[i].key, f->string)) {
				fd = &img_fields[i];
			}
		}
		if (!fd) {
			add_error(errors, path, "NOT_SUPPORTED"); /* flicker, wdr, ldc */
			continue;
		}
		int *dst = (int *)((char *)s + fd->off), v;
		if (fd->boolean) {
			if (!cJSON_IsBool(f)) {
				add_error(errors, path, "BAD_VALUE");
				continue;
			}
			if (fd->bit) {
				*dst = cJSON_IsTrue(f) ? (*dst | fd->bit) : (*dst & ~fd->bit);
			} else {
				*dst = cJSON_IsTrue(f);
			}
		} else if (fd->names && cJSON_IsString(f)) {
			int k = 0;
			while (fd->names[k] && strcmp(fd->names[k], f->valuestring)) {
				k++;
			}
			if (fd->names[k]) {
				*dst = k;
			} else {
				add_error(errors, path, "BAD_VALUE");
			}
		} else if (get_int(f, fd->lo, fd->hi, &v)) {
			*dst = v;
		} else {
			add_error(errors, path, "BAD_VALUE");
		}
	}
}

static void apply_capture(const cJSON *item, settings_t *s, cJSON *errors)
{
	const cJSON *c;
	cJSON_ArrayForEach(c, item)
	{
		if (!strcmp(c->string, "jpegQuality")) {
			if (!get_int(c, 1, 100, &s->jpeg_quality)) {
				add_error(errors, "capture.jpegQuality", "BAD_VALUE");
			}
		} else if (!strcmp(c->string, "resolution")) {
			const char *r = cJSON_GetStringValue(c);
			if (r && !strcmp(r, "main")) {
				r = "max"; /* protocol 8: the main stream size is the sensor's full size here */
			}
			if (!r || !store_apply_kv("captureSize", r)) {
				add_error(errors, "capture.resolution", "BAD_VALUE");
			}
		} else if (!strcmp(c->string, "window")) {
			int from = 0, to = 1439;
			if (!cJSON_IsNull(c) && (!cJSON_IsObject(c) || (from = parse_hhmm(cJSON_GetObjectItem(c, "from"))) < 0 ||
									 (to = parse_hhmm(cJSON_GetObjectItem(c, "to"))) < 0)) {
				add_error(errors, "capture.window", "BAD_VALUE");
			} else {
				char a[8], b[8];
				snprintf(a, sizeof(a), "%d", from);
				snprintf(b, sizeof(b), "%d", to);
				store_apply_kv("schedFrom", a);
				store_apply_kv("schedTo", b);
			}
		} else if (!strcmp(c->string, "intervalMin")) {
			if (cJSON_IsNull(c)) {
				s->interval_min = 0; /* back to the portal / default value */
			} else if (!get_int(c, 1, 1440, &s->interval_min)) {
				add_error(errors, "capture.intervalMin", "BAD_VALUE");
			}
		} else {
			char path[48];
			snprintf(path, sizeof(path), "capture.%s", c->string);
			add_error(errors, path, "NOT_SUPPORTED");
		}
	}
}

static void apply_power(const cJSON *item, settings_t *s, cJSON *errors)
{
	const cJSON *c;
	cJSON_ArrayForEach(c, item)
	{
		if (!strcmp(c->string, "mode")) {
			power_mode_t m;
			if (cJSON_IsNull(c)) {
				s->power_mode = -1;
			} else if (power_mode_parse(cJSON_GetStringValue(c), &m)) {
				s->power_mode = m;
			} else {
				add_error(errors, "power.mode", "BAD_VALUE");
			}
		} else if (!strcmp(c->string, "highMv")) {
			if (!get_int(c, 3000, 4400, &s->power_high_mv)) {
				add_error(errors, "power.highMv", "BAD_VALUE");
			}
		} else if (!strcmp(c->string, "lowMv")) {
			if (!get_int(c, 3000, 4400, &s->power_low_mv)) {
				add_error(errors, "power.lowMv", "BAD_VALUE");
			}
		} else {
			char path[48];
			snprintf(path, sizeof(path), "power.%s", c->string);
			add_error(errors, path, "NOT_SUPPORTED");
		}
	}
}

/* The web console's settings that FlyBot may set too: desired path -> store.c key */
typedef enum { R_BOOL, R_INT, R_ENUM, R_AREA } remote_kind_t;
static const struct {
	const char *obj, *field, *key;
	remote_kind_t kind;
	int lo, hi;
	const char *const *names;
} remote[] = {
	{ "motion", "enabled", "motion", R_BOOL, 0, 0, NULL },
	{ "motion", "sensitivity", "motionSens", R_INT, 1, 100, NULL },
	{ "motion", "cooldownS", "motionCooldown", R_INT, 5, 3600, NULL },
	{ "motion", "upload", "motionUpload", R_BOOL, 0, 0, NULL },
	{ "motion", "saveSd", "motionSd", R_BOOL, 0, 0, NULL },
	{ "motion", "area", "motionRoi", R_AREA, 0, 0, NULL },
	{ "storage", "sdSave", "sdSave", R_BOOL, 0, 0, NULL },
	{ "led", "enabled", "ledEnabled", R_BOOL, 0, 0, NULL },
	{ "gps", "enabled", "gps", R_BOOL, 0, 0, NULL },
	{ "console", "language", "uiLang", R_ENUM, 0, 0, (const char *const[]){ "en", "vi", NULL } },
};
#define N_REMOTE (sizeof(remote) / sizeof(remote[0]))

static bool remote_obj(const char *k)
{
	for (size_t i = 0; i < N_REMOTE; i++) {
		if (!strcmp(remote[i].obj, k)) {
			return true;
		}
	}
	return false;
}

static void apply_remote(const char *obj, const cJSON *item, cJSON *errors)
{
	const cJSON *c;
	cJSON_ArrayForEach(c, item)
	{
		char path[48], v[32];
		snprintf(path, sizeof(path), "%s.%s", obj, c->string);
		size_t i = 0;
		while (i < N_REMOTE && (strcmp(remote[i].obj, obj) || strcmp(remote[i].field, c->string))) {
			i++;
		}
		if (i == N_REMOTE) {
			add_error(errors, path, "NOT_SUPPORTED");
			continue;
		}
		bool ok = false;
		int n = 0;
		switch (remote[i].kind) {
		case R_BOOL:
			ok = cJSON_IsBool(c);
			strlcpy(v, cJSON_IsTrue(c) ? "1" : "0", sizeof(v));
			break;
		case R_INT:
			ok = get_int(c, remote[i].lo, remote[i].hi, &n);
			snprintf(v, sizeof(v), "%d", n);
			break;
		case R_ENUM:
			for (int e = 0; cJSON_IsString(c) && remote[i].names[e]; e++) {
				ok = ok || !strcmp(remote[i].names[e], c->valuestring);
			}
			strlcpy(v, ok ? c->valuestring : "", sizeof(v));
			break;
		case R_AREA: { /* [x, y, w, h] percent of the picture */
			int a[4];
			ok = cJSON_IsArray(c) && cJSON_GetArraySize(c) == 4;
			for (int e = 0; ok && e < 4; e++) {
				ok = get_int(cJSON_GetArrayItem(c, e), 0, 100, &a[e]);
			}
			snprintf(v, sizeof(v), "%d,%d,%d,%d", ok ? a[0] : 0, ok ? a[1] : 0, ok ? a[2] : 0, ok ? a[3] : 0);
			break;
		}
		}
		if (!ok || !store_apply_kv(remote[i].key, v)) {
			add_error(errors, path, "BAD_VALUE");
		}
	}
}

char *shadow_apply(const char *json, int len, bool persist)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		ESP_LOGW(TAG, "desired is not a JSON object");
		return NULL;
	}
	settings_t s;
	shadow_get(&s);
	if (persist) {
		/* a saved change starts from the saved settings: an unsaved web console preview (persist false) is not
		 * written to NVS with it (a preview brightness stayed after a FlyBot desired update or a live request) */
		settings_t saved;
		if (load_saved(&saved)) {
			s = saved;
		}
	}
	cJSON *errors = cJSON_CreateArray();

	const cJSON *item;
	cJSON_ArrayForEach(item, root)
	{
		const char *k = item->string;
		if (!strcmp(k, "v")) {
			continue;
		} else if (!strcmp(k, "version")) {
			get_int(item, 0, 2147483647, &s.version);
		} else if (!strcmp(k, "timezone")) {
			const char *tz = cJSON_IsString(item) ? tz_lookup(item->valuestring) : NULL;
			if (cJSON_IsNull(item)) {
				s.tz_set = false; /* back to the automatic timezone */
			} else if (tz && strlen(tz) < sizeof(s.tz) && strlen(item->valuestring) < sizeof(s.tz_iana)) {
				strcpy(s.tz, tz);
				strcpy(s.tz_iana, item->valuestring);
				s.tz_set = true;
			} else {
				add_error(errors, "timezone", "BAD_VALUE"); /* unknown zone: add it to tz.c */
			}
		} else if (!strcmp(k, "detections") && cJSON_IsArray(item)) {
			apply_detections(item, &s, errors);
		} else if (!strcmp(k, "capture") && cJSON_IsObject(item)) {
			apply_capture(item, &s, errors);
		} else if (!strcmp(k, "power") && cJSON_IsObject(item)) {
			apply_power(item, &s, errors);
		} else if (!strcmp(k, "claimed")) {
			if (cJSON_IsBool(item)) {
				s.claimed = cJSON_IsTrue(item);
				if (s.claimed != g_cfg.claimed) {
					store_apply_kv("claimed", s.claimed ? "1" : "0");
				}
			} else {
				add_error(errors, "claimed", "BAD_VALUE");
			}
		} else if (!strcmp(k, "log")) {
			logfwd_desired(item, errors); /* remote log: not part of the saved settings */
		} else if (!strcmp(k, "disabled")) {
			if (cJSON_IsBool(item)) {
				s.disabled = cJSON_IsTrue(item);
				disabled_hint = false; /* the shadow decides from now on */
			} else {
				add_error(errors, "disabled", "BAD_VALUE");
			}
		} else if (!strcmp(k, "live") && cJSON_IsObject(item)) {
			const cJSON *r = cJSON_GetObjectItem(item, "requested");
			if (r && !cJSON_IsBool(r)) {
				add_error(errors, "live.requested", "BAD_VALUE");
			} else if (r) {
				s.live_requested = cJSON_IsTrue(r);
			}
		} else if (!strcmp(k, "kvs") && cJSON_IsObject(item)) {
			const char *chan = cJSON_GetStringValue(cJSON_GetObjectItem(item, "channelName"));
			if (chan && !store_apply_kv("kvsChannel", chan)) {
				add_error(errors, "kvs.channelName", "BAD_VALUE");
			}
			const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(item, "region"));
			if (region && !store_apply_kv("kvsRegion", region)) {
				add_error(errors, "kvs.region", "BAD_VALUE");
			}
			/* the IoT credential provider for the live view credentials, for cameras that registered without it */
			const char *cred = cJSON_GetStringValue(cJSON_GetObjectItem(item, "credentialsEndpoint"));
			if (cred && !store_apply_kv("credHost", cred)) {
				add_error(errors, "kvs.credentialsEndpoint", "BAD_VALUE");
			}
			const char *alias = cJSON_GetStringValue(cJSON_GetObjectItem(item, "roleAlias"));
			if (alias && !store_apply_kv("roleAlias", alias)) {
				add_error(errors, "kvs.roleAlias", "BAD_VALUE");
			}
		} else if (remote_obj(k) && cJSON_IsObject(item)) {
			apply_remote(k, item, errors);
		} else if (!strcmp(k, "video") && cJSON_IsObject(item)) {
			const cJSON *v;
			cJSON_ArrayForEach(v, item)
			{
				if (!strcmp(v->string, "image") && cJSON_IsObject(v)) {
					apply_image(v, &s, errors);
				} else {
					char path[48];
					snprintf(path, sizeof(path), "video.%s", v->string); /* main, sub, osd, flip, nightMode ... */
					add_error(errors, path, "NOT_SUPPORTED");
				}
			}
		} else {
			add_error(errors, k, "NOT_SUPPORTED"); /* record, net, ota, audio, ... */
		}
	}

	xSemaphoreTake(lock, portMAX_DELAY);
	cur = s;
	cJSON_Delete(last_errors);
	last_errors = errors;
	xSemaphoreGive(lock);
	tz_apply();
	camera_settings_changed(); /* live: a running preview / stream picks the new image settings up now */
	if (s.disabled && (live_active() || live_streaming())) {
		live_stop(); /* paused: the viewer goes too */
	}
	if (persist) {
		store_set_blob("settings", &s, sizeof(s));
	}
	ESP_LOGI(TAG, "applied version %d: %d schedule(s), interval %d, tz %s, jpeg %d, power %d, live %d, %d error(s)",
			 s.version, s.n_sched, s.interval_min, s.tz, s.jpeg_quality, s.power_mode, s.live_requested,
			 cJSON_GetArraySize(errors));
	cJSON_Delete(root);
	sched_changed();
	return shadow_reported_json();
}

/* ------------------------------------------------------------------ reported (and the same blocks in `state`) */

void shadow_image_json(cJSON *parent)
{
	settings_t s;
	shadow_get(&s);
	cJSON *video = cJSON_AddObjectToObject(parent, "video");
	cJSON *img = cJSON_AddObjectToObject(video, "image");
	for (size_t i = 0; i < sizeof(img_fields) / sizeof(img_fields[0]); i++) {
		const img_field_t *fd = &img_fields[i];
		int v = *(const int *)((const char *)&s + fd->off);
		if (fd->boolean) {
			cJSON_AddBoolToObject(img, fd->key, fd->bit ? (v & fd->bit) != 0 : v != 0);
		} else if (fd->names) {
			cJSON_AddStringToObject(img, fd->key, fd->names[v]);
		} else {
			cJSON_AddNumberToObject(img, fd->key, v);
		}
	}
	cam_info_json(cJSON_AddObjectToObject(parent, "camera")); /* sensor and its sizes: FlyBot's menus */
	cJSON *cap = cJSON_AddObjectToObject(parent, "capture");
	int w, h;
	cam_photo_size(&w, &h);
	cJSON_AddStringToObject(cap, "resolution", g_cfg.capture_size);
	cJSON_AddNumberToObject(cap, "jpegQuality", s.jpeg_quality);
	cJSON_AddNumberToObject(cap, "intervalMin", shadow_interval_min());
	char hm[16];
	cJSON *win = cJSON_AddObjectToObject(cap, "window");
	snprintf(hm, sizeof(hm), "%02d:%02d", g_cfg.sched_from / 60, g_cfg.sched_from % 60);
	cJSON_AddStringToObject(win, "from", hm);
	snprintf(hm, sizeof(hm), "%02d:%02d", g_cfg.sched_to / 60, g_cfg.sched_to % 60);
	cJSON_AddStringToObject(win, "to", hm);
	if (w) {
		cJSON_AddNumberToObject(cap, "w", w);
		cJSON_AddNumberToObject(cap, "h", h);
	} else {
		cJSON_AddNullToObject(cap, "w"); /* known after the first capture */
		cJSON_AddNullToObject(cap, "h");
	}
}

void shadow_additions_json(cJSON *parent)
{
	settings_t s;
	shadow_get(&s);
	power_json(parent);
	cJSON *live = cJSON_AddObjectToObject(parent, "live");
	cJSON_AddBoolToObject(live, "requested", s.live_requested);
	cJSON_AddBoolToObject(live, "supported", live_supported());
	cJSON_AddBoolToObject(live, "active", live_active() || live_streaming());
	last_reset_json(parent); /* reset or sleep: FlyBot shows why the camera started */
	cJSON *mo = cJSON_AddObjectToObject(parent, "motion");
	cJSON_AddBoolToObject(mo, "enabled", g_cfg.motion);
	cJSON_AddNumberToObject(mo, "sensitivity", g_cfg.motion_sens);
	cJSON_AddNumberToObject(mo, "cooldownS", g_cfg.motion_cool);
	cJSON_AddBoolToObject(mo, "upload", g_cfg.motion_upload);
	cJSON_AddBoolToObject(mo, "saveSd", g_cfg.motion_sd);
	int roi[4];
	if (roi_parse(g_cfg.motion_roi, roi)) {
		cJSON_AddItemToObject(mo, "area", cJSON_CreateIntArray(roi, 4));
	}
	cJSON_AddBoolToObject(cJSON_AddObjectToObject(parent, "storage"), "sdSave", g_cfg.sd_save);
	cJSON_AddBoolToObject(cJSON_AddObjectToObject(parent, "led"), "enabled", g_cfg.led);
	cJSON_AddStringToObject(cJSON_AddObjectToObject(parent, "console"), "language", g_cfg.ui_lang);
	cJSON *kvs = cJSON_AddObjectToObject(parent, "kvs");
	cJSON_AddStringToObject(kvs, "channelName", g_cfg.kvs_channel);
	cJSON_AddStringToObject(kvs, "region", g_cfg.kvs_region);
	gps_json(parent);
	cJSON_AddStringToObject(parent, "net", net_type_name()); /* CONTRACTS 3: the string stays */
	char ip[16];
	net_ip(ip, sizeof(ip));
	cJSON_AddStringToObject(parent, "localIp", ip);          /* 2a.6: the DHCP / PPP address */
	if (g_public_ip[0]) {
		cJSON_AddStringToObject(parent, "publicIp", g_public_ip); /* as FlyBot saw it (/api/device/time) */
	}
	cJSON_AddStringToObject(parent, "timezone", tz_effective_iana());
	cJSON_AddStringToObject(parent, "tzSource", tz_effective_source());
	cJSON_AddStringToObject(parent, "clockSource", clock_source());
	cJSON_AddBoolToObject(parent, "claimed", shadow_claimed());
	cJSON_AddBoolToObject(parent, "disabled", shadow_disabled());
	logfwd_json(parent);
	battery_json(parent);
	time_t next = sched_next_capture();
	if (next) {
		char ts[25];
		iso8601(next, ts);
		cJSON_AddStringToObject(parent, "nextCaptureAt", ts);
	}
}

char *shadow_reported_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", CAMOS_PROTOCOL);
	cJSON_AddNumberToObject(o, "version", shadow_version());
	cJSON_AddNumberToObject(o, "appliedAt", (double)now_ms());
	if (last_errors && cJSON_GetArraySize(last_errors) > 0) {
		cJSON_AddItemToObject(o, "errors", cJSON_Duplicate(last_errors, true));
	}
	shadow_image_json(o);
	shadow_additions_json(o);
	/* not in shadow_additions_json: the state message has its own protocol 4 gps {lat, lon, fix, ts} */
	cJSON_AddBoolToObject(cJSON_AddObjectToObject(o, "gps"), "enabled", g_cfg.gps);
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	return s;
}
