/* SPDX-License-Identifier: AGPL-3.0-only */
/* NMEA 0183 parser for the ATGM336H (GPS + BeiDou: $GNRMC / $GNGGA, also $GPxxx / $BDxxx).
 * Plain C, no ESP-IDF dependency, so it is unit-tested on the host (tests/test_nmea.c). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool valid;       /* RMC status 'A' seen */
	double lat, lng;  /* decimal degrees, south / west negative */
	double speed_kn;
	int64_t epoch;    /* UTC seconds from the RMC date + time, 0 when the date is unknown */
	int sats;         /* GGA satellites in use, -1 unknown */
	double hdop;      /* GGA, -1 unknown */
	double alt_m;     /* GGA, 0 unknown */
	int fix_quality;  /* GGA, -1 unknown */
} nmea_fix_t;

void nmea_init(nmea_fix_t *fix);
bool nmea_checksum_ok(const char *line); /* "$....*hh" */
/* Parses one line ("$GNRMC,...*hh", with or without CR/LF). Returns 1 for RMC, 2 for GGA, 0 for other sentences,
 * -1 for a bad checksum or a malformed line. On RMC with status 'A' the position, speed and epoch are updated and
 * valid is set; a 'V' RMC clears valid. GGA updates sats, hdop, alt, fix_quality. */
int nmea_parse_line(const char *line, nmea_fix_t *fix);
/* "ddmm.mmmm" + hemisphere -> decimal degrees; returns false when malformed */
bool nmea_coord(const char *field, char hemi, double *out);
int64_t nmea_epoch(int year, int mon, int day, int hour, int min, int sec); /* UTC */
/* Satellite systems of the ATGM336H, the bits of its $PCAS04 command */
#define GNSS_GPS 1
#define GNSS_BDS 2
#define GNSS_GLONASS 4
/* setting gpsSystems -> mask: "gps" | "bds" | "gps+bds" | "gps+glonass"; 0 for "auto" (by region) or unknown */
int gnss_mask(const char *name);
const char *gnss_name(int mask); /* the reverse, "gps+bds" ...; "?" for another mask */
/* "auto": GPS + BeiDou (the most satellites over Asia - Pacific, and global since BeiDou-3); GPS + GLONASS at 55
 * degrees north or south and beyond (its orbits are inclined 65 degrees, GPS's and BeiDou's 55); GPS + BeiDou without
 * a position */
int gnss_auto(bool have_pos, double lat);
/* $xxGSV talker -> its system bit (GP, BD or GB, GL); 0 for other sentences and talkers */
int gnss_talker(const char *line);
/* "$<body>*hh\r\n" (a command to the module, "PCAS06,2"); returns its length, 0 for a short buffer */
int nmea_command(const char *body, char *buf, int n);
/* "$PCAS04,3*1A\r\n" for mask 3; returns its length, 0 for a bad mask or a short buffer */
int gnss_pcas04(int mask, char *buf, int n);
/* SIMCom AT+CCLK? answer `+CCLK: "26/10/16,15:00:00+28"` (local time, zone in quarter hours, NITZ) -> UTC epoch;
 * 0 when malformed or before 2025 (a modem without network time says 1980/2004 ...) */
int64_t cclk_to_epoch(const char *answer);
