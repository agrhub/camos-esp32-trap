/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * HTTP pieces shared by the setup portal and the local web console, and the console itself.
 *
 * Local web console (CAMOS_WEB_CONSOLE, Wi-Fi only): http://<camera IP>/ serves console.html, the same layout as the
 * CamOS camera console (sign-in gate, Live view / Configuration, grouped side menu). The first visit sets the admin
 * password ("activate", 8-64 characters with letters and digits); it is kept as a salted, iterated SHA-256 in NVS.
 * Sessions are a random cookie (30 min idle); every POST needs the header X-Requested-With: camos (no cross-site
 * forms); 5 wrong passwords lock sign-in for 5 minutes. Forgot the password: hold the setup button 3 s (it clears the
 * password and opens the portal), or `factory` on the serial console.
 *
 *   GET  /api/session                 {activated, signedIn, model, serial, fw, uiLang}
 *   POST /api/activate {password}     first use only        POST /api/login {password}     POST /api/logout
 *   POST /api/password {current, new}
 *   GET  /api/status                  device, time, net, cloud, power, battery, location, led
 *   GET  /api/config  POST /api/config (portal keys)          POST /api/cert {cert, key}
 *   GET  /api/wifi/scan[?refresh=1]   [{ssid, rssi, secure, channel}]
 *   GET  /api/image                   {video: {image}, capture, sensor}
 *   POST /api/image {image: {...video.image keys}, jpegQuality, save}   save false = preview only; {restore: true}
 *   GET  /api/stream?w=&h=&q=         MJPEG live view (multipart/x-mixed-replace), one viewer at a time (stream.c)
 *   GET  /api/frame.jpg?w=640&h=480   one preview frame        POST /api/snapshot   capture + upload (trigger rpc)
 *   GET  /api/schedule                {source, items, next}    GET  /api/events     recent events, newest first
 *   GET  /api/sd  GET /api/sd/files?day=  GET /api/sd/file?day=&name=[&dl=1]  POST /api/sd/delete  POST /api/sd/format
 *   POST /api/firmware                application/octet-stream app image, optional X-Fw-Sha256 header; restarts
 *   POST /api/pair  POST /api/gps  POST /api/ota  POST /api/reboot  POST /api/factory-reset
 *   GET  /api/logs?since=N            {text, next, lost}
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mbedtls/sha256.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "web";

/* ------------------------------------------------------------------ shared helpers */

esp_err_t web_send_json(httpd_req_t *req, cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	esp_err_t e = httpd_resp_sendstr(req, s ? s : "{}");
	free(s);
	return e;
}

esp_err_t web_send_ok(httpd_req_t *req)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "ok");
	return web_send_json(req, o);
}

esp_err_t web_send_error(httpd_req_t *req, const char *status, const char *code, const char *message)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddFalseToObject(o, "ok");
	cJSON_AddStringToObject(o, "error", code);
	cJSON_AddStringToObject(o, "message", message ? message : code);
	httpd_resp_set_status(req, status);
	return web_send_json(req, o);
}

cJSON *web_read_json(httpd_req_t *req, size_t max)
{
	if (req->content_len == 0 || req->content_len > max) {
		return NULL;
	}
	char *buf = malloc(req->content_len + 1);
	if (!buf) {
		return NULL;
	}
	size_t got = 0;
	while (got < req->content_len) {
		int r = httpd_req_recv(req, buf + got, req->content_len - got);
		if (r <= 0) {
			free(buf);
			return NULL;
		}
		got += r;
	}
	buf[got] = 0;
	cJSON *j = cJSON_Parse(buf);
	free(buf);
	return j;
}

esp_err_t web_config_post(httpd_req_t *req)
{
	cJSON *j = web_read_json(req, 2048);
	if (!cJSON_IsObject(j)) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "BAD_PARAMS", "JSON object expected");
	}
	const cJSON *item;
	char bad[160] = "";
	cJSON_ArrayForEach(item, j)
	{
		char value[256];
		if (cJSON_IsString(item)) {
			strlcpy(value, item->valuestring, sizeof(value));
		} else if (cJSON_IsNumber(item)) {
			snprintf(value, sizeof(value), "%d", (int)item->valuedouble);
		} else if (cJSON_IsBool(item)) {
			strlcpy(value, cJSON_IsTrue(item) ? "1" : "0", sizeof(value));
		} else {
			continue;
		}
		if (!store_apply_kv(item->string, value)) {
			strlcat(bad, bad[0] ? ", " : "", sizeof(bad));
			strlcat(bad, item->string, sizeof(bad));
		}
	}
	cJSON_Delete(j);
	if (bad[0]) {
		return web_send_error(req, "400 Bad Request", "BAD_VALUE", bad);
	}
	ESP_LOGI(TAG, "configuration saved");
	return web_send_ok(req);
}

esp_err_t web_cert_post(httpd_req_t *req)
{
	cJSON *j = web_read_json(req, 12000);
	const char *cert = cJSON_GetStringValue(cJSON_GetObjectItem(j, "cert"));
	const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(j, "key"));
	bool ok = store_set_pem(cert, key);
	cJSON_Delete(j);
	if (!ok) {
		return web_send_error(req, "400 Bad Request", "BAD_PEM", "cert must be a CERTIFICATE PEM and key a PRIVATE KEY PEM (each < 6000 bytes)");
	}
	ESP_LOGI(TAG, "certificate and key stored");
	return web_send_ok(req);
}

/* Scan results cached between requests: a phone opening the page gets the boot scan at once */
static wifi_net_t scan_cache[WIFI_SCAN_MAX];
static int scan_count = -1;
static int64_t scan_at_us;

void web_scan_cache_set(const wifi_net_t *nets, int n)
{
	if (n >= 0) {
		memcpy(scan_cache, nets, sizeof(wifi_net_t) * n);
		scan_count = n;
		scan_at_us = esp_timer_get_time();
	}
}

