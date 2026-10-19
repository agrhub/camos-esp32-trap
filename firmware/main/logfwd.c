/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Remote log for cameras nobody can plug a cable into (docs/dev/PROTOCOL_ADDITIONS.md, "Remote log").
 *
 *  - Off unless FlyBot asks: RPC log.start {level, minutes, backlog} / log.stop, or shadow desired log {level, until}
 *    (a camera on battery starts it at its next wake and stays awake for it, at most 10 minutes a wake). It always ends
 *    by itself (60 minutes at most).
 *  - The lines come from the log ring the web console already keeps (web.c); every 2 s, or at 3 KB, one QoS 0 message
 *    on camos/<serial>/log: {v, session, seq, uptimeMs, at, lines: ["I (123) tag: message", ...], dropped}.
 *    4 KB/s at most (12 KB burst), 1 MB a session; a running live view goes first.
 *  - Secrets never leave the camera: the device token, Wi-Fi and portal passwords, the SIM PIN, the provisioning token
 *    and the label verify code are replaced by *** in every line (logfwd_core.c).
 *  - After a crash, a watchdog reset or a brownout, the next boot sends one `crash` event: the reason, the last 2 KB of
 *    log before the reset (RTC memory) and, when the camera has a coredump partition, the panicking task, PC and
 *    backtrace. Without MQTT it goes over HTTPS (POST /api/device/logs), and so does a short diagnosis (the last 8 KB of
 *    log) when MQTT stays down for 2 minutes while the network is up, once an hour.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif
#include "camos.h"

static const char *TAG = "logfwd";

#define BATCH_BYTES 3072
#define PERIOD_MS 2000
#define RATE_BPS 4096
#define RATE_BURST (12 * 1024)
#define SESSION_MAX (1024 * 1024)
#define BACKLOG_MAX (24 * 1024)
#define DIAG_BYTES (8 * 1024)
#define DIAG_EVERY_S 3600
#define MQTT_DOWN_S 120
#define AWAKE_MAX_S 600

static SemaphoreHandle_t lock;
static struct {
	bool on;
	bool from_shadow;
	int level;
	int64_t until_us;      /* esp_timer time the session ends */
	int64_t until_epoch;   /* the same as wall clock (epoch s), 0 when the clock was not set */
	uint32_t cursor;       /* next log ring position to send */
	uint32_t sent;         /* bytes sent this session */
	uint32_t dropped;      /* lines dropped (rate, overwritten) since the last message */
	uint32_t seq;
	char session[12];
} st;
static log_rate_t rate;
static bool crash_pending;
static bool crash_sent_http;
static int64_t last_diag_us = -1;
static int64_t mqtt_down_since_us;

/* ------------------------------------------------------------------ secrets */

static int secrets(const char *out[8])
{
	int n = 0;
	out[n++] = g_cfg.token;
	out[n++] = g_cfg.wifi_pass;
	out[n++] = g_cfg.ap_pass;
	out[n++] = g_cfg.sim_pin;
	out[n++] = g_cfg.prov_token;
	out[n++] = g_cfg.verify_code;
	return n;
}

/* A log text (several lines) with every secret replaced; in place, returns the new length */
static size_t redact_text(char *text, size_t len)
{
	const char *sec[8];
	int n = secrets(sec);
	return log_redact(text, len, sec, n);
}

/* ------------------------------------------------------------------ session */

static bool is_crash(void)
{
	const char *r = chip_reset_name();
	if (!strcmp(r, "crash") || !strcmp(r, "int_watchdog") || !strcmp(r, "task_watchdog") || !strcmp(r, "watchdog") ||
		!strcmp(r, "brownout")) {
		return true;
	}
	const char *d = boot_reason_name(); /* "watchdog" = the firmware restarted itself (no MQTT for too long) */
	return !strcmp(r, "software") && d && !strcmp(d, "watchdog");
}

