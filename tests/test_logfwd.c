/* SPDX-License-Identifier: AGPL-3.0-only */
/* Host tests of logfwd_core.c: secrets out of log lines, esp_log parsing, the rate limit, the crash tail */
#include <stdio.h>
#include <string.h>
#include "../firmware/main/logfwd_core.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	/* redaction: every secret, every occurrence, short ones left alone */
	const char *secrets[] = { "K7Q2MX", "wifi-pass-123", "", "ab", NULL, "tok_abcdefghijklmnop" };
	char line[200];
	strcpy(line, "I (812) store: serial ESP1234567890AB, verify code K7Q2MX, wifi Cafe, token yes");
	size_t n = log_redact(line, strlen(line), secrets, 6);
	CHECK(!strcmp(line, "I (812) store: serial ESP1234567890AB, verify code ***, wifi Cafe, token yes"));
	CHECK(n == strlen(line));
	strcpy(line, "pass=wifi-pass-123 again wifi-pass-123 tok_abcdefghijklmnop.");
	log_redact(line, strlen(line), secrets, 6);
	CHECK(!strcmp(line, "pass=*** again *** ***."));
	strcpy(line, "ab abab");
	log_redact(line, strlen(line), secrets, 6);
	CHECK(!strcmp(line, "ab abab"));

	/* parsing */
	char tag[16];
	const char *msg;
	CHECK(log_parse_line("W (165398) wifi: disconnected, retry in 880 ms", tag, sizeof(tag), &msg) == 2);
	CHECK(!strcmp(tag, "wifi") && !strcmp(msg, "disconnected, retry in 880 ms"));
	CHECK(log_parse_line("E (1250) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107", tag, sizeof(tag), &msg) == 1);
	CHECK(!strcmp(tag, "sdmmc_common") && !strcmp(msg, "sdmmc_init_ocr: send_op_cond (1) returned 0x107"));
	CHECK(log_parse_line("I (31) boot: ESP-IDF v5.4.4 2nd stage bootloader", tag, sizeof(tag), &msg) == 3);
	CHECK(log_parse_line("camos> info", tag, sizeof(tag), &msg) == 0 && msg[0] == 'c' && tag[0] == 0);
	CHECK(log_parse_line("I (5) a_very_long_tag_name_here: x", tag, 8, &msg) == 3 && !strcmp(tag, "a_very_") && !strcmp(msg, "x"));
	CHECK(log_level_from_name("debug") == 4 && log_level_from_name("nope") == 0 && !strcmp(log_level_name(2), "warn"));

	/* rate limit: 1000 B/s, burst 3000 */
	log_rate_t r;
	log_rate_init(&r, 1000, 3000, 0);
	CHECK(log_rate_take(&r, 3000, 0));
	CHECK(!log_rate_take(&r, 100, 0));
	CHECK(!log_rate_take(&r, 600, 500));
	CHECK(log_rate_take(&r, 500, 500));
	CHECK(log_rate_take(&r, 3000, 60000)); /* refilled, capped at the burst */
	CHECK(!log_rate_take(&r, 1, 60000));

	/* crash tail: whole lines only */
	char ring[16];
	const char *text = "line one\nline two\nline three\n"; /* 29 bytes into a 16-byte ring */
	unsigned total = 0;
	for (const char *p = text; *p; p++) {
		ring[total++ % sizeof(ring)] = *p;
	}
	char out[32];
	size_t t = log_ring_tail(ring, sizeof(ring), total, out, sizeof(out));
	CHECK(!strcmp(out, "line three\n") && t == strlen(out));
	t = log_ring_tail(ring, sizeof(ring), total, out, 8); /* 7 bytes: inside "line three": nothing whole */
	CHECK(t == 0 && out[0] == 0);
	char small[64] = "abc\ndef\n";
	t = log_ring_tail(small, sizeof(small), 8, out, sizeof(out)); /* not wrapped, all of it */
	CHECK(!strcmp(out, "abc\ndef\n"));

	printf("test_logfwd: %d failure(s)\n", failures);
	return failures ? 1 : 0;
}
