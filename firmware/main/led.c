/*
 * Status LED (docs/LED.md). One base state plus short one-shot overlays:
 *
 *   base     LED_PORTAL      blue slow breathe (2 s period)      setup portal (AMB82: blue blinking during Wi-Fi setup)
 *            LED_CONNECTING  yellow fast blink (4 Hz)            Wi-Fi / 4G / MQTT not up yet
 *            LED_ONLINE      green 60 ms blip every 5 s, for the first 60 s online, then dark (power)
 *            LED_ERROR       red double blink every 2 s           no network after give-up, certificate missing
 *            LED_OTA         purple steady                        firmware update running
 *            LED_OFF         dark
 *   overlay  LED_FLASH_CAPTURE  white 150 ms flash                capture / upload start
 *            LED_FLASH_ERROR    red double blink, 3 times         upload failed
 *            LED_FLASH_LOWBATT  orange blink, 3 times (blocking)  low battery, just before deep sleep
 *
 * Backends: a WS2812 on an RMT channel (espressif/led_strip; Freenove board GPIO48) at CAMOS_LED_BRIGHTNESS
 * (about 10 % by default), or a plain GPIO LED (AI-Thinker ESP32-CAM red LED on GPIO33, active low) where colours
 * collapse to on/off. NVS key "led" (portal / console `ledEnabled`) 0 switches it off. Every state change is logged
 * ("led: portal (blue breathe)") so a serial log proves what the LED shows.
 */
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "led";

#if CONFIG_CAMOS_LED_WS2812
#include "led_strip.h"
static led_strip_handle_t strip;
#endif

static TaskHandle_t task;
static volatile led_state_t base_state = LED_OFF;
static volatile led_flash_t pending = LED_FLASH_NONE;
static volatile bool enabled = true;
static bool ready;

static const char *const state_names[] = {
	[LED_OFF] = "off", [LED_PORTAL] = "portal (blue breathe)", [LED_CONNECTING] = "connecting (yellow fast blink)",
	[LED_ONLINE] = "online (green blip every 5 s for 60 s, then dark)", [LED_ERROR] = "error (red double blink)",
	[LED_OTA] = "ota (purple)",
};
static const char *const flash_names[] = {
	[LED_FLASH_NONE] = "", [LED_FLASH_CAPTURE] = "capture (white flash)", [LED_FLASH_ERROR] = "upload error (red double blink x3)",
	[LED_FLASH_LOWBATT] = "low battery (orange blink x3)",
};

/* r, g, b at full scale 0..255, scaled by the brightness setting (and 0..1 for breathing) */
static void show(uint8_t r, uint8_t g, uint8_t b, float level)
{
#if CONFIG_CAMOS_LED_WS2812
	if (!strip) {
		return;
	}
	const float k = level * CONFIG_CAMOS_LED_BRIGHTNESS / 255.0f;
	if (r == 0 && g == 0 && b == 0) {
		led_strip_clear(strip);
	} else {
		led_strip_set_pixel(strip, 0, (uint32_t)(r * k + 0.5f), (uint32_t)(g * k + 0.5f), (uint32_t)(b * k + 0.5f));
		led_strip_refresh(strip);
	}
#elif CONFIG_CAMOS_LED_GPIO_LED
	bool on = level > 0.3f && (r || g || b);
	gpio_set_level(CONFIG_CAMOS_LED_GPIO, CONFIG_CAMOS_LED_ACTIVE_LOW ? !on : on);
#else
	(void)r, (void)g, (void)b, (void)level;
#endif
}

static void dark(void) { show(0, 0, 0, 0); }

/* Waits up to ms, returns true when something changed (state, overlay, enable) */
static bool wait_ms(int ms)
{
	return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) != 0;
}

static void blink_n(uint8_t r, uint8_t g, uint8_t b, int n, int on_ms, int off_ms)
{
	for (int i = 0; i < n; i++) {
		show(r, g, b, 1);
		vTaskDelay(pdMS_TO_TICKS(on_ms));
		dark();
		vTaskDelay(pdMS_TO_TICKS(off_ms));
	}
}

static void run_flash(led_flash_t f)
{
	switch (f) {
	case LED_FLASH_CAPTURE:
		show(255, 255, 255, 1);
		vTaskDelay(pdMS_TO_TICKS(150));
		dark();
		break;
	case LED_FLASH_ERROR:
		for (int i = 0; i < 3; i++) {
			blink_n(255, 0, 0, 2, 120, 150);
			vTaskDelay(pdMS_TO_TICKS(400));
		}
		break;
	case LED_FLASH_LOWBATT:
		blink_n(255, 90, 0, 3, 250, 250);
		break;
	default:
		break;
	}
}

