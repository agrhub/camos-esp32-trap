/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Setup portal (protocol 12.3 SoftAP), same look and flow as the AMB82 camera's setup page: Wi-Fi CAM-xxxx (last 4
 * characters of the serial, WPA2), a catch-all DNS answer so phones open the page by themselves, and the page at
 * http://192.168.4.1/ (portal.html, embedded). The radio runs APSTA, so the Wi-Fi scan happens with the SoftAP up.
 *
 *   GET  /api/config              every setting with its effective value (secrets as set / not set), the keys that
 *                                 are build defaults, serial, verify code, label QR text, claim URL, last errors
 *   POST /api/config {key: value} any subset of the portal keys (docs/PORTAL.md)
 *   GET  /api/wifi/scan[?refresh=1]  [{ssid, rssi, secure, channel}] deduplicated, strongest first (scanned at start)
 *   POST /api/connect {ssid, password, ...}  save the other keys, then join that network with the AP still up
 *                                 (APSTA): {ok, checking, ssid, timeoutS}; the network is stored once it gave an IP.
 *                                 With 4G on: save everything and restart at once (as before)
 *   GET  /api/connect             {state: idle|joining|ok|failed, ssid, reason: auth|not_found|timeout, ip, elapsedS};
 *                                 ok = the camera has an IP and restarts onto the network in a few seconds
 *   POST /api/cert {cert, key}    device certificate and private key (PEM) for the owner path
 *   POST /api/pair                join the Wi-Fi (AP stays up) and POST /api/device/pair with a provisioning token
 *   POST /api/restart
 *   GET  /api/qr.svg?what=claim|label
 *   GET  /info                    protocol 12.3 {serial, model, fw, wifiScan}
 *   POST /provision               protocol 12.3 {ssid, password, provisionToken?, endpoint?, apn?} -> {ok}, restarts
 *   GET  /                        the page; any other URL redirects there (captive portal)
 *
 * The portal closes after 10 minutes without a Connect (protocol 12.3), once a Connect got an IP, or on restart.
 * A failed check (wrong password, network not found, no IP within 30 s) keeps the portal up with the error. When the
 * phone loses the AP meanwhile (the AP follows the router's channel) and nobody comes back, the 10 minutes run out,
 * and the next boot's 75 s give-up (wifi.c) brings the portal back if the network still cannot be joined.
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "portal";
extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");
static volatile bool sta_up, sta_dropped;
static volatile uint8_t sta_reason; /* the last disconnect reason (wifi_err_reason_t) */
static char ap_ssid[16];

#define PORTAL_IDLE_MS (10 * 60 * 1000)
#define JOIN_TIMEOUT_S 30
typedef enum { JOIN_IDLE, JOIN_RUNNING, JOIN_OK, JOIN_FAILED } join_state_t;
static volatile join_state_t join_state;
static char join_ssid[33], join_pass[65], join_reason[12], join_ip[16];
static int64_t join_t0_us;

const char *portal_ssid(void)
{
	if (!ap_ssid[0]) {
		size_t n = strlen(g_cfg.serial);
		snprintf(ap_ssid, sizeof(ap_ssid), "CAM-%.4s", g_cfg.serial + (n > 4 ? n - 4 : 0));
	}
	return ap_ssid;
}

static void restart_cb(void *arg)
{
	esp_restart();
}

static void schedule_restart(int64_t ms)
{
	static esp_timer_handle_t t;
	const esp_timer_create_args_t a = { .callback = restart_cb, .name = "portal-restart" };
	if (t || esp_timer_create(&a, &t) == ESP_OK) {
		esp_timer_stop(t);
		esp_timer_start_once(t, ms * 1000);
	}
}

/* ------------------------------------------------------------------ DNS: every name resolves to 192.168.4.1 */

static void dns_task(void *arg)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ESP_LOGE(TAG, "dns bind failed");
		vTaskDelete(NULL);
		return;
	}
	uint8_t buf[256];
	for (;;) {
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
		if (n < 12) {
			continue;
		}
		/* Walk the single question to find its end and type */
		int i = 12;
		while (i < n && buf[i]) {
			i += buf[i] + 1;
		}
		if (i + 5 > n) {
			continue;
		}
		int qtype = (buf[i + 1] << 8) | buf[i + 2];
		int qend = i + 5;
		buf[2] = 0x81; /* response, recursion desired */
		buf[3] = 0x80; /* recursion available */
		buf[6] = 0;
		buf[7] = qtype == 1 ? 1 : 0; /* one answer for A, none for the rest */
		buf[8] = buf[9] = buf[10] = buf[11] = 0;
		int len = qend;
		if (qtype == 1) {
			static const uint8_t answer[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1 };
			memcpy(buf + len, answer, sizeof(answer));
			len += sizeof(answer);
		}
		sendto(s, buf, len, 0, (struct sockaddr *)&from, fl);
	}
}

