/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * power.mode (CONTRACTS section 3): "powered" keeps MQTT connected with snapshot and live on demand, "battery" is
 * the scheduled capture + deep sleep cycle (sched.c), "auto" picks one from the cell voltage with hysteresis:
 * above highMv (or charging at full input and above lowMv) -> powered; below lowMv -> battery. Online on a cell that
 * is neither full nor charging at full input, the level is "eco" (power_eco: the live view keeps to 480p). The
 * decision survives deep sleep
 * in RTC memory so the hysteresis holds across wake-ups. Thresholds: shadow power.highMv / power.lowMv, else Kconfig.
 * The decision itself (power_decide) is a pure function, tested on the host (tests/test_power.c).
 */
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "camos.h"

static const char *TAG = "power";
#define EFF_MAGIC 0x50575230
static RTC_DATA_ATTR uint32_t eff_magic;
static RTC_DATA_ATTR int eff_prev; /* previous effective mode for the hysteresis */

const char *power_mode_name(power_mode_t m)
{
	return m == POWER_BATTERY ? "battery" : m == POWER_POWERED ? "powered" : "auto";
}

bool power_mode_parse(const char *s, power_mode_t *out)
{
	if (!s) {
		return false;
	}
	if (!strcmp(s, "auto")) {
		*out = POWER_AUTO;
	} else if (!strcmp(s, "battery")) {
		*out = POWER_BATTERY;
	} else if (!strcmp(s, "powered")) {
		*out = POWER_POWERED;
	} else {
		return false;
	}
	return true;
}

static void thresholds(int *high, int *low)
{
	settings_t s;
	shadow_get(&s);
	*high = s.power_high_mv > 0 ? s.power_high_mv : CONFIG_CAMOS_POWER_HIGH_MV;
	*low = s.power_low_mv > 0 ? s.power_low_mv : CONFIG_CAMOS_POWER_LOW_MV;
	if (*low >= *high) {
		*low = *high - 100;
	}
}

power_mode_t power_effective(void)
{
	int high, low;
	thresholds(&high, &low);
	if (eff_magic != EFF_MAGIC) {
		eff_magic = EFF_MAGIC;
		eff_prev = POWER_BATTERY;
	}
	int mv = battery_mv();
	/* a charger that charges at full input counts as a full cell as long as the cell is above low; a slow charge (a
	 * small panel sagging to the cell) may not keep up with an always-on camera, so the cell voltage decides then */
	if (battery_charging_fast() && mv >= low) {
		mv = high;
	}
	power_mode_t m = power_decide(shadow_power_mode(), (power_mode_t)eff_prev, g_cfg.batt_mon, mv, high, low);
	if (m != eff_prev) {
		ESP_LOGI(TAG, "effective mode %s -> %s (%d mV, high %d, low %d)", power_mode_name(eff_prev), power_mode_name(m),
				 battery_mv(), high, low);
	}
	eff_prev = m;
	return m;
}

bool power_may_deep_sleep(void)
{
	return CAMOS_DEEP_SLEEP && power_effective() == POWER_BATTERY;
}

/* Online on a cell that is not full and not charging at full input (a small panel, cloud, a cell between low and
 * high): the live view keeps to the 480p tier, so the camera stays up longer before the battery mode */
bool power_eco(void)
{
	if (!g_cfg.batt_mon || battery_mv() < 0 || shadow_power_mode() == POWER_POWERED) {
		return false; /* no cell measured, or the owner asked for full power */
	}
	int high, low;
	thresholds(&high, &low);
	return !battery_charging_fast() && battery_mv() < high;
}

const char *power_level_name(void)
{
	return power_effective() == POWER_BATTERY ? "saver" : power_eco() ? "eco" : "full";
}

void power_json(cJSON *parent)
{
	int high, low;
	thresholds(&high, &low);
	cJSON *p = cJSON_AddObjectToObject(parent, "power");
	cJSON_AddStringToObject(p, "mode", power_mode_name(shadow_power_mode()));
	cJSON_AddStringToObject(p, "effective", power_mode_name(power_effective()));
	cJSON_AddStringToObject(p, "level", power_level_name());
	cJSON_AddNumberToObject(p, "highMv", high);
	cJSON_AddNumberToObject(p, "lowMv", low);
	cJSON_AddBoolToObject(p, "nightSleep", g_cfg.night_sleep != 0);
	cJSON_AddBoolToObject(p, "adaptive", g_cfg.adaptive != 0);
	cJSON *night = cJSON_AddObjectToObject(p, "nightWindow");
	char hm[16];
	snprintf(hm, sizeof(hm), "%02d:%02d", g_cfg.night_from / 60, g_cfg.night_from % 60);
	cJSON_AddStringToObject(night, "from", hm);
	snprintf(hm, sizeof(hm), "%02d:%02d", g_cfg.night_to / 60, g_cfg.night_to % 60);
	cJSON_AddStringToObject(night, "to", hm);
}