esp_err_t web_scan_get(httpd_req_t *req)
{
	char q[32] = "", v[4] = "";
	bool refresh = httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "refresh", v, sizeof(v)) == ESP_OK && v[0] == '1';
	if (refresh || scan_count < 0 || esp_timer_get_time() - scan_at_us > 120LL * 1000000) {
		static wifi_net_t nets[WIFI_SCAN_MAX];
		int n = wifi_scan(nets, WIFI_SCAN_MAX);
		if (n < 0 && scan_count < 0) {
			return web_send_error(req, "503 Service Unavailable", "NOT_AVAILABLE", "Wi-Fi is not running (4G mode)");
		}
		web_scan_cache_set(nets, n);
	}
	return web_send_json(req, wifi_scan_json(scan_cache, scan_count < 0 ? 0 : scan_count));
}

/* ------------------------------------------------------------------ log ring buffer (/api/logs) */

#define LOG_RING (32 * 1024)
static char *ring;
static volatile uint32_t ring_total; /* bytes ever written; position = total % LOG_RING */
static portMUX_TYPE ring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t prev_vprintf;

static int log_hook(const char *fmt, va_list ap)
{
	char line[256];
	va_list cp;
	va_copy(cp, ap);
	int n = vsnprintf(line, sizeof(line), fmt, cp);
	va_end(cp);
	if (n > 0 && ring) {
		if (n > (int)sizeof(line) - 1) {
			n = sizeof(line) - 1;
		}
		portENTER_CRITICAL(&ring_mux);
		for (int i = 0; i < n; i++) {
			if (line[i] == 0x1b) { /* drop colour escape sequences */
				while (i < n && line[i] != 'm') {
					i++;
				}
				continue;
			}
			ring[ring_total % LOG_RING] = line[i];
			ring_total++;
		}
		portEXIT_CRITICAL(&ring_mux);
	}
	return prev_vprintf ? prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void log_capture_start(void)
{
	if (ring) {
		return;
	}
	ring = heap_caps_malloc(LOG_RING, MALLOC_CAP_SPIRAM);
	if (ring) {
		prev_vprintf = esp_log_set_vprintf(log_hook);
	}
}

#if CONFIG_CAMOS_WEB_CONSOLE

extern const char console_html_start[] asm("_binary_console_html_start");
extern const char console_html_end[] asm("_binary_console_html_end");

/* ------------------------------------------------------------------ admin password and sessions */

#define PW_ROUNDS 2000
#define MAX_SESSIONS 4
#define SESSION_IDLE_S (30 * 60)
typedef struct {
	char id[33];
	int64_t last_us;
} session_t;
static session_t sessions[MAX_SESSIONS];
static int fails;
static int64_t locked_until_us;
static httpd_handle_t srv;

static void hex(const uint8_t *in, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", in[i]);
	}
}

static void pw_hash(const char *salt_hex, const char *pw, char out[65])
{
	uint8_t d[32];
	mbedtls_sha256_context c;
	mbedtls_sha256_init(&c);
	mbedtls_sha256_starts(&c, 0);
	mbedtls_sha256_update(&c, (const uint8_t *)salt_hex, strlen(salt_hex));
	mbedtls_sha256_update(&c, (const uint8_t *)pw, strlen(pw));
	mbedtls_sha256_finish(&c, d);
	for (int i = 1; i < PW_ROUNDS; i++) {
		mbedtls_sha256_starts(&c, 0);
		mbedtls_sha256_update(&c, d, sizeof(d));
		mbedtls_sha256_update(&c, (const uint8_t *)salt_hex, strlen(salt_hex));
		mbedtls_sha256_finish(&c, d);
	}
	mbedtls_sha256_free(&c);
	hex(d, sizeof(d), out);
}

static bool get_nvs_str(const char *key, char *out, size_t n);

static bool activated(void)
{
	char h[66];
	return get_nvs_str("admin_hash", h, sizeof(h)) && strlen(h) == 64;
}

#include "nvs.h"
static bool get_nvs_str(const char *key, char *out, size_t n)
{
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) != ESP_OK) {
		return false;
	}
	size_t len = n;
	bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
	nvs_close(h);
	return ok;
}

static bool pw_strong(const char *pw)
{
	size_t n = pw ? strlen(pw) : 0;
	bool letter = false, digit = false;
	for (size_t i = 0; i < n; i++) {
		if ((pw[i] >= 'a' && pw[i] <= 'z') || (pw[i] >= 'A' && pw[i] <= 'Z')) {
			letter = true;
		} else if (pw[i] >= '0' && pw[i] <= '9') {
			digit = true;
		}
	}
	return n >= 8 && n <= 64 && letter && digit;
}

static void pw_set(const char *pw)
{
	uint8_t s[16];
	esp_fill_random(s, sizeof(s));
	char salt[33], h[65];
	hex(s, sizeof(s), salt);
	pw_hash(salt, pw, h);
	store_set_str("admin_salt", salt);
	store_set_str("admin_hash", h);
}

static bool pw_check(const char *pw)
{
	char salt[34], want[66], got[65];
	if (!pw || !get_nvs_str("admin_salt", salt, sizeof(salt)) || !get_nvs_str("admin_hash", want, sizeof(want))) {
		return false;
	}
	pw_hash(salt, pw, got);
	unsigned diff = 0; /* constant time */
	for (int i = 0; i < 64; i++) {
		diff |= (unsigned)(got[i] ^ want[i]);
	}
	return diff == 0;
}

static const char *session_new(void)
{
	int slot = 0;
	for (int i = 1; i < MAX_SESSIONS; i++) {
		if (sessions[i].last_us < sessions[slot].last_us) {
			slot = i;
		}
	}
	uint8_t r[16];
	esp_fill_random(r, sizeof(r));
	hex(r, sizeof(r), sessions[slot].id);
	sessions[slot].last_us = esp_timer_get_time();
	return sessions[slot].id;
}

/* A new firmware image that a signed-in browser reached over the network has proven itself (a camera without the
 * cloud would otherwise roll back at the next restart; proto.c does the same on the first MQTT connect) */
static void mark_valid_once(void)
{
	static bool done;
	if (!done) {
		done = true;
		esp_ota_mark_app_valid_cancel_rollback();
	}
}

