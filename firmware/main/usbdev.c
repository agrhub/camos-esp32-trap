/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * The native USB port (USB OTG, device mode) of the ESP32-S3 boards, set by the usbMode setting and applied at boot:
 *  - "auto" (default): nothing to choose. Plugged into a computer, the camera is a drive (the microSD card) and a webcam
 *    at once, each part as described below; with a USB stick on the port instead (read from the D+ line, usbhost.c),
 *    the port is the stick's host and photos are saved on the stick;
 *  - "off": the port stays the USB serial/JTAG console, as before;
 *  - "storage": the microSD card appears on the computer as a drive (mass storage). While the computer has it, the
 *    camera does not write to the card (photos still upload); it gets the card back when the drive is ejected or the
 *    cable is unplugged;
 *  - "webcam": a UVC camera (MJPEG, 640x480 by default, up to 2560x1920 at about 1 fps) that any video app can
 *    open. While an app streams, the camera is taken like for the cloud live view: scheduled photos and the web preview
 *    wait.
 * The CH340 port (UART0) keeps the serial console and flashing in every mode. While a computer is connected the camera
 * does not deep sleep. The streaming loop follows espressif/usb_device_uvc 1.3.1 (Apache-2.0).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "usb";

typedef enum { USB_OFF = 0, USB_STORAGE, USB_WEBCAM, USB_AUTO } usb_mode_t;

static usb_mode_t parse_mode(const char *s)
{
	if (s && !strcmp(s, "auto")) {
		return USB_AUTO;
	}
	if (s && !strcmp(s, "storage")) {
		return USB_STORAGE;
	}
	if (s && !strcmp(s, "webcam")) {
		return USB_WEBCAM;
	}
	return USB_OFF;
}

bool usb_mode_valid(const char *s)
{
	return s && (!strcmp(s, "auto") || !strcmp(s, "off") || !strcmp(s, "storage") || !strcmp(s, "webcam"));
}

#if CONFIG_SOC_USB_OTG_SUPPORTED && CONFIG_CAMOS_USB_OTG

#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb/usbdev_desc.h"
#include "usb/usb_frames.h"
#include "sdmmc_cmd.h"

static usb_mode_t mode;
static bool host_mode; /* the port is a USB host for a stick (usbhost.c) */

static usb_phy_handle_t phy;
static TaskHandle_t worker;
static TaskHandle_t video_task_hdl;

/* worker requests (task notification bits) */
#define REQ_LEND BIT0
#define REQ_RETURN BIT1
#define REQ_SUSPENDED BIT2

/* the parts of the device */
#define HAS_STORAGE (mode == USB_STORAGE || mode == USB_AUTO)
#define HAS_WEBCAM (mode == USB_WEBCAM || mode == USB_AUTO)

usb_desc_kind_t usbdev_desc_kind(void)
{
	return mode == USB_AUTO ? USB_DESC_BOTH : mode == USB_STORAGE ? USB_DESC_STORAGE : USB_DESC_WEBCAM;
}

static const char *mode_name(usb_mode_t m)
{
	return m == USB_AUTO ? "auto" : m == USB_STORAGE ? "storage" : m == USB_WEBCAM ? "webcam" : "off";
}

const char *usbdev_desc_serial(void)
{
	return g_cfg.serial[0] ? g_cfg.serial : "000000000000";
}

/* -------------------------------------------------------------------------------------------- bus events */

void tud_mount_cb(void)
{
	ESP_LOGI(TAG, "computer connected (%s)", mode_name(mode));
	if (HAS_STORAGE && worker) {
		xTaskNotify(worker, REQ_LEND, eSetBits);
	}
}

void tud_umount_cb(void)
{
	ESP_LOGI(TAG, "computer disconnected");
	if (HAS_STORAGE && worker) {
		xTaskNotify(worker, REQ_RETURN, eSetBits);
	}
}

void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;
	/* no SOF any more: the cable is out or the computer sleeps; the worker decides after a while */
	if (worker) {
		xTaskNotify(worker, REQ_SUSPENDED, eSetBits);
	}
}

void tud_resume_cb(void)
{
}

static void tusb_task(void *arg)
{
	for (;;) {
		tud_task(); /* blocks until there is USB work */
	}
}

/* ---------------------------------------------------------------------------------- storage: the microSD card */

