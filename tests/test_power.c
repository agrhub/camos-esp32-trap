/* Host unit test for firmware/main/power_decide.c (power.mode auto hysteresis). */
#include <stdio.h>
#include "../firmware/main/power_decide.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void)
{
	const int high = 4050, low = 3850;
	/* fixed modes ignore the battery */
	CHECK(power_decide(POWER_BATTERY, POWER_POWERED, 4200, high, low) == POWER_BATTERY);
	CHECK(power_decide(POWER_POWERED, POWER_BATTERY, 3000, high, low) == POWER_POWERED);
	/* auto without a monitor sleeps */
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, -1, high, low) == POWER_BATTERY);
	/* auto from battery: only a full cell goes powered */
	CHECK(power_decide(POWER_AUTO, POWER_BATTERY, 4049, high, low) == POWER_BATTERY);
	CHECK(power_decide(POWER_AUTO, POWER_BATTERY, 4050, high, low) == POWER_POWERED);
	CHECK(power_decide(POWER_AUTO, POWER_BATTERY, 3900, high, low) == POWER_BATTERY);
	/* auto from powered: stays until below low (hysteresis band) */
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, 3900, high, low) == POWER_POWERED);
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, 3850, high, low) == POWER_POWERED);
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, 3849, high, low) == POWER_BATTERY);
	/* a walk through a solar day: morning low, charging up, evening down */
	power_mode_t m = POWER_BATTERY;
	int day[] = { 3700, 3800, 3900, 4000, 4060, 4100, 4000, 3900, 3860, 3840, 3700 };
	power_mode_t expect[] = { POWER_BATTERY, POWER_BATTERY, POWER_BATTERY, POWER_BATTERY, POWER_POWERED, POWER_POWERED,
							  POWER_POWERED, POWER_POWERED, POWER_POWERED, POWER_BATTERY, POWER_BATTERY };
	for (unsigned i = 0; i < sizeof(day) / sizeof(day[0]); i++) {
		m = power_decide(POWER_AUTO, m, day[i], high, low);
		CHECK(m == expect[i]);
	}
	/* inverted thresholds are repaired (low = high - 100) */
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, 3960, 4000, 4100) == POWER_POWERED);
	CHECK(power_decide(POWER_AUTO, POWER_POWERED, 3890, 4000, 4100) == POWER_BATTERY);
	printf("%s: %d failure(s)\n", __FILE__, fails);
	return fails ? 1 : 0;
}