/* CAMOS_WEB_BENCH_IP: the bench computer is treated as signed in (empty in every image that leaves the bench) */
static bool bench_client(httpd_req_t *req)
{
	const char *bench = CONFIG_CAMOS_WEB_BENCH_IP;
	if (!bench[0]) {
		return false;
	}
	struct sockaddr_storage a;
	socklen_t l = sizeof(a);
	char ip[48] = "";
	if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) != 0) {
		return false;
	}
	if (a.ss_family == AF_INET) {
		inet_ntop(AF_INET, &((struct sockaddr_in *)&a)->sin_addr, ip, sizeof(ip));
	} else if (a.ss_family == AF_INET6) {
		inet_ntop(AF_INET, &((struct sockaddr_in6 *)&a)->sin6_addr.s6_addr[12], ip, sizeof(ip)); /* v4-mapped */
	}
	return !strcmp(ip, bench);
}

static bool session_ok(httpd_req_t *req)
{
	if (bench_client(req)) {
		sched_keep_awake(120);
		mark_valid_once();
		return true;
	}
	char sid[40];
	size_t n = sizeof(sid);
	if (httpd_req_get_cookie_val(req, "camos_sid", sid, &n) != ESP_OK || strlen(sid) != 32) {
		return false;
	}
	int64_t now = esp_timer_get_time();
	for (int i = 0; i < MAX_SESSIONS; i++) {
		if (sessions[i].id[0] && !strcmp(sessions[i].id, sid) && now - sessions[i].last_us < (int64_t)SESSION_IDLE_S * 1000000) {
			sessions[i].last_us = now;
			sched_keep_awake(120); /* battery mode: someone is looking */
			mark_valid_once();
			return true;
		}
	}
	return false;
}

static void session_drop(httpd_req_t *req)
{
	char sid[40];
	size_t n = sizeof(sid);
	if (httpd_req_get_cookie_val(req, "camos_sid", sid, &n) == ESP_OK) {
		for (int i = 0; i < MAX_SESSIONS; i++) {
			if (!strcmp(sessions[i].id, sid)) {
				memset(&sessions[i], 0, sizeof(sessions[i]));
			}
		}
	}
}

static esp_err_t set_session_cookie(httpd_req_t *req, const char *sid)
{
	static char cookie[96]; /* httpd keeps the pointer until the response is sent */
	snprintf(cookie, sizeof(cookie), "camos_sid=%s; Path=/; HttpOnly; SameSite=Strict", sid);
	return httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

/* Guards for the API: a POST needs X-Requested-With: camos; everything but session/login needs a session */
static bool csrf_ok(httpd_req_t *req)
{
	char v[16];
	return req->method != HTTP_POST || (httpd_req_get_hdr_value_str(req, "X-Requested-With", v, sizeof(v)) == ESP_OK && !strcmp(v, "camos"));
}

#define GUARD(req)                                                                                            \
	do {                                                                                                      \
		if (!csrf_ok(req)) {                                                                                  \
			return web_send_error(req, "403 Forbidden", "NOT_ALLOWED", "missing X-Requested-With");           \
		}                                                                                                     \
		if (!session_ok(req)) {                                                                               \
			return web_send_error(req, "401 Unauthorized", "LOGIN_REQUIRED", "sign in");                      \
		}                                                                                                     \
	} while (0)

/* ------------------------------------------------------------------ handlers */

extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[] asm("_binary_logo_png_end");

/* The CamOS logo (www/logo.png, 48x48): page header and favicon of the console and the setup portal, no sign-in */
esp_err_t web_logo_get(httpd_req_t *req)
{
	httpd_resp_set_type(req, "image/png");
	httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
	return httpd_resp_send(req, (const char *)logo_png_start, logo_png_end - logo_png_start);
}

static esp_err_t page_get(httpd_req_t *req)
{
	sched_keep_awake(120); /* battery mode: someone opened the console (also before signing in) */
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, console_html_start, console_html_end - console_html_start - 1);
}

static esp_err_t session_get(httpd_req_t *req)
{
	sched_keep_awake(120); /* the page pings this every 30 s while it is open: stay awake for it */
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "activated", activated());
	cJSON_AddBoolToObject(o, "signedIn", session_ok(req));
	cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "serial", g_cfg.serial);
	cJSON_AddStringToObject(o, "fw", fw_version());
	cJSON_AddStringToObject(o, "uiLang", g_cfg.ui_lang); /* the gate is drawn in the saved language */
	return web_send_json(req, o);
}

/* POST /api/lang {uiLang}: the page language, also before sign-in (not a secret, changes nothing else) */
static esp_err_t lang_post(httpd_req_t *req)
{
	if (!csrf_ok(req)) {
		return web_send_error(req, "403 Forbidden", "NOT_ALLOWED", NULL);
	}
	cJSON *j = web_read_json(req, 128);
	const char *l = cJSON_GetStringValue(cJSON_GetObjectItem(j, "uiLang"));
	bool ok = l && store_apply_kv("uiLang", l);
	cJSON_Delete(j);
	return ok ? web_send_ok(req) : web_send_error(req, "400 Bad Request", "BAD_VALUE", "uiLang");
}

static esp_err_t activate_post(httpd_req_t *req)
{
	if (!csrf_ok(req)) {
		return web_send_error(req, "403 Forbidden", "NOT_ALLOWED", NULL);
	}
	if (activated()) {
		return web_send_error(req, "409 Conflict", "NOT_ALLOWED", "already activated");
	}
	cJSON *j = web_read_json(req, 256);
	const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
	if (!pw_strong(pw)) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "WEAK_PASSWORD", NULL);
	}
	pw_set(pw);
	cJSON_Delete(j);
	ESP_LOGI(TAG, "admin password set (activated)");
	set_session_cookie(req, session_new());
	return web_send_ok(req);
}