static sdmmc_card_t *lent_card; /* sd_usb_lend(): the card, its file system unmounted from the camera */
static volatile bool ejected;

/* The worker task: lends the card to the computer and takes it back (both block on the card), keeps the camera awake
 * while a computer is connected */
static void worker_task(void *arg)
{
	int64_t suspended_since = 0, unused_since = 0;
	int no_computer = 0;
	for (;;) {
		uint32_t req = 0;
		xTaskNotifyWait(0, UINT32_MAX, &req, pdMS_TO_TICKS(5000));
		/* "auto" and no computer using the camera for 10 s (never set up, or no SOF since: the cable is out or the
		 * computer put the idle device to sleep): a stick plugged in later takes the port. A computer still on the
		 * cable is left alone (D- read without disconnecting; it must be gone on two reads in a row); otherwise the D+
		 * pull-up is let go for a moment to read the line, and the camera restarts as the stick's host. */
		if (mode == USB_AUTO && (!tud_mounted() || tud_suspended())) {
			if (!unused_since) {
				unused_since = esp_timer_get_time();
				no_computer = 0;
			} else if (esp_timer_get_time() - unused_since > 10LL * 1000000 &&
					   (!tud_mounted() || (no_computer = usbhost_computer_on_line() ? 0 : no_computer + 1) >= 2)) {
				tud_disconnect();
				vTaskDelay(pdMS_TO_TICKS(20));
				bool stick = usbhost_stick_on_line();
				tud_connect();
				if (stick) {
					ESP_LOGI(TAG, "a USB drive on the native USB port: restarting as its host");
					events_note("usb", "USB drive plugged in: restarting to save photos on it");
					vTaskDelay(pdMS_TO_TICKS(500));
					esp_restart();
				}
			}
		} else {
			unused_since = 0;
		}
		if (req & REQ_SUSPENDED) {
			suspended_since = esp_timer_get_time();
		}
		if (tud_mounted() && !tud_suspended()) {
			suspended_since = 0;
			sched_keep_awake(30); /* the computer is there: no deep sleep */
		}
		/* suspended for 10 s: the cable is out (the S3 sees no VBUS), give the card back */
		bool gone = suspended_since && esp_timer_get_time() - suspended_since > 10LL * 1000000 && tud_suspended();
		if ((req & REQ_RETURN) || gone || (ejected && lent_card)) {
			if (lent_card) {
				sd_usb_return();
				lent_card = NULL;
				ESP_LOGI(TAG, "microSD card back with the camera%s", ejected ? " (ejected)" : "");
			}
			suspended_since = 0;
		}
		if ((req & REQ_LEND) && !lent_card) {
			ejected = false;
			lent_card = (sdmmc_card_t *)sd_usb_lend();
			if (lent_card) {
				ESP_LOGI(TAG, "microSD card lent to the computer (%llu MB)",
						 (unsigned long long)((uint64_t)lent_card->csd.capacity * lent_card->csd.sector_size >> 20));
			} else {
				ESP_LOGW(TAG, "no microSD card to show the computer");
			}
		}
	}
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
	(void)lun;
	memcpy(vendor_id, "FlyBot  ", 8);
	memcpy(product_id, "Trap camera SD  ", 16);
	memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
	(void)lun;
	if (!lent_card || ejected) {
		tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); /* medium not present */
		return false;
	}
	return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
	(void)lun;
	*block_count = lent_card ? (uint32_t)lent_card->csd.capacity : 0;
	*block_size = lent_card ? (uint16_t)lent_card->csd.sector_size : 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
	(void)lun;
	(void)power_condition;
	if (load_eject && !start) {
		ejected = true; /* "Eject" on the computer: the camera takes the card back */
		if (worker) {
			xTaskNotify(worker, 0, eNoAction);
		}
	}
	return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
	(void)lun;
	if (!lent_card || offset || bufsize % 512) {
		return -1;
	}
	return sdmmc_read_sectors(lent_card, buffer, lba, bufsize / 512) == ESP_OK ? (int32_t)bufsize : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
	(void)lun;
	if (!lent_card || offset || bufsize % 512) {
		return -1;
	}
	return sdmmc_write_sectors(lent_card, buffer, lba, bufsize / 512) == ESP_OK ? (int32_t)bufsize : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
	(void)buffer;
	(void)bufsize;
	switch (scsi_cmd[0]) {
	case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
		return 0; /* the card can always be taken back */
	default:
		tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); /* invalid command */
		return -1;
	}
}

