/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Network manager: Wi-Fi when one is set and it gives an IP, else 4G (esp_modem PPP). Wi-Fi first because it costs
 * the battery less: a wake on a good Wi-Fi is online in 2-5 s with the ESP32-S3 alone, 4G needs 7-15 s plus the
 * module's own transmit current (peaks of 2 A). MQTT to IoT Core and HTTPS to FlyBot run on whichever is up. Time: SNTP first; when nothing answers within TIME_SNTP_WAIT_S (some carriers block UDP
 * 123 on 4G) the firmware asks FlyBot (GET /api/device/time).
 *
 * The choice is made at each boot (each deep-sleep wake in battery mode) and watched afterwards (watch_tick):
 * Wi-Fi without an IP or without internet hands over to 4G, and on 4G the camera restarts onto the Wi-Fi once it
 * is in range again. With Wi-Fi in use an always-on 4G module has its radio switched off (modem_park).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "net";
#define TIME_SNTP_WAIT_S 15

static net_type_t chosen = NET_NONE;
static bool started, sntp_started;
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

void net_4g_restarted(void)
{
	if (chosen == NET_4G) {
		on_up();
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
		xTaskCreate(time_task, "time", 8192, NULL, 3, NULL); /* HTTPS to FlyBot: a TLS handshake needs more than 4 KB */
	} else {
		esp_netif_sntp_start(); /* a network change: ask again */
	}
}

/* ------------------------------------------------------------------ start */

/* 4G for this boot; false when it does not come up either */
static bool start_4g(void)
{
	led_set(LED_CONNECTING);
	chosen = NET_4G;
	if (modem_start()) {
		on_up();
		return true;
	}
	if (g_cfg.wifi_ssid[0]) {
		/* Wi-Fi came first and failed too: nothing left for this boot (battery mode tries again at the next wake,
		 * powered mode when the Wi-Fi probe below sees the network) */
		ESP_LOGE(TAG, "neither Wi-Fi %s nor 4G is available", g_cfg.wifi_ssid);
		led_set(LED_ERROR);
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
	}
	chosen = NET_NONE;
	return false;
}

static void modem_task(void *arg)
{
	start_4g();
	vTaskDelete(NULL);
}

#define WIFI_FIRST_WAIT_S 20     /* no IP within this: 4G */
#define WIFI_FIRST_TRIES 3       /* or after this many failed joins (wrong password, network not in range) */
#define WIFI_NO_CLOUD_S 90       /* an IP but no MQTT for this long (no internet behind the router, or it went): 4G */
#define WIFI_PROBE_MIN 10        /* on 4G: look for the Wi-Fi every 10 min, doubled per no-internet fallback, max x16 */

/* Wi-Fi fallbacks for "no internet" in a row, kept through restarts and deep sleep: the probe waits longer each
 * time, so a router that keeps its Wi-Fi up without internet does not pull the camera back every 10 minutes */
#define NO_CLOUD_MAGIC 0x4e4f4331
static RTC_NOINIT_ATTR uint32_t no_cloud_magic;
static RTC_NOINIT_ATTR int no_cloud_count;

static int no_cloud(void)
{
	return no_cloud_magic == NO_CLOUD_MAGIC ? no_cloud_count : 0;
}

static void set_no_cloud(int n)
{
	no_cloud_magic = NO_CLOUD_MAGIC;
	no_cloud_count = n;
}

/* Keeps the network that works, checked every second by an esp_timer (no task of its own: internal RAM is for
 * the live encoder's stack); what takes time runs in a short-lived task:
 *  - Wi-Fi joining: no IP within WIFI_FIRST_WAIT_S / WIFI_FIRST_TRIES -> 4G;
 *  - Wi-Fi with an IP: the always-on 4G module's radio goes off (modem_park); no MQTT for WIFI_NO_CLOUD_S (no
 *    internet behind the router, the router went, the line went) -> 4G;
 *  - 4G with a Wi-Fi set: every WIFI_PROBE_MIN a scan for it; seen -> restart, and the boot takes Wi-Fi first
 *    (not while a live viewer watches). 4G that loses its signal is modem.c's watchdog: a restart after 180 s.
 * A network just set in the portal (wifi_new) never falls back: wifi.c sends the user back to the portal. */
static esp_timer_handle_t watch_timer;
static volatile bool watch_busy; /* a worker below is running */
static bool joined;
static int watch_s, down_s;
static int64_t next_probe;
static bool has_4g, wifi_new; /* read once at the start: the timer task has a small stack for NVS */
static const char *fallback_why;

static void park_worker(void *arg)
{
	modem_park();
	watch_busy = false;
	vTaskDelete(NULL);
}

