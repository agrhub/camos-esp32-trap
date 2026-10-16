/* Wi-Fi station with the reconnect policy of protocol 1 (1, 2, 4 ... 300 s, +-20 %), and the portal button.
 * Time sync and the 4G alternative live in net.c / modem.c. */
#include <string.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"
#include <stdio.h>

static const char *TAG = "wifi";
static esp_timer_handle_t retry_timer;
static int backoff_s = 1;
static volatile bool s_up;
static bool started, stopping;
static void (*up_cb)(void);

void wifi_set_up_cb(void (*cb)(void)) { up_cb = cb; }
bool wifi_up(void) { return s_up; }
bool wifi_started(void) { return started; }

int wifi_rssi(void)
{
	wifi_ap_record_t ap;
	return started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void wifi_ip(char *out, size_t n)
{
	esp_netif_ip_info_t info;
	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
		snprintf(out, n, IPSTR, IP2STR(&info.ip));
	} else {
		out[0] = 0;
	}
}

static void retry_cb(void *arg)
{
	esp_wifi_connect();
}

/* A network just set in the portal that never gives an IP (wrong password, out of range): back to the portal after
 * WIFI_NEW_GIVEUP_S with the error shown there, like the AMB82 camera ("setup comes back in about a minute"). */
#define WIFI_NEW_GIVEUP_S 75
static esp_timer_handle_t giveup_timer;

static void giveup_cb(void *arg)
{
	ESP_LOGE(TAG, "could not join new Wi-Fi \"%s\" in %d s: back to the setup portal", g_cfg.wifi_ssid, WIFI_NEW_GIVEUP_S);
	store_set_str("wifi_err", g_cfg.wifi_ssid);
	store_set_flag("wifi_new", 0);
	store_set_flag("portal", 1);
	led_set(LED_ERROR);
	esp_restart();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		s_up = false;
		if (stopping) {
			return;
		}
		/* Exponential back-off with +-20 % jitter, capped at 300 s */
		int jitter = (int)(esp_random() % 41) - 20;
		int64_t delay_ms = (int64_t)backoff_s * 1000 * (100 + jitter) / 100;
		ESP_LOGW(TAG, "disconnected, retry in %d ms", (int)delay_ms);
		esp_timer_start_once(retry_timer, delay_ms * 1000);
		backoff_s = backoff_s * 2 > 300 ? 300 : backoff_s * 2;
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		backoff_s = 1;
		s_up = true;
		if (giveup_timer) {
			esp_timer_stop(giveup_timer);
		}
		if (store_get_flag("wifi_new")) {
			store_set_flag("wifi_new", 0);
			store_set_str("wifi_err", "");
			g_wifi_error[0] = 0;
		}
		char ip[16];
		wifi_ip(ip, sizeof(ip));
		ESP_LOGI(TAG, "got IP %s (rssi %d)", ip, wifi_rssi());
		if (up_cb) {
			up_cb(); /* protocol 1: reconnect MQTT right away after a network change */
		}
	}
}

void wifi_start_sta(void)
{
	if (started) {
		return;
	}
	started = true;
	stopping = false;
	esp_netif_t *sta = esp_netif_create_default_wifi_sta();
	if (g_cfg.ip_mode == 1 && g_cfg.static_ip[0] && g_cfg.static_gw[0]) {
		/* static address from the console's TCP/IP page (GOT_IP still comes once the station associates) */
		esp_netif_ip_info_t ip = { 0 };
		ip.ip.addr = esp_ip4addr_aton(g_cfg.static_ip);
		ip.netmask.addr = esp_ip4addr_aton(g_cfg.static_mask[0] ? g_cfg.static_mask : "255.255.255.0");
		ip.gw.addr = esp_ip4addr_aton(g_cfg.static_gw);
		esp_netif_dhcpc_stop(sta);
		if (esp_netif_set_ip_info(sta, &ip) == ESP_OK) {
			esp_netif_dns_info_t dns = { 0 };
			dns.ip.type = ESP_IPADDR_TYPE_V4;
			dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(g_cfg.static_dns[0] ? g_cfg.static_dns : g_cfg.static_gw);
			esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
			ESP_LOGI(TAG, "static IP %s / %s, gateway %s", g_cfg.static_ip, g_cfg.static_mask, g_cfg.static_gw);
		} else {
			esp_netif_dhcpc_start(sta);
			ESP_LOGW(TAG, "static IP %s rejected: DHCP", g_cfg.static_ip);
		}
	}
	wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&init));
	const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi-retry" };
	ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
	wifi_config_t wc = { 0 };
	strlcpy((char *)wc.sta.ssid, g_cfg.wifi_ssid, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, g_cfg.wifi_pass, sizeof(wc.sta.password));
	wc.sta.threshold.authmode = g_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
	/* Modem sleep between beacons: the base of the power saving (light sleep adds to it, see main.c) */
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
	ESP_ERROR_CHECK(esp_wifi_start());
	led_set(LED_CONNECTING);
	if (store_get_flag("wifi_new")) {
		const esp_timer_create_args_t ga = { .callback = giveup_cb, .name = "wifi-giveup" };
		if (esp_timer_create(&ga, &giveup_timer) == ESP_OK) {
			esp_timer_start_once(giveup_timer, (uint64_t)WIFI_NEW_GIVEUP_S * 1000000ULL);
			ESP_LOGI(TAG, "new network %s: back to the portal if no IP within %d s", g_cfg.wifi_ssid, WIFI_NEW_GIVEUP_S);
		}
	}
}