static esp_err_t login_post(httpd_req_t *req)
{
	if (!csrf_ok(req)) {
		return web_send_error(req, "403 Forbidden", "NOT_ALLOWED", NULL);
	}
	if (esp_timer_get_time() < locked_until_us) {
		return web_send_error(req, "429 Too Many Requests", "LOCKED", NULL);
	}
	cJSON *j = web_read_json(req, 256);
	const char *pw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
	bool ok = pw_check(pw);
	cJSON_Delete(j);
	if (!ok) {
		if (++fails >= 5) {
			fails = 0;
			locked_until_us = esp_timer_get_time() + 5LL * 60 * 1000000;
			ESP_LOGW(TAG, "5 wrong passwords: sign-in locked for 5 minutes");
		}
		return web_send_error(req, "401 Unauthorized", "BAD_LOGIN", NULL);
	}
	fails = 0;
	set_session_cookie(req, session_new());
	return web_send_ok(req);
}

static esp_err_t logout_post(httpd_req_t *req)
{
	session_drop(req);
	return web_send_ok(req);
}

static esp_err_t password_post(httpd_req_t *req)
{
	GUARD(req);
	cJSON *j = web_read_json(req, 512);
	const char *cur = cJSON_GetStringValue(cJSON_GetObjectItem(j, "current"));
	const char *nw = cJSON_GetStringValue(cJSON_GetObjectItem(j, "new"));
	esp_err_t e;
	if (!pw_check(cur)) {
		e = web_send_error(req, "400 Bad Request", "BAD_PASSWORD", NULL);
	} else if (!pw_strong(nw)) {
		e = web_send_error(req, "400 Bad Request", "WEAK_PASSWORD", NULL);
	} else {
		pw_set(nw);
		e = web_send_ok(req);
	}
	cJSON_Delete(j);
	return e;
}

static void add_time(cJSON *o)
{
	cJSON *t = cJSON_AddObjectToObject(o, "time");
	cJSON_AddBoolToObject(t, "synced", clock_synced());
	if (clock_synced()) {
		time_t now = time(NULL);
		struct tm lt;
		localtime_r(&now, &lt);
		char s[32];
		strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &lt);
		cJSON_AddStringToObject(t, "local", s);
		char u[25];
		iso8601(now, u);
		cJSON_AddStringToObject(t, "utc", u);
	}
	cJSON_AddStringToObject(t, "timezone", tz_effective_iana());
}

static esp_err_t status_get(httpd_req_t *req)
{
	GUARD(req);
	cJSON *o = cJSON_CreateObject();
	const esp_app_desc_t *d = esp_app_get_description();
	cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "serial", g_cfg.serial);
	cJSON_AddStringToObject(o, "verifyCode", g_cfg.verify_code);
	cJSON_AddStringToObject(o, "fw", d->version);
	char built[40];
	snprintf(built, sizeof(built), "%s %s", d->date, d->time);
	cJSON_AddStringToObject(o, "built", built);
	cJSON_AddNumberToObject(o, "uptimeS", (double)(esp_timer_get_time() / 1000000));
	cJSON_AddNumberToObject(o, "heapFreeKB", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
	cJSON_AddNumberToObject(o, "psramFreeKB", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
	add_time(o);

	cJSON *n = cJSON_AddObjectToObject(o, "net");
	cJSON_AddStringToObject(n, "type", net_type_name());
	cJSON_AddBoolToObject(n, "connected", net_up());
	char ip[16];
	net_ip(ip, sizeof(ip));
	cJSON_AddStringToObject(n, "ip", ip);
	cJSON_AddStringToObject(n, "hostname", local_hostname()); /* <hostname>.local over mDNS on Wi-Fi */
	cJSON_AddNumberToObject(n, "rssi", net_rssi());
	if (net_type() == NET_WIFI) {
		wifi_ap_record_t ap;
		if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
			cJSON_AddStringToObject(n, "ssid", (const char *)ap.ssid);
			cJSON_AddNumberToObject(n, "channel", ap.primary);
		} else {
			cJSON_AddStringToObject(n, "ssid", g_cfg.wifi_ssid);
		}
		esp_netif_ip_info_t info;
		esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
		if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
			char m[16], g[16];
			snprintf(m, sizeof(m), IPSTR, IP2STR(&info.netmask));
			snprintf(g, sizeof(g), IPSTR, IP2STR(&info.gw));
			cJSON_AddStringToObject(n, "mask", m);
			cJSON_AddStringToObject(n, "gateway", g);
		}
		uint8_t mac[6];
		esp_read_mac(mac, ESP_MAC_WIFI_STA);
		char ms[18];
		snprintf(ms, sizeof(ms), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		cJSON_AddStringToObject(n, "mac", ms);
	}
	net_cell_json(n);
	cJSON_AddStringToObject(n, "apn", g_cfg.apn);

	cJSON *c = cJSON_AddObjectToObject(o, "cloud");
	cJSON_AddBoolToObject(c, "connected", proto_connected());
	cJSON_AddStringToObject(c, "host", g_cfg.mqtt_host);
	cJSON_AddNumberToObject(c, "port", 8883);
	cJSON_AddBoolToObject(c, "certificate", g_cfg.cert && g_cfg.key);
	cJSON_AddBoolToObject(c, "paired", store_paired());
	cJSON_AddStringToObject(c, "thing", g_cfg.thing_name);
	cJSON_AddStringToObject(c, "apiBase", g_cfg.api_base);
	cJSON_AddStringToObject(c, "kvsChannel", g_cfg.kvs_channel);
	cJSON *lv = cJSON_AddObjectToObject(o, "live"); /* cloud live view (KVS): state, viewer, frame rate */
	cJSON_AddBoolToObject(lv, "supported", live_supported());
	cJSON_AddBoolToObject(lv, "window", live_active());
	live_status_json(lv);

	settings_t s;
	shadow_get(&s);
	cJSON *img = cJSON_AddObjectToObject(o, "image");
	cJSON_AddNumberToObject(img, "brightness", s.brightness);
	cJSON_AddNumberToObject(img, "contrast", s.contrast);
	cJSON_AddNumberToObject(img, "saturation", s.saturation);
	cJSON_AddNumberToObject(img, "jpegQuality", s.jpeg_quality);
	cJSON_AddNumberToObject(img, "shadowVersion", s.version);
	int w, h;
	cam_max_size(&w, &h);
	cJSON_AddNumberToObject(img, "w", w);
	cJSON_AddNumberToObject(img, "h", h);

	battery_json(o);
	power_json(o);
	gps_json(o);
	cJSON_AddStringToObject(o, "timezone", tz_effective_iana());
	cJSON_AddStringToObject(o, "tzSource", tz_effective_source());
	cJSON_AddStringToObject(o, "clockSource", clock_source());
	cJSON_AddStringToObject(o, "publicIp", g_public_ip);
	cJSON_AddBoolToObject(o, "claimed", shadow_claimed());
	cJSON_AddNumberToObject(o, "intervalMin", shadow_interval_min());
	time_t next = sched_next_capture();
	if (next) {
		char ts[25];
		iso8601(next, ts);
		cJSON_AddStringToObject(o, "nextCaptureAt", ts);
	}
	cJSON *l = cJSON_AddObjectToObject(o, "led");
	cJSON_AddStringToObject(l, "state", led_state_name(led_get()));
	cJSON_AddBoolToObject(l, "enabled", led_enabled());
	cJSON_AddStringToObject(img, "sensor", cam_sensor_name());
	cam_info_json(cJSON_AddObjectToObject(o, "camera")); /* the stream and photo size menus */
	stream_stats_t ss;
	stream_get_stats(&ss);
	cJSON *sj = cJSON_AddObjectToObject(o, "stream");
	cJSON_AddBoolToObject(sj, "active", stream_active());
	cJSON_AddNumberToObject(sj, "w", ss.w);
	cJSON_AddNumberToObject(sj, "h", ss.h);
	cJSON_AddNumberToObject(sj, "quality", ss.quality);
	cJSON_AddNumberToObject(sj, "fps", (int)(ss.fps * 10 + 0.5f) / 10.0);
	cJSON_AddNumberToObject(sj, "avgBytes", ss.avg_bytes);
	cJSON_AddNumberToObject(sj, "frames", ss.frames);
	motion_json(o);
	cJSON *sd = cJSON_AddObjectToObject(o, "sd");
	uint64_t total = 0, free_b = 0;
	cJSON_AddBoolToObject(sd, "present", sd_present());
	if (sd_usage(&total, &free_b)) {
		cJSON_AddNumberToObject(sd, "totalMB", (double)(total >> 20));
		cJSON_AddNumberToObject(sd, "freeMB", (double)(free_b >> 20));
	}
	cJSON_AddBoolToObject(sd, "save", g_cfg.sd_save);
	cJSON *f = cJSON_AddObjectToObject(o, "features");
	cJSON_AddBoolToObject(f, "modem", CAMOS_MODEM);
	cJSON_AddBoolToObject(f, "gps", gps_available());
	cJSON_AddBoolToObject(f, "live", live_supported());
#if CONFIG_CAMOS_SD
	cJSON_AddBoolToObject(f, "sd", true);
#else
	cJSON_AddBoolToObject(f, "sd", false);
#endif
	return web_send_json(req, o);
}

