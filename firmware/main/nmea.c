/* NMEA 0183 RMC / GGA parser (see nmea.h). Tolerates empty fields (no fix yet) and the ATGM336H's GN/GP/BD talkers. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "nmea.h"

void nmea_init(nmea_fix_t *fix)
{
	memset(fix, 0, sizeof(*fix));
	fix->sats = -1;
	fix->hdop = -1;
	fix->fix_quality = -1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

bool nmea_checksum_ok(const char *line)
{
	if (!line || line[0] != '$') {
		return false;
	}
	const char *star = strchr(line, '*');
	if (!star || hexval(star[1]) < 0 || hexval(star[2]) < 0) {
		return false;
	}
	unsigned char sum = 0;
	for (const char *p = line + 1; p < star; p++) {
		sum ^= (unsigned char)*p;
	}
	return sum == (unsigned char)(hexval(star[1]) * 16 + hexval(star[2]));
}

bool nmea_coord(const char *field, char hemi, double *out)
{
	/* ddmm.mmmm (latitude) or dddmm.mmmm (longitude): the degrees are everything before the last two integer digits */
	const char *dot = strchr(field, '.');
	size_t ilen = dot ? (size_t)(dot - field) : strlen(field);
	if (ilen < 3 || ilen > 5) {
		return false;
	}
	for (size_t i = 0; i < ilen; i++) {
		if (!isdigit((unsigned char)field[i])) {
			return false;
		}
	}
	char deg[4] = { 0 };
	memcpy(deg, field, ilen - 2);
	double minutes = atof(field + ilen - 2);
	if (minutes >= 60.0) {
		return false;
	}
	double v = atoi(deg) + minutes / 60.0;
	if (hemi == 'S' || hemi == 'W') {
		v = -v;
	} else if (hemi != 'N' && hemi != 'E') {
		return false;
	}
	*out = v;
	return true;
}

/* days since 1970-01-01 for a civil date (Howard Hinnant's algorithm) */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

int64_t nmea_epoch(int year, int mon, int day, int hour, int min, int sec)
{
	if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60) {
		return 0;
	}
	return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

/* Splits the sentence body (between '$' and '*') into fields; returns the count. Empty fields stay "". */
static int split(char *body, char *fields[], int max)
{
	int n = 0;
	char *p = body;
	fields[n++] = p;
	while (*p && n < max) {
		if (*p == ',') {
			*p = 0;
			fields[n++] = p + 1;
		}
		p++;
	}
	return n;
}

int nmea_parse_line(const char *line, nmea_fix_t *fix)
{
	if (!nmea_checksum_ok(line)) {
		return -1;
	}
	char body[128];
	const char *star = strchr(line, '*');
	size_t len = (size_t)(star - line - 1);
	if (len >= sizeof(body) || len < 5) {
		return -1;
	}
	memcpy(body, line + 1, len);
	body[len] = 0;
	char *f[20];
	int n = split(body, f, 20);
	const char *type = f[0] + 2; /* skip the talker: GP, GN, BD, GL */
	if (strlen(f[0]) != 5) {
		return 0;
	}
	if (!strcmp(type, "RMC")) {
		if (n < 10) {
			return -1;
		}
		if (f[2][0] != 'A') {
			fix->valid = false;
			return 1;
		}
		double lat, lng;
		if (!nmea_coord(f[3], f[4][0], &lat) || !nmea_coord(f[5], f[6][0], &lng)) {
			return -1;
		}
		fix->lat = lat;
		fix->lng = lng;
		fix->speed_kn = f[7][0] ? atof(f[7]) : 0;
		fix->valid = true;
		if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
			int hh = (f[1][0] - '0') * 10 + (f[1][1] - '0');
			int mm = (f[1][2] - '0') * 10 + (f[1][3] - '0');
			int ss = (f[1][4] - '0') * 10 + (f[1][5] - '0');
			int dd = (f[9][0] - '0') * 10 + (f[9][1] - '0');
			int mo = (f[9][2] - '0') * 10 + (f[9][3] - '0');
			int yy = (f[9][4] - '0') * 10 + (f[9][5] - '0');
			fix->epoch = nmea_epoch(2000 + yy, mo, dd, hh, mm, ss);
		}
		return 1;
	}
	if (!strcmp(type, "GGA")) {
		if (n < 10) {
			return -1;
		}
		fix->fix_quality = f[6][0] ? atoi(f[6]) : 0;
		fix->sats = f[7][0] ? atoi(f[7]) : 0;
		fix->hdop = f[8][0] ? atof(f[8]) : -1;
		fix->alt_m = f[9][0] ? atof(f[9]) : 0;
		return 2;
	}
	return 0;
}

int64_t cclk_to_epoch(const char *answer)
{
	const char *p = answer ? strchr(answer, '"') : NULL;
	if (!p) {
		return 0;
	}
	int yy, mo, dd, hh, mi, ss, q = 0;
	char sign = '+';
	int n = sscanf(p + 1, "%2d/%2d/%2d,%2d:%2d:%2d%c%d", &yy, &mo, &dd, &hh, &mi, &ss, &sign, &q);
	if (n < 6) {
		return 0;
	}
	if (n < 8 || (sign != '+' && sign != '-')) {
		q = 0;
	}
	/* two-digit year: a module without network time says 80/01/06 (1980) or 04/01/01 */
	int64_t local = nmea_epoch(yy < 70 ? 2000 + yy : 1900 + yy, mo, dd, hh, mi, ss);
	if (local == 0) {
		return 0;
	}
	int64_t utc = local - (sign == '-' ? -1 : 1) * (int64_t)q * 15 * 60;
	return utc >= 1735689600 ? utc : 0;
}
