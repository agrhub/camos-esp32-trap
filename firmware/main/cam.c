/*
 * Camera: JPEG at the maximum size of the sensor (protocol 9 `capture`: UXGA 1600x1200 on OV2640, QXGA 2048x1536 on
 * OV3660, QSXGA 2560x1920 on OV5640). The sensor is powered up for each capture and powered down afterwards (PWDN),
 * which is what lets the board sleep between captures; auto exposure restarts each time, hence the warm-up frames.
 * Live view (ESP32-S3) keeps the sensor up at a small size and hands JPEG frames to live.c.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "cam";
static RTC_DATA_ATTR framesize_t max_fs; /* known after the first power-up; kept through deep sleep */
static bool streaming;

/* The sensor needs a steady XCLK and SCCB clock: with automatic light sleep and frequency scaling on (station mode,
 * main.c power_setup) camera init fails with garbage SCCB reads. Hold the CPU at full speed and out of light sleep
 * from power-up to power-down. */
static esp_pm_lock_handle_t pm_freq, pm_awake;
static bool pm_on;

static void cam_pm(bool on)
{
#if CONFIG_PM_ENABLE
	if (!pm_freq) {
		esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "camera", &pm_freq);
		esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "camera", &pm_awake);
	}
	if (on && !pm_on) {
		esp_pm_lock_acquire(pm_freq);
		esp_pm_lock_acquire(pm_awake);
		pm_on = true;
	} else if (!on && pm_on) {
		esp_pm_lock_release(pm_awake);
		esp_pm_lock_release(pm_freq);
		pm_on = false;
	}
#endif
	(void)on;
}

/* Camera power. PWDN (AI-Thinker: GPIO32) is driven by esp32-camera itself; boards without PWDN can switch the camera
 * 3V3 with a P-MOSFET on CAMOS_CAM_POWER_GPIO (docs/WIRING.md). Both are held through deep sleep. */
static void cam_power(bool on)
{
	if (on) {
		cam_pm(true);
	}
#if CONFIG_CAMOS_CAM_POWER_GPIO >= 0
	const int pin = CONFIG_CAMOS_CAM_POWER_GPIO;
	const int level = (on != CAMOS_POWER_ACTIVE_LOW);
	gpio_hold_dis(pin);
	gpio_set_direction(pin, GPIO_MODE_OUTPUT);
	gpio_set_level(pin, level);
	if (on) {
		vTaskDelay(pdMS_TO_TICKS(30)); /* let the supply and the sensor settle */
	}
#endif
	(void)on;
}

static void flash_led(bool on)
{
#if CONFIG_CAMOS_PIN_FLASH_LED >= 0 && CONFIG_CAMOS_FLASH_ON_CAPTURE
	gpio_hold_dis(CONFIG_CAMOS_PIN_FLASH_LED);
	gpio_set_direction(CONFIG_CAMOS_PIN_FLASH_LED, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_CAMOS_PIN_FLASH_LED, on);
#endif
	(void)on;
}

static void cam_off(void)
{
	esp_camera_deinit(); /* PWDN high: the sensor sleeps */
	cam_power(false);
	cam_pm(false);
}