static esp_err_t config_get(httpd_req_t *req)
{
	GUARD(req);
	cJSON *o = cJSON_CreateObject();
	store_config_json(o, false);
	return web_send_json(req, o);
}

static esp_err_t config_post(httpd_req_t *req)
{
	GUARD(req);
	return web_config_post(req);
}

static esp_err_t cert_post(httpd_req_t *req)
{
	GUARD(req);
	return web_cert_post(req);
}

static esp_err_t scan_get(httpd_req_t *req)
{
	GUARD(req);
	return web_scan_get(req);
}

/* Local image settings go through the shadow code so the same ranges and errors apply; the cloud overrides them.
 * save false: applied to the running camera only (the Image page previews while a slider moves); restore: back to
 * the saved values. */
static esp_err_t image_get(httpd_req_t *req)
{
	GUARD(req);
	cJSON *o = cJSON_CreateObject();
	shadow_image_json(o);
	cJSON_AddStringToObject(o, "sensor", cam_sensor_name());
	cJSON_AddNumberToObject(o, "shadowVersion", shadow_version());
	return web_send_json(req, o);
}

static esp_err_t image_post(httpd_req_t *req)
{
	GUARD(req);
	cJSON *j = web_read_json(req, 1024);
	if (!cJSON_IsObject(j)) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "BAD_PARAMS", NULL);
	}
	if (cJSON_IsTrue(cJSON_GetObjectItem(j, "restore"))) {
		cJSON_Delete(j);
		shadow_reload();
		return image_get(req);
	}
	bool save = !cJSON_IsFalse(cJSON_GetObjectItem(j, "save"));
	cJSON *doc = cJSON_CreateObject();
	cJSON *in = cJSON_GetObjectItem(j, "image");
	cJSON *video = cJSON_AddObjectToObject(doc, "video");
	if (cJSON_IsObject(in)) {
		cJSON_AddItemToObject(video, "image", cJSON_Duplicate(in, true));
	} else {
		/* the 0.3 page: brightness, contrast, saturation at the top level */
		cJSON *img = cJSON_AddObjectToObject(video, "image");
		const char *keys[] = { "brightness", "contrast", "saturation" };
		for (int i = 0; i < 3; i++) {
			cJSON *v = cJSON_GetObjectItem(j, keys[i]);
			if (cJSON_IsNumber(v)) {
				cJSON_AddNumberToObject(img, keys[i], v->valuedouble);
			}
		}
	}
	cJSON *q = cJSON_GetObjectItem(j, "jpegQuality");
	if (cJSON_IsNumber(q)) {
		cJSON *cap = cJSON_AddObjectToObject(doc, "capture");
		cJSON_AddNumberToObject(cap, "jpegQuality", q->valuedouble);
	}
	cJSON_Delete(j);
	char *s = cJSON_PrintUnformatted(doc);
	cJSON_Delete(doc);
	char *reported = s ? shadow_apply(s, strlen(s), save) : NULL;
	free(s);
	if (!reported) {
		return web_send_error(req, "500 Internal Server Error", "INTERNAL", NULL);
	}
	if (save) {
		proto_publish_reported(reported);
		events_note("image", "image settings saved in the console");
	}
	cJSON *r = cJSON_Parse(reported);
	free(reported);
	cJSON *errors = cJSON_DetachItemFromObject(r, "errors");
	cJSON_Delete(r);
	if (errors && cJSON_GetArraySize(errors) > 0) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddFalseToObject(o, "ok");
		cJSON_AddStringToObject(o, "error", "BAD_VALUE");
		char paths[160] = "";
		const cJSON *e;
		cJSON_ArrayForEach(e, errors)
		{
			const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(e, "path"));
			if (p) {
				strlcat(paths, paths[0] ? ", " : "", sizeof(paths));
				strlcat(paths, p, sizeof(paths));
			}
		}
		cJSON_AddStringToObject(o, "message", paths);
		cJSON_AddItemToObject(o, "errors", errors);
		httpd_resp_set_status(req, "400 Bad Request");
		return web_send_json(req, o);
	}
	cJSON_Delete(errors);
	return web_send_ok(req);
}