/* ------------------------------------------------------------------ handlers */

static esp_err_t page_get(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html; charset=utf-8");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	return httpd_resp_send(req, portal_html_start, portal_html_end - portal_html_start - 1);
}

static esp_err_t config_get(httpd_req_t *req)
{
	cJSON *o = cJSON_CreateObject();
	store_config_json(o, false);
	cJSON_AddStringToObject(o, "timezoneAuto", tz_effective_iana());
	cJSON_AddStringToObject(o, "timezoneSource", tz_effective_source());
	return web_send_json(req, o);
}

static void sta_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		sta_up = true;
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		sta_up = false;
		sta_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
		sta_dropped = true;
	}
}

static const char *reason_kind(uint8_t r)
{
	switch (r) {
	case WIFI_REASON_AUTH_FAIL:
	case WIFI_REASON_AUTH_EXPIRE:
	case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_HANDSHAKE_TIMEOUT:
	case WIFI_REASON_MIC_FAILURE:
		return "auth";
	case WIFI_REASON_NO_AP_FOUND:
	case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
	case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
	case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
		return "not_found";
	default:
		return NULL;
	}
}

/* Join ssid on the station side while the AP keeps running. True with an IP; otherwise why = auth (refused twice:
 * wrong password), not_found (three times not seen) or timeout, and the station is left disconnected. */
