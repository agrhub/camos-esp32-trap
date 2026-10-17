/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Live view: KVS WebRTC MASTER (CONTRACTS section 4). The camera joins the signaling channel `kvs.channelName`
 * all the time in powered mode (CAMOS_LIVE_STANDBY), or for `live.start {minutes}` / `live.requested` otherwise, and
 * only encodes while a VIEWER is connected (live_kvs.c). See docs/dev/LIVE_VIEW.md.
 *
 * CAMOS_LIVE off (the ESP32-CAM, and any build without it): every call answers NOT_SUPPORTED and FlyBot uses
 * `snapshot.now` instead.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "camos.h"

static const char *TAG = "live";

#if CAMOS_LIVE

/* implemented in live_kvs.c */
void live_kvs_init(void);
esp_err_t live_kvs_start(void);
void live_kvs_stop(void);
bool live_kvs_creds_source(void);
static volatile int64_t until_us;

void live_init(void) { live_kvs_init(); }

/* Built in, paired (channel and region from FlyBot) and able to get KVS credentials (credential provider or bench
 * keys): otherwise FlyBot offers a capture and the camera's local page instead of a viewer that would wait forever */
bool live_supported(void) { return g_cfg.kvs_channel[0] && g_cfg.kvs_region[0] && live_kvs_creds_source(); }
bool live_active(void) { return until_us > esp_timer_get_time(); }

int live_remaining_s(void)
{
	int64_t left = until_us - esp_timer_get_time();
	return left > 0 ? (int)(left / 1000000) : 0;
}

esp_err_t live_start(int minutes, char *err, size_t errn)
{
	err[0] = 0;
	if (!g_cfg.kvs_channel[0] || !g_cfg.kvs_region[0]) {
		strlcpy(err, "no kvs.channelName / region (pair first)", errn);
		return ESP_ERR_NOT_SUPPORTED;
	}
	if (!live_kvs_creds_source()) {
		strlcpy(err, "no KVS credentials: credHost / roleAlias missing (see LIVE_VIEW.md)", errn);
		return ESP_ERR_NOT_SUPPORTED;
	}
	if (minutes < 1) {
		minutes = 1;
	} else if (minutes > 60) {
		minutes = 60;
	}
	bool extend = live_active();
	esp_err_t e = live_kvs_start();
	if (e != ESP_OK) {
		snprintf(err, errn, "kvs master: %s", esp_err_to_name(e));
		return e;
	}
	until_us = esp_timer_get_time() + (int64_t)minutes * 60 * 1000000;
	sched_keep_awake(minutes * 60 + 10);
	shadow_clear_live_request();
	ESP_LOGI(TAG, "live view %s: %d min on channel %s", extend ? "extended" : "started", minutes, g_cfg.kvs_channel);
	return ESP_OK;
}

void live_stop(void)
{
	until_us = 0;
	live_kvs_stop();
	ESP_LOGI(TAG, "live view stopped");
}

#else

void live_init(void) {}
bool live_supported(void) { return false; }
bool live_active(void) { return false; }
bool live_streaming(void) { return false; }
int live_pause(int timeout_ms) { return 0; }
void live_resume(void) {}
int live_remaining_s(void) { return 0; }

esp_err_t live_start(int minutes, char *err, size_t errn)
{
	(void)minutes;
	strlcpy(err, "live view not built (CAMOS_LIVE, ESP32-S3 only): use snapshot.now", errn);
	ESP_LOGW(TAG, "%s", err);
	return ESP_ERR_NOT_SUPPORTED;
}

void live_stop(void) {}

void live_status_json(cJSON *o)
{
	cJSON_AddStringToObject(o, "state", "not built");
}

#endif
