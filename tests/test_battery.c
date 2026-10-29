/* SPDX-License-Identifier: AGPL-3.0-only */
/* Host tests of battery_est.h: the cell's voltage under a charger's lift */
#include <stdio.h>
#include "../firmware/main/battery_est.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	/* the Vinh Long trap, 29 Oct 2026: 3400 mV without charge, 3946 mV on USB minutes later */
	CHECK(charge_lift_mv(3400, 3946) == 546);
	CHECK(cell_mv_while_charging(3946, 3946, 546) == 3400);          /* the first reading: the cell as it was */
	int later = cell_mv_while_charging(4050, 3946, 546);              /* the cell has gained: so has the estimate */
	CHECK(later > 3400 && later < 4050);
	CHECK(cell_mv_while_charging(4200, 3946, 546) == 4200);           /* full voltage: no lift left */
	CHECK(cell_mv_while_charging(4150, 3946, 546) == 4150 - 546 * 50 / 254);
	CHECK(cell_mv_while_charging(3900, 3946, 546) == 3900 - 546);     /* under the start (a weaker sun): the whole lift */

	/* nothing to correct */
	CHECK(charge_lift_mv(-1, 3946) == 0);                             /* no reading before the charge (power-on) */
	CHECK(charge_lift_mv(3800, 3790) == 0);
	CHECK(charge_lift_mv(3000, 4100) == CHARGE_LIFT_MAX_MV);          /* clamped */
	CHECK(cell_mv_while_charging(3946, 3946, 0) == 3946);

	printf("test_battery: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
