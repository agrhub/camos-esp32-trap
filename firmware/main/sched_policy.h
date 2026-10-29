/* SPDX-License-Identifier: AGPL-3.0-only */
/* Capture schedule decisions, pure C (no ESP-IDF) so they are unit-tested on the host (tests/test_sched.c).
 * Used by sched.c. Header only: nothing to add to the component sources. */
#pragma once
#include <stdbool.h>

typedef enum { BOOT_CAPTURE_WAIT = 0, BOOT_CAPTURE_NOW = 1, BOOT_CAPTURE_SKIP = 2 } boot_capture_t;

/* The "boot" photo after a power-on, reset or OTA restart. cold_boot: not a deep-sleep timer wake-up
 * (boot_reason_name() != NULL). The photo goes out once MQTT is up: SNTP usually syncs the clock seconds before MQTT
 * connects, so the first look comes too early. Waited for until uptime_s reaches giveup_s; never on a timer wake, on
 * an unpaired camera or on one an administrator paused. */
static inline boot_capture_t boot_capture_decide(bool cold_boot, bool paired, bool paused, bool connected,
												 long long uptime_s, int giveup_s)
{
	if (!cold_boot || !paired || paused) {
		return BOOT_CAPTURE_SKIP;
	}
	if (connected) {
		return BOOT_CAPTURE_NOW;
	}
	return uptime_s < giveup_s ? BOOT_CAPTURE_WAIT : BOOT_CAPTURE_SKIP;
}

#define NIGHT_MIN_SLEEP_S 60 /* less left than this: stay up (an RTC wake a few seconds early must not sleep again) */

/* Night sleep (power.nightSleep, power.nightWindow): seconds from `now_s` (local seconds after midnight) to the end of
 * the window [from_min, to_min) in local minutes after midnight (`to_min` before `from_min` crosses midnight); 0 when
 * the camera is outside it, night sleep is off, the window is empty (from == to) or less than NIGHT_MIN_SLEEP_S is left. */
static inline int night_sleep_left_s(bool enabled, int from_min, int to_min, int now_s)
{
	if (!enabled || from_min == to_min) {
		return 0;
	}
	int from = from_min * 60, to = to_min * 60, left;
	if (from < to) {
		if (now_s < from || now_s >= to) {
			return 0;
		}
		left = to - now_s;
	} else if (now_s >= from) {
		left = 24 * 3600 - now_s + to;
	} else if (now_s < to) {
		left = to - now_s;
	} else {
		return 0;
	}
	return left >= NIGHT_MIN_SLEEP_S ? left : 0;
}

/* Adaptive interval (power.adaptive, on by default): a cell running low with little or no charge coming in stretches the
 * capture interval so the trap lasts until the sun is back (the Vinh Long trap ran from 34 % to 3 % in a day on a
 * panel that could not charge it, 29 Oct 2026). Levels by the cell's percent: 0 at 60 % and above (the set interval),
 * 1 from 40 % (x2), 2 from 20 % (x3), 3 below (x4). A level is left upwards only ADAPT_HYST_PCT above its threshold,
 * so a reading that wobbles does not flip the plan at every wake. A slow charge (a small or shaded panel) takes one
 * level off, a full-current charge all of them. Never longer than ADAPT_MAX_MIN, unless the set interval already is. */
#define ADAPT_LEVELS 4
#define ADAPT_HYST_PCT 5
#define ADAPT_MAX_MIN 240

typedef enum { CHARGE_NONE = 0, CHARGE_SLOW = 1, CHARGE_FULL = 2 } charge_t;

/* The level for `pct` (-1 unknown: level 0) after `prev` (the level of the last wake) */
static inline int adapt_level(int pct, int prev)
{
	static const int threshold[ADAPT_LEVELS - 1] = { 60, 40, 20 };
	if (pct < 0) {
		return 0;
	}
	int level = prev < 0 ? 0 : prev >= ADAPT_LEVELS ? ADAPT_LEVELS - 1 : prev;
	while (level > 0 && pct >= threshold[level - 1] + ADAPT_HYST_PCT) {
		level--;
	}
	while (level < ADAPT_LEVELS - 1 && pct < threshold[level]) {
		level++;
	}
	return level;
}

/* The interval to capture at for the set interval `every_min`, a level and the charge coming in */
static inline int adapt_interval_min(int every_min, int level, charge_t charge)
{
	if (charge == CHARGE_FULL || every_min <= 0) {
		return every_min;
	}
	if (charge == CHARGE_SLOW && level > 0) {
		level--;
	}
	int stretched = every_min * (level + 1);
	int cap = every_min > ADAPT_MAX_MIN ? every_min : ADAPT_MAX_MIN;
	return stretched > cap ? cap : stretched;
}

/* What a wake may spend, decided from the battery read before any radio starts (power.adaptive, same levels): the
 * check-in wakes and the daily GPS fix (about 25 mA for up to minutes) stop below 40 %, the 4G fallback (about 1 mAh a
 * connection, five times Wi-Fi) below 20 %; a slow charge takes one level off, a full-current charge lifts every limit.
 * Without 4G a photo the Wi-Fi cannot send is not sent (with storage.sdSave it is on the memory card). An explicit
 * network.prefer "4g" is the owner's choice and is kept. */
typedef struct {
	bool checkins, gps_fix, use_4g;
} wake_budget_t;

static inline wake_budget_t wake_budget(int level, charge_t charge)
{
	if (charge == CHARGE_FULL) {
		level = 0;
	} else if (charge == CHARGE_SLOW && level > 0) {
		level--;
	}
	wake_budget_t b = { .checkins = level <= 1, .gps_fix = level <= 1, .use_4g = level <= 2 };
	return b;
}

/* Battery protection (a cell under CAMOS_BATT_CRITICAL_MV with no charge): the camera takes no photo and keeps the radio
 * off, and looks at the battery again after this many seconds: every hour by day, when a panel may start charging, and
 * not before PROTECT_DAY_FROM_H through the night, when nothing can (at least an hour). `now_s`: local seconds after
 * midnight. */
#define PROTECT_DAY_FROM_H 6
#define PROTECT_DAY_TO_H 18

static inline int protect_sleep_s(int now_s)
{
	const int from = PROTECT_DAY_FROM_H * 3600, to = PROTECT_DAY_TO_H * 3600;
	if (now_s >= from && now_s < to) {
		return 3600;
	}
	int left = now_s >= to ? 24 * 3600 - now_s + from : from - now_s;
	return left < 3600 ? 3600 : left;
}

/* After a night sleep that ended at `night_end` (epoch seconds, 0 = none): the time to look for the next slot after.
 * The slot at the window's end (06:00) is strictly after now only when the camera woke before it; a wake up to
 * `max_late_s` late (the RTC drifts, the boot takes seconds) looks from just before the end, so that slot is taken now
 * instead of being skipped for the next one (the first photo came at 06:30, 29 Oct 2026). Later than that: from now. */
static inline long long night_resume_after(long long now, long long night_end, int max_late_s)
{
	if (night_end && now >= night_end && now - night_end <= max_late_s) {
		return night_end - 1;
	}
	return now;
}
