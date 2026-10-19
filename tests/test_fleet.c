/* SPDX-License-Identifier: AGPL-3.0-only */
/* Host tests of fleet_policy.c: the OTA battery guard, the progress milestones, a paused camera's upload answer */
#include <stdio.h>
#include "../firmware/main/fleet_policy.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	/* battery guard */
	CHECK(ota_battery_ok(3600, false, true, 3700));   /* mains power */
	CHECK(ota_battery_ok(3600, true, false, 3700));   /* charging */
	CHECK(ota_battery_ok(-1, false, false, 3700));    /* no monitor fitted */
	CHECK(ota_battery_ok(3700, false, false, 3700));
	CHECK(!ota_battery_ok(3699, false, false, 3700));
	CHECK(ota_battery_ok(3000, false, false, 0));     /* limit off */

	/* progress: 25 % steps, each once, never 0 or 100 */
	CHECK(ota_progress_next(0, 1000, 0, 25) == -1);
	CHECK(ota_progress_next(249, 1000, 0, 25) == -1);
	CHECK(ota_progress_next(250, 1000, 0, 25) == 25);
	CHECK(ota_progress_next(300, 1000, 25, 25) == -1);
	CHECK(ota_progress_next(760, 1000, 25, 25) == 75); /* a jump reports the latest milestone */
	CHECK(ota_progress_next(1000, 1000, 75, 25) == -1);
	CHECK(ota_progress_next(500, -1, 0, 25) == -1);    /* chunked: total unknown */
	CHECK(ota_progress_next(3145728LL, 3145728LL * 2, 25, 25) == 50);

	/* a paused camera */
	CHECK(upload_says_disabled(403, "{\"ok\":false,\"error\":\"disabled\",\"nextWakeAt\":\"2026-10-20T00:00:00Z\"}"));
	CHECK(upload_says_disabled(403, "{\"ok\": false, \"error\" : \"disabled\"}"));
	CHECK(!upload_says_disabled(401, "{\"error\":\"disabled\"}"));
	CHECK(!upload_says_disabled(403, "{\"error\":\"daily_limit\"}"));
	CHECK(!upload_says_disabled(403, NULL));

	printf("test_fleet: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
