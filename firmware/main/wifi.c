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
	esp_netif_create_default_wifi_sta();
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
