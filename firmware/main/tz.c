/* SPDX-License-Identifier: AGPL-3.0-only */
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

/* ------------------------------------------------------------------ effective timezone (CONTRACTS 2a.6) */

#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "camos_defaults.h"

static const char *TZTAG = "tz";
static char auto_iana[48], auto_posix[64], auto_src[16];
static bool auto_loaded;
static char eff_iana[48], eff_src[16], eff_posix[64];
static const char *clk_src = "none";

const char *clock_source(void) { return clk_src; }

void clock_set_source(const char *src)
{
	if (!strcmp(clk_src, src)) {
		return; /* a resync from the same source */
	}
	clk_src = src;
	time_t now = time(NULL);
	char ts[25];
	iso8601(now, ts);
	ESP_LOGI(TZTAG, "clock set from %s: %s", src, ts);
}

static void load_auto(void)
{
	if (auto_loaded) {
		return;
	}
	auto_loaded = true;
	nvs_tz_read(auto_iana, sizeof(auto_iana), auto_posix, sizeof(auto_posix), auto_src, sizeof(auto_src));
}

void tz_auto_set(const char *iana, const char *posix, const char *source)
{
	load_auto();
	const char *p = posix && posix[0] ? posix : (iana ? tz_lookup(iana) : NULL);
	if (!p || strlen(p) >= sizeof(auto_posix)) {
		ESP_LOGW(TZTAG, "device/time gave no usable timezone (%s / %s)", iana ? iana : "-", posix ? posix : "-");
		return;
	}
	bool changed = strcmp(auto_posix, p) || strcmp(auto_iana, iana ? iana : "");
	strlcpy(auto_iana, iana ? iana : "", sizeof(auto_iana));
	strlcpy(auto_posix, p, sizeof(auto_posix));
	strlcpy(auto_src, source && source[0] ? source : "flybot", sizeof(auto_src));
	if (changed) {
		nvs_tz_write(auto_iana, auto_posix, auto_src);
	}
	tz_apply();
}

void tz_apply(void)
{
	load_auto();
	char siana[48] = "", sposix[64] = "";
	const char *posix = NULL;
	if (store_overridden("timezone") && (posix = tz_lookup(g_cfg.tz)) != NULL) {
		strlcpy(eff_iana, g_cfg.tz, sizeof(eff_iana));
		strlcpy(eff_src, "manual", sizeof(eff_src));
	} else if (shadow_tz(siana, sizeof(siana), sposix, sizeof(sposix))) {
		posix = sposix;
		strlcpy(eff_iana, siana, sizeof(eff_iana));
		strlcpy(eff_src, "shadow", sizeof(eff_src));
	} else if (auto_posix[0]) {
		posix = auto_posix;
		strlcpy(eff_iana, auto_iana[0] ? auto_iana : auto_posix, sizeof(eff_iana));
		strlcpy(eff_src, auto_src, sizeof(eff_src));
	} else {
		posix = tz_lookup(CAMOS_DEFAULT_TIMEZONE);
		strlcpy(eff_iana, CAMOS_DEFAULT_TIMEZONE, sizeof(eff_iana));
		strlcpy(eff_src, "build", sizeof(eff_src));
	}
	if (!posix) {
		posix = "UTC0";
	}
	if (strcmp(eff_posix, posix)) {
		strlcpy(eff_posix, posix, sizeof(eff_posix));
		setenv("TZ", posix, 1);
		tzset();
		ESP_LOGI(TZTAG, "timezone %s (%s) from %s", eff_iana, posix, eff_src);
		sched_changed();
	}
}

const char *tz_effective_iana(void) { return eff_iana[0] ? eff_iana : CAMOS_DEFAULT_TIMEZONE; }
const char *tz_effective_source(void) { return eff_src[0] ? eff_src : "build"; }