static esp_err_t stream_get(httpd_req_t *req)
{
	GUARD(req);
	return web_stream_get(req);
}

static esp_err_t frame_get(httpd_req_t *req)
{
	GUARD(req);
	char q[48] = "", v[8];
	int w = 640, h = 480;
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		if (httpd_query_key_value(q, "w", v, sizeof(v)) == ESP_OK) {
			w = atoi(v);
		}
		if (httpd_query_key_value(q, "h", v, sizeof(v)) == ESP_OK) {
			h = atoi(v);
		}
	}
	if (!frame_size_ok(w, h)) {
		w = 640, h = 480;
	}
	camera_fb_t *fb = NULL;
	esp_err_t e = preview_frame(w, h, 12, &fb);
	if (e != ESP_OK) {
		return web_send_error(req, "503 Service Unavailable", e == ESP_ERR_INVALID_STATE ? "BUSY" : "NO_PREVIEW", esp_err_to_name(e));
	}
	httpd_resp_set_type(req, "image/jpeg");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	e = httpd_resp_send(req, (const char *)fb->buf, fb->len);
	preview_release(fb);
	return e;
}

static esp_err_t snapshot_post(httpd_req_t *req)
{
	GUARD(req);
	upload_t up;
	esp_err_t e = capture_now("rpc", &up);
	cJSON *o = cJSON_CreateObject();
	cJSON_AddBoolToObject(o, "ok", e == ESP_OK);
	cJSON_AddNumberToObject(o, "w", up.w);
	cJSON_AddNumberToObject(o, "h", up.h);
	cJSON_AddNumberToObject(o, "bytes", (double)up.bytes);
	if (up.sd_file[0]) {
		cJSON_AddStringToObject(o, "sdFile", up.sd_file);
	}
	if (e == ESP_OK) {
		cJSON_AddStringToObject(o, "snapshotId", up.key);
	} else {
		cJSON_AddStringToObject(o, "error", up.bytes ? "UPLOAD_FAILED" : "CAMERA");
		cJSON_AddStringToObject(o, "message", up.error);
	}
	return web_send_json(req, o);
}

static esp_err_t pair_post(httpd_req_t *req)
{
	GUARD(req);
	char err[96];
	if (flybot_pair(err, sizeof(err)) != ESP_OK) {
		return web_send_error(req, "502 Bad Gateway", "PAIR_FAILED", err);
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "ok");
	cJSON_AddStringToObject(o, "thingName", g_cfg.thing_name);
	cJSON_AddBoolToObject(o, "restartRequired", true);
	return web_send_json(req, o);
}

static void gps_task(void *arg)
{
	gps_fix(CONFIG_CAMOS_GPS_FIX_TIMEOUT_S, NULL);
	proto_publish_reported_now();
	vTaskDelete(NULL);
}

static esp_err_t gps_post(httpd_req_t *req)
{
	GUARD(req);
	if (!gps_available()) {
		return web_send_error(req, "400 Bad Request", "NOT_SUPPORTED", NULL);
	}
	sched_keep_awake(CONFIG_CAMOS_GPS_FIX_TIMEOUT_S + 30);
	xTaskCreate(gps_task, "gps-web", 4096, NULL, 3, NULL);
	return web_send_ok(req); /* the page polls /api/status for the new location */
}

static void ota_task(void *arg)
{
	char fw[32], url[256], sha[80], err[96];
	if (flybot_ota_check(fw, sizeof(fw), url, sizeof(url), sha, sizeof(sha)) && strcmp(fw, fw_version())) {
		if (ota_apply(url, sha, err, sizeof(err)) == ESP_OK) {
			remember_reboot_reason("ota");
			esp_restart();
		}
		ESP_LOGE(TAG, "ota: %s", err);
	} else {
		ESP_LOGI(TAG, "ota: nothing newer offered by GET /api/device/ota");
	}
	vTaskDelete(NULL);
}

static esp_err_t ota_post(httpd_req_t *req)
{
	GUARD(req);
	sched_keep_awake(600);
	xTaskCreate(ota_task, "ota-web", 8192, NULL, 3, NULL);
	return web_send_ok(req); /* progress in the log page */
}

static void restart_later(void *arg)
{
	esp_restart();
}

static esp_err_t reboot_post(httpd_req_t *req)
{
	GUARD(req);
	static esp_timer_handle_t t;
	const esp_timer_create_args_t a = { .callback = restart_later, .name = "web-restart" };
	if (t || esp_timer_create(&a, &t) == ESP_OK) {
		esp_timer_start_once(t, 1000000);
	}
	remember_reboot_reason("rpc");
	return web_send_ok(req);
}

static esp_err_t factory_post(httpd_req_t *req)
{
	GUARD(req);
	store_erase_all(); /* also the admin password: the next visit activates again */
	return reboot_post(req);
}

static esp_err_t qr_get(httpd_req_t *req)
{
	GUARD(req);
	return web_qr_get(req);
}

