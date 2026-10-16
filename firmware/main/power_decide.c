/* SPDX-License-Identifier: AGPL-3.0-only */
#include "power_decide.h"

power_mode_t power_decide(power_mode_t configured, power_mode_t previous, int mv, int high_mv, int low_mv)
{
	if (configured != POWER_AUTO) {
		return configured;
	}
	if (mv < 0) {
		return POWER_BATTERY; /* no battery monitor: the safe choice is to sleep */
	}
	if (low_mv >= high_mv) {
		low_mv = high_mv - 100;
	}
	if (previous == POWER_POWERED) {
		return mv < low_mv ? POWER_BATTERY : POWER_POWERED; /* stay powered until the cell falls below low */
	}
	return mv >= high_mv ? POWER_POWERED : POWER_BATTERY;     /* from battery: only a full cell goes powered */
}
