/* Shadow `timezone` (protocol 8) is an IANA name; the C library needs a POSIX TZ string.
 * A small table of zones likely for traps; a string that already is POSIX (no '/', has a digit) passes through. */
#include <string.h>
#include <ctype.h>
#include "camos.h"

static const struct { const char *name, *posix; } zones[] = {
	{ "UTC", "UTC0" },
	{ "Etc/UTC", "UTC0" },
	{ "Asia/Ho_Chi_Minh", "<+07>-7" },
	{ "Asia/Saigon", "<+07>-7" },
	{ "Asia/Bangkok", "<+07>-7" },
	{ "Asia/Jakarta", "WIB-7" },
	{ "Asia/Phnom_Penh", "<+07>-7" },
	{ "Asia/Vientiane", "<+07>-7" },
	{ "Asia/Yangon", "<+0630>-6:30" },
	{ "Asia/Singapore", "<+08>-8" },
	{ "Asia/Kuala_Lumpur", "<+08>-8" },
	{ "Asia/Manila", "PST-8" },
	{ "Asia/Shanghai", "CST-8" },
	{ "Asia/Hong_Kong", "HKT-8" },
	{ "Asia/Taipei", "CST-8" },
	{ "Asia/Tokyo", "JST-9" },
	{ "Asia/Seoul", "KST-9" },
	{ "Asia/Kolkata", "IST-5:30" },
	{ "Asia/Dhaka", "<+06>-6" },
	{ "Asia/Dubai", "<+04>-4" },
	{ "Australia/Perth", "AWST-8" },
	{ "Australia/Brisbane", "AEST-10" },
	{ "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
	{ "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
	{ "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
	{ "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
	{ "America/Sao_Paulo", "<-03>3" },
	{ "Africa/Nairobi", "EAT-3" },
	{ "Africa/Johannesburg", "SAST-2" },
};

const char *tz_lookup(const char *name)
{
	for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
		if (!strcmp(zones[i].name, name)) {
			return zones[i].posix;
		}
	}
	/* POSIX string given directly, e.g. "ICT-7" */
	if (!strchr(name, '/') && strlen(name) >= 4) {
		for (const char *p = name; *p; p++) {
			if (isdigit((unsigned char)*p)) {
				return name;
			}
		}
	}
	return NULL;
}
