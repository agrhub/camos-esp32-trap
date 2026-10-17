/* SPDX-License-Identifier: AGPL-3.0-only */
/* power.mode decision, pure C (no ESP-IDF) so it is unit-tested on the host (tests/test_power.c). */
#pragma once

typedef enum { POWER_AUTO = 0, POWER_BATTERY = 1, POWER_POWERED = 2 } power_mode_t;

/* configured: the shadow / portal value. previous: the last effective mode (hysteresis state). monitor: the battery /
 * solar dividers are fitted (batteryMonitor). mv: cell voltage, -1 unknown. high_mv / low_mv: auto goes powered at or
 * above high, back to battery below low. Auto without a monitor is powered: that is the USB / mains Wi-Fi build (a
 * camera on a cell fits the monitor so auto can follow it); a fitted monitor that cannot be read sleeps to be safe. */
power_mode_t power_decide(power_mode_t configured, power_mode_t previous, int monitor, int mv, int high_mv, int low_mv);