/* ------------------------------------------------------------------------------------------------- webcam */

#define UVC_BUF_SIZE (1024 * 1024) /* a 5 MP JPEG at quality 12 is a few hundred KB (PSRAM) */
#define UVC_QUALITY 12

static uint8_t *uvc_buf;
static volatile uint32_t interval_ms = 1000 / 15;
static volatile int want_w, want_h; /* the size the host committed, 0 = none */
static bool cam_held;
static int held_w, held_h;

static void cam_stop(void)
{
	if (cam_held) {
		cam_stream_stop();
		camera_give();
		cam_held = false;
		ESP_LOGI(TAG, "webcam stopped");
	}
}

static bool cam_start(int w, int h)
{
	if (cam_held && w == held_w && h == held_h) {
		return true;
	}
	cam_stop();
	if (!camera_take(5000)) {
		ESP_LOGW(TAG, "webcam: the camera is busy (a capture or the live view)");
		return false;
	}
	if (cam_stream_start(w, h, UVC_QUALITY) != ESP_OK) {
		camera_give();
		ESP_LOGW(TAG, "webcam: camera start failed at %dx%d", w, h);
		return false;
	}
	cam_held = true;
	held_w = w, held_h = h;
	ESP_LOGI(TAG, "webcam streaming %dx%d at %u ms per frame", w, h, (unsigned)interval_ms);
	return true;
}

static void video_task(void *arg)
{
	uint32_t next_ms = 0;
	bool tx_busy = false;
	int64_t idle_since = 0;
	for (;;) {
		if (!tud_video_n_streaming(0, 0)) {
			tx_busy = false;
			/* the app closed the camera (no SET_INTERFACE stop arrives on every host): let it go after 2 s */
			if (cam_held) {
				if (!idle_since) {
					idle_since = esp_timer_get_time();
				} else if (esp_timer_get_time() - idle_since > 2000000) {
					cam_stop();
				}
			}
			vTaskDelay(pdMS_TO_TICKS(20));
			continue;
		}
		idle_since = 0;
		bool size_ok = cam_held && want_w == held_w && want_h == held_h;
		if (!size_ok && !(want_w && cam_start(want_w, want_h))) {
			vTaskDelay(pdMS_TO_TICKS(200));
			continue;
		}
		sched_keep_awake(10);
		if (tx_busy) {
			if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500))) {
				continue; /* the previous frame is still going out */
			}
			tx_busy = false;
		}
		uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
		if ((int32_t)(now - next_ms) < 0) {
			vTaskDelay(pdMS_TO_TICKS(next_ms - now));
		}
		next_ms = (uint32_t)(esp_timer_get_time() / 1000) + interval_ms;
		camera_fb_t *fb = esp_camera_fb_get();
		if (!fb) {
			continue;
		}
		size_t len = fb->len;
		bool ok = fb->format == PIXFORMAT_JPEG && len <= UVC_BUF_SIZE;
		if (ok) {
			memcpy(uvc_buf, fb->buf, len);
		}
		esp_camera_fb_return(fb);
		if (!ok) {
			ESP_LOGW(TAG, "webcam frame dropped (%u bytes)", (unsigned)len);
			continue;
		}
		tx_busy = tud_video_n_frame_xfer(0, 0, uvc_buf, len);
	}
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
	(void)ctl_idx;
	(void)stm_idx;
	if (video_task_hdl) {
		xTaskNotifyGive(video_task_hdl);
	}
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, video_probe_and_commit_control_t const *parameters)
{
	(void)ctl_idx;
	(void)stm_idx;
	if (parameters->bFrameIndex < 1 || parameters->bFrameIndex > UVC_FRAME_NUM) {
		return VIDEO_ERROR_OUT_OF_RANGE;
	}
	int i = parameters->bFrameIndex - 1;
	uint32_t iv = parameters->dwFrameInterval / 10000; /* 100 ns units to ms */
	interval_ms = iv ? iv : 1000 / (uint32_t)UVC_FRAMES_INFO[0][i].rate;
	/* the video task (re)starts the sensor at this size; a new size stops the old stream there */
	want_w = UVC_FRAMES_INFO[0][i].width;
	want_h = UVC_FRAMES_INFO[0][i].height;
	return VIDEO_ERROR_NONE;
}

