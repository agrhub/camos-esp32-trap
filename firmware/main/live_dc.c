/* SPDX-License-Identifier: AGPL-3.0-only */
/* The live view data channel protocol (see live_dc.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "live_dc.h"

static int dc_sizes[LIVE_DC_MAX_SIZES][2] = { { 320, 240 }, { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 } };
static int n_dc_sizes = 5;

void live_dc_set_sizes(const int sizes[][2], int n)
{
	n_dc_sizes = n > LIVE_DC_MAX_SIZES ? LIVE_DC_MAX_SIZES : n;
	for (int i = 0; i < n_dc_sizes; i++) {
		dc_sizes[i][0] = sizes[i][0], dc_sizes[i][1] = sizes[i][1];
	}
}

int live_dc_sizes(const int (**sizes)[2])
{
	*sizes = (const int (*)[2])dc_sizes;
	return n_dc_sizes;
}

bool live_dc_size_ok(int w, int h)
{
	for (int i = 0; i < n_dc_sizes; i++) {
		if (dc_sizes[i][0] == w && dc_sizes[i][1] == h) {
			return true;
		}
	}
	return false;
}

/* Video tiers by picture area (client/src/utils/camosLive.js in FlyBot): the largest area each takes, and its own size */
static const struct {
	long max_area, area;
} TIERS[] = {
	{ 120000, 320 * 240 },   { 265000, 640 * 360 },    { 600000, 640 * 480 },
	{ 1450000, 1280 * 720 }, { 2800000, 1920 * 1080 }, { 0x7fffffffL, 2560 * 1440 },
};
#define N_TIERS (int)(sizeof(TIERS) / sizeof(TIERS[0]))

static int tier_of(int w, int h)
{
	int t = 0;
	while (t < N_TIERS - 1 && (long)w * h > TIERS[t].max_area) {
		t++;
	}
	return t;
}

bool live_dc_step_down(const live_quality_t *cur, live_quality_t *out)
{
	if (cur->mode != LIVE_MODE_JPEG) {
		return false;
	}
	for (int t = tier_of(cur->w, cur->h) - 1; t >= 0; t--) {
		int best = -1;
		long best_off = 0;
		for (int i = 0; i < n_dc_sizes; i++) {
			int w = dc_sizes[i][0], h = dc_sizes[i][1];
			long off = labs((long)w * h - TIERS[t].area);
			if (w > 320 && tier_of(w, h) == t && (best < 0 || off < best_off)) {
				best = i, best_off = off;
			}
		}
		if (best >= 0) {
			*out = *cur;
			out->w = dc_sizes[best][0], out->h = dc_sizes[best][1];
			return true;
		}
	}
	return false;
}

static live_dc_kind_t bad(live_dc_msg_t *out, const char *why)
{
	out->kind = LIVE_DC_BAD;
	snprintf(out->error, sizeof(out->error), "%s", why);
	return LIVE_DC_BAD;
}

static bool get_int(const cJSON *o, const char *key, int *v)
{
	const cJSON *n = cJSON_GetObjectItem(o, key);
	if (!n) {
		return true; /* absent: unchanged */
	}
	if (!cJSON_IsNumber(n)) {
		return false;
	}
	*v = (int)n->valuedouble;
	return true;
}

