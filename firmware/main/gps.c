/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * GPS: ATGM336H (NMEA 0183 at 9600 baud, 3.3 V) on a UART, power-gated by a GPIO (P-MOSFET). A fix is taken
 * once every CAMOS_GPS_FIX_INTERVAL_H (24 h) at a wake-up, or on the RPC `gps.fix` / console `gps`. The last fix
 * survives deep sleep in RTC memory and reboots in NVS, and is reported as shadow `location {lat,lng,fixAt,source,
 * accuracyM}`
 * (CONTRACTS section 3) and `state.gps` (protocol 4). When the clock is not set yet, the RMC date/time sets it
 * (protocol 3: GPS time when there is no NTP).
 *
 * A trap does not move, so one fix a day is plenty; the module is off the rest of the time (it keeps no almanac
 * without a backup supply, so a fix after a power-off is a cold start: about 35 s with a clear sky per the datasheet,
 * minutes with few satellites in view), except for GPS_WARM_S after a try that found no fix.
 *
 * No GPS fix: the position of the 4G cell (AT+CLBS, SIMCom's location service) is taken instead, `source: "cell"`
 * with its accuracy radius. It never replaces a GPS fix, and the GPS is tried again 6 h later.
 */
#include <string.h>
#include <stdlib.h>
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

#define FIX_MAGIC 0x47505347 /* changed with gps_fix_t (acc_m, cell) */
static RTC_DATA_ATTR uint32_t rtc_magic;
static RTC_DATA_ATTR gps_fix_t rtc_fix;
/* the last fix that found nothing: indoors the daily fix ran again every minute (2 of every 3 minutes with the module on,
 * seen 23 Oct 2026), so it waits GPS_RETRY_S first; deep sleep keeps it, a power-on clears it */
static RTC_DATA_ATTR time_t failed_at;
#define GPS_RETRY_S 3600

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
	if (failed_at && time(NULL) - failed_at < GPS_RETRY_S) {
		return false;
	}
	time_t every = rtc_fix.cell ? 6 * 3600 : (time_t)CONFIG_CAMOS_GPS_FIX_INTERVAL_H * 3600; /* a cell position: try the GPS again */
	return rtc_fix.fix_at == 0 || time(NULL) - rtc_fix.fix_at >= every;
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
	cJSON_AddStringToObject(loc, "source", f.cell ? "cell" : "gps");
	if (f.acc_m > 0) {
		cJSON_AddNumberToObject(loc, "accuracyM", f.acc_m);
	}
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
		cJSON_AddStringToObject(g, "source", f.cell ? "cell" : "gps");
	}
	cJSON_AddBoolToObject(g, "fix", have);
}

#if CAMOS_GPS
#include "driver/uart.h"

bool gps_available(void)
{
	return CONFIG_CAMOS_GPS_RX_GPIO >= 0 && g_cfg.gps && !store_get_flag("gps_absent");
}

/* After a fix that did not come, the module stays powered for GPS_WARM_S: it keeps the satellites it has found and the
 * ephemeris it has read, so the next try is a warm start instead of another cold one (outdoors on 24 Oct 2026 each
 * 120 s try started from nothing, with 2-3 satellites in view). Deep sleep switches it off (gps_sleep_prepare). */
#define GPS_WARM_S 600
static esp_timer_handle_t warm_timer;

