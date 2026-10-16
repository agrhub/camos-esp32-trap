/*
 * GPS: ATGM336H (NMEA 0183 at 9600 baud, 3.3 V) on a UART, power-gated by a GPIO (P-MOSFET). A fix is taken
 * once every CAMOS_GPS_FIX_INTERVAL_H (24 h) at a wake-up, or on the RPC `gps.fix` / console `gps`. The last fix
 * survives deep sleep in RTC memory and reboots in NVS, and is reported as shadow `location {lat,lng,fixAt}`
 * (CONTRACTS section 3) and `state.gps` (protocol 4). When the clock is not set yet, the RMC date/time sets it
 * (protocol 3: GPS time when there is no NTP).
 *
 * A trap does not move, so one fix a day is plenty; the module is off the rest of the time (it keeps no almanac
 * without a backup supply, so every fix is a cold start: about 35 s with a clear sky per the datasheet, more
 * under trees; not measured here).
 */
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"
#include "nmea.h"

static const char *TAG = "gps";

#define FIX_MAGIC 0x47505346
static RTC_DATA_ATTR uint32_t rtc_magic;
static RTC_DATA_ATTR gps_fix_t rtc_fix;

static void load_last(void)
{
	static bool loaded;
	if (loaded) {
		return;
	}
	loaded = true;
	if (rtc_magic != FIX_MAGIC) {
		gps_fix_t f;
		if (store_get_blob("gps_fix", &f, sizeof(f))) {
			rtc_fix = f;
		} else {
			memset(&rtc_fix, 0, sizeof(rtc_fix));
		}
		rtc_magic = FIX_MAGIC;
	}
}

bool gps_last(gps_fix_t *out)
{
	load_last();
	*out = rtc_fix;
	return rtc_fix.fix_at != 0;
}

bool gps_fix_due(void)
{
	if (!gps_available() || !clock_synced()) {
		return false;
	}
	load_last();
	return rtc_fix.fix_at == 0 || time(NULL) - rtc_fix.fix_at >= (time_t)CONFIG_CAMOS_GPS_FIX_INTERVAL_H * 3600;
}

void gps_json(cJSON *parent)
{
	gps_fix_t f;
	if (!gps_last(&f)) {
		return;
	}
	cJSON *loc = cJSON_AddObjectToObject(parent, "location");
	cJSON_AddNumberToObject(loc, "lat", round(f.lat * 1e6) / 1e6);
	cJSON_AddNumberToObject(loc, "lng", round(f.lng * 1e6) / 1e6);
	char s[25];
	iso8601(f.fix_at, s);
	cJSON_AddStringToObject(loc, "fixAt", s);
}

void gps_state_json(cJSON *parent)
{
	gps_fix_t f;
	if (!gps_available()) {
		return;
	}
	cJSON *g = cJSON_AddObjectToObject(parent, "gps");
	bool have = gps_last(&f);
	if (have) {
		cJSON_AddNumberToObject(g, "lat", round(f.lat * 1e6) / 1e6);
		cJSON_AddNumberToObject(g, "lon", round(f.lng * 1e6) / 1e6);
		cJSON_AddNumberToObject(g, "ts", (double)f.fix_at * 1000);
	}
	cJSON_AddBoolToObject(g, "fix", have);
}

#if CAMOS_GPS
#include "driver/uart.h"

bool gps_available(void)
{
	return CONFIG_CAMOS_GPS_RX_GPIO >= 0 && g_cfg.gps && !store_get_flag("gps_absent");
}

static void gps_power(bool on)
{
#if CONFIG_CAMOS_GPS_POWER_GPIO >= 0
	int active_low =
#ifdef CONFIG_CAMOS_GPS_POWER_ACTIVE_LOW
		1;
#else
		0;
#endif
	gpio_hold_dis(CONFIG_CAMOS_GPS_POWER_GPIO);
	gpio_set_direction(CONFIG_CAMOS_GPS_POWER_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_CAMOS_GPS_POWER_GPIO, on != active_low);
	if (!on) {
		gpio_hold_en(CONFIG_CAMOS_GPS_POWER_GPIO); /* stays off through deep sleep */
	}
#endif
	(void)on;
}

/* Haversine distance in metres, for the location_changed event */
static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
	double r = 6371000, p = M_PI / 180;
	double a = 0.5 - cos((lat2 - lat1) * p) / 2 + cos(lat1 * p) * cos(lat2 * p) * (1 - cos((lon2 - lon1) * p)) / 2;
	return 2 * r * asin(sqrt(a));
}