esp_err_t logfwd_start(int level, int minutes, bool backlog, char *err, size_t errn)
{
	if (level < 1 || level > 5) {
		strlcpy(err, "level must be error, warn, info, debug or verbose", errn);
		return ESP_ERR_INVALID_ARG;
	}
	if (minutes < 1) {
		minutes = 1;
	} else if (minutes > 60) {
		minutes = 60;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	bool was_on = st.on;
	st.on = true;
	st.level = level;
	st.until_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000;
	st.until_epoch = clock_synced() ? (int64_t)time(NULL) + minutes * 60 : 0;
	if (!was_on) {
		uint32_t total = log_ring_total();
		st.cursor = backlog ? (total > BACKLOG_MAX ? total - BACKLOG_MAX : 0) : total;
		st.sent = 0;
		st.dropped = 0;
		st.seq = 0;
		snprintf(st.session, sizeof(st.session), "%08lx", (unsigned long)esp_random());
		log_rate_init(&rate, RATE_BPS, RATE_BURST, esp_timer_get_time() / 1000);
	}
	xSemaphoreGive(lock);
	ESP_LOGI(TAG, "remote log on: %s for %d min%s", log_level_name(level), minutes, was_on ? " (extended)" : "");
	if (power_may_deep_sleep()) {
		sched_keep_awake(minutes * 60 < AWAKE_MAX_S ? minutes * 60 : AWAKE_MAX_S);
	}
	return ESP_OK;
}

static void publish_batch(const char *text, size_t len, bool end);

void logfwd_stop(const char *why)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	bool was_on = st.on;
	st.on = false;
	st.from_shadow = false;
	st.until_epoch = 0;
	if (was_on) {
		publish_batch("", 0, true); /* the viewer learns the session ended */
	}
	xSemaphoreGive(lock);
	if (was_on) {
		ESP_LOGI(TAG, "remote log off (%s)", why);
	}
}

bool logfwd_on(void)
{
	return st.on;
}

int64_t logfwd_until_epoch(void)
{
	return st.on ? st.until_epoch : 0;
}

void logfwd_json(cJSON *parent)
{
	cJSON *o = cJSON_AddObjectToObject(parent, "log");
	cJSON_AddBoolToObject(o, "on", st.on);
	if (st.on) {
		cJSON_AddStringToObject(o, "level", log_level_name(st.level));
		if (st.until_epoch) {
			char ts[25];
			iso8601((time_t)st.until_epoch, ts);
			cJSON_AddStringToObject(o, "until", ts);
		}
		cJSON_AddNumberToObject(o, "sent", st.sent);
		cJSON_AddStringToObject(o, "session", st.session);
	}
}

/* Shadow desired log {level, until}: until (ISO 8601, UTC) in the future turns it on, null / in the past off */
void logfwd_desired(const cJSON *item, cJSON *errors)
{
	if (cJSON_IsNull(item) || cJSON_IsFalse(item)) {
		if (st.from_shadow) {
			logfwd_stop("shadow");
		}
		return;
	}
	if (!cJSON_IsObject(item)) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "path", "log");
		cJSON_AddStringToObject(e, "code", "BAD_VALUE");
		cJSON_AddItemToArray(errors, e);
		return;
	}
	const char *lvl = cJSON_GetStringValue(cJSON_GetObjectItem(item, "level"));
	const char *until = cJSON_GetStringValue(cJSON_GetObjectItem(item, "until"));
	int level = lvl ? log_level_from_name(lvl) : 3;
	time_t end = until ? parse_iso8601(until) : 0;
	if (!level || (until && !end)) {
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "path", "log");
		cJSON_AddStringToObject(e, "code", "BAD_VALUE");
		cJSON_AddItemToArray(errors, e);
		return;
	}
	if (!clock_synced()) {
		return; /* applied again with the retained document once the clock is set */
	}
	time_t now = time(NULL);
	if (!end || end <= now) {
		if (st.from_shadow) {
			logfwd_stop("shadow");
		}
		return;
	}
	int minutes = (int)((end - now + 59) / 60);
	char err[64];
	if (logfwd_start(level, minutes, true, err, sizeof(err)) == ESP_OK) {
		st.from_shadow = true;
		st.until_epoch = end;
	}
}

/* ------------------------------------------------------------------ sending */

