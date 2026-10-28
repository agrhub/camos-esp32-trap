/* SPDX-License-Identifier: AGPL-3.0-only */
/* Live view data channel protocol (firmware/main/live_dc.c): control messages, chunk header, ack window. */
#include <stdio.h>
#include <string.h>
#include "../firmware/main/live_dc.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static live_dc_kind_t parse(const char *s, const live_quality_t *cur, live_dc_msg_t *m)
{
	return live_dc_parse(s, strlen(s), cur, 320, 240, m);
}

int main(void)
{
	live_quality_t cur = { LIVE_MODE_H264, 320, 240, 12, 10 };
	live_dc_msg_t m;

	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480,\"q\":12,\"fps\":10}", &cur, &m) == LIVE_DC_QUALITY);
	CHECK(m.quality.mode == LIVE_MODE_JPEG && m.quality.w == 640 && m.quality.h == 480 && m.quality.q == 12 && m.quality.fps == 10);

	/* fields left out keep the current values */
	live_quality_t j = { LIVE_MODE_JPEG, 800, 600, 14, 8 };
	CHECK(parse("{\"t\":\"quality\",\"q\":20}", &j, &m) == LIVE_DC_QUALITY);
	CHECK(m.quality.mode == LIVE_MODE_JPEG && m.quality.w == 800 && m.quality.q == 20 && m.quality.fps == 8);

	/* h264 always uses the encoder's size */
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"h264\",\"w\":1280,\"h\":720}", &j, &m) == LIVE_DC_QUALITY);
	CHECK(m.quality.mode == LIVE_MODE_H264 && m.quality.w == 320 && m.quality.h == 240);

	/* bad values */
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":641,\"h\":480}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"mjpeg\"}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480,\"q\":2}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480,\"fps\":60}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"quality\",\"w\":\"640\"}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("not json", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"x\":1}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"hello\"}", &cur, &m) == LIVE_DC_NONE); /* unknown types are ignored */

	CHECK(parse("{\"t\":\"ack\",\"seq\":42}", &cur, &m) == LIVE_DC_ACK && m.seq == 42);

	/* the firmware sets the sensor's sizes: 1600x1200 offered, 1280x720 no longer */
	static const int sz[][2] = { { 320, 240 }, { 640, 480 }, { 1600, 1200 } };
	live_dc_set_sizes(sz, 3);
	const int(*got)[2];
	CHECK(live_dc_sizes(&got) == 3 && got[2][0] == 1600 && got[2][1] == 1200);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":1600,\"h\":1200}", &cur, &m) == LIVE_DC_QUALITY);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":1280,\"h\":720}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"ack\"}", &cur, &m) == LIVE_DC_BAD);

	/* header: little endian */
	uint8_t h[LIVE_DC_HDR_LEN];
	live_dc_header(h, 0x01020304, 50000, 16000, 123456);
	CHECK(h[0] == 'C' && h[1] == 'J' && h[2] == 1 && h[3] == 20);
	CHECK(h[4] == 4 && h[5] == 3 && h[6] == 2 && h[7] == 1);
	CHECK(h[8] == (50000 & 255) && h[9] == (50000 >> 8));
	CHECK(h[12] == (16000 & 255) && h[13] == (16000 >> 8));
	CHECK(h[16] == (123456 & 255) && h[17] == ((123456 >> 8) & 255) && h[18] == (123456 >> 16));

	/* ack window */
	live_dc_flow_t f;
	live_dc_flow_reset(&f);
	CHECK(live_dc_may_send(&f, 0));
	live_dc_on_sent(&f, 1, 0, 0, 0);
	CHECK(live_dc_may_send(&f, 10));
	live_dc_on_sent(&f, 2, 0, 0, 10);
	CHECK(!live_dc_may_send(&f, 20));          /* two in flight */
	live_dc_on_ack(&f, 1, 60);
	CHECK(f.last_rtt_ms == 60);
	CHECK(live_dc_may_send(&f, 70));
	live_dc_on_sent(&f, 3, 0, 0, 70);
	CHECK(!live_dc_may_send(&f, 500));
	CHECK(live_dc_may_send(&f, 70 + 501));     /* nothing acked for half a second: taken as lost */
	live_dc_on_ack(&f, 2, 1100);                /* an ack from before the reset is ignored */
	CHECK(f.acked == 3);
	live_dc_on_sent(&f, 4, 0, 0, 1200);
	live_dc_on_ack(&f, 9, 1210);                /* never sent */
	CHECK(f.acked == 3);
	live_dc_on_ack(&f, 4, 1250);
	CHECK(f.acked == 4 && f.last_rtt_ms == 50);
	/* a FullHD frame (105 KB) gets its transfer at 0.8 Mbit/s on top; a 2K one hits the cap */
	live_dc_on_sent(&f, 5, 105000, 0, 2000);
	live_dc_on_sent(&f, 6, 105000, 0, 2010);
	CHECK(f.timeout_ms == 1550);
	CHECK(!live_dc_may_send(&f, 2010 + 1000));   /* the fixed 500 ms of old gave it up */
	CHECK(live_dc_may_send(&f, 2010 + 1551));
	/* slow mode (FullHD and 2K): one frame at a time, LIVE_DC_SLOW_GAP_MS apart */
	live_dc_on_ack(&f, 6, 3600);
	live_dc_flow_size(&f, 1600, 1200, 0);
	CHECK(!f.slow);
	live_dc_flow_size(&f, 1920, 1080, 0);
	CHECK(f.slow);
	live_dc_on_sent(&f, 7, 280000, 0, 3700);
	live_dc_on_ack(&f, 7, 4000);
	CHECK(!live_dc_may_send(&f, 4100));          /* acked, but not the gap yet */
	CHECK(live_dc_may_send(&f, 4201));
	live_dc_on_sent(&f, 8, 280000, 0, 4300);
	CHECK(!live_dc_may_send(&f, 5000));          /* the gap is over, but not acked */
	live_dc_on_ack(&f, 8, 5100);
	CHECK(live_dc_may_send(&f, 5100));
	live_dc_flow_size(&f, 1920, 1080, 1000);     /* a test page's gap */
	live_dc_on_sent(&f, 9, 280000, 0, 5100);
	live_dc_on_ack(&f, 9, 5300);
	CHECK(!live_dc_may_send(&f, 5900) && live_dc_may_send(&f, 6101));
	f.slow = false;
	/* slow mode paces: 1000 kbit/s to start, then the delivery rate of acknowledged frames + 25 %, halved on a loss */
	live_dc_flow_t p;
	live_dc_flow_reset(&p);
	CHECK(live_dc_pace_kbps(&p) == 0);              /* not slow */
	live_dc_flow_size(&p, 2048, 1536, 0);
	CHECK(live_dc_pace_kbps(&p) == LIVE_DC_PACE_START_KBPS);
	live_dc_on_sent(&p, 1, 150000, 1000, 10000);    /* 150 KB handed over in 1 s */
	live_dc_on_ack(&p, 1, 11500);                   /* acked 1.5 s later: 1.2 Mbit over 2.5 s */
	CHECK(p.rate_kbps == 480 && live_dc_pace_kbps(&p) == 600);
	live_dc_on_sent(&p, 2, 150000, 1000, 12500);
	CHECK(live_dc_may_send(&p, 12500 + LIVE_DC_ACK_TIMEOUT_MAX_MS + 1)); /* no ack: lost */
	CHECK(p.rate_kbps == LIVE_DC_PACE_MIN_KBPS);
	CHECK(live_dc_pace_kbps(&p) == 375);
	live_dc_on_sent(&f, 10, 400000, 0, 6200);
	CHECK(f.timeout_ms == LIVE_DC_ACK_TIMEOUT_MAX_MS);

	CHECK(!live_dc_size_ok(1024, 768) && live_dc_size_ok(1600, 1200)); /* the sizes set above */

	/* experiment knobs from a test page */
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480,\"exp\":{\"kbps\":800,\"gapMs\":1500,\"chunk\":8000}}", &cur, &m) == LIVE_DC_QUALITY);
	CHECK(m.exp.set && m.exp.kbps == 800 && m.exp.gap_ms == 1500 && m.exp.chunk == 8000 && m.exp.udp_wait_ms == 0);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480}", &cur, &m) == LIVE_DC_QUALITY && !m.exp.set);
	CHECK(parse("{\"t\":\"quality\",\"exp\":{\"chunk\":500}}", &cur, &m) == LIVE_DC_BAD);
	CHECK(parse("{\"t\":\"quality\",\"exp\":{\"udpWaitMs\":5000}}", &cur, &m) == LIVE_DC_BAD);
	/* "fixed": a size the person picked */
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480,\"fixed\":true}", &cur, &m) == LIVE_DC_QUALITY && m.fixed);
	CHECK(parse("{\"t\":\"quality\",\"mode\":\"jpeg\",\"w\":640,\"h\":480}", &cur, &m) == LIVE_DC_QUALITY && !m.fixed);

	/* Auto on a weak link: one tier down at a time (an OV5640's live sizes), then H.264 */
	static const int ov[][2] = { { 320, 240 },   { 640, 480 },   { 800, 600 },   { 1024, 768 },  { 1280, 720 },
								 { 1280, 1024 }, { 1600, 1200 }, { 1920, 1080 }, { 2048, 1536 } };
	live_dc_set_sizes(ov, 9);
	live_quality_t q = { LIVE_MODE_JPEG, 2048, 1536, 12, 10 }, d;
	CHECK(live_dc_step_down(&q, &d) && d.w == 1920 && d.h == 1080 && d.q == 12 && d.fps == 10); /* 2K -> FullHD */
	q = d;
	CHECK(live_dc_step_down(&q, &d) && d.w == 1280 && d.h == 720);                            /* FullHD -> HD */
	q.w = 1600, q.h = 1200;
	CHECK(live_dc_step_down(&q, &d) && d.w == 1280 && d.h == 720);                            /* a 4:3 FullHD too */
	q = d;
	CHECK(live_dc_step_down(&q, &d) && d.w == 640 && d.h == 480);                             /* HD -> 480p */
	q = d;
	CHECK(!live_dc_step_down(&q, &d));                                                        /* 480p: H.264 next */
	q.w = 800, q.h = 600;
	CHECK(!live_dc_step_down(&q, &d));                                                        /* 800x600 is 480p too */
	q.mode = LIVE_MODE_H264;
	CHECK(!live_dc_step_down(&q, &d));
	static const int small[][2] = { { 320, 240 }, { 640, 480 }, { 1600, 1200 } };
	live_dc_set_sizes(small, 3);
	q = (live_quality_t){ LIVE_MODE_JPEG, 1600, 1200, 12, 10 };
	CHECK(live_dc_step_down(&q, &d) && d.w == 640 && d.h == 480);                             /* no HD size: 480p */

	printf("test_live_dc: %d failure(s)\n", fails);
	return fails ? 1 : 0;
}