static void led_task(void *arg)
{
	int64_t online_since = 0;
	led_state_t last = LED_OFF;
	for (;;) {
		led_flash_t f = pending;
		if (f != LED_FLASH_NONE) {
			pending = LED_FLASH_NONE;
			if (enabled) {
				run_flash(f);
			}
			continue;
		}
		led_state_t s = enabled ? base_state : LED_OFF;
		if (s != last) {
			online_since = s == LED_ONLINE ? xTaskGetTickCount() : 0;
			last = s;
		}
		switch (s) {
		case LED_PORTAL: { /* 2 s breathe: 40 steps of 50 ms */
			bool changed = false;
			for (int i = 0; i < 40 && !changed; i++) {
				float x = 0.5f - 0.5f * cosf((float)i / 40.0f * 2.0f * (float)M_PI);
				show(0, 60, 255, 0.08f + 0.92f * x);
				changed = wait_ms(50);
			}
			break;
		}
		case LED_CONNECTING:
			show(255, 180, 0, 1);
			if (wait_ms(125)) {
				break;
			}
			dark();
			wait_ms(125);
			break;
		case LED_ONLINE:
			if ((xTaskGetTickCount() - online_since) * portTICK_PERIOD_MS > 60000) {
				dark();
				wait_ms(portMAX_DELAY); /* dark until something changes: no wake-ups, no light-sleep penalty */
				break;
			}
			show(0, 255, 0, 1);
			vTaskDelay(pdMS_TO_TICKS(60));
			dark();
			wait_ms(4940);
			break;
		case LED_ERROR:
			blink_n(255, 0, 0, 2, 120, 150);
			wait_ms(1460);
			break;
		case LED_OTA:
			show(160, 0, 255, 1);
			wait_ms(portMAX_DELAY);
			break;
		case LED_OFF:
		default:
			dark();
			wait_ms(portMAX_DELAY);
			break;
		}
	}
}

void led_init(void)
{
	if (ready) {
		return;
	}
	enabled = store_get_flag_default("led", 1) != 0;
#if CONFIG_CAMOS_LED_WS2812
	led_strip_config_t sc = {
		.strip_gpio_num = CONFIG_CAMOS_LED_GPIO,
		.max_leds = CONFIG_CAMOS_LED_COUNT,
		.led_model = LED_MODEL_WS2812,
		.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
		.flags.invert_out = false,
	};
	led_strip_rmt_config_t rc = {
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = 10 * 1000 * 1000,
		.mem_block_symbols = 0,
		.flags.with_dma = false,
	};
	gpio_hold_dis(CONFIG_CAMOS_LED_GPIO);
	esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &strip);
	if (e != ESP_OK) {
		ESP_LOGE(TAG, "WS2812 on GPIO%d: %s", CONFIG_CAMOS_LED_GPIO, esp_err_to_name(e));
		strip = NULL;
		return;
	}
	led_strip_clear(strip);
	ESP_LOGI(TAG, "WS2812 x%d on GPIO%d, brightness %d/255, %s", CONFIG_CAMOS_LED_COUNT, CONFIG_CAMOS_LED_GPIO,
			 CONFIG_CAMOS_LED_BRIGHTNESS, enabled ? "enabled" : "disabled (NVS led=0)");
#elif CONFIG_CAMOS_LED_GPIO_LED
	gpio_hold_dis(CONFIG_CAMOS_LED_GPIO);
	gpio_reset_pin(CONFIG_CAMOS_LED_GPIO);
	gpio_set_direction(CONFIG_CAMOS_LED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_CAMOS_LED_GPIO, CONFIG_CAMOS_LED_ACTIVE_LOW);
	ESP_LOGI(TAG, "status LED on GPIO%d (%s), %s", CONFIG_CAMOS_LED_GPIO, CONFIG_CAMOS_LED_ACTIVE_LOW ? "active low" : "active high",
			 enabled ? "enabled" : "disabled (NVS led=0)");
#else
	ESP_LOGI(TAG, "no status LED on this board");
	return;
#endif
	ready = true;
	xTaskCreate(led_task, "led", 3072, NULL, 2, &task);
}

void led_set(led_state_t s)
{
	if (s == base_state) {
		return;
	}
	base_state = s;
	ESP_LOGI(TAG, "%s", state_names[s]);
	if (task) {
		xTaskNotifyGive(task);
	}
}

led_state_t led_get(void) { return base_state; }

void led_flash(led_flash_t f)
{
	if (!task || f == LED_FLASH_NONE) {
		return;
	}
	ESP_LOGI(TAG, "flash %s", flash_names[f]);
	pending = f;
	xTaskNotifyGive(task);
}

void led_enable(bool on)
{
	enabled = on;
	store_set_flag("led", on ? 1 : 0);
	ESP_LOGI(TAG, "%s", on ? "enabled" : "disabled");
	if (task) {
		xTaskNotifyGive(task);
	}
}

bool led_enabled(void) { return enabled; }

/* Before deep sleep: optional low-battery warning (blocking), then dark and the pin held low */
void led_sleep_prepare(bool low_battery)
{
	if (!ready) {
		return;
	}
	if (low_battery && enabled) {
		ESP_LOGI(TAG, "flash %s", flash_names[LED_FLASH_LOWBATT]);
		vTaskSuspend(task);
		run_flash(LED_FLASH_LOWBATT);
	} else {
		vTaskSuspend(task);
	}
	dark();
	ESP_LOGI(TAG, "off for deep sleep");
#if CONFIG_CAMOS_LED_WS2812
	/* the WS2812 keeps its last colour while powered: it has been cleared above; hold the data line low */
	gpio_set_direction(CONFIG_CAMOS_LED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_CAMOS_LED_GPIO, 0);
	gpio_hold_en(CONFIG_CAMOS_LED_GPIO);
#elif CONFIG_CAMOS_LED_GPIO_LED
	gpio_hold_en(CONFIG_CAMOS_LED_GPIO);
#endif
}

const char *led_state_name(led_state_t s)
{
	return s <= LED_OTA ? state_names[s] : "?";
}
