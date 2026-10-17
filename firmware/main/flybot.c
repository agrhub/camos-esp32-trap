/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * FlyBot device API (flybot-aws docs/CONTRACTS.md section 2), HTTPS with the ESP-IDF certificate bundle:
 *
 *   POST {apiBase}/api/device/snapshot   multipart: image (image/jpeg, <= 2 MB) + meta (application/json)
 *                                         -> {"ok":true,"snapshotId":"...","nextWakeAt":"2026-10-16T09:00:00Z"}
 *   GET  {apiBase}/api/device/time       -> {"now":"...","epoch":1792396800}
 *   GET  {apiBase}/api/device/ota        -> {"fw":"0.2.0","url":"https://...bin","sha256":"..."} or 204
 *   POST {apiBase}/api/device/pair       {"serial","verifyCode","provisionToken"} -> deviceToken, iot, kvs
 *
 * Headers on every call: X-Device-Token (when paired) and X-Device-Serial. The token is only ever sent to apiBase.
 * The JPEG is streamed straight from the PSRAM frame buffer in 4 KB writes.
 */
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "flybot";
#define MAX_UPLOAD_BYTES 2000000 /* CONTRACTS: <= 2 MB, 413 above */
#define RESP_MAX 1024

static esp_http_client_handle_t open_client(const char *path, esp_http_client_method_t method, int timeout_ms)
{
	char url[320];
	snprintf(url, sizeof(url), "%s%s", g_cfg.api_base, path);
	esp_http_client_config_t cfg = {
		.url = url,
		.method = method,
		.timeout_ms = timeout_ms,
		.buffer_size = 1024,
		.buffer_size_tx = 1024,
	};
	if (strncmp(url, "https://", 8) == 0) {
		cfg.crt_bundle_attach = esp_crt_bundle_attach;
	}
	esp_http_client_handle_t h = esp_http_client_init(&cfg);
	if (!h) {
		return NULL;
	}
	esp_http_client_set_header(h, "X-Device-Serial", g_cfg.serial);
	if (g_cfg.token[0]) {
		esp_http_client_set_header(h, "X-Device-Token", g_cfg.token);
	}
	esp_http_client_set_header(h, "User-Agent", "camos-esp32-trap/" CONFIG_CAMOS_MODEL);
	return h;
}

/* Sends an optional JSON body, reads the response; returns the HTTP status or -1 */
int flybot_request(const char *path, esp_http_client_method_t method, const char *body, char *resp, size_t respn,
				   int timeout_ms)
{
	if (!g_cfg.api_base[0]) {
		return -1;
	}
	esp_http_client_handle_t h = open_client(path, method, timeout_ms);
	if (!h) {
		return -1;
	}
	int blen = body ? (int)strlen(body) : 0;
	if (body) {
		esp_http_client_set_header(h, "Content-Type", "application/json");
	}
	esp_err_t err = esp_http_client_open(h, blen);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "%s: connect %s", path, esp_err_to_name(err));
		esp_http_client_cleanup(h);
		return -1;
	}
	if (body && esp_http_client_write(h, body, blen) != blen) {
		esp_http_client_cleanup(h);
		return -1;
	}
	esp_http_client_fetch_headers(h);
	int status = esp_http_client_get_status_code(h);
	int n = esp_http_client_read_response(h, resp, (int)respn - 1);
	resp[n > 0 ? n : 0] = 0;
	esp_http_client_cleanup(h);
	if (status == 401 && strcmp(path, "/api/device/register") && strcmp(path, "/api/device/pair") &&
		strcmp(path, "/api/device/ota")) {
		register_check_identity("device API 401");
	}
	return status;
}

/* ------------------------------------------------------------------ snapshot */

