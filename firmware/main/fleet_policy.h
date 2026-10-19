/* SPDX-License-Identifier: AGPL-3.0-only */
/* Remote update and remote pause decisions, pure C (no ESP-IDF) so they are unit-tested on the host
 * (tests/test_fleet.c). Used by proto.c (ota.apply), ota.c (progress) and flybot.c (a paused camera). */
#pragma once
#include <stdbool.h>

/* May an ota.apply start now? Yes on mains power, while charging, without a battery monitor (mv < 0) or at
 * min_mv and above (min_mv 0 = no limit). A camera on a weak cell answers LOW_BATTERY and FlyBot waits. */
bool ota_battery_ok(int mv, bool charging, bool powered, int min_mv);

/* The next ota_progress milestone to report: a multiple of `step` percent (25 -> 25, 50, 75) that `done` of `total`
 * bytes has reached and `last` (the last reported, 0 at start) has not; -1 when there is none (unknown total, or
 * 100 %, which ota_result says). */
int ota_progress_next(long long done, long long total, int last, int step);

/* FlyBot answered a snapshot upload with 403 {"error":"disabled"}: an administrator paused this camera. */
bool upload_says_disabled(int status, const char *body);