static void gps_power(bool on)
{
	if (warm_timer) {
		esp_timer_stop(warm_timer); /* not running: harmless */
	}
#if CONFIG_CAMOS_GPS_POWER_GPIO >= 0
	int active_low = g_cfg.gps_pwr_low;
	gpio_hold_dis(CONFIG_CAMOS_GPS_POWER_GPIO);
	gpio_set_direction(CONFIG_CAMOS_GPS_POWER_GPIO, GPIO_MODE_OUTPUT);
	if (!active_low) {
		/* the pin powers the module itself (no switch): strongest drive, about 40 mA */
		gpio_set_drive_capability(CONFIG_CAMOS_GPS_POWER_GPIO, GPIO_DRIVE_CAP_3);
	}
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

static bool fix_once(int timeout_s, gps_fix_t *out);
static bool cell_fallback(gps_fix_t *out);
static volatile bool fixing;

static void warm_end(void *arg)
{
	if (!fixing) {
		ESP_LOGI(TAG, "no fix asked for %d min: GPS off", GPS_WARM_S / 60);
		gps_power(false);
	}
}

/* Keep the module on for GPS_WARM_S after a fix that did not come */
static void gps_keep_warm(void)
{
	if (!warm_timer) {
		const esp_timer_create_args_t a = { .callback = warm_end, .name = "gps-warm" };
		if (esp_timer_create(&a, &warm_timer) != ESP_OK) {
			gps_power(false);
			return;
		}
	}
	esp_timer_stop(warm_timer);
	esp_timer_start_once(warm_timer, (uint64_t)GPS_WARM_S * 1000000ULL);
	ESP_LOGI(TAG, "GPS kept on for %d min: the next try starts warm", GPS_WARM_S / 60);
}

void gps_sleep_prepare(void)
{
	if (gps_available()) {
		gps_power(false);
	}
}

/* One fix at a time: the scheduler's daily fix and a console or RPC request would share the UART and the first to
 * finish would switch the module off under the other (seen 22 Oct 2026: 166, then 67, then 7 sentences): `fixing` */

bool gps_fix(int timeout_s, gps_fix_t *out)
{
	if (__atomic_test_and_set(&fixing, __ATOMIC_ACQUIRE)) {
		ESP_LOGW(TAG, "a fix is already running");
		return false;
	}
	/* The 4G module's transmitter drowns the GPS: outdoors with 4G on, 2-3 satellites in view and no fix in three 300 s
	 * tries (24 Oct 2026); with 4G off it had found 5 outdoors (22 Oct). Its radio is off for the fix and back after
	 * (the cell position needs it), and the caller's reply or report waits for MQTT; not while a live viewer watches. */
	bool quiet = net_type() == NET_4G && modem_up() && !live_streaming();
	if (quiet) {
		/* FlyBot takes the silence that follows as a fix, not an outage (event gps_fix: started, then done) */
		char id[27];
		cJSON *e = evt_new("gps_fix", id);
		cJSON_AddStringToObject(e, "state", "started");
		cJSON_AddNumberToObject(e, "seconds", timeout_s);
		proto_publish_event(e);
		proto_flush(3000);
		modem_radio_pause();
	}
	bool ok = fix_once(timeout_s, out);
	if (quiet) {
		modem_radio_resume();
		for (int i = 0; i < 30 && !proto_connected(); i++) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
		char id[27];
		cJSON *e = evt_new("gps_fix", id);
		cJSON_AddStringToObject(e, "state", "done");
		cJSON_AddBoolToObject(e, "fix", ok);
		proto_publish_event(e);
	}
	ok = ok || cell_fallback(out);
	failed_at = ok ? 0 : (clock_synced() ? time(NULL) : 0);
	__atomic_clear(&fixing, __ATOMIC_RELEASE);
	return ok;
}

static bool fix_once(int timeout_s, gps_fix_t *out)
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
	int64_t next_note = esp_timer_get_time() + 15000000;
	int in_view = 0; /* GSV field 3, the largest of the talkers (GP, BD, GN): 0 means the antenna hears nothing */
	int best_snr = 0; /* strongest satellite in this 15 s window, dB-Hz: under 20 weak, 30 and up good */
	uint16_t strong[32]; /* satellites at 25 dB-Hz or more in this window (talker << 8 | id): a fix needs 4 */
	int n_strong = 0;
	bool ok = false;
	while (esp_timer_get_time() < deadline) {
		if (esp_timer_get_time() >= next_note) {
			/* progress: no sentences = no module or wiring; sentences but 0 satellites = no sky view yet */
			ESP_LOGI(TAG, "still searching: %d sentences, %d satellites in view, %d at 25 dB-Hz or more, %d in use, best %d dB-Hz",
					 sentences, in_view, n_strong, fix.sats < 0 ? 0 : fix.sats, best_snr);
			best_snr = 0;
			n_strong = 0;
			next_note += 15000000;
		}
		uint8_t c;
		if (uart_read_bytes(u, &c, 1, pdMS_TO_TICKS(1000)) != 1) {
			continue;
		}
		if (c == '\n' || c == '\r') {
			if (li > 6) {
				line[li] = 0;
				/* $xxGSV,<msgs>,<msg>,<in view>,... */
				if (line[0] == '$' && !strncmp(line + 3, "GSV,", 4)) {
					/* fields: 1 msgs, 2 msg, 3 in view, then per satellite: id, elevation, azimuth, SNR (dB-Hz) */
					const char *p = line;
					int sat_id = 0;
					for (int fi = 1; fi <= 19 && (p = strchr(p, ',')) != NULL; fi++) {
						p++;
						if (fi >= 4 && (fi - 4) % 4 == 0) {
							sat_id = atoi(p);
						}
						if (fi == 3) {
							int v = atoi(p);
							in_view = v > in_view ? v : in_view;
						} else if (fi >= 7 && (fi - 7) % 4 == 0 && *p >= '0' && *p <= '9') {
							int snr = atoi(p);
							best_snr = snr > best_snr ? snr : best_snr;
							uint16_t key = (uint16_t)((uint8_t)line[2] << 8 | (sat_id & 0xff));
							bool seen = false;
							for (int k = 0; k < n_strong; k++) {
								seen |= strong[k] == key;
							}
							if (snr >= 25 && !seen && n_strong < 32) {
								strong[n_strong++] = key;
							}
						}
					}
				}
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
	if (ok || !sentences) {
		gps_power(false);
	} else {
		gps_keep_warm();
	}
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
	rtc_fix.cell = false;
	rtc_fix.acc_m = fix.hdop > 0 ? (int)lround(fix.hdop * 5) : 0; /* HDOP x about 5 m for a consumer receiver */
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

/* No GPS fix: the 4G cell's position. A GPS fix already taken stays (a trap does not move, and it is far more
 * precise); a cell position only fills in when there is none, or replaces an older cell one. */
static bool cell_fallback(gps_fix_t *out)
{
	double lat, lng;
	int acc;
	if (!modem_up() || !modem_cell_location(&lat, &lng, &acc)) {
		return false;
	}
	load_last();
	if (rtc_fix.fix_at && !rtc_fix.cell) {
		ESP_LOGI(TAG, "cell position %.4f, %.4f (%d m) not used: the GPS fix stays", lat, lng, acc);
		return false;
	}
	rtc_fix.lat = lat;
	rtc_fix.lng = lng;
	rtc_fix.sats = 0;
	rtc_fix.acc_m = acc;
	rtc_fix.cell = true;
	rtc_fix.fix_at = time(NULL);
	store_set_blob("gps_fix", &rtc_fix, sizeof(rtc_fix));
	ESP_LOGI(TAG, "no GPS fix: cell position %.6f, %.6f, accuracy %d m", lat, lng, acc);
	if (out) {
		*out = rtc_fix;
	}
	return true;
}

#else

bool gps_available(void) { return false; }
void gps_sleep_prepare(void) {}
bool gps_fix(int timeout_s, gps_fix_t *out)
{
	(void)timeout_s;
	(void)out;
	ESP_LOGW(TAG, "no GPS in this build (CAMOS_GPS)");
	return false;
}

#endif
