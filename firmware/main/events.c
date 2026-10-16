/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * The last events in RAM for the web console's Events page: every protocol event the camera publishes (capture,
 * motion, low_power, ...) and a few local ones (SD card, firmware upload). Lost at restart; the cloud keeps the
 * protocol events.
 */
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "camos.h"

#define N_EVENTS 64
typedef struct {
	time_t at;          /* wall clock, 0 when the clock was not synced */
	uint32_t uptime_s;
	char type[16];
	char text[96];
} event_t;

static event_t ring[N_EVENTS];
static unsigned total;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void events_note(const char *type, const char *text)
{
	event_t e = { 0 };
	e.at = clock_synced() ? time(NULL) : 0;
	e.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
	strlcpy(e.type, type ? type : "?", sizeof(e.type));
	strlcpy(e.text, text ? text : "", sizeof(e.text));
	portENTER_CRITICAL(&mux);
	ring[total % N_EVENTS] = e;
	total++;
	portEXIT_CRITICAL(&mux);
}

cJSON *events_json(void)
{
	cJSON *arr = cJSON_CreateArray();
	static event_t copy[N_EVENTS];
	portENTER_CRITICAL(&mux);
	unsigned n = total < N_EVENTS ? total : N_EVENTS, end = total;
	for (unsigned i = 0; i < n; i++) {
		copy[i] = ring[(end - 1 - i) % N_EVENTS];
	}
	portEXIT_CRITICAL(&mux);
	for (unsigned i = 0; i < n; i++) {
		cJSON *o = cJSON_CreateObject();
		if (copy[i].at) {
			struct tm lt;
			localtime_r(&copy[i].at, &lt);
			char s[24];
			strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &lt);
			cJSON_AddStringToObject(o, "time", s);
		}
		cJSON_AddNumberToObject(o, "uptimeS", copy[i].uptime_s);
		cJSON_AddStringToObject(o, "type", copy[i].type);
		cJSON_AddStringToObject(o, "text", copy[i].text);
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}