static void publish_batch(const char *text, size_t len, bool end)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", CAMOS_PROTOCOL);
	cJSON_AddStringToObject(o, "session", st.session);
	cJSON_AddNumberToObject(o, "seq", ++st.seq);
	cJSON_AddNumberToObject(o, "uptimeMs", (double)(esp_timer_get_time() / 1000));
	if (clock_synced()) {
		cJSON_AddNumberToObject(o, "at", (double)now_ms());
	}
	cJSON_AddStringToObject(o, "level", log_level_name(st.level));
	cJSON *lines = cJSON_AddArrayToObject(o, "lines");
	const char *p = text;
	const char *stop = text + len;
	while (p < stop) {
		const char *nl = memchr(p, '\n', (size_t)(stop - p));
		size_t n = nl ? (size_t)(nl - p) : (size_t)(stop - p);
		while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) {
			n--;
		}
		if (n) {
			char line[300];
			size_t m = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
			memcpy(line, p, m);
			line[m] = 0;
			char tag[2];
			const char *msg;
			int lvl = log_parse_line(line, tag, sizeof(tag), &msg);
			/* below the chosen level: not sent; a line that is not esp_log output (console, panic) counts as info */
			if ((lvl ? lvl : 3) <= st.level) {
				cJSON_AddItemToArray(lines, cJSON_CreateString(line));
			}
		}
		p = nl ? nl + 1 : stop;
	}
	if (st.dropped) {
		cJSON_AddNumberToObject(o, "dropped", st.dropped);
	}
	if (end) {
		cJSON_AddTrueToObject(o, "end");
	}
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (s) {
		if (proto_publish_log(s)) {
			st.sent += strlen(s);
			st.dropped = 0;
		}
		free(s);
	}
}

/* One round: the next whole lines from the ring, at most BATCH_BYTES, within the rate (call with the lock). Returns
 * false when the session reached its size limit. */
static bool forward_round(void)
{
	if (!proto_connected()) {
		return true;
	}
	uint32_t total = log_ring_total();
	uint32_t behind = total - st.cursor;
	if (!behind) {
		return true;
	}
	/* a live viewer goes first, unless the ring is about to overwrite what was not sent */
	if (live_streaming() && behind < 24 * 1024) {
		return true;
	}
	char *buf = malloc(BATCH_BYTES + 1);
	if (!buf) {
		return true;
	}
	uint32_t lost = 0;
	uint32_t n = log_ring_copy(st.cursor, buf, BATCH_BYTES, &lost);
	if (lost) {
		st.dropped += lost / 80 + 1; /* lines, roughly */
		st.cursor += lost;
	}
	/* whole lines only (the rest goes next round), unless one line fills the batch */
	uint32_t whole = n;
	while (whole && buf[whole - 1] != '\n') {
		whole--;
	}
	if (!whole && n == BATCH_BYTES) {
		whole = n;
	}
	if (whole && !log_rate_take(&rate, (int)whole, esp_timer_get_time() / 1000)) {
		if (total - st.cursor > 24 * 1024) { /* too far behind: skip ahead */
			st.dropped += whole / 80 + 1;
			st.cursor += whole;
		}
		whole = 0;
	}
	if (whole) {
		buf[whole] = 0;
		size_t len = redact_text(buf, whole);
		publish_batch(buf, len, false);
		st.cursor += whole;
	}
	free(buf);
	return st.sent < SESSION_MAX;
}

/* ------------------------------------------------------------------ crash report */

static cJSON *crash_json(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "reason", chip_reset_name());
	const char *detail = boot_reason_name();
	if (detail) {
		cJSON_AddStringToObject(o, "detail", detail);
	}
	cJSON_AddStringToObject(o, "fw", fw_version());
	const char *tail = log_crash_tail();
	if (tail) {
		size_t n = strlen(tail);
		char *copy = malloc(n + 1);
		if (copy) {
			memcpy(copy, tail, n + 1);
			redact_text(copy, n);
			cJSON_AddStringToObject(o, "logTail", copy);
			free(copy);
		}
	}
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
	if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL) &&
		esp_core_dump_image_check() == ESP_OK) {
		esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
		if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
			cJSON *cd = cJSON_AddObjectToObject(o, "coredump");
			cJSON_AddStringToObject(cd, "task", sum->exc_task);
			char hex[12];
			snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)sum->exc_pc);
			cJSON_AddStringToObject(cd, "pc", hex);
#if CONFIG_IDF_TARGET_ARCH_XTENSA
			cJSON *bt = cJSON_AddArrayToObject(cd, "backtrace");
			for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
				snprintf(hex, sizeof(hex), "0x%08lx", (unsigned long)sum->exc_bt_info.bt[i]);
				cJSON_AddItemToArray(bt, cJSON_CreateString(hex));
			}
			cJSON_AddBoolToObject(cd, "corrupted", sum->exc_bt_info.corrupted);
#endif
			char sha[sizeof(sum->app_elf_sha256) + 1];
			strlcpy(sha, (const char *)sum->app_elf_sha256, sizeof(sha)); /* the first characters, as the app description has them */
			cJSON_AddStringToObject(cd, "elfSha256", sha);
		}
		free(sum);
	}
#endif
	return o;
}