bool gps_fix(int timeout_s, gps_fix_t *out)
{
	const uart_port_t u = CONFIG_CAMOS_GPS_UART_NUM;
	uart_config_t uc = {
		.baud_rate = CONFIG_CAMOS_GPS_BAUD,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	if (!uart_is_driver_installed(u)) {
		if (uart_driver_install(u, 2048, 0, 0, NULL, 0) != ESP_OK || uart_param_config(u, &uc) != ESP_OK ||
			uart_set_pin(u, CONFIG_CAMOS_GPS_TX_GPIO, CONFIG_CAMOS_GPS_RX_GPIO, -1, -1) != ESP_OK) {
			ESP_LOGE(TAG, "UART %d init failed", u);
			return false;
		}
	}
	gps_power(true);
	uart_flush_input(u);
	ESP_LOGI(TAG, "waiting for a fix (up to %d s)", timeout_s);

	nmea_fix_t fix;
	nmea_init(&fix);
	char line[128];
	int li = 0, sentences = 0;
	int64_t deadline = esp_timer_get_time() + (int64_t)timeout_s * 1000000, got_rmc_at = 0;
	bool ok = false;
	while (esp_timer_get_time() < deadline) {
		uint8_t c;
		if (uart_read_bytes(u, &c, 1, pdMS_TO_TICKS(1000)) != 1) {
			continue;
		}
		if (c == '\n' || c == '\r') {
			if (li > 6) {
				line[li] = 0;
				int t = nmea_parse_line(line, &fix);
				if (t > 0) {
					sentences++;
				}
				if (t == 1 && fix.valid) {
					if (!got_rmc_at) {
						got_rmc_at = esp_timer_get_time();
					}
					/* give GGA 3 s to arrive for the satellite count, then stop */
					if (fix.sats >= 0 || esp_timer_get_time() - got_rmc_at > 3000000) {
						ok = true;
						break;
					}
				}
			}
			li = 0;
		} else if (li < (int)sizeof(line) - 1) {
			line[li++] = (char)c;
		}
	}
	gps_power(false);
	if (!ok) {
		ESP_LOGW(TAG, "no fix in %d s (%d sentences seen%s)", timeout_s, sentences, sentences ? "" : ": is the module powered and TX wired to the RX pin?");
		if (sentences == 0 && timeout_s >= 30) {
			store_set_flag("gps_absent", 1); /* no NMEA at all: no module; set gps on again in the portal to retry */
			ESP_LOGW(TAG, "no GPS module answered: GPS skipped from now on (portal / console `set gps 1` to retry)");
		}
		return false;
	}
	if (!clock_synced() && fix.epoch > 1735689600) {
		struct timeval tv = { .tv_sec = (time_t)fix.epoch };
		settimeofday(&tv, NULL);
		clock_set_source("gps");
	}
	load_last();
	gps_fix_t prev = rtc_fix;
	rtc_fix.lat = fix.lat;
	rtc_fix.lng = fix.lng;
	rtc_fix.sats = fix.sats;
	rtc_fix.fix_at = clock_synced() ? time(NULL) : (time_t)fix.epoch;
	store_set_blob("gps_fix", &rtc_fix, sizeof(rtc_fix));
	ESP_LOGI(TAG, "fix %.6f, %.6f, %d sats, hdop %.1f", fix.lat, fix.lng, fix.sats, fix.hdop);
	if (prev.fix_at) {
		double d = distance_m(prev.lat, prev.lng, fix.lat, fix.lng);
		if (d > 50) { /* protocol 5 location_changed; 50 m is well above the ATGM336H's position error */
			char id[27];
			cJSON *e = evt_new("location_changed", id);
			cJSON_AddNumberToObject(e, "lat", round(fix.lat * 1e6) / 1e6);
			cJSON_AddNumberToObject(e, "lon", round(fix.lng * 1e6) / 1e6);
			cJSON_AddNumberToObject(e, "distanceM", round(d));
			proto_publish_event(e);
		}
	}
	if (out) {
		*out = rtc_fix;
	}
	return true;
}

#else

bool gps_available(void) { return false; }
bool gps_fix(int timeout_s, gps_fix_t *out)
{
	(void)timeout_s;
	(void)out;
	ESP_LOGW(TAG, "no GPS in this build (CAMOS_GPS)");
	return false;
}

#endif
