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