static void fallback_worker(void *arg)
{
	events_note("net", fallback_why);
	web_stop();
	wifi_release();
	start_4g();
	watch_busy = false;
	vTaskDelete(NULL);
}

static void probe_worker(void *arg)
{
	int rssi = 0;
	if (wifi_probe(&rssi)) {
		ESP_LOGW(TAG, "Wi-Fi %s is back (%d dBm): restarting onto it", g_cfg.wifi_ssid, rssi);
		events_note("net", "Wi-Fi is back: restarting onto it");
		vTaskDelay(pdMS_TO_TICKS(1000));
		esp_restart();
	}
	ESP_LOGI(TAG, "Wi-Fi %s not in range", g_cfg.wifi_ssid);
	watch_busy = false;
	vTaskDelete(NULL);
}

static void run_worker(TaskFunction_t fn, const char *name, uint32_t stack)
{
	watch_busy = true;
	if (xTaskCreate(fn, name, stack, NULL, 4, NULL) != pdPASS) {
		watch_busy = false; /* no memory now: the next tick tries again */
	}
}

static void to_4g(const char *why)
{
	fallback_why = why;
	chosen = NET_4G;
	run_worker(fallback_worker, "net-4g", 6144); /* the PPP bring-up, as modem_task */
	if (!watch_busy) {
		chosen = NET_WIFI;
	}
}

static void watch_tick(void *arg)
{
	watch_s++;
	if (watch_busy) {
		return;
	}
	if (chosen == NET_WIFI) {
		bool can_4g = has_4g && (joined || !wifi_new); /* wifi.c clears wifi_new at the first IP */
		if (!joined) {
			if (wifi_up()) {
				joined = true;
				run_worker(park_worker, "net-park", 4096);
			} else if (can_4g && (watch_s >= WIFI_FIRST_WAIT_S || wifi_failures() >= WIFI_FIRST_TRIES)) {
				ESP_LOGW(TAG, "Wi-Fi %s gave no IP (%d attempts, %d s): 4G instead", g_cfg.wifi_ssid, wifi_failures(),
						 watch_s);
				to_4g("Wi-Fi gave no IP: 4G instead");
			}
			return;
		}
		if (proto_connected()) {
			down_s = 0;
			if (no_cloud()) {
				set_no_cloud(0);
			}
		} else if (proto_provisioned() && ++down_s >= WIFI_NO_CLOUD_S && can_4g) {
			ESP_LOGW(TAG, "Wi-Fi %s: no cloud connection for %d s (%s): 4G instead", g_cfg.wifi_ssid, down_s,
					 wifi_up() ? "an IP but no internet" : "Wi-Fi lost");
			if (wifi_up()) {
				set_no_cloud(no_cloud() + 1); /* the probe waits longer only for a Wi-Fi without internet */
			}
			to_4g("Wi-Fi without internet: 4G instead");
		}
	} else if (chosen == NET_4G && g_cfg.wifi_ssid[0] && wifi_started()) {
		int64_t now = esp_timer_get_time() / 1000000;
		if (!next_probe) {
			int n = no_cloud() > 4 ? 4 : no_cloud();
			next_probe = now + (int64_t)WIFI_PROBE_MIN * 60 * (1 << n);
			ESP_LOGI(TAG, "on 4G: looking for Wi-Fi %s again in %d min", g_cfg.wifi_ssid, WIFI_PROBE_MIN << n);
		} else if (now >= next_probe) {
			next_probe = 0;
			if (live_streaming()) {
				ESP_LOGI(TAG, "live viewer: Wi-Fi probe later");
			} else {
				run_worker(probe_worker, "net-probe", 4096);
			}
		}
	}
}

static void watch_start(void)
{
	has_4g = modem_available();
	wifi_new = store_get_flag("wifi_new");
	const esp_timer_create_args_t args = { .callback = watch_tick, .name = "net-watch" };
	if (esp_timer_create(&args, &watch_timer) == ESP_OK) {
		esp_timer_start_periodic(watch_timer, 1000000);
	}
}

void net_start(void)
{
	if (started) {
		return;
	}
	started = true;
	if (g_cfg.wifi_ssid[0]) {
		chosen = NET_WIFI;
		ESP_LOGI(TAG, "Wi-Fi %s first%s", g_cfg.wifi_ssid, modem_available() ? ", 4G if it gives no IP" : "");
		wifi_set_up_cb(on_up);
		wifi_start_sta();
		watch_start();
	} else if (modem_available()) {
		chosen = NET_4G;
		ESP_LOGI(TAG, "4G on (APN %s)", g_cfg.apn[0] ? g_cfg.apn : "auto from the SIM");
		xTaskCreate(modem_task, "modem-up", 6144, NULL, 4, NULL); /* PPP bring-up takes 10-30 s */
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