/* ------------------------------------------------------------------ scan (portal list, console Wi-Fi page) */

int wifi_scan(wifi_net_t *out, int max)
{
	wifi_mode_t mode;
	if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) {
		return -1; /* Wi-Fi not started (4G) or AP only: the portal runs APSTA */
	}
	/* Short active dwell and a return to the home channel between channels: the SoftAP keeps beaconing and a
	 * phone associated to it stays associated (it only sees ~120 ms gaps) */
	wifi_scan_config_t sc = {
		.show_hidden = false,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active = { .min = 50, .max = 120 },
		.home_chan_dwell_time = 60,
	};
	int64_t t0 = esp_timer_get_time();
	wifi_ps_type_t ps = WIFI_PS_NONE;
	esp_wifi_get_ps(&ps);
	esp_wifi_set_ps(WIFI_PS_NONE); /* modem sleep between beacons misses probe responses: 3 networks instead of 10 */
	esp_err_t e = esp_wifi_scan_start(&sc, true);
	esp_wifi_set_ps(ps);
	if (e != ESP_OK) {
		ESP_LOGW(TAG, "scan: %s", esp_err_to_name(e));
		return -1;
	}
	uint16_t n = 0;
	esp_wifi_scan_get_ap_num(&n);
	if (n > 48) {
		n = 48;
	}
	wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
	if (!recs) {
		esp_wifi_clear_ap_list();
		return -1;
	}
	esp_wifi_scan_get_ap_records(&n, recs);
	int count = 0, seen = n;
	for (int i = 0; i < seen; i++) {
		const char *ssid = (const char *)recs[i].ssid;
		if (!ssid[0]) {
			continue; /* hidden: "Other network" in the page */
		}
		int j = 0;
		for (; j < count && strcmp(out[j].ssid, ssid); j++) {
		}
		if (j < count) {
			if (recs[i].rssi > out[j].rssi) {
				out[j].rssi = recs[i].rssi;
				out[j].channel = recs[i].primary;
			}
			continue;
		}
		if (count >= max) {
			continue;
		}
		strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
		out[count].rssi = recs[i].rssi;
		out[count].channel = recs[i].primary;
		out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
		count++;
	}
	free(recs);
	/* strongest first */
	for (int i = 1; i < count; i++) {
		wifi_net_t t = out[i];
		int j = i - 1;
		while (j >= 0 && out[j].rssi < t.rssi) {
			out[j + 1] = out[j];
			j--;
		}
		out[j + 1] = t;
	}
	ESP_LOGI(TAG, "scan: %d networks (%d records) in %lld ms", count, seen, (long long)((esp_timer_get_time() - t0) / 1000));
	return count;
}

cJSON *wifi_scan_json(const wifi_net_t *nets, int n)
{
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON *a = cJSON_CreateObject();
		cJSON_AddStringToObject(a, "ssid", nets[i].ssid);
		cJSON_AddNumberToObject(a, "rssi", nets[i].rssi);
		cJSON_AddBoolToObject(a, "secure", nets[i].secure);
		cJSON_AddNumberToObject(a, "channel", nets[i].channel);
		cJSON_AddItemToArray(arr, a);
	}
	return arr;
}

/* Before deep sleep: a clean stop so the AP sees a disassociation (it would otherwise hold the session) */
void wifi_stop(void)
{
	if (!started) {
		return;
	}
	stopping = true;
	esp_timer_stop(retry_timer);
	esp_wifi_disconnect();
	esp_wifi_stop();
	s_up = false;
}

/* ------------------------------------------------------------------ setup button (protocol 12.3: hold to start the portal) */

bool button_held(int ms)
{
	if (CONFIG_CAMOS_BUTTON_GPIO < 0) {
		return false;
	}
	gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_CAMOS_BUTTON_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
	gpio_config(&io);
	for (int t = 0; t < ms; t += 50) {
		if (gpio_get_level(CONFIG_CAMOS_BUTTON_GPIO)) {
			return false;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	return true;
}

static void button_task(void *arg)
{
	int held = 0;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(100));
		held = gpio_get_level(CONFIG_CAMOS_BUTTON_GPIO) ? 0 : held + 100;
		if (held >= 3000) {
			ESP_LOGW(TAG, "button held: restart into the setup portal");
			store_set_flag("portal", 1);
			esp_restart();
		}
	}
}

void button_task_start(void)
{
	if (CONFIG_CAMOS_BUTTON_GPIO >= 0) {
		gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_CAMOS_BUTTON_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
		gpio_config(&io);
		xTaskCreate(button_task, "button", 2560, NULL, 3, NULL);
	}
}
