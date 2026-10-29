/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Battery and solar monitor: LiPo voltage (and optionally the panel / charger input) through resistor dividers on
 * ADC pins, plus an optional charger status pin (CHRG of a TP4056 / CN3791 board).
 * Reported as `battery {voltage, percent}` (the fields of the reference firmware, kept) plus the FlyBot fields
 * `mv, pct, charging, solarMv` (CONTRACTS section 3) in `state`, `telemetry` and the shadow `reported`, and as the
 * `low_power` event (protocol 5, `voltage`) when the cell is below CAMOS_BATT_LOW_MV.
 *
 * ESP32 only: ADC2 pins cannot be read while Wi-Fi runs. The firmware samples before it starts the network (every
 * wake in battery mode) and keeps the last value; with an ADC2 pin and Wi-Fi always on, the value is only refreshed
 * at boot. ESP32-S3 pins on ADC1 (GPIO1-10) and ESP32 ADC1 pins (GPIO32-39) have no such limit.
 */
#include <math.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "camos.h"
#include "battery_est.h"

static const char *TAG = "battery";

static int last_mv = -1;   /* cell voltage in mV as read (a charger lifts it), -1 unknown */
static int last_solar = -1;
static int last_charging = -1; /* -1 unknown */
/* The charge's lift on the reading (battery_est.h), kept through deep sleep: the last reading without charge, and the
 * first reading and lift of the charge in progress (lift -1: not charging) */
static RTC_DATA_ATTR int rest_mv = -1;
static RTC_DATA_ATTR int charge_start_mv = -1;
static RTC_DATA_ATTR int charge_lift = -1;
#define CHARGE_MARGIN_MV 100       /* the solar input this far above the cell: current flows into it */

/* Typical single-cell LiPo discharge curve (approximation: it varies with load, temperature and cell) */
static const struct { int mv, pct; } curve[] = {
	{ 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3900, 65 }, { 3800, 50 },
	{ 3700, 30 }, { 3600, 15 }, { 3500, 7 }, { 3300, 0 },
};

static int percent_from_mv(int mv)
{
	if (mv >= curve[0].mv) {
		return 100;
	}
	for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv >= curve[i].mv) {
			return curve[i].pct + (curve[i - 1].pct - curve[i].pct) * (mv - curve[i].mv) / (curve[i - 1].mv - curve[i].mv);
		}
	}
	return 0;
}

/* One divider on one pin -> source voltage in mV, -1 on failure. 16 samples averaged, calibrated when the chip has
 * calibration data (otherwise a rough 12 dB scale). */
