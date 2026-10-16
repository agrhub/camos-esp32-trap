/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Network manager: 4G (SIM present and APN set, esp_modem PPP) else Wi-Fi. MQTT to IoT Core and HTTPS to FlyBot
 * run on whichever is up. Time: SNTP first; when nothing answers within TIME_SNTP_WAIT_S (some carriers block UDP
 * 123 on 4G) the firmware asks FlyBot (GET /api/device/time).
 *
 * The choice is made once per boot: a modem that does not answer (no module, no SIM, no registration within the
 * modem's own timeout) falls back to Wi-Fi for this boot. A `network_switch` event is published when that happens.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "net";
#define TIME_SNTP_WAIT_S 15

static net_type_t chosen = NET_NONE;
static bool started, sntp_started, fallback_wifi;
static void (*up_cb)(void);

void net_set_up_cb(void (*cb)(void)) { up_cb = cb; }
bool net_started(void) { return started; }
net_type_t net_type(void) { return chosen; }

const char *net_type_name(void)
{
	return chosen == NET_4G ? "4g" : chosen == NET_WIFI ? "wifi" : "none";
}

bool net_up(void)
{
	return chosen == NET_4G ? modem_up() : chosen == NET_WIFI ? wifi_up() : false;
}

int net_rssi(void)
{
	return chosen == NET_4G ? modem_rssi() : wifi_rssi();
}

void net_ip(char *out, size_t n)
{
	if (chosen == NET_4G) {
		modem_ip(out, n);
	} else {
		wifi_ip(out, n);
	}
}

void net_cell_json(cJSON *net)
{
	if (chosen == NET_4G) {
		modem_cell_json(net);
	}
}

static void web_start_task(void *arg)
{
	web_start(); /* local console (CAMOS_WEB_CONSOLE) */
	vTaskDelete(NULL);
}

static void on_up(void)
{
	if (!proto_provisioned() && !register_pending()) {
		led_set(LED_ERROR); /* on the network, no certificate and no registration going on: cannot reach the cloud */
	}
	if (chosen == NET_WIFI) {
		/* not in the event task: httpd_start and NVS reads overflow its small stack */
		xTaskCreate(web_start_task, "web-start", 4096, NULL, 3, NULL);
	}
	time_sync_start();
	if (up_cb) {
		up_cb();
	}
}

/* ------------------------------------------------------------------ time */

/* CONTRACTS 2a.6 clock chain, the first that answers wins: SNTP (pool.ntp.org, time.google.com) -> modem network
 * time (AT+CCLK? with NITZ, read by modem.c while it registers) -> GPS RMC time -> GET /api/device/time. A deep
 * sleep keeps the RTC ("rtc"), and SNTP still resyncs it. Then /api/device/time once more for the timezone, the POSIX
 * string and the public IP (it works before registration, without auth). */
static void time_task(void *arg)
{
	if (clock_synced() && !strcmp(clock_source(), "none")) {
		clock_set_source("rtc");
	}
	for (int round = 0;; round++) {
		if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SNTP_WAIT_S * 1000)) == ESP_OK) {
			break; /* sntp_synced() recorded it */
		}
		if (clock_synced() && strcmp(clock_source(), "rtc")) {
			break; /* the modem or the GPS set it meanwhile */
		}
		if (round == 0 && !clock_synced() && gps_available()) {
			ESP_LOGW(TAG, "NTP did not answer: trying GPS time");
			if (gps_fix(45, NULL) && clock_synced()) {
				break; /* gps.c logged "clock set from gps" */
			}
		}
		if (net_up() && g_cfg.api_base[0] && flybot_time(!clock_synced() || !strcmp(clock_source(), "rtc")) == ESP_OK &&
			clock_synced()) {
			if (strcmp(clock_source(), "rtc") && strcmp(clock_source(), "none")) {
				break;
			}
			clock_set_source("flybot");
			break;
		}
		if (clock_synced() && round >= 1) {
			break; /* RTC is good enough when nothing else answers */
		}
		vTaskDelay(pdMS_TO_TICKS(round < 3 ? 5000 : 30000));
	}
	/* timezone + public IP (the clock is valid now, so TLS works) */
	for (int i = 0; i < 3 && g_cfg.api_base[0]; i++) {
		if (net_up() && flybot_time(false) == ESP_OK) {
			ESP_LOGI(TAG, "timezone %s from %s, public IP %s", tz_effective_iana(), tz_effective_source(),
					 g_public_ip[0] ? g_public_ip : "?");
			proto_publish_reported_now();
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(10000));
	}
	vTaskDelete(NULL);
}

static void sntp_synced(struct timeval *tv)
{
	(void)tv;
	clock_set_source("sntp"); /* also when it answers after the chain moved on */
}

void time_sync_start(void)
{
	if (!sntp_started) {
		esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
		cfg.start = true;
		cfg.sync_cb = sntp_synced;
		esp_netif_sntp_init(&cfg);
		sntp_started = true;
		xTaskCreate(time_task, "time", 4096, NULL, 3, NULL);
	} else {
		esp_netif_sntp_start(); /* a network change: ask again */
	}
}

/* ------------------------------------------------------------------ start */

static void modem_task(void *arg)
{
	led_set(LED_CONNECTING);
	if (modem_start()) {
		chosen = NET_4G;
		on_up();
	} else if (g_cfg.wifi_ssid[0]) {
		ESP_LOGW(TAG, "4G not available, falling back to Wi-Fi %s", g_cfg.wifi_ssid);
		fallback_wifi = true;
		chosen = NET_WIFI;
		wifi_set_up_cb(on_up);
		wifi_start_sta();
	} else {
		ESP_LOGE(TAG, "4G not available and no Wi-Fi configured");
		led_set(LED_ERROR);
		modem_fail_t f = modem_last_failure();
		if (f == MODEM_NOT_FITTED || f == MODEM_NO_SIM) {
			/* nothing to connect with: the user has to pick a Wi-Fi (or insert a SIM) in the setup portal */
			ESP_LOGW(TAG, "%s and no Wi-Fi: opening the setup portal", f == MODEM_NOT_FITTED ? "no 4G module" : "no SIM");
			store_set_flag("portal", 1);
			vTaskDelay(pdMS_TO_TICKS(1500));
			esp_restart();
		}
		chosen = NET_NONE;
	}
	vTaskDelete(NULL);
}

void net_start(void)
{
	if (started) {
		return;
	}
	started = true;
	if (modem_available()) {
		chosen = NET_4G;
		ESP_LOGI(TAG, "4G on (APN %s): trying the modem first", g_cfg.apn[0] ? g_cfg.apn : "auto from the SIM");
		xTaskCreate(modem_task, "modem-up", 6144, NULL, 4, NULL); /* PPP bring-up takes 10-30 s */
	} else if (g_cfg.wifi_ssid[0]) {
		chosen = NET_WIFI;
		wifi_set_up_cb(on_up);
		wifi_start_sta();
	} else {
		ESP_LOGE(TAG, "no network configured (portal: Wi-Fi or APN)");
	}
}

void net_ensure(void)
{
	if (!started) {
		net_start();
	}
}

bool net_wait_up(int timeout_s)
{
	for (int i = 0; i < timeout_s && !(net_up() && clock_synced()); i++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	return net_up() && clock_synced();
}

void net_sleep_prepare(void)
{
	if (chosen == NET_4G || modem_available()) {
		modem_stop(); /* AT+CPOF, PWRKEY, rail off: the modem must not stay up through deep sleep */
	}
	if (wifi_started()) {
		wifi_stop();
	}
}