static esp_err_t logs_get(httpd_req_t *req)
{
	GUARD(req);
	char q[32] = "", v[16] = "";
	uint32_t since = 0;
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
		since = strtoul(v, NULL, 10);
	}
	uint32_t total = ring_total;
	bool lost = false;
	if (since > total) {
		since = 0;
	}
	if (total - since > LOG_RING) {
		since = total - LOG_RING;
		lost = since > 0;
	}
	uint32_t n = total - since;
	char *text = malloc(n + 1);
	if (!text) {
		return web_send_error(req, "500 Internal Server Error", "INTERNAL", NULL);
	}
	portENTER_CRITICAL(&ring_mux);
	for (uint32_t i = 0; i < n; i++) {
		text[i] = ring ? ring[(since + i) % LOG_RING] : 0;
	}
	portEXIT_CRITICAL(&ring_mux);
	text[n] = 0;
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "text", text);
	free(text);
	cJSON_AddNumberToObject(o, "next", total);
	cJSON_AddBoolToObject(o, "lost", lost);
	return web_send_json(req, o);
}

static esp_err_t schedule_get(httpd_req_t *req)
{
	GUARD(req);
	cJSON *o = sched_json(8);
	cJSON_AddNumberToObject(o, "intervalMin", shadow_interval_min());
	cJSON_AddNumberToObject(o, "schedFrom", g_cfg.sched_from);
	cJSON_AddNumberToObject(o, "schedTo", g_cfg.sched_to);
	cJSON_AddStringToObject(o, "powerEffective", power_mode_name(power_effective()));
	return web_send_json(req, o);
}

static esp_err_t events_get(httpd_req_t *req)
{
	GUARD(req);
	return web_send_json(req, events_json());
}

/* ------------------------------------------------------------------ microSD (Playback and Storage pages) */

static void query_arg(httpd_req_t *req, const char *key, char *out, size_t n)
{
	char q[96];
	out[0] = 0;
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		httpd_query_key_value(q, key, out, n);
	}
}

static esp_err_t sd_get(httpd_req_t *req)
{
	GUARD(req);
	cJSON *o = cJSON_CreateObject();
	uint64_t total = 0, free_b = 0;
	bool ok = sd_usage(&total, &free_b);
	cJSON_AddBoolToObject(o, "present", sd_present());
	cJSON_AddStringToObject(o, "card", sd_card_name());
	if (ok) {
		cJSON_AddNumberToObject(o, "totalMB", (double)(total >> 20));
		cJSON_AddNumberToObject(o, "freeMB", (double)(free_b >> 20));
	}
	cJSON_AddBoolToObject(o, "save", g_cfg.sd_save);
	cJSON_AddItemToObject(o, "days", sd_list_days());
	return web_send_json(req, o);
}

static esp_err_t sd_files_get(httpd_req_t *req)
{
	GUARD(req);
	char day[12];
	query_arg(req, "day", day, sizeof(day));
	return web_send_json(req, sd_list_files(day));
}

static esp_err_t sd_file_get(httpd_req_t *req)
{
	GUARD(req);
	char day[12], name[16], dl[4];
	query_arg(req, "day", day, sizeof(day));
	query_arg(req, "name", name, sizeof(name));
	query_arg(req, "dl", dl, sizeof(dl));
	return sd_serve(req, day, name, dl[0] == '1');
}

static esp_err_t sd_delete_post(httpd_req_t *req)
{
	GUARD(req);
	cJSON *j = web_read_json(req, 256);
	const char *day = cJSON_GetStringValue(cJSON_GetObjectItem(j, "day"));
	const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
	esp_err_t e = day ? sd_delete(day, name) : ESP_ERR_INVALID_ARG;
	char line[48];
	snprintf(line, sizeof(line), "deleted %s%s%s", day ? day : "", name ? "/" : "", name ? name : "");
	cJSON_Delete(j);
	if (e != ESP_OK) {
		return web_send_error(req, "400 Bad Request", "BAD_VALUE", "day / name");
	}
	events_note("sd", line);
	return web_send_ok(req);
}

static esp_err_t sd_format_post(httpd_req_t *req)
{
	GUARD(req);
	esp_err_t e = sd_format();
	if (e != ESP_OK) {
		return web_send_error(req, "500 Internal Server Error", e == ESP_ERR_INVALID_STATE ? "NO_CARD" : "INTERNAL", esp_err_to_name(e));
	}
	events_note("sd", "card formatted");
	return web_send_ok(req);
}

/* ------------------------------------------------------------------ firmware upload (System > Maintenance) */