static bool sta_join(const char *ssid, const char *pass, int timeout_s, char *why, size_t whyn)
{
	wifi_config_t wc = { 0 };
	strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
	wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
	esp_wifi_disconnect();
	vTaskDelay(pdMS_TO_TICKS(200));
	esp_wifi_set_config(WIFI_IF_STA, &wc);
	sta_up = sta_dropped = false;
	esp_wifi_connect();
	int auth = 0, missing = 0;
	int64_t end = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
	while (esp_timer_get_time() < end) {
		if (sta_up) {
			return true;
		}
		if (sta_dropped) {
			sta_dropped = false;
			const char *k = reason_kind(sta_reason);
			ESP_LOGW(TAG, "joining \"%s\": disconnected, reason %d (%s)", ssid, sta_reason, k ? k : "other");
			if (k && !strcmp(k, "auth") && ++auth >= 2) {
				strlcpy(why, k, whyn);
				esp_wifi_disconnect();
				return false;
			}
			if (k && !strcmp(k, "not_found") && ++missing >= 3) {
				strlcpy(why, k, whyn);
				esp_wifi_disconnect();
				return false;
			}
			vTaskDelay(pdMS_TO_TICKS(500));
			esp_wifi_connect();
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	strlcpy(why, "timeout", whyn);
	esp_wifi_disconnect();
	return false;
}

static void join_task(void *arg)
{
	char why[12] = "";
	ESP_LOGI(TAG, "connect: checking Wi-Fi \"%s\" with the setup network up", join_ssid);
	bool ok = sta_join(join_ssid, join_pass, JOIN_TIMEOUT_S, why, sizeof(why));
	if (!ok) {
		memset(join_pass, 0, sizeof(join_pass));
	}
	if (ok) {
		char ip[16];
		wifi_ip(ip, sizeof(ip));
		strlcpy(join_ip, ip, sizeof(join_ip));
		ESP_LOGI(TAG, "connect: \"%s\" gave the camera %s in %d ms: restarting onto it", join_ssid, join_ip,
				 (int)((esp_timer_get_time() - join_t0_us) / 1000));
		store_apply_kv("ssid", join_ssid); /* only a network that worked replaces the stored one */
		store_apply_kv("password", join_pass);
		memset(join_pass, 0, sizeof(join_pass));
		store_set_str("wifi_err", "");
		g_wifi_error[0] = 0;
		store_set_flag("portal", 0);
		led_set(LED_CONNECTING);
		join_state = JOIN_OK;
		schedule_restart(5000); /* the page shows the address first */
	} else {
		ESP_LOGW(TAG, "connect: could not join \"%s\" (%s): the setup portal stays", join_ssid, why);
		strlcpy(join_reason, why, sizeof(join_reason));
		store_set_str("wifi_err", join_ssid);
		strlcpy(g_wifi_error, join_ssid, sizeof(g_wifi_error));
		led_set(LED_PORTAL);
		join_state = JOIN_FAILED;
		schedule_restart(PORTAL_IDLE_MS); /* the user retypes: the 10 minutes start again */
	}
	vTaskDelete(NULL);
}

/* Start the check of a network (Connect, console `connect`). ESP_ERR_INVALID_STATE: one is running. */
esp_err_t portal_try(const char *ssid, const char *pass)
{
	if (join_state == JOIN_RUNNING) {
		return ESP_ERR_INVALID_STATE;
	}
	strlcpy(join_ssid, ssid, sizeof(join_ssid));
	strlcpy(join_pass, pass, sizeof(join_pass));
	join_reason[0] = join_ip[0] = 0;
	join_t0_us = esp_timer_get_time();
	join_state = JOIN_RUNNING;
	led_set(LED_CONNECTING);
	if (xTaskCreate(join_task, "join", 4096, NULL, 4, NULL) != pdPASS) {
		join_state = JOIN_IDLE;
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

static esp_err_t connect_get(httpd_req_t *req)
{
	static const char *const names[] = { "idle", "joining", "ok", "failed" };
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "state", names[join_state]);
	if (join_state != JOIN_IDLE) {
		cJSON_AddStringToObject(o, "ssid", join_ssid);
		cJSON_AddNumberToObject(o, "elapsedS", (int)((esp_timer_get_time() - join_t0_us) / 1000000));
		cJSON_AddNumberToObject(o, "timeoutS", JOIN_TIMEOUT_S);
	}
	if (join_state == JOIN_FAILED) {
		cJSON_AddStringToObject(o, "reason", join_reason);
	}
	if (join_state == JOIN_OK) {
		cJSON_AddStringToObject(o, "ip", join_ip);
		cJSON_AddStringToObject(o, "hostname", local_hostname());
	}
	return web_send_json(req, o);
}

/* AMB82 flow: pick a network, type the password, "Connect": the camera saves and joins that network with this
 * setup network still up, and closes it only once it has an IP; otherwise the page shows why and the user retypes.
 * With 4G on, the camera restarts at once (a SIM may be the network) and wifi.c's give-up brings the portal back. */
static esp_err_t connect_post(httpd_req_t *req)
{
	cJSON *j = web_read_json(req, 4096);
	if (!cJSON_IsObject(j)) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "BAD_PARAMS", NULL);
	}
	const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
	const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
	bool cell = g_cfg.cellular && CAMOS_MODEM;
	if ((!ssid || !ssid[0]) && !g_cfg.wifi_ssid[0] && !cell) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "CHOOSE_NETWORK", NULL);
	}
	if (ssid && ssid[0] && pass && pass[0] && strlen(pass) < 8) {
		cJSON_Delete(j);
		return web_send_error(req, "400 Bad Request", "SHORT_PASSWORD", NULL);
	}
	char bad[160] = "";
	/* the network to try: the stored one is replaced only once it gave an IP (join_task), except with 4G */
	char try_ssid[33], try_pass[65];
	strlcpy(try_ssid, ssid && ssid[0] ? ssid : g_cfg.wifi_ssid, sizeof(try_ssid));
	bool same = !strcmp(try_ssid, g_cfg.wifi_ssid);
	strlcpy(try_pass, pass && (pass[0] || !same) ? pass : (same ? g_cfg.wifi_pass : ""), sizeof(try_pass));
	const cJSON *item;
	cJSON_ArrayForEach(item, j)
	{
		char value[256];
		if (!cell && (!strcmp(item->string, "ssid") || !strcmp(item->string, "password"))) {
			continue;
		}
		if (cJSON_IsString(item)) {
			if (!strcmp(item->string, "password") && !item->valuestring[0] && !(ssid && strcmp(ssid, g_cfg.wifi_ssid))) {
				continue; /* empty = keep the stored password of the same network */
			}
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
	if (cell || !try_ssid[0]) {
		ESP_LOGI(TAG, "connect: Wi-Fi \"%s\"%s, restarting", g_cfg.wifi_ssid, cell ? " (4G first when a SIM answers)" : "");
		store_set_flag("portal", 0);
		led_set(LED_CONNECTING);
		schedule_restart(1500);
		return web_send_ok(req);
	}
	esp_err_t e = portal_try(try_ssid, try_pass);
	memset(try_pass, 0, sizeof(try_pass));
	if (e != ESP_OK) {
		return web_send_error(req, e == ESP_ERR_INVALID_STATE ? "409 Conflict" : "503 Service Unavailable", "BUSY",
							  "a check is running");
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "ok");
	cJSON_AddTrueToObject(o, "checking");
	cJSON_AddStringToObject(o, "ssid", join_ssid);
	cJSON_AddNumberToObject(o, "timeoutS", JOIN_TIMEOUT_S);
	return web_send_json(req, o);
}


/* Owner path: join the configured Wi-Fi in APSTA mode (the portal stays reachable) and pair with a provisioning
 * token. The STA channel follows the router, so the AP moves channel too: phones usually follow. */
static esp_err_t pair_post(httpd_req_t *req)
{
	cJSON *j = web_read_json(req, 512);
	const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "provisionToken"));
	if (tok && tok[0]) {
		store_apply_kv("provisionToken", tok);
	}
	cJSON_Delete(j);
	if (!g_cfg.wifi_ssid[0]) {
		return web_send_error(req, "400 Bad Request", "CHOOSE_NETWORK", "no Wi-Fi configured (over 4G the camera pairs at the next boot)");
	}
	if (!g_cfg.api_base[0] || !g_cfg.prov_token[0]) {
		return web_send_error(req, "400 Bad Request", "BAD_PARAMS", "FlyBot URL and provisioning token are needed");
	}
	if (join_state == JOIN_RUNNING) {
		return web_send_error(req, "409 Conflict", "BUSY", "a Wi-Fi check is running");
	}
	char why[12];
	if (!sta_up && !sta_join(g_cfg.wifi_ssid, g_cfg.wifi_pass, JOIN_TIMEOUT_S, why, sizeof(why))) {
		char m[64];
		snprintf(m, sizeof(m), "%.40s (%s)", g_cfg.wifi_ssid, why);
		return web_send_error(req, "502 Bad Gateway", "JOIN_FAILED", m);
	}
	time_sync_start();
	for (int i = 0; i < 20 && !clock_synced(); i++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	if (!clock_synced() && flybot_time(true) != ESP_OK) {
		return web_send_error(req, "502 Bad Gateway", "NO_TIME", "NTP and FlyBot unreachable");
	}
	char err[96];
	if (flybot_pair(err, sizeof(err)) != ESP_OK) {
		return web_send_error(req, "502 Bad Gateway", "PAIR_FAILED", err);
	}
	cJSON *o = cJSON_CreateObject();
	cJSON_AddTrueToObject(o, "ok");
	cJSON_AddStringToObject(o, "thingName", g_cfg.thing_name);
	cJSON_AddStringToObject(o, "iotHost", g_cfg.mqtt_host);
	cJSON_AddStringToObject(o, "kvsChannel", g_cfg.kvs_channel);
	return web_send_json(req, o);
}