void cam_sleep_prepare(void)
{
#if CONFIG_CAMOS_PIN_PWDN >= 0
	gpio_set_direction(CONFIG_CAMOS_PIN_PWDN, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_CAMOS_PIN_PWDN, 1); /* power down stays asserted while the chip sleeps */
	gpio_hold_en(CONFIG_CAMOS_PIN_PWDN);
#endif
#if CONFIG_CAMOS_CAM_POWER_GPIO >= 0
	gpio_set_level(CONFIG_CAMOS_CAM_POWER_GPIO, CAMOS_POWER_ACTIVE_LOW); /* switch off */
	gpio_hold_en(CONFIG_CAMOS_CAM_POWER_GPIO);
#endif
#if CONFIG_CAMOS_PIN_FLASH_LED >= 0 && CONFIG_CAMOS_FLASH_ON_CAPTURE
	gpio_set_level(CONFIG_CAMOS_PIN_FLASH_LED, 0);
	gpio_hold_en(CONFIG_CAMOS_PIN_FLASH_LED);
#endif
	gpio_deep_sleep_hold_en();
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Protocol capture.jpegQuality 1..100 (higher = better) -> esp32-camera 0..63 (lower = better).
 * Values below 4 overflow the frame buffer on the ESP32 hardware, so 4 is the floor. */
static int jpeg_q(int proto)
{
	return clampi((100 - proto) * 63 / 100, 4, 63);
}

static esp_err_t cam_up(framesize_t fs, int quality, int fb_count)
{
	camera_config_t c = {
		.pin_pwdn = CONFIG_CAMOS_PIN_PWDN,
		.pin_reset = CONFIG_CAMOS_PIN_RESET,
		.pin_xclk = CONFIG_CAMOS_PIN_XCLK,
		.pin_sccb_sda = CONFIG_CAMOS_PIN_SIOD,
		.pin_sccb_scl = CONFIG_CAMOS_PIN_SIOC,
		.pin_d0 = CONFIG_CAMOS_PIN_D0,
		.pin_d1 = CONFIG_CAMOS_PIN_D1,
		.pin_d2 = CONFIG_CAMOS_PIN_D2,
		.pin_d3 = CONFIG_CAMOS_PIN_D3,
		.pin_d4 = CONFIG_CAMOS_PIN_D4,
		.pin_d5 = CONFIG_CAMOS_PIN_D5,
		.pin_d6 = CONFIG_CAMOS_PIN_D6,
		.pin_d7 = CONFIG_CAMOS_PIN_D7,
		.pin_vsync = CONFIG_CAMOS_PIN_VSYNC,
		.pin_href = CONFIG_CAMOS_PIN_HREF,
		.pin_pclk = CONFIG_CAMOS_PIN_PCLK,
		.xclk_freq_hz = CONFIG_CAMOS_XCLK_MHZ * 1000000,
		.ledc_timer = LEDC_TIMER_0,
		.ledc_channel = LEDC_CHANNEL_0,
		.pixel_format = PIXFORMAT_JPEG,
		.frame_size = fs,
		.jpeg_quality = quality,
		/* The driver default is width*height/5, which a detailed scene at good quality can exceed (the frame is then
		 * lost); half of the pixel count is generous and still fits twice in PSRAM at the largest size */
		.jpeg_buffer_size = resolution[fs].width * resolution[fs].height / 2,
		.fb_count = fb_count,
		.fb_location = CAMERA_FB_IN_PSRAM,
		.grab_mode = CAMERA_GRAB_LATEST, /* after the warm-up, the newest frame */
	};
	return esp_camera_init(&c);
}

static framesize_t max_framesize(int pid)
{
	switch (pid) {
	case OV3660_PID:
		return FRAMESIZE_QXGA;
	case OV5640_PID:
		return FRAMESIZE_QSXGA;
	default:
		return FRAMESIZE_UXGA; /* OV2640 and anything we do not know */
	}
}

void cam_max_size(int *w, int *h)
{
	*w = max_fs ? resolution[max_fs].width : 0;
	*h = max_fs ? resolution[max_fs].height : 0;
}

static void apply_image_settings(const settings_t *s)
{
	/* Shadow video.image (protocol 8) -> esp32-camera -2..2.
	 * brightness -64..64 -> /32; contrast and saturation 0..100 -> (v-50)/25 */
	sensor_t *sn = esp_camera_sensor_get();
	sn->set_brightness(sn, clampi((s->brightness + (s->brightness >= 0 ? 16 : -16)) / 32, -2, 2));
	sn->set_contrast(sn, clampi((s->contrast - 50 + (s->contrast >= 50 ? 12 : -12)) / 25, -2, 2));
	sn->set_saturation(sn, clampi((s->saturation - 50 + (s->saturation >= 50 ? 12 : -12)) / 25, -2, 2));
	sn->set_vflip(sn, CAMOS_VFLIP);
	sn->set_hmirror(sn, CAMOS_HMIRROR);
}

/* First power-up: UXGA works on every sensor; the detected one decides the real size */
static esp_err_t detect_and_up(framesize_t want, int q, int fb_count)
{
	esp_err_t err;
	if (!max_fs) {
		err = cam_up(FRAMESIZE_UXGA, q, fb_count);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "camera init failed 0x%x (wiring, power, or wrong board in menuconfig)", err);
			return err;
		}
		sensor_t *sn = esp_camera_sensor_get();
		max_fs = max_framesize(sn->id.PID);
		ESP_LOGI(TAG, "sensor PID 0x%x, maximum %dx%d", sn->id.PID, resolution[max_fs].width, resolution[max_fs].height);
		if (want == FRAMESIZE_UXGA || (want == FRAMESIZE_INVALID && max_fs == FRAMESIZE_UXGA)) {
			return ESP_OK;
		}
		esp_camera_deinit(); /* switch to the real size; the supply stays on */
	}
	err = cam_up(want == FRAMESIZE_INVALID ? max_fs : want, q, fb_count);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "camera init failed 0x%x", err);
	}
	return err;
}