live_dc_kind_t live_dc_parse(const char *text, size_t len, const live_quality_t *cur, int h264_w, int h264_h,
							 live_dc_msg_t *out)
{
	memset(out, 0, sizeof(*out));
	cJSON *j = cJSON_ParseWithLength(text, len);
	if (!cJSON_IsObject(j)) {
		cJSON_Delete(j);
		return bad(out, "not a JSON object");
	}
	const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
	live_dc_kind_t kind = LIVE_DC_NONE;
	if (!t) {
		kind = bad(out, "no t");
	} else if (!strcmp(t, "ack")) {
		const cJSON *s = cJSON_GetObjectItem(j, "seq");
		if (!cJSON_IsNumber(s) || s->valuedouble < 0) {
			kind = bad(out, "ack without seq");
		} else {
			out->kind = kind = LIVE_DC_ACK;
			out->seq = (uint32_t)s->valuedouble;
		}
	} else if (!strcmp(t, "quality")) {
		live_quality_t q = *cur;
		const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mode"));
		if (mode && !strcmp(mode, "jpeg")) {
			q.mode = LIVE_MODE_JPEG;
		} else if (mode && !strcmp(mode, "h264")) {
			q.mode = LIVE_MODE_H264;
		} else if (mode) {
			cJSON_Delete(j);
			return bad(out, "mode is h264 or jpeg");
		}
		int w = q.w, h = q.h;
		if (!get_int(j, "w", &w) || !get_int(j, "h", &h) || !get_int(j, "q", &q.q) || !get_int(j, "fps", &q.fps)) {
			cJSON_Delete(j);
			return bad(out, "w, h, q and fps are numbers");
		}
		if (q.mode == LIVE_MODE_H264) {
			q.w = h264_w, q.h = h264_h; /* the encoder's size is fixed at build time */
		} else if (!live_dc_size_ok(w, h)) {
			cJSON_Delete(j);
			return bad(out, "size not offered");
		} else {
			q.w = w, q.h = h;
		}
		if (q.q < LIVE_DC_Q_MIN || q.q > LIVE_DC_Q_MAX) {
			cJSON_Delete(j);
			return bad(out, "q is 8..40");
		}
		if (q.fps < 1 || q.fps > 25) {
			cJSON_Delete(j);
			return bad(out, "fps is 1..25");
		}
		out->kind = kind = LIVE_DC_QUALITY;
		out->quality = q;
		out->fixed = cJSON_IsTrue(cJSON_GetObjectItem(j, "fixed"));
		const cJSON *x = cJSON_GetObjectItem(j, "exp");
		if (cJSON_IsObject(x)) {
			live_dc_exp_t e = { .set = true };
			if (!get_int(x, "kbps", &e.kbps) || !get_int(x, "gapMs", &e.gap_ms) || !get_int(x, "chunk", &e.chunk) ||
				!get_int(x, "udpWaitMs", &e.udp_wait_ms) || e.kbps < 0 || e.kbps > 20000 || e.gap_ms < 0 || e.gap_ms > 10000 ||
				(e.chunk && (e.chunk < 1000 || e.chunk > LIVE_DC_CHUNK)) || e.udp_wait_ms < 0 || e.udp_wait_ms > 1000) {
				cJSON_Delete(j);
				return bad(out, "exp out of range");
			}
			out->exp = e;
		}
	}
	cJSON_Delete(j);
	return kind;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

void live_dc_header(uint8_t out[LIVE_DC_HDR_LEN], uint32_t seq, uint32_t total, uint32_t offset, uint32_t ts_ms)
{
	out[0] = 'C', out[1] = 'J', out[2] = LIVE_DC_VERSION, out[3] = LIVE_DC_HDR_LEN;
	put32(out + 4, seq);
	put32(out + 8, total);
	put32(out + 12, offset);
	put32(out + 16, ts_ms);
}

void live_dc_flow_reset(live_dc_flow_t *f)
{
	memset(f, 0, sizeof(*f));
	f->last_rtt_ms = -1;
	f->timeout_ms = LIVE_DC_ACK_TIMEOUT_MS;
}

void live_dc_flow_size(live_dc_flow_t *f, int w, int h, int gap_ms)
{
	f->slow = (long)w * h > LIVE_DC_SLOW_AREA;
	f->gap_ms = gap_ms > 0 ? gap_ms : LIVE_DC_SLOW_GAP_MS;
}

bool live_dc_may_send(live_dc_flow_t *f, int64_t now_ms)
{
	if (f->slow && f->sent && now_ms - f->sent_ms[f->sent % 8] < f->gap_ms) {
		return false;
	}
	if (f->sent - f->acked < (f->slow ? 1u : LIVE_DC_WINDOW)) {
		return true;
	}
	if (now_ms - f->last_ms > f->timeout_ms) {
		f->acked = f->sent; /* the frames in flight are taken as lost */
		if (f->rate_kbps) {
			f->rate_kbps = f->rate_kbps / 2 > LIVE_DC_PACE_MIN_KBPS ? f->rate_kbps / 2 : LIVE_DC_PACE_MIN_KBPS;
		}
		f->last_ms = now_ms;
		return true;
	}
	return false;
}

void live_dc_on_sent(live_dc_flow_t *f, uint32_t seq, uint32_t bytes, int32_t push_ms, int64_t now_ms)
{
	f->sent = seq;
	f->sent_bytes[seq % 8] = bytes;
	f->sent_push_ms[seq % 8] = push_ms;
	uint32_t t = LIVE_DC_ACK_TIMEOUT_MS + bytes / LIVE_DC_SLOW_BYTES_PER_MS;
	f->timeout_ms = (int32_t)(t > LIVE_DC_ACK_TIMEOUT_MAX_MS ? LIVE_DC_ACK_TIMEOUT_MAX_MS : t);
	f->last_ms = now_ms;
	f->sent_ms[seq % 8] = now_ms;
}

void live_dc_on_ack(live_dc_flow_t *f, uint32_t seq, int64_t now_ms)
{
	if (seq > f->sent || seq <= f->acked) {
		return; /* stale, or from before a reset */
	}
	f->acked = seq;
	f->last_ms = now_ms;
	if (f->sent - seq < 8) {
		f->last_rtt_ms = (int32_t)(now_ms - f->sent_ms[seq % 8]);
		/* delivery rate: the frame's bytes over the time from the start of its hand-over to its ack */
		int64_t ms = f->last_rtt_ms + f->sent_push_ms[seq % 8];
		if (ms > 0 && f->sent_bytes[seq % 8] > 0) {
			int32_t kbps = (int32_t)((int64_t)f->sent_bytes[seq % 8] * 8 / ms);
			f->rate_kbps = f->rate_kbps ? (f->rate_kbps * 3 + kbps) / 4 : kbps;
		}
	}
}

int live_dc_pace_kbps(const live_dc_flow_t *f)
{
	if (!f->slow) {
		return 0;
	}
	int r = f->rate_kbps ? f->rate_kbps * 5 / 4 : LIVE_DC_PACE_START_KBPS;
	return r < LIVE_DC_PACE_MIN_KBPS ? LIVE_DC_PACE_MIN_KBPS : r > LIVE_DC_PACE_MAX_KBPS ? LIVE_DC_PACE_MAX_KBPS : r;
}
