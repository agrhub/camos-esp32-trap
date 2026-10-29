/* SPDX-License-Identifier: AGPL-3.0-only */
/* The cell's own voltage while a charger lifts the reading, pure C (no ESP-IDF) so it is unit-tested on the host
 * (tests/test_battery.c). Used by battery.c. Header only: nothing to add to the component sources.
 *
 * A charger pushes current into the cell, and the divider then reads the cell plus its internal resistance and wiring
 * times that current: the Vinh Long trap read 3.40 V (3 %) and, plugged into USB minutes later, 3.95 V (71 %),
 * 29 Oct 2026. The lift is measured once, at the first reading of a charge (that reading minus the last one without
 * charge), and taken off the readings that follow. It fades as the reading nears the charger's full voltage: in the
 * constant-voltage phase the current, and with it the lift, falls to nothing. */
#pragma once

#define CHARGE_LIFT_MAX_MV 700 /* more than this between two readings is not the charger's lift */
#define CHARGE_FULL_MV 4200    /* single-cell LiPo / Li-ion charge voltage */

/* The lift at the start of a charge: the first reading with the charger minus the last one without it (`rest_mv`,
 * -1 unknown: then no correction). */
static inline int charge_lift_mv(int rest_mv, int first_charging_mv)
{
	if (rest_mv <= 0 || first_charging_mv <= rest_mv) {
		return 0;
	}
	int lift = first_charging_mv - rest_mv;
	return lift > CHARGE_LIFT_MAX_MV ? CHARGE_LIFT_MAX_MV : lift;
}

/* The cell's voltage for a reading `measured_mv` of a charge that started at `start_mv` with lift `lift_mv`. */
static inline int cell_mv_while_charging(int measured_mv, int start_mv, int lift_mv)
{
	if (lift_mv <= 0 || measured_mv >= CHARGE_FULL_MV || start_mv >= CHARGE_FULL_MV) {
		return measured_mv;
	}
	long long lift = (long long)lift_mv * (CHARGE_FULL_MV - measured_mv) / (CHARGE_FULL_MV - start_mv);
	if (lift > lift_mv) {
		lift = lift_mv; /* a reading under the start (a weaker sun): the whole lift at most */
	}
	return measured_mv - (int)lift;
}
