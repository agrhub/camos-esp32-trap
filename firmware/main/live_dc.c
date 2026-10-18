/* SPDX-License-Identifier: AGPL-3.0-only */
/* The live view data channel protocol (see live_dc.h). */
#include <stdio.h>
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
}

bool live_dc_may_send(live_dc_flow_t *f, int64_t now_ms)
{
	if (f->sent - f->acked < LIVE_DC_WINDOW) {
		return true;
	}
	if (now_ms - f->last_ms > LIVE_DC_ACK_TIMEOUT_MS) {
		f->acked = f->sent; /* the frames in flight are taken as lost */
		f->last_ms = now_ms;
		return true;
	}
	return false;
}

void live_dc_on_sent(live_dc_flow_t *f, uint32_t seq, int64_t now_ms)
{
	f->sent = seq;
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
	}
}