cJSON *flybot_meta_json(const char *trigger, time_t captured_at)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddNumberToObject(m, "v", 1);
	cJSON_AddStringToObject(m, "serial", g_cfg.serial);
	char ts[25];
	iso8601(captured_at, ts);
	cJSON_AddStringToObject(m, "capturedAt", ts);
	cJSON_AddStringToObject(m, "trigger", trigger);
	if (battery_mv() >= 0) {
		cJSON *b = cJSON_AddObjectToObject(m, "battery");
		cJSON_AddNumberToObject(b, "mv", battery_mv());
		cJSON_AddNumberToObject(b, "pct", battery_pct());
		cJSON_AddBoolToObject(b, "charging", battery_charging());
		if (solar_mv() >= 0) {
			cJSON_AddNumberToObject(b, "solarMv", solar_mv());
		}
	}
	cJSON_AddStringToObject(m, "net", net_type_name());
	cJSON_AddNumberToObject(m, "rssi", net_rssi());
	gps_json(m); /* location {lat, lng, fixAt} when a fix exists */
	cJSON_AddStringToObject(m, "fw", fw_version());
	cJSON_AddStringToObject(m, "model", CONFIG_CAMOS_MODEL);
	return m;
}

static esp_err_t snapshot_attempt(const camera_fb_t *fb, const char *meta, upload_t *out)
{
	char boundary[40];
	snprintf(boundary, sizeof(boundary), "----camos%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
	char head_meta[200], head_img[200], tail[64];
	int n_meta = snprintf(head_meta, sizeof(head_meta),
						  "--%s\r\nContent-Disposition: form-data; name=\"meta\"\r\nContent-Type: application/json\r\n\r\n", boundary);
	int n_img = snprintf(head_img, sizeof(head_img),
						 "\r\n--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"snapshot.jpg\"\r\n"
						 "Content-Type: image/jpeg\r\n\r\n", boundary);
	int n_tail = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
	int total = n_meta + (int)strlen(meta) + n_img + (int)fb->len + n_tail;

	esp_http_client_handle_t h = open_client("/api/device/snapshot", HTTP_METHOD_POST, 60000);
	if (!h) {
		strlcpy(out->error, "http init", sizeof(out->error));
		return ESP_FAIL;
	}
	char ctype[80];
	snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
	esp_http_client_set_header(h, "Content-Type", ctype);
	esp_err_t err = esp_http_client_open(h, total);
	if (err != ESP_OK) {
		snprintf(out->error, sizeof(out->error), "connect: %s", esp_err_to_name(err));
		esp_http_client_cleanup(h);
		return err;
	}
	bool ok = esp_http_client_write(h, head_meta, n_meta) == n_meta &&
			  esp_http_client_write(h, meta, (int)strlen(meta)) == (int)strlen(meta) &&
			  esp_http_client_write(h, head_img, n_img) == n_img;
	for (size_t off = 0; ok && off < fb->len;) {
		size_t chunk = fb->len - off > 4096 ? 4096 : fb->len - off;
		int w = esp_http_client_write(h, (const char *)fb->buf + off, (int)chunk);
		if (w <= 0) {
			ok = false;
			break;
		}
		off += (size_t)w;
	}
	ok = ok && esp_http_client_write(h, tail, n_tail) == n_tail;
	if (!ok) {
		strlcpy(out->error, "write", sizeof(out->error));
		esp_http_client_cleanup(h);
		return ESP_FAIL;
	}
	esp_http_client_fetch_headers(h);
	int status = esp_http_client_get_status_code(h);
	char body[RESP_MAX];
	int n = esp_http_client_read_response(h, body, sizeof(body) - 1);
	body[n > 0 ? n : 0] = 0;
	esp_http_client_cleanup(h);

	if (status == 401) {
		register_check_identity("snapshot upload 401");
	}
	if (status != 200 && status != 201) {
		snprintf(out->error, sizeof(out->error), "http %d", status); /* 401 bad token, 413 too large */
		return ESP_FAIL;
	}
	cJSON *j = cJSON_Parse(body);
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(j, "snapshotId"));
	if (!cJSON_IsTrue(cJSON_GetObjectItem(j, "ok")) || !id || strlen(id) >= sizeof(out->key)) {
		cJSON_Delete(j);
		strlcpy(out->error, "bad response", sizeof(out->error));
		return ESP_FAIL;
	}
	strlcpy(out->key, id, sizeof(out->key));
	const char *nw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "nextWakeAt"));
	out->next_wake_at = nw ? parse_iso8601(nw) : 0;
	cJSON_Delete(j);
	return ESP_OK;
}

