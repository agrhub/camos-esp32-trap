/*
 * Live view: KVS WebRTC MASTER (CONTRACTS section 4). The camera joins the signaling channel `kvs.channelName`
 * when power.mode is powered, or for `live.start {minutes}` / `live.requested` otherwise, and only encodes while a
 * VIEWER is connected. See docs/dev/LIVE_VIEW.md for the state of this part.
 *
 * CAMOS_LIVE off (the default, and always on the ESP32-CAM): every call answers NOT_SUPPORTED and FlyBot uses
 * `snapshot.now` instead. CAMOS_LIVE on (ESP32-S3): live_kvs.c provides the implementation.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "camos.h"

static const char *TAG = "live";

#if CAMOS_LIVE

/* implemented in live_kvs.c */
esp_err_t live_kvs_start(void);
void live_kvs_stop(void);
static int64_t until_us;

bool live_supported(void) { return g_cfg.kvs_channel[0] && g_cfg.kvs_region[0]; }
bool live_active(void) { return until_us > esp_timer_get_time(); }

int live_remaining_s(void)
{
	int64_t left = until_us - esp_timer_get_time();
	return left > 0 ? (int)(left / 1000000) : 0;
}

esp_err_t live_start(int minutes, char *err, size_t errn)
{
	err[0] = 0;
	if (!live_supported()) {
		strlcpy(err, "no kvs.channelName / region (pair first)", errn);
		return ESP_ERR_NOT_SUPPORTED;
	}
	if (live_active()) {
		until_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000; /* extend */
		sched_keep_awake(minutes * 60 + 10);
		return ESP_OK;
	}
	esp_err_t e = live_kvs_start();
	if (e != ESP_OK) {
		snprintf(err, errn, "kvs master: %s", esp_err_to_name(e));
		return e;
	}
	until_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000;
	sched_keep_awake(minutes * 60 + 10);
	shadow_clear_live_request();
	ESP_LOGI(TAG, "live view for %d min on channel %s", minutes, g_cfg.kvs_channel);
	return ESP_OK;
}

void live_stop(void)
{
	if (!live_active()) {
		return;
	}
	until_us = 0;
	live_kvs_stop();
	ESP_LOGI(TAG, "live view stopped");
}

#else

bool live_supported(void) { return false; }
bool live_active(void) { return false; }
int live_remaining_s(void) { return 0; }

esp_err_t live_start(int minutes, char *err, size_t errn)
{
	(void)minutes;
	strlcpy(err, "live view not built (CAMOS_LIVE, ESP32-S3 only): use snapshot.now", errn);
	ESP_LOGW(TAG, "%s", err);
	return ESP_ERR_NOT_SUPPORTED;
}

void live_stop(void) {}

#endif
