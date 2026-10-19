/* SPDX-License-Identifier: AGPL-3.0-only */
#include <string.h>
#include "logfwd_core.h"

size_t log_redact(char *line, size_t len, const char *const secrets[], int n_secrets)
{
	for (int s = 0; s < n_secrets; s++) {
		const char *sec = secrets[s];
		size_t sl = sec ? strlen(sec) : 0;
		if (sl < 4) {
			continue;
		}
		size_t i = 0;
		while (i + sl <= len) {
			if (memcmp(line + i, sec, sl) == 0) {
				memcpy(line + i, "***", 3);
				memmove(line + i + 3, line + i + sl, len - i - sl);
				len -= sl - 3;
				line[len] = 0;
				i += 3;
			} else {
				i++;
			}
		}
	}
	line[len] = 0;
	return len;
}

int log_parse_line(const char *line, char *tag, size_t tagn, const char **msg)
{
	if (tagn) {
		tag[0] = 0;
	}
	*msg = line;
	static const char levels[] = "EWIDV";
	const char *p = strchr(levels, line[0]);
	if (!p || !line[0] || line[1] != ' ' || line[2] != '(') {
		return 0;
	}
	const char *close = strchr(line + 3, ')');
	if (!close || close[1] != ' ') {
		return 0;
	}
	const char *t = close + 2;
	const char *colon = strstr(t, ": ");
	if (!colon) {
		*msg = t;
		return (int)(p - levels) + 1;
	}
	size_t n = (size_t)(colon - t);
	if (tagn) {
		if (n >= tagn) {
			n = tagn - 1;
		}
		memcpy(tag, t, n);
		tag[n] = 0;
	}
	*msg = colon + 2;
	return (int)(p - levels) + 1;
}

static const char *const level_names[] = { "", "error", "warn", "info", "debug", "verbose" };

int log_level_from_name(const char *name)
{
	for (int i = 1; i <= 5; i++) {
		if (name && !strcmp(name, level_names[i])) {
			return i;
		}
	}
	return 0;
}

const char *log_level_name(int level)
{
	return level >= 1 && level <= 5 ? level_names[level] : "";
}

void log_rate_init(log_rate_t *r, int rate, int burst, int64_t now_ms)
{
	r->rate = rate;
	r->burst = burst;
	r->tokens = burst;
	r->last_ms = now_ms;
}

bool log_rate_take(log_rate_t *r, int bytes, int64_t now_ms)
{
	if (now_ms > r->last_ms) {
		r->tokens += (double)(now_ms - r->last_ms) * r->rate / 1000.0;
		if (r->tokens > r->burst) {
			r->tokens = r->burst;
		}
		r->last_ms = now_ms;
	}
	if (r->tokens < bytes) {
		return false;
	}
	r->tokens -= bytes;
	return true;
}

size_t log_ring_tail(const char *ring, size_t size, uint32_t total, char *out, size_t outn)
{
	if (!outn) {
		return 0;
	}
	size_t have = total < size ? total : size;
	size_t want = outn - 1 < have ? outn - 1 : have;
	uint32_t start = total - (uint32_t)want;
	size_t n = 0;
	bool skipping = want < have || total > size; /* cut inside a line: skip to the next one */
	for (size_t i = 0; i < want; i++) {
		char c = ring[(start + i) % size];
		if (skipping) {
			if (c == '\n') {
				skipping = false;
			}
			continue;
		}
		out[n++] = c;
	}
	out[n] = 0;
	return n;
}
