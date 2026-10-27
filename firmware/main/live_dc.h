/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * The "camos" WebRTC data channel of the live view (docs/dev/LIVE_VIEW.md, "Data channel"): the viewer's JSON control
 * messages, the header of the binary JPEG chunks, and the ack window that keeps the camera from queueing frames faster
 * than the channel drains. Portable C (cJSON only), host-tested in tests/test_live_dc.c.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_DC_LABEL "camos"
#define LIVE_DC_VERSION 1
#define LIVE_DC_HDR_LEN 20      /* 'C' 'J' version hdr_len | seq | total | offset | ts_ms (little endian) */
#define LIVE_DC_CHUNK 16000     /* JPEG bytes per binary message */
#define LIVE_DC_WINDOW 2        /* frames sent but not acknowledged */
/* Slow mode, above 1600x1200 (FullHD, 2K): one frame in flight, the next LIVE_DC_SLOW_GAP_MS after it. Their frames
 * are 60-420 KB, and sent as fast as the Wi-Fi took them, esp_peer's SCTP stalled within seconds and MQTT with it
 * (27 Oct 2026). On the trap at -60 dBm: a 1000 ms gap gave FullHD and 2K 0.7 fps, 400 ms gave FullHD 1.2-1.4 fps,
 * both steady; 150 ms stalled after 15 s. */
#define LIVE_DC_SLOW_AREA (1600 * 1200)
#define LIVE_DC_SLOW_GAP_MS 500
/* A frame in flight is taken as lost after LIVE_DC_ACK_TIMEOUT_MS plus its transfer at a slow link (100 bytes per ms,
 * 0.8 Mbit/s), at most LIVE_DC_ACK_TIMEOUT_MAX_MS: FlyBot gives a chunk 1.5 s on Wi-Fi and 2 s over 4G
 * (maxPacketLifeTime), and a 2K frame took more than a second on a good Wi-Fi (27 Oct 2026) */
#define LIVE_DC_ACK_TIMEOUT_MS 500
#define LIVE_DC_SLOW_BYTES_PER_MS 100
#define LIVE_DC_ACK_TIMEOUT_MAX_MS 2500

typedef enum { LIVE_MODE_H264 = 0, LIVE_MODE_JPEG = 1 } live_mode_t;

typedef struct {
	live_mode_t mode;
	int w, h; /* JPEG: a camera frame size; H.264: the build's fixed size */
	int q;    /* JPEG quality, esp32-camera scale 8..40 (lower = better) */
	int fps;  /* 1..25 */
} live_quality_t;

typedef enum { LIVE_DC_NONE = 0, LIVE_DC_QUALITY, LIVE_DC_ACK, LIVE_DC_BAD } live_dc_kind_t;

/* Experiment knobs for one session, sent by a test page with a quality request ("exp"); 0 keeps the default. They let
 * FullHD and 2K be tuned on a camera in the field without a new build; FlyBot never sends them. */
typedef struct {
	bool set;
	int kbps;        /* "kbps" 0..20000: the JPEG frames paced to this rate */
	int gap_ms;      /* "gapMs" 0..10000: slow mode's gap between frames instead of LIVE_DC_SLOW_GAP_MS */
	int chunk;       /* "chunk" 1000..16000: JPEG bytes per message instead of LIVE_DC_CHUNK */
	int udp_wait_ms; /* "udpWaitMs" 0..1000: how long a UDP send waits for a full Wi-Fi TX queue (main/peer_udp.c) */
} live_dc_exp_t;

typedef struct {
	live_dc_kind_t kind;
	live_quality_t quality; /* LIVE_DC_QUALITY: the request applied over the current setting */
	bool fixed;             /* LIVE_DC_QUALITY: "fixed": true, a size the person picked (no step down on a weak link) */
	live_dc_exp_t exp;      /* LIVE_DC_QUALITY: "exp", when the request has it */
	uint32_t seq;           /* LIVE_DC_ACK */
	char error[64];         /* LIVE_DC_BAD */
} live_dc_msg_t;

/* JPEG sizes a viewer may ask for: live_dc_set_sizes (the sensor's live sizes), by default 320x240 .. 1280x720 */
#define LIVE_DC_MAX_SIZES 16
void live_dc_set_sizes(const int sizes[][2], int n);
int live_dc_sizes(const int (**sizes)[2]);
bool live_dc_size_ok(int w, int h);
#define LIVE_DC_Q_MIN 8  /* JPEG quality range, esp32-camera scale (lower = better), the web console's stream too */
#define LIVE_DC_Q_MAX 40

/*
 * One control message from the viewer:
 *   {"t":"quality","mode":"h264"|"jpeg","w":640,"h":480,"q":12,"fps":10}   fields left out keep the current value;
 *                                                                         "h264" always uses h264_w x h264_h;
 *                                                                         "fixed":true keeps it on a weak link;
 *                                                                         "exp":{...} experiment knobs (live_dc_exp_t)
 *   {"t":"ack","seq":123}                                                  a complete JPEG frame arrived (acks are
 *                                                                          cumulative: a lost one is covered by the next)
 * Anything else, or a bad value: LIVE_DC_BAD with the reason (unknown "t" is LIVE_DC_NONE: ignored).
 */
live_dc_kind_t live_dc_parse(const char *text, size_t len, const live_quality_t *cur, int h264_w, int h264_h,
							 live_dc_msg_t *out);

/*
 * Auto on a weak link: the next lower video tier that the offered sizes have (tiers by picture area, as FlyBot names
 * them: 240p, 360p, 480p, HD, FullHD, 2K; within a tier the size closest to the tier's own), same q and fps. False when
 * no JPEG size above 320 wide is left below `cur`: the small H.264 stream then.
 */
bool live_dc_step_down(const live_quality_t *cur, live_quality_t *out);

/* The 20-byte header in front of each chunk of frame `seq` (total JPEG bytes, offset of this chunk, capture time) */
void live_dc_header(uint8_t out[LIVE_DC_HDR_LEN], uint32_t seq, uint32_t total, uint32_t offset, uint32_t ts_ms);

/* Ack window: at most LIVE_DC_WINDOW frames in flight (slow mode: one, gap_ms apart); a frame not
 * acknowledged in time (LIVE_DC_ACK_TIMEOUT_MS and its size) is taken as lost (the channel, not the window, decides then) */
typedef struct {
	uint32_t sent;        /* last frame sent completely */
	uint32_t acked;       /* last frame acknowledged */
	int64_t last_ms;      /* time of the last send or ack */
	int64_t sent_ms[8];   /* send time per seq % 8, for the delivery time */
	int32_t last_rtt_ms;  /* send-to-ack time of the last acknowledged frame, -1 none */
	int32_t timeout_ms;   /* the last frame's ack timeout */
	bool slow;            /* slow mode: live_dc_flow_size after each reset */
	int32_t gap_ms;       /* slow mode: the gap between frames */
} live_dc_flow_t;

void live_dc_flow_reset(live_dc_flow_t *f);
/* The JPEG size now sent: slow mode above LIVE_DC_SLOW_AREA, frames gap_ms apart (0: LIVE_DC_SLOW_GAP_MS) */
void live_dc_flow_size(live_dc_flow_t *f, int w, int h, int gap_ms);
bool live_dc_may_send(live_dc_flow_t *f, int64_t now_ms);
void live_dc_on_sent(live_dc_flow_t *f, uint32_t seq, uint32_t bytes, int64_t now_ms);
void live_dc_on_ack(live_dc_flow_t *f, uint32_t seq, int64_t now_ms);