static esp_err_t firmware_post(httpd_req_t *req)
{
	GUARD(req);
	const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
	if (!part) {
		return web_send_error(req, "500 Internal Server Error", "NO_OTA_SLOT", NULL);
	}
	if (req->content_len < 64 * 1024 || req->content_len > part->size) {
		return web_send_error(req, "400 Bad Request", "BAD_IMAGE", "size");
	}
	char want[72] = "";
	httpd_req_get_hdr_value_str(req, "X-Fw-Sha256", want, sizeof(want));
	sched_keep_awake(600);
	led_set(LED_OTA);
	char *buf = malloc(4096);
	esp_ota_handle_t h = 0;
	esp_err_t e = buf ? esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) : ESP_ERR_NO_MEM;
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts(&sha, 0);
	size_t got = 0;
	const char *why = NULL;
	ESP_LOGI(TAG, "firmware upload: %u bytes into %s", (unsigned)req->content_len, part->label);
	while (e == ESP_OK && got < req->content_len) {
		int r = httpd_req_recv(req, buf, 4096);
		if (r == HTTPD_SOCK_ERR_TIMEOUT) {
			continue;
		}
		if (r <= 0) {
			why = "upload interrupted";
			e = ESP_FAIL;
			break;
		}
		if (got == 0 && (uint8_t)buf[0] != 0xE9) {
			why = "not an ESP32 app image";
			e = ESP_ERR_INVALID_ARG;
			break;
		}
		mbedtls_sha256_update(&sha, (const uint8_t *)buf, r);
		e = esp_ota_write(h, buf, r);
		got += r;
	}
	free(buf);
	uint8_t d[32];
	char hexd[65];
	mbedtls_sha256_finish(&sha, d);
	mbedtls_sha256_free(&sha);
	hex(d, sizeof(d), hexd);
	if (e == ESP_OK) {
		e = esp_ota_end(h); /* checks the image (and its signature when secure boot is on) */
		h = 0;
		if (e != ESP_OK) {
			why = "image check failed";
		}
	} else if (h) {
		esp_ota_abort(h);
	}
	if (e == ESP_OK && want[0] && strcasecmp(want, hexd)) {
		why = "SHA-256 differs";
		e = ESP_ERR_INVALID_CRC;
	}
	if (e == ESP_OK) {
		e = esp_ota_set_boot_partition(part);
	}
	if (e != ESP_OK) {
		led_set(proto_connected() ? LED_ONLINE : LED_CONNECTING);
		ESP_LOGE(TAG, "firmware upload failed: %s (%s)", why ? why : "write", esp_err_to_name(e));
		events_note("firmware", why ? why : "upload failed");
		return web_send_error(req, "400 Bad Request", "BAD_IMAGE", why ? why : esp_err_to_name(e));
	}
	ESP_LOGW(TAG, "firmware upload OK (sha256 %s), restarting into %s", hexd, part->label);
	events_note("firmware", "new firmware uploaded, restarting");
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "ok");
	cJSON_AddStringToObject(o, "sha256", hexd);
	cJSON_AddNumberToObject(o, "bytes", (double)got);
	web_send_json(req, o);
	remember_reboot_reason("ota");
	static esp_timer_handle_t t;
	const esp_timer_create_args_t a = { .callback = restart_later, .name = "fw-restart" };
	if (t || esp_timer_create(&a, &t) == ESP_OK) {
		esp_timer_start_once(t, 1500000);
	}
	return ESP_OK;
}

void web_start(void)
{
	if (srv) {
		return;
	}
	httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
	hc.max_uri_handlers = 40;
	hc.max_open_sockets = 8; /* LWIP_MAX_SOCKETS 16: 8 + 3 for the server + MQTT, HTTPS, SNTP, DNS */
	hc.lru_purge_enable = true; /* a live stream (async request) is never the one purged */
	hc.stack_size = 10240; /* snapshot (TLS upload) and pairing run inside handlers */
	hc.recv_wait_timeout = 15;
	hc.send_wait_timeout = 15;
	if (httpd_start(&srv, &hc) != ESP_OK) {
		ESP_LOGE(TAG, "console server did not start");
		srv = NULL;
		return;
	}
	const httpd_uri_t uris[] = {
		{ .uri = "/", .method = HTTP_GET, .handler = page_get },
		{ .uri = "/index.html", .method = HTTP_GET, .handler = page_get },
		{ .uri = "/logo.png", .method = HTTP_GET, .handler = web_logo_get },
		{ .uri = "/favicon.ico", .method = HTTP_GET, .handler = web_logo_get },
		{ .uri = "/api/session", .method = HTTP_GET, .handler = session_get },
		{ .uri = "/api/lang", .method = HTTP_POST, .handler = lang_post },
		{ .uri = "/api/activate", .method = HTTP_POST, .handler = activate_post },
		{ .uri = "/api/login", .method = HTTP_POST, .handler = login_post },
		{ .uri = "/api/logout", .method = HTTP_POST, .handler = logout_post },
		{ .uri = "/api/password", .method = HTTP_POST, .handler = password_post },
		{ .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
		{ .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
		{ .uri = "/api/config", .method = HTTP_POST, .handler = config_post },
		{ .uri = "/api/cert", .method = HTTP_POST, .handler = cert_post },
		{ .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = scan_get },
		{ .uri = "/api/image", .method = HTTP_GET, .handler = image_get },
		{ .uri = "/api/image", .method = HTTP_POST, .handler = image_post },
		{ .uri = "/api/stream", .method = HTTP_GET, .handler = stream_get },
		{ .uri = "/api/frame.jpg", .method = HTTP_GET, .handler = frame_get },
		{ .uri = "/api/schedule", .method = HTTP_GET, .handler = schedule_get },
		{ .uri = "/api/events", .method = HTTP_GET, .handler = events_get },
		{ .uri = "/api/sd", .method = HTTP_GET, .handler = sd_get },
		{ .uri = "/api/sd/files", .method = HTTP_GET, .handler = sd_files_get },
		{ .uri = "/api/sd/file", .method = HTTP_GET, .handler = sd_file_get },
		{ .uri = "/api/sd/delete", .method = HTTP_POST, .handler = sd_delete_post },
		{ .uri = "/api/sd/format", .method = HTTP_POST, .handler = sd_format_post },
		{ .uri = "/api/firmware", .method = HTTP_POST, .handler = firmware_post },
		{ .uri = "/api/snapshot", .method = HTTP_POST, .handler = snapshot_post },
		{ .uri = "/api/pair", .method = HTTP_POST, .handler = pair_post },
		{ .uri = "/api/gps", .method = HTTP_POST, .handler = gps_post },
		{ .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post },
		{ .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post },
		{ .uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_post },
		{ .uri = "/api/logs", .method = HTTP_GET, .handler = logs_get },
		{ .uri = "/api/qr.svg", .method = HTTP_GET, .handler = qr_get },
	};
	for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
		httpd_register_uri_handler(srv, &uris[i]);
	}
	char ip[16];
	net_ip(ip, sizeof(ip));
	ESP_LOGI(TAG, "web console at http://%s/ (%s)", ip, activated() ? "admin password set" : "first visit sets the admin password");
	if (CONFIG_CAMOS_WEB_BENCH_IP[0]) {
		ESP_LOGW(TAG, "BENCH BUILD: requests from %s skip the sign-in (CAMOS_WEB_BENCH_IP)", CONFIG_CAMOS_WEB_BENCH_IP);
	}
}

#else
void web_start(void) {}
#endif
