/* SPDX-License-Identifier: AGPL-3.0-only */
/* Host tests of sched_policy.h: when the boot photo goes out */
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

	printf("test_sched: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
