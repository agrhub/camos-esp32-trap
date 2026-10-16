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

static void on_up(void)
{
	time_sync_start();
	if (up_cb) {
		up_cb();
	}
}

/* ------------------------------------------------------------------ time */

static void time_task(void *arg)
{
	/* SNTP (pool + Google) is tried first on every network; FlyBot's clock is the fallback for 4G carriers
	 * that block NTP. Both are retried while the clock is unset. */
	for (int round = 0; !clock_synced(); round++) {
		if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(TIME_SNTP_WAIT_S * 1000)) == ESP_OK || clock_synced()) {
			break;
		}
		if (net_up() && g_cfg.api_base[0] && flybot_time() == ESP_OK) {
			ESP_LOGW(TAG, "clock set from FlyBot (GET /api/device/time); NTP did not answer");
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(round < 3 ? 5000 : 30000));
	}
	if (clock_synced()) {
		char s[25];
		iso8601(time(NULL), s);
		ESP_LOGI(TAG, "clock %s", s);
	}
	vTaskDelete(NULL);
}

void time_sync_start(void)
{
	if (!sntp_started) {
		esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
		cfg.start = true;
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
		ESP_LOGI(TAG, "APN %s set: trying 4G first", g_cfg.apn);
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