static esp_err_t restart_post(httpd_req_t *req)
{
	store_set_flag("portal", 0);
	schedule_restart(1000);
	return web_send_ok(req);
}

/* GET /info (protocol 12.3) */
static esp_err_t info_get(httpd_req_t *req)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "serial", g_cfg.serial);
	cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "fw", fw_version());
	wifi_net_t nets[WIFI_SCAN_MAX];
	int n = wifi_scan(nets, WIFI_SCAN_MAX);
	if (n >= 0) {
		web_scan_cache_set(nets, n);
	}
	cJSON *arr = cJSON_AddArrayToObject(o, "wifiScan");
	for (int i = 0; i < n; i++) {
		cJSON *a = cJSON_CreateObject();
		cJSON_AddStringToObject(a, "ssid", nets[i].ssid);
		cJSON_AddNumberToObject(a, "rssi", nets[i].rssi);
		cJSON_AddBoolToObject(a, "secure", nets[i].secure);
		cJSON_AddItemToArray(arr, a);
	}
	return web_send_json(req, o);
}

/* POST /provision {ssid, password, provisionToken?, endpoint?, apn?} (protocol 12.3) */
static esp_err_t provision_post(httpd_req_t *req)
{
	cJSON *j = web_read_json(req, 1024);
	const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
	const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
	const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "provisionToken"));
	const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(j, "endpoint"));
	const char *apn = cJSON_GetStringValue(cJSON_GetObjectItem(j, "apn"));
	const char *api = cJSON_GetStringValue(cJSON_GetObjectItem(j, "apiBase")); /* addition: the FlyBot URL */
	bool ok = (ssid && ssid[0] && store_apply_kv("ssid", ssid) && store_apply_kv("password", pass ? pass : "")) ||
			  (apn && apn[0]);
	if (apn) {
		ok = store_apply_kv("apn", apn) && ok;
	}
	if (tok) {
		ok = store_apply_kv("provisionToken", tok) && ok;
	}
	if (endpoint) {
		ok = store_apply_kv("iotHost", endpoint) && ok;
	}
	if (api) {
		ok = store_apply_kv("apiBase", api) && ok;
	}
	cJSON_Delete(j);
	if (!ok) {
		return web_send_error(req, "400 Bad Request", "BAD_PARAMS", "ssid/password/apn");
	}
	store_set_flag("portal", 0);
	schedule_restart(1500);
	return web_send_ok(req);
}

