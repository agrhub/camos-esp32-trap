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
	live_dc_on_sent(&f, 1, 0);
	CHECK(live_dc_may_send(&f, 10));
	live_dc_on_sent(&f, 2, 10);
	CHECK(!live_dc_may_send(&f, 20));          /* two in flight */
	live_dc_on_ack(&f, 1, 60);
	CHECK(f.last_rtt_ms == 60);
	CHECK(live_dc_may_send(&f, 70));
	live_dc_on_sent(&f, 3, 70);
	CHECK(!live_dc_may_send(&f, 500));
	CHECK(live_dc_may_send(&f, 70 + 501));     /* nothing acked for half a second: taken as lost */
	live_dc_on_ack(&f, 2, 1100);                /* an ack from before the reset is ignored */
	CHECK(f.acked == 3);
	live_dc_on_sent(&f, 4, 1200);
	live_dc_on_ack(&f, 9, 1210);                /* never sent */
	CHECK(f.acked == 3);
	live_dc_on_ack(&f, 4, 1250);
	CHECK(f.acked == 4 && f.last_rtt_ms == 50);

	CHECK(!live_dc_size_ok(1024, 768) && live_dc_size_ok(1600, 1200)); /* the sizes set above */

	printf("test_live_dc: %d failure(s)\n", fails);
	return fails ? 1 : 0;
}
