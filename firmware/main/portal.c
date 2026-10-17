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
 *   POST /api/connect {ssid, password, ...}  save everything and restart onto that network (AMB82 "Connect")
 *   POST /api/cert {cert, key}    device certificate and private key (PEM) for the owner path
 *   POST /api/pair                join the Wi-Fi (AP stays up) and POST /api/device/pair with a provisioning token
 *   POST /api/restart
 *   GET  /api/qr.svg?what=claim|label
 *   GET  /info                    protocol 12.3 {serial, model, fw, wifiScan}
 *   POST /provision               protocol 12.3 {ssid, password, provisionToken?, endpoint?, apn?} -> {ok}, restarts
 *   GET  /                        the page; any other URL redirects there (captive portal)
 *
 * The portal closes after 10 minutes (protocol 12.3) or on connect / restart.
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
static volatile bool sta_up;
static char ap_ssid[16];

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

/* AMB82 flow: pick a network, type the password, "Connect": the camera saves, closes this network and joins.
 * A wrong password brings the portal back in about a minute with the error (wifi.c). */
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
	const cJSON *item;
	cJSON_ArrayForEach(item, j)
	{
		char value[256];
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
	ESP_LOGI(TAG, "connect: Wi-Fi \"%s\"%s, restarting", g_cfg.wifi_ssid, cell ? " (4G first when a SIM answers)" : "");
	store_set_flag("portal", 0);
	led_set(LED_CONNECTING);
	schedule_restart(1500);
	return web_send_ok(req);
}

static void sta_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		sta_up = true;
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		sta_up = false;
	}
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
	static bool sta_configured;
	if (!sta_configured) {
		esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event, NULL);
		esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sta_event, NULL);
		sta_configured = true;
	}
	wifi_config_t wc = { 0 };
	strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof(wc.sta.password));
	wc.sta.threshold.authmode = g_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
	esp_wifi_set_config(WIFI_IF_STA, &wc);
	esp_wifi_disconnect();
	sta_up = false;
	esp_wifi_connect();
	for (int i = 0; i < 30 && !sta_up; i++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	if (!sta_up) {
		return web_send_error(req, "502 Bad Gateway", "JOIN_FAILED", g_cfg.wifi_ssid);
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

void portal_run(void)
{
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

	schedule_restart(10 * 60 * 1000); /* protocol 12.3: the AP closes after 10 minutes */
	for (;;) {
		vTaskDelay(portMAX_DELAY);
	}
}
