/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * camos-esp32-trap: a "cloud-counted" fruit-fly trap camera. It speaks the CamOS device protocol v1 (docs/dev/protocol-v1.md)
 * to AWS IoT Core and uploads JPEGs to FlyBot (docs/dev/PROTOCOL_ADDITIONS.md); the counting is done in the cloud.
 *
 * Boot: NVS -> (nothing configured, or button held 3 s: setup portal CAM-xxxx) -> battery -> console
 *       -> schedule task + MQTT agent -> network (4G when an APN is set, else Wi-Fi) + time -> pairing when pending.
 * Battery mode: after a timer wake-up with a valid clock, the network stays off until the schedule task has the JPEG.
 */
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "camos";

static void power_setup(void)
{
#if CONFIG_CAMOS_LIGHT_SLEEP
	/* Automatic light sleep: the CPU sleeps whenever every task is waiting (Wi-Fi modem sleep sets the rhythm) */
	esp_pm_config_t pm = {
		.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
		.min_freq_mhz = 40,
		.light_sleep_enable = true,
	};
	esp_err_t err = esp_pm_configure(&pm);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "power management not enabled: %s", esp_err_to_name(err));
	}
#endif
}

/* Owner path (CONTRACTS 2 / 2a.7): a provisioning token from FlyBot "add camera" pairs the camera to that account */
static void pair_task(void *arg)
{
	sched_keep_awake(180);
	net_ensure();
	if (net_wait_up(150)) {
		char err[96];
		bool had_mqtt = g_cfg.mqtt_host[0] && g_cfg.cert;
		if (flybot_pair(err, sizeof(err)) == ESP_OK) {
			if (!had_mqtt) {
				ESP_LOGI(TAG, "paired, restarting to connect to IoT Core");
				vTaskDelay(pdMS_TO_TICKS(500));
				esp_restart();
			}
			ESP_LOGI(TAG, "paired with the provisioning token");
		} else {
			ESP_LOGE(TAG, "pairing failed: %s (fix it in the portal: hold the button 3 s)", err);
		}
	}
	vTaskDelete(NULL);
}

void app_main(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_LOGE(TAG, "NVS unreadable (%s): erasing it, configure the camera again", esp_err_to_name(err));
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	log_capture_start(); /* the web console's log page */
	store_load();
	led_init();
	ESP_LOGI(TAG, "camos-esp32-trap %s fw %s serial %s", CONFIG_CAMOS_MODEL, fw_version(), g_cfg.serial);

	uint32_t flash_size = 0;
	esp_flash_get_size(NULL, &flash_size);
	ESP_LOGI(TAG, "flash %u MB, PSRAM %u KB%s", (unsigned)(flash_size >> 20), (unsigned)(esp_psram_get_size() >> 10),
			 esp_psram_is_initialized() ? "" : " (NOT initialized: wrong PSRAM mode in sdkconfig? N16R8 / N8R8 are octal)");

	/* a default / saved Wi-Fi or 4G (on by default, APN auto) is enough to start: the portal opens when neither works */
	bool configured = g_cfg.wifi_ssid[0] || modem_available();
	bool held = button_held(3000);
	if (held) {
		store_clear_admin(); /* the button held 3 s: forgot the console password */
	}
	if (!configured || store_get_flag("portal") || held) {
		portal_run(); /* does not return: restarts after /api/restart or after 10 minutes */
	}

	battery_sample(); /* before Wi-Fi: the ESP32 cannot read ADC2 pins while Wi-Fi runs */
	power_setup();
	button_task_start();
	shadow_init();
	if (sd_mount()) {
		events_note("sd", "card mounted");
	}
	console_start();
	sched_start();
	proto_start();
	motion_start(); /* idle unless enabled in the console and the effective mode is powered */

	bool timer_wake = esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
	if (!g_cfg.cert || !g_cfg.key) {
		register_start(); /* CONTRACTS 2a: zero-touch, brings the network up itself */
	} else if (g_cfg.prov_token[0] && !g_cfg.token[0] && g_cfg.api_base[0]) {
		xTaskCreate(pair_task, "pair", 8192, NULL, 4, NULL);
	}
	if (!power_may_deep_sleep() || !timer_wake || !clock_synced()) {
		/* Powered mode, a power-on / reset, or a lost clock: network now (state, shadow, time first).
		 * Battery mode after a timer wake-up: the network can wait for the JPEG (capture_now starts it). */
		net_start();
	}
}
