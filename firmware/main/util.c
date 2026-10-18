/* SPDX-License-Identifier: AGPL-3.0-only */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "camos.h"

/* Time: SNTP (or GET /api/device/time) sets the system clock (net.c). Before that the clock starts at 1970. */
bool clock_synced(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec > 1735689600; /* after 2025-01-01 */
}

uint64_t now_ms(void)
{
	if (clock_synced()) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
	}
	return (uint64_t)(esp_timer_get_time() / 1000);
}

const char *fw_version(void)
{
	return esp_app_get_description()->version;
}

/* ULID (protocol 3): 48-bit ms timestamp + 80 random bits, Crockford base32. Uptime stands in for the
 * timestamp before the first SNTP answer; the random part keeps the id unique either way. */
void new_ulid(char out[27])
{
	static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	uint64_t t = now_ms();
	for (int i = 9; i >= 0; i--) {
		out[i] = alphabet[t & 31];
		t >>= 5;
	}
	for (int i = 10; i < 26; i++) {
		out[i] = alphabet[esp_random() & 31];
	}
	out[26] = 0;
}

/* Reboot reason for the `boot` event (protocol 5): "rpc" and "watchdog" requests survive the restart in RTC memory */
#define REASON_MAGIC 0x43414d53
static RTC_NOINIT_ATTR uint32_t rtc_magic;
static RTC_NOINIT_ATTR char rtc_reason[12];

void remember_reboot_reason(const char *reason)
{
	rtc_magic = REASON_MAGIC;
	strlcpy(rtc_reason, reason, sizeof(rtc_reason));
}

const char *boot_reason_name(void)
{
	static char reason[12];
	static const char *cached;
	static bool decided;
	if (decided) {
		return cached; /* idempotent: the RTC flag is consumed once */
	}
	decided = true;
	bool flagged = rtc_magic == REASON_MAGIC;
	if (flagged) {
		strlcpy(reason, rtc_reason, sizeof(reason));
		rtc_magic = 0;
	}
	switch (esp_reset_reason()) {
	case ESP_RST_DEEPSLEEP:
		cached = NULL; /* a scheduled wake-up is not a boot */
		break;
	case ESP_RST_PANIC:
		cached = "crash";
		break;
	case ESP_RST_INT_WDT:
	case ESP_RST_TASK_WDT:
	case ESP_RST_WDT:
		cached = "watchdog";
		break;
	case ESP_RST_SW:
		cached = flagged ? reason : "power_on";
		break;
	default:
		cached = "power_on";
	}
	return cached;
}

/* The last reset (not a deep sleep wake): kept in RTC memory through deep sleep, so a sleeping camera still tells it */
static RTC_DATA_ATTR char rst_reason[16];
static RTC_DATA_ATTR int64_t rst_epoch; /* when it happened (epoch s), once the clock was set */

static const char *reset_name(esp_reset_reason_t r)
{
	switch (r) {
	case ESP_RST_POWERON:
		return "power_on";
	case ESP_RST_EXT:
		return "external";
	case ESP_RST_SW:
		return "software";
	case ESP_RST_PANIC:
		return "crash";
	case ESP_RST_INT_WDT:
		return "int_watchdog";
	case ESP_RST_TASK_WDT:
		return "task_watchdog";
	case ESP_RST_WDT:
		return "watchdog";
	case ESP_RST_BROWNOUT:
		return "brownout";
	case ESP_RST_USB:
		return "usb";
	case ESP_RST_JTAG:
		return "jtag";
	default:
		return "unknown";
	}
}

/* lastReset {reason, detail, at, uptimeS}: reason from the chip (power_on, software, crash, task_watchdog, brownout,
 * usb ...), detail the firmware's own reason for a software restart (ota, rpc, watchdog = no MQTT, reregister) */
void last_reset_json(cJSON *parent)
{
	esp_reset_reason_t r = esp_reset_reason();
	if (r != ESP_RST_DEEPSLEEP && !rst_reason[0]) {
		strlcpy(rst_reason, reset_name(r), sizeof(rst_reason));
		rst_epoch = 0;
	}
	int64_t up_s = esp_timer_get_time() / 1000000;
	if (!rst_epoch && clock_synced() && r != ESP_RST_DEEPSLEEP) {
		rst_epoch = (int64_t)time(NULL) - up_s;
	}
	cJSON *o = cJSON_AddObjectToObject(parent, "lastReset");
	cJSON_AddStringToObject(o, "reason", rst_reason[0] ? rst_reason : "unknown");
	const char *detail = boot_reason_name();
	if (r == ESP_RST_SW && detail && strcmp(detail, "power_on")) {
		cJSON_AddStringToObject(o, "detail", detail);
	}
	if (rst_epoch) {
		char ts[25];
		iso8601((time_t)rst_epoch, ts);
		cJSON_AddStringToObject(o, "at", ts);
		cJSON_AddNumberToObject(o, "uptimeS", (double)((int64_t)time(NULL) - rst_epoch));
	} else {
		cJSON_AddNullToObject(o, "at");
		cJSON_AddNumberToObject(o, "uptimeS", (double)up_s);
	}
}

uint32_t reboot_count(void)
{
	static uint32_t boots;
	if (!boots) {
		boots = (uint32_t)store_get_flag("boots") + 1;
		store_set_flag("boots", (int)boots);
	}
	return boots;
}

/* Width and height from the first start-of-frame marker (protocol 9 `capture`: w/h are read from the JPEG) */
int jpeg_dims(const uint8_t *jpg, size_t len, int *w, int *h)
{
	size_t i = 2;
	while (i + 9 < len) {
		if (jpg[i] != 0xFF) {
			i++;
			continue;
		}
		uint8_t m = jpg[i + 1];
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
			*h = (jpg[i + 5] << 8) | jpg[i + 6];
			*w = (jpg[i + 7] << 8) | jpg[i + 8];
			return 0;
		}
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) {
			i += (m == 0xFF) ? 1 : 2;
			continue;
		}
		i += 2 + ((jpg[i + 2] << 8) | jpg[i + 3]);
	}
	return -1;
}

/* ------------------------------------------------------------------ ISO 8601 (CONTRACTS section 2: capturedAt, nextWakeAt, now) */

void iso8601(time_t t, char out[25])
{
	struct tm tm;
	gmtime_r(&t, &tm);
	strftime(out, 25, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm), no timegm needed */
static long days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

time_t parse_iso8601(const char *s)
{
	int y, mo, d, h, mi, sec = 0, n = 0;
	if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) < 5) {
		return 0;
	}
	if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
		return 0;
	}
	const char *p = s + n;
	if (*p == '.') { /* fractional seconds */
		p++;
		while (*p >= '0' && *p <= '9') {
			p++;
		}
	}
	long off = 0;
	if (*p == '+' || *p == '-') {
		int oh = 0, om = 0;
		sscanf(p + 1, "%2d:%2d", &oh, &om);
		off = (oh * 60L + om) * 60L * (*p == '+' ? 1 : -1);
	}
	return (time_t)(days_from_civil(y, mo, d) * 86400L + h * 3600L + mi * 60L + sec - off);
}