esp_err_t flybot_snapshot(const camera_fb_t *fb, const char *trigger, upload_t *out)
{
	memset(out, 0, sizeof(*out));
	if (!g_cfg.api_base[0]) {
		strlcpy(out->error, "no api base", sizeof(out->error));
		return ESP_ERR_INVALID_STATE;
	}
	if (fb->len > MAX_UPLOAD_BYTES) {
		strlcpy(out->error, "too large", sizeof(out->error));
		return ESP_ERR_INVALID_SIZE;
	}
	if (jpeg_dims(fb->buf, fb->len, &out->w, &out->h) != 0) {
		out->w = fb->width;
		out->h = fb->height;
	}
	out->bytes = fb->len;
	cJSON *m = flybot_meta_json(trigger, fb->timestamp.tv_sec > 1735689600 ? fb->timestamp.tv_sec : time(NULL));
	char *meta = cJSON_PrintUnformatted(m);
	cJSON_Delete(m);
	if (!meta) {
		strlcpy(out->error, "no memory", sizeof(out->error));
		return ESP_ERR_NO_MEM;
	}
	/* Back-off 2, 4, 8 s (+ jitter); a client error (4xx) will not get better by retrying */
	esp_err_t err = ESP_FAIL;
	for (int tries = 1; tries <= 4 && err != ESP_OK; tries++) {
		err = snapshot_attempt(fb, meta, out);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "snapshot attempt %d: %s", tries, out->error);
			if (!strncmp(out->error, "http 4", 6)) {
				break;
			}
			if (tries < 4) {
				vTaskDelay(pdMS_TO_TICKS((1000 << tries) + esp_random() % 1000));
			}
		}
	}
	free(meta);
	if (err == ESP_OK) {
		char nw[25] = "-";
		if (out->next_wake_at) {
			iso8601(out->next_wake_at, nw);
		}
		ESP_LOGI(TAG, "snapshot %dx%d, %u bytes -> %s (nextWakeAt %s)", out->w, out->h, (unsigned)out->bytes, out->key, nw);
	}
	return err;
}

/* ------------------------------------------------------------------ pairing */

esp_err_t flybot_pair(char *err, size_t errn)
{
	err[0] = 0;
	if (!g_cfg.api_base[0]) {
		strlcpy(err, "no FlyBot URL", errn);
		return ESP_ERR_INVALID_STATE;
	}
	if (!g_cfg.prov_token[0]) {
		strlcpy(err, "no provisioning token", errn);
		return ESP_ERR_INVALID_STATE;
	}
	cJSON *b = cJSON_CreateObject();
	cJSON_AddStringToObject(b, "serial", g_cfg.serial);
	cJSON_AddStringToObject(b, "verifyCode", g_cfg.verify_code);
	cJSON_AddStringToObject(b, "provisionToken", g_cfg.prov_token);
	cJSON_AddStringToObject(b, "model", CONFIG_CAMOS_MODEL); /* additive, ignored by a strict server */
	cJSON_AddStringToObject(b, "fw", fw_version());
	char *body = cJSON_PrintUnformatted(b);
	cJSON_Delete(b);
	char resp[RESP_MAX];
	int status = flybot_request("/api/device/pair", HTTP_METHOD_POST, body, resp, sizeof(resp), 20000);
	free(body);
	if (status < 0) {
		strlcpy(err, "no connection to FlyBot", errn);
		return ESP_FAIL;
	}
	cJSON *j = cJSON_Parse(resp);
	if (status != 200 || !cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"))) {
		const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(j, "error"));
		snprintf(err, errn, "http %d%s%s", status, msg ? ": " : "", msg ? msg : "");
		cJSON_Delete(j);
		return ESP_FAIL;
	}
	const char *token = cJSON_GetStringValue(cJSON_GetObjectItem(j, "deviceToken"));
	cJSON *iot = cJSON_GetObjectItem(j, "iot"), *kvs = cJSON_GetObjectItem(j, "kvs");
	const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(iot, "endpoint"));
	const char *thing = cJSON_GetStringValue(cJSON_GetObjectItem(iot, "thingName"));
	if (!token || !endpoint) {
		strlcpy(err, "pair response without deviceToken / iot.endpoint", errn);
		cJSON_Delete(j);
		return ESP_FAIL;
	}
	bool ok = store_apply_kv("token", token) && store_apply_kv("iotHost", endpoint);
	if (thing) {
		store_apply_kv("thingName", thing);
	}
	const char *chan = cJSON_GetStringValue(cJSON_GetObjectItem(kvs, "channelName"));
	const char *region = cJSON_GetStringValue(cJSON_GetObjectItem(kvs, "region"));
	if (chan) {
		store_apply_kv("kvsChannel", chan);
	}
	if (region) {
		store_apply_kv("kvsRegion", region);
	}
	/* optional: the credential provider for live view (iot.credentialsEndpoint, iot.roleAlias) */
	const char *cred = cJSON_GetStringValue(cJSON_GetObjectItem(iot, "credentialsEndpoint"));
	const char *alias = cJSON_GetStringValue(cJSON_GetObjectItem(iot, "roleAlias"));
	if (cred) {
		store_apply_kv("credHost", cred);
	}
	if (alias) {
		store_apply_kv("roleAlias", alias);
	}
	cJSON_Delete(j);
	if (!ok) {
		strlcpy(err, "token or endpoint too long", errn);
		return ESP_FAIL;
	}
	store_apply_kv("provisionToken", ""); /* used once */
	store_apply_kv("claimed", "1"); /* the owner path: paired to the account that issued the token */
	ESP_LOGI(TAG, "paired: thing %s, iot %s, kvs %s", g_cfg.thing_name, g_cfg.mqtt_host, g_cfg.kvs_channel);
	return ESP_OK;
}

