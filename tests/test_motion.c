/* Host test of the frame-difference motion detection (firmware/main/motion_core.c) */
#include <stdio.h>
#include <string.h>
#include "../firmware/main/motion_core.h"

static int fails;
#define CHECK(c)                                                    \
	do {                                                            \
		if (!(c)) {                                                 \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);      \
			fails++;                                                \
		}                                                           \
	} while (0)

#define W 80
#define H 60
static uint8_t img[W * H];
static motion_model_t model;

static void fill(int v) { memset(img, v, sizeof(img)); }
static void square(int x, int y, int n, int v)
{
	for (int j = y; j < y + n && j < H; j++) {
		for (int i = x; i < x + n && i < W; i++) {
			img[j * W + i] = (uint8_t)v;
		}
	}
}

int main(void)
{
	const int all[4] = { 0, 0, 100, 100 }, left[4] = { 0, 0, 50, 100 };
	CHECK(motion_pixel_threshold(100) == 12 && motion_pixel_threshold(1) == 51);
	CHECK(motion_area_threshold(100) == 1 && motion_area_threshold(50) == 16);

	/* first picture: background only */
	fill(100);
	motion_result_t r = motion_compare(&model, img, W, H, all, 50);
	CHECK(!r.motion && model.have_bg);
	/* same scene with sensor noise below the pixel threshold: nothing */
	fill(110);
	r = motion_compare(&model, img, W, H, all, 50);
	CHECK(!r.motion && r.level == 0);

	/* a 12x12 object (3 % of the picture) on the right: motion with the whole frame, not with the left half */
	fill(100);
	square(60, 20, 12, 220);
	r = motion_compare(&model, img, W, H, all, 50);
	CHECK(r.motion && r.level >= 25 && r.level <= 35);
	memset(&model, 0, sizeof(model));
	fill(100);
	motion_compare(&model, img, W, H, left, 50);
	square(60, 20, 12, 220);
	r = motion_compare(&model, img, W, H, left, 50);
	CHECK(!r.motion && r.level == 0);

	/* a tiny 3x3 object: only at a high sensitivity */
	memset(&model, 0, sizeof(model));
	fill(100);
	motion_compare(&model, img, W, H, all, 50);
	square(10, 10, 3, 200);
	r = motion_compare(&model, img, W, H, all, 50);
	CHECK(!r.motion);
	memset(&model, 0, sizeof(model));
	fill(100);
	motion_compare(&model, img, W, H, all, 100);
	square(10, 10, 3, 200);
	r = motion_compare(&model, img, W, H, all, 100);
	CHECK(r.motion);

	/* the lights go on: the whole picture changes, not motion, the background follows */
	memset(&model, 0, sizeof(model));
	fill(60);
	motion_compare(&model, img, W, H, all, 80);
	fill(200);
	r = motion_compare(&model, img, W, H, all, 80);
	CHECK(r.lighting && !r.motion);
	r = motion_compare(&model, img, W, H, all, 80);
	CHECK(!r.motion && r.level == 0);

	/* an object that stays is absorbed into the background after a while */
	memset(&model, 0, sizeof(model));
	fill(100);
	motion_compare(&model, img, W, H, all, 50);
	square(30, 30, 12, 220);
	int frames = 0;
	do {
		r = motion_compare(&model, img, W, H, all, 50);
		frames++;
	} while (r.motion && frames < 100);
	CHECK(frames > 3 && frames < 40);

	/* a new size resets the background */
	uint8_t small[40 * 30];
	memset(small, 100, sizeof(small));
	r = motion_compare(&model, small, 40, 30, all, 50);
	CHECK(!r.motion && model.w == 40);

	printf("test_motion: %d failure(s)\n", fails);
	return fails != 0;
}