static int read_divider_mv(int gpio, int ratio_x100)
{
	adc_unit_t unit;
	adc_channel_t chan;
	if (gpio < 0 || adc_oneshot_io_to_channel(gpio, &unit, &chan) != ESP_OK) {
		if (gpio >= 0) {
			ESP_LOGE(TAG, "GPIO%d is not an ADC pin", gpio);
		}
		return -1;
	}
	if (unit == ADC_UNIT_2 && wifi_started()) {
		return -2; /* ESP32: ADC2 is taken by Wi-Fi, keep the value from before it started */
	}
	adc_oneshot_unit_handle_t adc;
	adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
	if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) {
		return -1;
	}
	adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
	adc_oneshot_config_channel(adc, chan, &ccfg);

	adc_cali_handle_t cali = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	adc_cali_curve_fitting_config_t cc = { .unit_id = unit, .chan = chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
	if (adc_cali_create_scheme_curve_fitting(&cc, &cali) != ESP_OK) {
		cali = NULL;
	}
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	adc_cali_line_fitting_config_t lc = { .unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
	if (adc_cali_create_scheme_line_fitting(&lc, &cali) != ESP_OK) {
		cali = NULL;
	}
#endif
	int sum = 0, n = 0;
	for (int i = 0; i < 16; i++) {
		int raw;
		if (adc_oneshot_read(adc, chan, &raw) == ESP_OK) {
			sum += raw;
			n++;
		}
	}
	int mv = -1;
	if (n > 0) {
		int raw = sum / n, pin_mv;
		if (cali) {
			adc_cali_raw_to_voltage(cali, raw, &pin_mv);
		} else {
			pin_mv = raw * 3100 / 4095; /* uncalibrated: rough, 12 dB range is about 0..3.1 V */
		}
		/* divider: Vsrc = Vpin * (Rtop + Rbottom) / Rbottom, given x 100 */
		mv = pin_mv * ratio_x100 / 100;
	}
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	if (cali) {
		adc_cali_delete_scheme_curve_fitting(cali);
	}
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	if (cali) {
		adc_cali_delete_scheme_line_fitting(cali);
	}
#endif
	adc_oneshot_del_unit(adc);
	return mv;
}

bool battery_sample(void)
{
	if (!g_cfg.batt_mon) {
		last_mv = last_solar = last_charging = -1; /* no divider fitted: report nothing rather than noise */
		return false;
	}
	int mv = read_divider_mv(CONFIG_CAMOS_BATT_ADC_GPIO, g_cfg.batt_div);
	if (mv >= 0) {
		last_mv = mv;
	}
	int sol = read_divider_mv(CONFIG_CAMOS_SOLAR_ADC_GPIO, g_cfg.solar_div);
	if (sol >= 0) {
		last_solar = sol;
	}
#if CONFIG_CAMOS_CHARGING_GPIO >= 0
	gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_CAMOS_CHARGING_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
	gpio_config(&io);
	int level = gpio_get_level(CONFIG_CAMOS_CHARGING_GPIO);
#ifdef CONFIG_CAMOS_CHARGING_ACTIVE_LOW
	last_charging = level == 0;
#else
	last_charging = level != 0;
#endif
#else
	/* a small panel that cannot give the charger its full current sags to just above the cell (3.98 V over a 3.84 V
	 * cell in full sun, 24 Oct 2026, the cell rising): above the cell by CHARGE_MARGIN_MV it still charges */
	last_charging = last_solar < 0 ? -1
					: last_solar >= CONFIG_CAMOS_SOLAR_PRESENT_MV || (last_mv > 0 && last_solar >= last_mv + CHARGE_MARGIN_MV);
#endif
	if (mv >= 0) {
		if (last_charging != 1) {
			rest_mv = mv, charge_lift = -1;
		} else if (charge_lift < 0) {
			charge_start_mv = mv, charge_lift = charge_lift_mv(rest_mv, mv); /* first reading of this charge */
		}
	}
	if (last_mv >= 0) {
		ESP_LOGI(TAG, "battery %d mV (cell %d mV, %d %%), solar %d mV, charging %s", last_mv, battery_cell_mv(),
				 battery_pct(), last_solar, battery_charge_name());
	}
	return mv >= 0;
}

int battery_cell_mv(void)
{
	if (last_mv < 0 || last_charging != 1 || charge_lift <= 0) {
		return last_mv;
	}
	return cell_mv_while_charging(last_mv, charge_start_mv, charge_lift);
}

/* Slow: charging, but the input under CAMOS_SOLAR_PRESENT_MV, so the charger gets less than its set current (a small
 * or shaded panel); the cell gains, slowly, and may not keep up with an always-on camera */
static bool charging_slow(void)
{
	return last_charging == 1 && last_solar >= 0 && last_solar < CONFIG_CAMOS_SOLAR_PRESENT_MV;
}

const char *battery_charge_name(void)
{
	return last_charging < 0 ? "unknown" : !last_charging ? "no" : charging_slow() ? "slow" : "yes";
}

bool battery_charging_fast(void) { return last_charging == 1 && !charging_slow(); }

int battery_mv(void) { return last_mv; }
int battery_pct(void) { return last_mv >= 0 ? percent_from_mv(battery_cell_mv()) : -1; }
int solar_mv(void) { return last_solar; }
bool battery_charging(void) { return last_charging == 1; }

/* low_power is repeated at most every 6 hours; the stamp survives deep sleep in RTC memory */
#define LOW_MAGIC 0x4c4f5750
static RTC_NOINIT_ATTR uint32_t low_magic;
static RTC_NOINIT_ATTR uint64_t low_sent_ms;

bool battery_low_due(void)
{
	if (last_mv < 0 || last_mv >= CONFIG_CAMOS_BATT_LOW_MV || !clock_synced()) {
		return false;
	}
	uint64_t now = now_ms();
	if (low_magic == LOW_MAGIC && now - low_sent_ms < 6ULL * 3600 * 1000) {
		return false;
	}
	low_magic = LOW_MAGIC;
	low_sent_ms = now;
	return true;
}

bool battery_critical(void)
{
	return CONFIG_CAMOS_BATT_CRITICAL_MV > 0 && last_mv >= 0 && last_mv < CONFIG_CAMOS_BATT_CRITICAL_MV;
}

/* A critical cell with no charge coming in: the camera only protects it. Charging (a panel in the sun, a charger), it
 * runs as usual even from an empty cell: on 26 Oct 2026 a trap on its charger at 2.0 V still slept 6 h and took no
 * update or request meanwhile. */
bool battery_protect(void)
{
	return battery_critical() && !battery_charging();
}

/* The warning before a protective sleep goes out once per spell (RTC memory, kept through the hourly sleeps) */
static RTC_DATA_ATTR bool protect_noticed;

bool battery_protect_notice_due(void)
{
	if (!battery_protect()) {
		protect_noticed = false;
		return false;
	}
	if (protect_noticed) {
		return false;
	}
	protect_noticed = true;
	return true;
}

void battery_json(cJSON *parent)
{
	if (last_mv < 0) {
		return;
	}
	cJSON *b = cJSON_AddObjectToObject(parent, "battery");
	int cell = battery_cell_mv(); /* without the charger's lift */
	cJSON_AddNumberToObject(b, "voltage", round(cell / 10.0) / 100.0); /* volts, 2 decimals (reference firmware) */
	cJSON_AddNumberToObject(b, "percent", battery_pct());
	cJSON_AddNumberToObject(b, "mv", cell); /* CONTRACTS section 3 */
	cJSON_AddNumberToObject(b, "pct", battery_pct());
	if (cell != last_mv) {
		cJSON_AddNumberToObject(b, "measuredMv", last_mv); /* the reading itself, charger included */
	}
	cJSON_AddBoolToObject(b, "charging", last_charging == 1);
	if (last_charging >= 0) {
		cJSON_AddStringToObject(b, "charge", !last_charging ? "none" : charging_slow() ? "slow" : "normal");
	}
	if (last_solar >= 0) {
		cJSON_AddNumberToObject(b, "solarMv", last_solar);
	}
}