/* ------------------------------------------------------------------ time */

/* GET /api/device/time (CONTRACTS 2 and 2a.6, no auth, works before registration): the clock when set_clock (the last
 * source of the chain), and always the timezone (IANA + POSIX + source) and the public IP FlyBot saw. */
esp_err_t flybot_time(bool set_clock)
{
	char resp[512];
	int status = flybot_request("/api/device/time", HTTP_METHOD_GET, NULL, resp, sizeof(resp), 10000);
	if (status != 200) {
		return ESP_FAIL;
	}
	cJSON *j = cJSON_Parse(resp);
	cJSON *epoch = cJSON_GetObjectItem(j, "epoch");
	time_t t = cJSON_IsNumber(epoch) ? (time_t)epoch->valuedouble : parse_iso8601(cJSON_GetStringValue(cJSON_GetObjectItem(j, "now")));
	const char *iana = cJSON_GetStringValue(cJSON_GetObjectItem(j, "timezone"));
	const char *posix = cJSON_GetStringValue(cJSON_GetObjectItem(j, "posix"));
	const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tzSource"));
	const char *pub = cJSON_GetStringValue(cJSON_GetObjectItem(j, "publicIp"));
	if (pub) {
		strlcpy(g_public_ip, pub, sizeof(g_public_ip));
	}
	if (iana || posix) {
		tz_auto_set(iana, posix, src);
	}
	cJSON_Delete(j);
	if (!set_clock) {
		return ESP_OK;
	}
	if (t < 1735689600) {
		return ESP_FAIL;
	}
	struct timeval tv = { .tv_sec = t };
	settimeofday(&tv, NULL);
	return ESP_OK;
}

/* ------------------------------------------------------------------ OTA check */

bool flybot_ota_check(char *fw, size_t fwn, char *url, size_t urln, char *sha256, size_t shan)
{
	char resp[RESP_MAX];
	int status = flybot_request("/api/device/ota", HTTP_METHOD_GET, NULL, resp, sizeof(resp), 10000);
	if (status != 200) {
		return false; /* 204: nothing to apply */
	}
	cJSON *j = cJSON_Parse(resp);
	const char *f = cJSON_GetStringValue(cJSON_GetObjectItem(j, "fw"));
	const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(j, "url"));
	const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sha256"));
	bool ok = f && u && s && strlen(f) < fwn && strlen(u) < urln && strlen(s) < shan;
	if (ok) {
		strcpy(fw, f);
		strcpy(url, u);
		strcpy(sha256, s);
	}
	cJSON_Delete(j);
	return ok;
}