/* --------------------------------------------------------------------------------------------------- start */

void usbdev_start(void)
{
	mode = parse_mode(g_cfg.usb_mode);
	if (mode == USB_OFF) {
		return;
	}
	/* "auto": a USB stick on the port takes it (host mode); a computer or nothing: drive + webcam */
	const usb_peer_t peer = mode == USB_AUTO ? usbhost_sense() : USB_PEER_UNKNOWN;
	if (peer != USB_PEER_UNKNOWN) {
		ESP_LOGI(TAG, "native USB port: %s on the line", peer == USB_PEER_STICK ? "a USB drive" :
						 peer == USB_PEER_COMPUTER ? "a computer" : "nothing");
	}
	if (peer == USB_PEER_STICK) {
		host_mode = true;
		ESP_LOGI(TAG, "native USB port: host for a USB drive (photos are saved on it)");
		usbhost_start();
		return;
	}
	if (HAS_WEBCAM) {
		uvc_buf = heap_caps_malloc(UVC_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!uvc_buf) {
			ESP_LOGE(TAG, "no memory for the webcam buffer: USB stays off");
			mode = USB_OFF;
			return;
		}
	}
	/* the port leaves the USB serial/JTAG console and becomes a TinyUSB device */
	usb_phy_config_t pc = { .controller = USB_PHY_CTRL_OTG, .otg_mode = USB_OTG_MODE_DEVICE, .target = USB_PHY_TARGET_INT };
	if (usb_new_phy(&pc, &phy) != ESP_OK || !tusb_init()) {
		ESP_LOGE(TAG, "USB device start failed");
		mode = USB_OFF;
		return;
	}
	xTaskCreatePinnedToCore(tusb_task, "tinyusb", 4096, NULL, 5, NULL, tskNO_AFFINITY);
	xTaskCreate(worker_task, "usb-worker", 4096, NULL, 4, &worker);
	if (HAS_WEBCAM) {
		xTaskCreatePinnedToCore(video_task, "uvc", 4096, NULL, 4, &video_task_hdl, tskNO_AFFINITY);
	}
	ESP_LOGI(TAG, "native USB port: %s", mode == USB_AUTO ? "drive (microSD card) + webcam" :
										 mode == USB_STORAGE ? "microSD card as a drive" : "webcam (UVC, MJPEG)");
}

void usbdev_json(cJSON *parent)
{
	cJSON *u = cJSON_AddObjectToObject(parent, "usb");
	cJSON_AddStringToObject(u, "mode", host_mode ? "stick" : mode_name(mode));
	if (host_mode) {
		cJSON_AddBoolToObject(u, "stick", usbhost_stick_present());
		cJSON_AddNumberToObject(u, "stickMB", (double)(usbhost_stick_bytes() >> 20));
	}
	cJSON_AddStringToObject(u, "setting", g_cfg.usb_mode);
	cJSON_AddBoolToObject(u, "connected", !host_mode && mode != USB_OFF && tud_mounted() && !tud_suspended());
	/* configured once, then no bus activity: the computer put the idle device to sleep, or the cable is out */
	cJSON_AddBoolToObject(u, "idle", !host_mode && mode != USB_OFF && tud_mounted() && tud_suspended());
	cJSON_AddBoolToObject(u, "cardLent", lent_card != NULL);
	cJSON_AddBoolToObject(u, "streaming", cam_held);
	if (cam_held) {
		cJSON_AddNumberToObject(u, "width", held_w);
		cJSON_AddNumberToObject(u, "height", held_h);
	}
}

#else /* no USB OTG on this chip (ESP32-CAM) or not built in */

void usbdev_start(void)
{
	if (parse_mode(g_cfg.usb_mode) != USB_OFF) {
		ESP_LOGW(TAG, "usbMode %s: this board has no native USB port", g_cfg.usb_mode);
	}
}

void usbdev_json(cJSON *parent)
{
	cJSON *u = cJSON_AddObjectToObject(parent, "usb");
	cJSON_AddStringToObject(u, "mode", "off");
	cJSON_AddStringToObject(u, "setting", g_cfg.usb_mode);
	cJSON_AddBoolToObject(u, "supported", false);
}

#endif
