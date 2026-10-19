/* SPDX-License-Identifier: AGPL-3.0-only */
#include <string.h>
#include "fleet_policy.h"

bool ota_battery_ok(int mv, bool charging, bool powered, int min_mv)
{
	if (powered || charging || mv < 0 || min_mv <= 0) {
		return true;
	}
	return mv >= min_mv;
}

int ota_progress_next(long long done, long long total, int last, int step)
{
	if (total <= 0 || done <= 0 || step <= 0 || step >= 100) {
		return -1;
	}
	long long pct = done * 100 / total;
	if (pct >= 100) {
		return -1;
	}
	int milestone = (int)(pct / step) * step;
	return milestone > last && milestone > 0 ? milestone : -1;
}

bool upload_says_disabled(int status, const char *body)
{
	if (status != 403 || !body) {
		return false;
	}
	/* {"ok":false,"error":"disabled",...}: the key and value with any spacing FlyBot's JSON may have */
	const char *k = strstr(body, "\"error\"");
	if (!k) {
		return false;
	}
	k += 7;
	while (*k == ' ' || *k == ':') {
		k++;
	}
	return !strncmp(k, "\"disabled\"", 10);
}
