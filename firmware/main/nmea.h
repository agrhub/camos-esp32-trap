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
/* SIMCom AT+CCLK? answer `+CCLK: "26/10/16,15:00:00+28"` (local time, zone in quarter hours, NITZ) -> UTC epoch;
 * 0 when malformed or before 2025 (a modem without network time says 1980/2004 ...) */
int64_t cclk_to_epoch(const char *answer);
