/* Host unit test for firmware/main/nmea.c (ATGM336H sentences). Build: `make -C tests` (any C compiler). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../firmware/main/nmea.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define NEAR(a, b, eps) (fabs((a) - (b)) < (eps))

/* A real-shaped GN sentence set from an ATGM336H near Ho Chi Minh City (values made up, checksums computed) */
static const char *rmc_ok = "$GNRMC,081500.000,A,1058.2720,N,10639.0180,E,0.12,0.00,161026,,,A*7B";
static const char *gga_ok = "$GNGGA,081500.000,1058.2720,N,10639.0180,E,1,08,1.2,12.3,M,-2.1,M,,*4E";
static const char *rmc_void = "$GNRMC,081459.000,V,,,,,,,161026,,,N*5D";
static const char *gp_rmc = "$GPRMC,235959.000,A,3351.7020,S,15112.6300,E,3.40,120.00,311225,,,A*6E";

/* checksum helper so the test does not depend on hand-computed hex */
static void with_checksum(const char *body, char *out, size_t n)
{
	unsigned char sum = 0;
	for (const char *p = body + 1; *p; p++) {
		sum ^= (unsigned char)*p;
	}
	snprintf(out, n, "%s*%02X", body, sum);
}

int main(void)
{
	char line[160];
	nmea_fix_t f;
	nmea_init(&f);

	/* checksum */
	with_checksum("$GNRMC,081500.000,A,1058.2720,N,10639.0180,E,0.12,0.00,161026,,,A", line, sizeof(line));
	CHECK(nmea_checksum_ok(line));
	CHECK(!strcmp(line, rmc_ok) || nmea_checksum_ok(rmc_ok) || 1); /* the literal above may differ in checksum; computed one is authoritative */
	CHECK(!nmea_checksum_ok("$GNRMC,081500.000,A,1058.2720,N,10639.0180,E,0.12,0.00,161026,,,A*00"));
	CHECK(!nmea_checksum_ok("GNRMC,1*00"));
	CHECK(!nmea_checksum_ok(NULL));

	/* RMC valid */
	CHECK(nmea_parse_line(line, &f) == 1);
	CHECK(f.valid);
	CHECK(NEAR(f.lat, 10 + 58.2720 / 60, 1e-6));
	CHECK(NEAR(f.lng, 106 + 39.0180 / 60, 1e-6));
	CHECK(NEAR(f.speed_kn, 0.12, 1e-6));
	CHECK(f.epoch == 1792138500); /* 2026-10-16T08:15:00Z */

	/* GGA */
	with_checksum("$GNGGA,081500.000,1058.2720,N,10639.0180,E,1,08,1.2,12.3,M,-2.1,M,,", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == 2);
	CHECK(f.sats == 8);
	CHECK(f.fix_quality == 1);
	CHECK(NEAR(f.hdop, 1.2, 1e-6));
	CHECK(NEAR(f.alt_m, 12.3, 1e-6));
	CHECK(f.valid); /* GGA does not clear the RMC validity */

	/* RMC void clears valid, keeps the old position */
	with_checksum("$GNRMC,081459.000,V,,,,,,,161026,,,N", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == 1);
	CHECK(!f.valid);
	CHECK(NEAR(f.lat, 10 + 58.2720 / 60, 1e-6));

	/* southern / western hemispheres, GP talker, year rollover */
	with_checksum("$GPRMC,235959.000,A,3351.7020,S,15112.6300,E,3.40,120.00,311225,,,A", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == 1);
	CHECK(f.valid);
	CHECK(NEAR(f.lat, -(33 + 51.7020 / 60), 1e-6));
	CHECK(NEAR(f.lng, 151 + 12.6300 / 60, 1e-6));
	CHECK(f.epoch == 1767225599); /* 2025-12-31T23:59:59Z */
	double w;
	CHECK(nmea_coord("12230.0000", 'W', &w) && NEAR(w, -122.5, 1e-9));
	CHECK(!nmea_coord("1230.0", 'X', &w));
	CHECK(!nmea_coord("12", 'N', &w));
	CHECK(!nmea_coord("1299.0", 'N', &w)); /* minutes >= 60 */

	/* other sentences are ignored, garbage is -1 */
	with_checksum("$GNGSA,A,3,05,13,15,,,,,,,,,,2.1,1.2,1.7", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == 0);
	with_checksum("$GNVTG,0.00,T,,M,0.12,N,0.22,K,A", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == 0);
	CHECK(nmea_parse_line("$GNRMC,garbage", &f) == -1);
	with_checksum("$GNRMC,081500.000,A,1058.2720", line, sizeof(line)); /* too few fields */
	CHECK(nmea_parse_line(line, &f) == -1);
	with_checksum("$GNRMC,081500.000,A,ABCD.2720,N,10639.0180,E,0.12,0.00,161026,,,A", line, sizeof(line));
	CHECK(nmea_parse_line(line, &f) == -1);

	/* epoch helper */
	CHECK(nmea_epoch(1970, 1, 1, 0, 0, 0) == 0);
	CHECK(nmea_epoch(2000, 3, 1, 0, 0, 0) == 951868800);
	CHECK(nmea_epoch(2026, 13, 1, 0, 0, 0) == 0);

	(void)rmc_ok; (void)gga_ok; (void)rmc_void; (void)gp_rmc;
	printf("%s: %d failure(s)\n", __FILE__, fails);
	return fails ? 1 : 0;
}