/* Captive portal: any other URL goes to the page */
static esp_err_t redirect(httpd_req_t *req, httpd_err_code_t err)
{
	httpd_resp_set_status(req, "302 Found");
	httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
	return httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
}

static bool running;

bool portal_active(void)
{
	return running;
}

void portal_run(void)
{
	running = true;
	const char *ssid = portal_ssid();
	ESP_LOGW(TAG, "setup portal: Wi-Fi %s, open http://192.168.4.1/  (serial %s, verify code %s)", ssid, g_cfg.serial,
			 g_cfg.verify_code);
	if (g_wifi_error[0]) {
		ESP_LOGW(TAG, "last attempt: could not join \"%s\"", g_wifi_error);
	}
	led_set(LED_PORTAL);
	store_set_flag("portal", 0);
	button_task_start(); /* 10 s = factory reset works in the portal too */

	esp_netif_create_default_wifi_ap();
	local_hostname_apply(esp_netif_create_default_wifi_sta());
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); /* AP for the phone + STA for scans and pairing */
	const char *pass = g_cfg.ap_pass[0] ? g_cfg.ap_pass : CONFIG_CAMOS_AP_PASSWORD;
	wifi_config_t ap = { 0 };
	strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
	ap.ap.ssid_len = strlen(ssid);
	ap.ap.channel = 1;
	ap.ap.max_connection = 4;
	if (strlen(pass) >= 8) {
		strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
		ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
		ESP_LOGW(TAG, "portal password: %s", pass);
	} else {
		ap.ap.authmode = WIFI_AUTH_OPEN;
	}
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_event, NULL);
	ESP_ERROR_CHECK(esp_wifi_start());
	wifi_net_t nets[WIFI_SCAN_MAX];
	web_scan_cache_set(nets, wifi_scan(nets, WIFI_SCAN_MAX)); /* the list is ready when the phone opens the page */

	httpd_handle_t srv;
	httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
	hc.max_uri_handlers = 16;
	hc.lru_purge_enable = true;
	hc.stack_size = 10240; /* pairing (TLS) runs inside the handler */
	hc.recv_wait_timeout = 30;
	ESP_ERROR_CHECK(httpd_start(&srv, &hc));
	const httpd_uri_t uris[] = {
		{ .uri = "/", .method = HTTP_GET, .handler = page_get },
		{ .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
		{ .uri = "/api/config", .method = HTTP_POST, .handler = web_config_post },
		{ .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = web_scan_get },
		{ .uri = "/api/connect", .method = HTTP_POST, .handler = connect_post },
		{ .uri = "/api/connect", .method = HTTP_GET, .handler = connect_get },
		{ .uri = "/api/cert", .method = HTTP_POST, .handler = web_cert_post },
		{ .uri = "/api/pair", .method = HTTP_POST, .handler = pair_post },
		{ .uri = "/api/restart", .method = HTTP_POST, .handler = restart_post },
		{ .uri = "/api/qr.svg", .method = HTTP_GET, .handler = web_qr_get },
		{ .uri = "/info", .method = HTTP_GET, .handler = info_get },
		{ .uri = "/provision", .method = HTTP_POST, .handler = provision_post },
	};
	for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
		httpd_register_uri_handler(srv, &uris[i]);
	}
	httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, redirect);
	xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
	shadow_init();
	sched_init_lock();
	console_start(); /* the bench console works in the portal too */

	schedule_restart(PORTAL_IDLE_MS); /* protocol 12.3: the AP closes after 10 minutes */
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
}