esp_err_t cam_capture(const settings_t *s, camera_fb_t **out)
{
	if (streaming) {
		return ESP_ERR_INVALID_STATE;
	}
	int q = jpeg_q(s->jpeg_quality);
#if CONFIG_CAMOS_PIN_PWDN >= 0
	gpio_hold_dis(CONFIG_CAMOS_PIN_PWDN); /* released from deep sleep hold */
#endif
	gpio_deep_sleep_hold_dis();
	cam_power(true);
	esp_err_t err = detect_and_up(FRAMESIZE_INVALID, q, 2);
	if (err != ESP_OK) {
		cam_off();
		return err;
	}
	apply_image_settings(s);
	flash_led(true);
	for (int i = 0; i < CONFIG_CAMOS_WARMUP_FRAMES; i++) {
		camera_fb_t *f = esp_camera_fb_get();
		if (f) {
			esp_camera_fb_return(f);
		}
	}
	camera_fb_t *fb = esp_camera_fb_get();
	flash_led(false);
	if (!fb || fb->format != PIXFORMAT_JPEG || fb->len < 1000) {
		ESP_LOGE(TAG, "no usable frame (%s)", fb ? "too small" : "timeout");
		if (fb) {
			esp_camera_fb_return(fb);
		}
		cam_off();
		return ESP_FAIL;
	}
	*out = fb;
	return ESP_OK;
}

void cam_release(camera_fb_t *fb)
{
	esp_camera_fb_return(fb);
	cam_off();
}

/* ------------------------------------------------------------------ live view stream (small frames, sensor kept up) */

static framesize_t framesize_for(int width, int height)
{
	for (int fs = 0; fs < FRAMESIZE_INVALID; fs++) {
		if (resolution[fs].width == width && resolution[fs].height == height) {
			return (framesize_t)fs;
		}
	}
	return FRAMESIZE_QVGA;
}

esp_err_t cam_stream_start(int width, int height, int quality)
{
	if (streaming) {
		return ESP_OK;
	}
	cam_power(true);
	esp_err_t err = detect_and_up(framesize_for(width, height), clampi(quality, 4, 63), 2);
	if (err != ESP_OK) {
		cam_off();
		return err;
	}
	settings_t s;
	shadow_get(&s);
	apply_image_settings(&s);
	streaming = true;
	return ESP_OK;
}

void cam_stream_stop(void)
{
	if (!streaming) {
		return;
	}
	streaming = false;
	cam_off();
}
