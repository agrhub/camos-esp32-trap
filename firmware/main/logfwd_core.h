/* SPDX-License-Identifier: AGPL-3.0-only */
/* The remote log forwarder's text work, pure C (no ESP-IDF) so it is unit-tested on the host (tests/test_logfwd.c):
 * secrets out of a line, the level and tag of an esp_log line, the rate limit, the tail kept for a crash report. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every occurrence of each secret (at least 4 characters; shorter ones are skipped) becomes "***". In place: the line
 * only gets shorter. Returns the new length. */
size_t log_redact(char *line, size_t len, const char *const secrets[], int n_secrets);

/* Level of an esp_log line "I (12345) tag: message": 1 error, 2 warn, 3 info, 4 debug, 5 verbose, 0 not an esp_log line
 * (console output, a panic). `tag` gets the tag ("" when none), `msg` points at the message (or the line). */
int log_parse_line(const char *line, char *tag, size_t tagn, const char **msg);

/* "error" | "warn" | "info" | "debug" | "verbose" -> 1..5, 0 when unknown */
int log_level_from_name(const char *name);
const char *log_level_name(int level);

/* Token bucket: `rate` bytes a second, at most `burst` saved up. */
typedef struct {
	double tokens;
	int64_t last_ms;
	int rate;
	int burst;
} log_rate_t;
void log_rate_init(log_rate_t *r, int rate, int burst, int64_t now_ms);
bool log_rate_take(log_rate_t *r, int bytes, int64_t now_ms);

/* The last bytes of a ring (`size` bytes, `total` written so far) into out (at most outn - 1 bytes, NUL-terminated),
 * starting after a line break so the first line is whole. Returns the length. */
size_t log_ring_tail(const char *ring, size_t size, uint32_t total, char *out, size_t outn);
