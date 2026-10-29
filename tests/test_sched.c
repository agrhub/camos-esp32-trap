/* SPDX-License-Identifier: AGPL-3.0-only */
/* Host tests of sched_policy.h: when the boot photo goes out, when the night sleep ends */
#include <stdio.h>
#include "../firmware/main/sched_policy.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	/* cold boot, paired, running: waits for MQTT (the clock synced first), then takes it */
	CHECK(boot_capture_decide(true, true, false, false, 6, 120) == BOOT_CAPTURE_WAIT);
	CHECK(boot_capture_decide(true, true, false, false, 119, 120) == BOOT_CAPTURE_WAIT);
	CHECK(boot_capture_decide(true, true, false, true, 9, 120) == BOOT_CAPTURE_NOW);
	CHECK(boot_capture_decide(true, true, false, true, 300, 120) == BOOT_CAPTURE_NOW); /* connected counts first */

	/* no MQTT after the give-up time: no boot photo */
	CHECK(boot_capture_decide(true, true, false, false, 120, 120) == BOOT_CAPTURE_SKIP);

	/* a deep-sleep timer wake, an unpaired camera, a paused camera: never */
	CHECK(boot_capture_decide(false, true, false, true, 9, 120) == BOOT_CAPTURE_SKIP);
	CHECK(boot_capture_decide(false, true, false, false, 6, 120) == BOOT_CAPTURE_SKIP);
	CHECK(boot_capture_decide(true, false, false, true, 9, 120) == BOOT_CAPTURE_SKIP);
	CHECK(boot_capture_decide(true, true, true, true, 9, 120) == BOOT_CAPTURE_SKIP);
	CHECK(boot_capture_decide(true, true, true, false, 6, 120) == BOOT_CAPTURE_SKIP);

	/* night sleep, default window 18:00-06:00 (crosses midnight) */
	const int F = 18 * 60, T = 6 * 60, H = 3600;
	CHECK(night_sleep_left_s(true, F, T, 18 * H) == 12 * H);
	CHECK(night_sleep_left_s(true, F, T, 23 * H + 30 * 60) == 6 * H + 30 * 60);
	CHECK(night_sleep_left_s(true, F, T, 0) == 6 * H);
	CHECK(night_sleep_left_s(true, F, T, 5 * H) == H);
	CHECK(night_sleep_left_s(true, F, T, 6 * H) == 0);              /* morning: awake */
	CHECK(night_sleep_left_s(true, F, T, 12 * H) == 0);
	CHECK(night_sleep_left_s(true, F, T, 18 * H - 1) == 0);
	CHECK(night_sleep_left_s(true, F, T, 6 * H - 30) == 0);         /* an RTC wake 30 s early does not sleep again */
	CHECK(night_sleep_left_s(true, F, T, 6 * H - 60) == 60);
	CHECK(night_sleep_left_s(false, F, T, 22 * H) == 0);            /* off */
	CHECK(night_sleep_left_s(true, 600, 600, 10 * H) == 0);         /* empty window */
	/* a window inside one day, 01:00-04:00 */
	CHECK(night_sleep_left_s(true, 60, 240, 2 * H) == 2 * H);
	CHECK(night_sleep_left_s(true, 60, 240, 4 * H) == 0);
	CHECK(night_sleep_left_s(true, 60, 240, 30 * 60) == 0);

	/* after the night: a late wake still takes the slot at the window's end (30 min slots: up to 900 s late) */
	const long long END = 1793228400LL; /* 2026-10-29 06:00 in Vietnam */
	CHECK(night_resume_after(END - 90, END, 900) == END - 90);   /* woke early (the lead): from now, 06:00 comes next */
	CHECK(night_resume_after(END + 5, END, 900) == END - 1);     /* booted after 06:00: 06:00 still counts */
	CHECK(night_resume_after(END + 900, END, 900) == END - 1);
	CHECK(night_resume_after(END + 901, END, 900) == END + 901); /* later: the next slot */
	CHECK(night_resume_after(END + 5, 0, 900) == END + 5);       /* no night sleep */

	/* adaptive interval: levels by the cell's percent */
	CHECK(adapt_level(80, 0) == 0);
	CHECK(adapt_level(59, 0) == 1);
	CHECK(adapt_level(39, 0) == 2);
	CHECK(adapt_level(3, 0) == 3);                               /* the Vinh Long trap, 29 Oct */
	CHECK(adapt_level(-1, 3) == 0);                              /* no battery monitor */
	/* worse at once, better only 5 points above the threshold */
	CHECK(adapt_level(61, 1) == 1);
	CHECK(adapt_level(65, 1) == 0);
	CHECK(adapt_level(22, 3) == 3);
	CHECK(adapt_level(25, 3) == 2);
	CHECK(adapt_level(70, 3) == 0);                              /* charged overnight: back in one go */
	CHECK(adapt_level(18, 0) == 3);
	/* the interval for a set 30 min */
	CHECK(adapt_interval_min(30, 0, CHARGE_NONE) == 30);
	CHECK(adapt_interval_min(30, 1, CHARGE_NONE) == 60);
	CHECK(adapt_interval_min(30, 3, CHARGE_NONE) == 120);
	CHECK(adapt_interval_min(30, 3, CHARGE_SLOW) == 90);         /* a weak panel: one level off */
	CHECK(adapt_interval_min(30, 3, CHARGE_FULL) == 30);         /* a real charge: none */
	CHECK(adapt_interval_min(120, 3, CHARGE_NONE) == 240);       /* capped at 4 h */
	CHECK(adapt_interval_min(360, 3, CHARGE_NONE) == 360);       /* a set interval over the cap stays */

	/* battery protection: hourly by day, through the night to 06:00 */
	CHECK(protect_sleep_s(10 * H) == H);
	CHECK(protect_sleep_s(6 * H) == H);
	CHECK(protect_sleep_s(18 * H) == 12 * H);                    /* 18:00 -> 06:00 */
	CHECK(protect_sleep_s(23 * H) == 7 * H);
	CHECK(protect_sleep_s(2 * H) == 4 * H);
	CHECK(protect_sleep_s(5 * H + 30 * 60) == H);                /* at least an hour */

	/* what a wake may spend */
	wake_budget_t b = wake_budget(0, CHARGE_NONE);
	CHECK(b.checkins && b.gps_fix && b.use_4g);
	b = wake_budget(2, CHARGE_NONE);                             /* 20-39 %: no check-ins, no GPS fix, 4G still */
	CHECK(!b.checkins && !b.gps_fix && b.use_4g);
	b = wake_budget(3, CHARGE_NONE);                             /* under 20 %: Wi-Fi only */
	CHECK(!b.checkins && !b.gps_fix && !b.use_4g);
	b = wake_budget(3, CHARGE_SLOW);                             /* a weak panel: one level off */
	CHECK(!b.checkins && !b.gps_fix && b.use_4g);
	b = wake_budget(3, CHARGE_FULL);                             /* charging properly: everything */
	CHECK(b.checkins && b.gps_fix && b.use_4g);

	printf("test_sched: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