static void crash_done(void)
{
	crash_pending = false;
	log_crash_tail_free();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
	if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL)) {
		esp_core_dump_image_erase(); /* reported once */
	}
#endif
}

void logfwd_connected(void)
{
	if (!crash_pending) {
		return;
	}
	char id[27];
	cJSON *e = evt_new("crash", id);
	cJSON *c = crash_json();
	cJSON *item = c->child;
	while (item) { /* the crash fields at the top of the event */
		cJSON *next = item->next;
		cJSON_DetachItemViaPointer(c, item);
		cJSON_AddItemToObject(e, item->string, item);
		item = next;
	}
	cJSON_Delete(c);
	ESP_LOGW(TAG, "crash report of the previous boot sent (%s)", chip_reset_name());
	proto_publish_event(e);
	crash_done();
}

/* Over HTTPS when MQTT does not come up: the crash report, or a diagnosis with the latest log */
static void http_report(const char *kind)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", 1);
	cJSON_AddStringToObject(o, "kind", kind);
	cJSON_AddStringToObject(o, "fw", fw_version());
	cJSON_AddNumberToObject(o, "uptimeMs", (double)(esp_timer_get_time() / 1000));
	if (!strcmp(kind, "crash")) {
		cJSON_AddItemToObject(o, "crash", crash_json());
	}
	char *buf = malloc(DIAG_BYTES + 1);
	if (buf) {
		uint32_t total = log_ring_total(), lost = 0;
		uint32_t since = total > DIAG_BYTES ? total - DIAG_BYTES : 0;
		uint32_t n = log_ring_copy(since, buf, DIAG_BYTES, &lost);
		buf[n] = 0;
		char *first = n == DIAG_BYTES ? memchr(buf, '\n', n) : NULL; /* start at a whole line */
		char *text = first ? first + 1 : buf;
		size_t len = redact_text(text, strlen(text));
		cJSON *lines = cJSON_AddArrayToObject(o, "lines");
		char *p = text, *stop = text + len;
		while (p < stop) {
			char *nl = memchr(p, '\n', (size_t)(stop - p));
			if (nl) {
				*nl = 0;
			}
			if (*p) {
				cJSON_AddItemToArray(lines, cJSON_CreateString(p));
			}
			p = nl ? nl + 1 : stop;
		}
		free(buf);
	}
	char *body = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (!body) {
		return;
	}
	char resp[128];
	int status = flybot_request("/api/device/logs", HTTP_METHOD_POST, body, resp, sizeof(resp), 20000);
	free(body);
	ESP_LOGW(TAG, "%s report over HTTPS (no MQTT): http %d", kind, status);
	if (!strcmp(kind, "crash") && status >= 200 && status < 300) {
		crash_sent_http = true;
		crash_done();
		last_diag_us = esp_timer_get_time(); /* it carried the latest log too: the hourly diagnosis waits */
	}
}

/* ------------------------------------------------------------------ task */

static void logfwd_task(void *arg)
{
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
		int64_t now = esp_timer_get_time();
		if (st.on && now >= st.until_us) {
			logfwd_stop("time is up");
		}
		if (st.on) {
			xSemaphoreTake(lock, portMAX_DELAY);
			bool more = forward_round();
			xSemaphoreGive(lock);
			if (!more) {
				logfwd_stop("1 MB sent");
			}
		}
		/* MQTT down while the network is up: crash report, then a diagnosis once an hour */
		if (proto_connected() || !net_up() || !g_cfg.token[0] || !g_cfg.api_base[0]) {
			mqtt_down_since_us = 0;
			continue;
		}
		if (!mqtt_down_since_us) {
			mqtt_down_since_us = now;
		}
		if (now - mqtt_down_since_us < (int64_t)MQTT_DOWN_S * 1000000) {
			continue;
		}
		if (crash_pending && !crash_sent_http) {
			http_report("crash");
		} else if (last_diag_us < 0 || now - last_diag_us >= (int64_t)DIAG_EVERY_S * 1000000) {
			last_diag_us = now;
			http_report("diag");
		}
	}
}

void logfwd_init(void)
{
	if (lock) {
		return;
	}
	lock = xSemaphoreCreateMutex();
	crash_pending = is_crash();
	if (!crash_pending) {
		log_crash_tail_free(); /* a normal restart: nothing to report */
	} else {
		ESP_LOGW(TAG, "previous boot ended with %s: a crash report goes to FlyBot", chip_reset_name());
	}
	xTaskCreate(logfwd_task, "logfwd", 6144, NULL, 3, NULL);
}
