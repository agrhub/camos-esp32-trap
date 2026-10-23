/* SPDX-License-Identifier: AGPL-3.0-only */
/* The webcam's frame sizes (MJPEG). The rates are what the OV5640 delivers at each size through esp32-camera (measured
 * on the Freenove board, 21 Oct 2026: about 6 fps at 640x480 and 4 fps at 1280x720); at 1080p and 5 MP the full-speed
 * USB link (about 500 KB/s isochronous) is the limit too, so those two are for stills more than for video. The first
 * entry is the default the host gets. Every size is an esp32-camera frame size. */
#pragma once

#define UVC_FRAME_NUM 6

static const struct {
	int width;
	int height;
	int rate;
} UVC_FRAMES_INFO[1][UVC_FRAME_NUM] = { {
	{ 640, 480, 8 },
	{ 800, 600, 6 },
	{ 1280, 720, 5 },
	{ 1920, 1080, 2 },
	{ 2560, 1920, 1 }, /* the photo size: at 2592x1944 the OV5640 leaves black bars on two edges */
	{ 320, 240, 10 },
} };
