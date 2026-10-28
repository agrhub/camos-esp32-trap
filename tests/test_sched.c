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

	printf("test_sched: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
