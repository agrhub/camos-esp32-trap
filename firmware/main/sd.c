/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * microSD card (Freenove ESP32-S3 CAM: SD_MMC 1-bit, CLK 39, CMD 38, D0 40). Every capture is also saved to the card
 * (when "save photos to SD" is on) as /sdcard/DCIM/YYYYMMDD/HHMMSSx.JPG (8.3 names, x = S schedule, R rpc / console,
 * B boot, M motion, W web). Below SD_MIN_FREE_PCT free, the oldest day is deleted. The web console's Playback and
 * Storage pages list, show, download and delete the files and format the card. With usbMode "storage" the card can be
 * lent to a computer on the native USB port (usbdev.c): its file system is unmounted from the camera meanwhile, and a
 * lock keeps a save, a listing or a download from running across the hand-over.
 * The AI-Thinker ESP32-CAM preset has no SD support here: its card pins are the flash LED, the button and the battery ADC.
 */
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camos.h"

static const char *TAG = "sd";
#define MOUNT "/sdcard"
#define ROOT MOUNT "/DCIM"
#define SD_MIN_FREE_PCT 5

#if CONFIG_CAMOS_SD
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"

static sdmmc_card_t *card;
static bool mounted;

bool sd_present(void) { return mounted; }

static SemaphoreHandle_t lock; /* recursive: sd_format remounts */
static bool lent;              /* the card is with the computer (usbMode storage) */
static sdmmc_card_t raw_card;  /* the lent card: initialised, no file system */

static void sd_lock(void)
{
	xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

static void sd_unlock(void)
{
	xSemaphoreGiveRecursive(lock);
}

static void sd_config(sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
	*host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
	host->flags = SDMMC_HOST_FLAG_1BIT;
	host->max_freq_khz = SDMMC_FREQ_DEFAULT;
	*slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
	slot->width = 1;
	slot->clk = CONFIG_CAMOS_SD_CLK;
	slot->cmd = CONFIG_CAMOS_SD_CMD;
	slot->d0 = CONFIG_CAMOS_SD_D0;
	slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}

bool sd_mount(void)
{
	if (!lock) {
		lock = xSemaphoreCreateRecursiveMutex();
	}
	if (mounted || lent) {
		return mounted;
	}
	sdmmc_host_t host;
	sdmmc_slot_config_t slot;
	sd_config(&host, &slot);
	esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16 * 1024 };
	esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &card);
	if (e != ESP_OK) {
		ESP_LOGI(TAG, "no card (%s)", esp_err_to_name(e));
		card = NULL;
		return false;
	}
	mounted = true;
	mkdir(ROOT, 0775);
	uint64_t total = 0, free_b = 0;
	sd_usage(&total, &free_b);
	ESP_LOGI(TAG, "card %s, %llu MB, %llu MB free", card->cid.name, (unsigned long long)(total >> 20), (unsigned long long)(free_b >> 20));
	return true;
}

void sd_unmount(void)
{
	if (mounted) {
		esp_vfs_fat_sdcard_unmount(MOUNT, card);
		mounted = false;
		card = NULL;
	}
}

bool sd_usage(uint64_t *total, uint64_t *free_bytes)
{
	*total = *free_bytes = 0;
	if (!mounted) {
		return false;
	}
	return esp_vfs_fat_info(MOUNT, total, free_bytes) == ESP_OK;
}

static bool valid_day(const char *d)
{
	if (!d || strlen(d) != 8) {
		return false;
	}
	for (int i = 0; i < 8; i++) {
		if (d[i] < '0' || d[i] > '9') {
			return false;
		}
	}
	return true;
}

static bool valid_file(const char *f)
{
	/* HHMMSSx.JPG */
	if (!f || strlen(f) != 11 || strcasecmp(f + 7, ".JPG")) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		if (f[i] < '0' || f[i] > '9') {
			return false;
		}
	}
	return (f[6] >= 'A' && f[6] <= 'Z') || (f[6] >= 'a' && f[6] <= 'z');
}

/* Delete the oldest day directory (files first) */
static void delete_oldest_day(void)
{
	DIR *dir = opendir(ROOT);
	if (!dir) {
		return;
	}
	char oldest[16] = "";
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (valid_day(e->d_name) && (!oldest[0] || strcmp(e->d_name, oldest) < 0)) {
			strlcpy(oldest, e->d_name, sizeof(oldest));
		}
	}
	closedir(dir);
	if (!oldest[0]) {
		return;
	}
	char path[64];
	snprintf(path, sizeof(path), ROOT "/%s", oldest);
	DIR *d = opendir(path);
	int n = 0;
	if (d) {
		while ((e = readdir(d))) {
			char f[96];
			snprintf(f, sizeof(f), "%s/%.15s", path, e->d_name);
			if (unlink(f) == 0) {
				n++;
			}
		}
		closedir(d);
	}
	rmdir(path);
	ESP_LOGW(TAG, "card nearly full: deleted day %s (%d files)", oldest, n);
}

static esp_err_t sd_save_jpeg_u(const uint8_t *buf, size_t len, const char *trigger, char *name_out, size_t name_n)
{
	if (!mounted) {
		return ESP_ERR_INVALID_STATE;
	}
	uint64_t total, free_b;
	for (int i = 0; i < 5 && sd_usage(&total, &free_b) && total && free_b * 100 / total < SD_MIN_FREE_PCT; i++) {
		delete_oldest_day();
	}
	time_t now = time(NULL);
	struct tm lt;
	localtime_r(&now, &lt);
	char day[16], path[64];
	if (clock_synced()) {
		strftime(day, sizeof(day), "%Y%m%d", &lt);
	} else {
		strlcpy(day, "19700101", sizeof(day)); /* no clock yet: still kept, sorted first */
	}
	snprintf(path, sizeof(path), ROOT "/%s", day);
	if (mkdir(path, 0775) != 0 && errno != EEXIST) {
		ESP_LOGW(TAG, "cannot create %s (errno %d %s)", path, errno, strerror(errno));
	}
	char letter = 'R';
	if (!strcmp(trigger, "schedule")) {
		letter = 'S';
	} else if (!strcmp(trigger, "boot")) {
		letter = 'B';
	} else if (!strcmp(trigger, "motion")) {
		letter = 'M';
	} else if (!strcmp(trigger, "web")) {
		letter = 'W';
	}
	char file[16];
	snprintf(file, sizeof(file), "%02d%02d%02d%c.JPG", lt.tm_hour, lt.tm_min, lt.tm_sec, letter);
	char full[96];
	snprintf(full, sizeof(full), "%s/%s", path, file);
	FILE *f = fopen(full, "wb");
	if (!f) {
		ESP_LOGW(TAG, "cannot write %s (errno %d %s)", full, errno, strerror(errno));
		return ESP_FAIL;
	}
	size_t w = fwrite(buf, 1, len, f);
	fclose(f);
	if (w != len) {
		unlink(full);
		ESP_LOGW(TAG, "short write %s (%u of %u)", full, (unsigned)w, (unsigned)len);
		return ESP_FAIL;
	}
	if (name_out) {
		snprintf(name_out, name_n, "%s/%s", day, file);
	}
	ESP_LOGI(TAG, "saved %s/%s (%u bytes)", day, file, (unsigned)len);
	return ESP_OK;
}

static cJSON *sd_list_days_u(void)
{
	cJSON *arr = cJSON_CreateArray();
	DIR *dir = mounted ? opendir(ROOT) : NULL;
	if (!dir) {
		return arr;
	}
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (!valid_day(e->d_name)) {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), ROOT "/%.15s", e->d_name);
		int n = 0;
		DIR *d = opendir(path);
		if (d) {
			struct dirent *x;
			while ((x = readdir(d))) {
				n += valid_file(x->d_name);
			}
			closedir(d);
		}
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "day", e->d_name);
		cJSON_AddNumberToObject(o, "files", n);
		cJSON_AddItemToArray(arr, o);
	}
	closedir(dir);
	return arr;
}

static cJSON *sd_list_files_u(const char *day)
{
	cJSON *arr = cJSON_CreateArray();
	if (!mounted || !valid_day(day)) {
		return arr;
	}
	char path[64];
	snprintf(path, sizeof(path), ROOT "/%s", day);
	DIR *d = opendir(path);
	if (!d) {
		return arr;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!valid_file(e->d_name)) {
			continue;
		}
		char full[96];
		snprintf(full, sizeof(full), "%s/%.15s", path, e->d_name);
		struct stat st;
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "name", e->d_name);
		cJSON_AddNumberToObject(o, "bytes", stat(full, &st) == 0 ? (double)st.st_size : 0);
		char tr[2] = { e->d_name[6], 0 };
		cJSON_AddStringToObject(o, "trigger", tr);
		cJSON_AddItemToArray(arr, o);
	}
	closedir(d);
	return arr;
}

static esp_err_t sd_serve_u(httpd_req_t *req, const char *day, const char *file, bool download)
{
	if (!mounted || !valid_day(day) || !valid_file(file)) {
		return web_send_error(req, "404 Not Found", "NOT_FOUND", NULL);
	}
	char full[96];
	snprintf(full, sizeof(full), ROOT "/%s/%s", day, file);
	FILE *f = fopen(full, "rb");
	if (!f) {
		return web_send_error(req, "404 Not Found", "NOT_FOUND", NULL);
	}
	httpd_resp_set_type(req, "image/jpeg");
	httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=3600");
	char disp[64];
	if (download) {
		snprintf(disp, sizeof(disp), "attachment; filename=\"%s-%s\"", day, file);
		httpd_resp_set_hdr(req, "Content-Disposition", disp);
	}
	char *buf = malloc(4096);
	esp_err_t e = ESP_OK;
	size_t n;
	while (buf && (n = fread(buf, 1, 4096, f)) > 0) {
		if ((e = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) {
			break;
		}
	}
	free(buf);
	fclose(f);
	if (e == ESP_OK) {
		e = httpd_resp_send_chunk(req, NULL, 0);
	}
	return e;
}

static esp_err_t sd_delete_u(const char *day, const char *file)
{
	if (!mounted || !valid_day(day)) {
		return ESP_ERR_INVALID_ARG;
	}
	char path[96];
	if (file && file[0]) {
		if (!valid_file(file)) {
			return ESP_ERR_INVALID_ARG;
		}
		snprintf(path, sizeof(path), ROOT "/%s/%s", day, file);
		return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
	}
	/* the whole day */
	snprintf(path, sizeof(path), ROOT "/%s", day);
	DIR *d = opendir(path);
	if (d) {
		struct dirent *e;
		while ((e = readdir(d))) {
			char f[128];
			snprintf(f, sizeof(f), "%s/%.15s", path, e->d_name);
			unlink(f);
		}
		closedir(d);
	}
	return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t sd_format_u(void)
{
	if (!mounted) {
		return ESP_ERR_INVALID_STATE;
	}
	esp_err_t e = esp_vfs_fat_sdcard_format(MOUNT, card);
	if (e == ESP_OK) {
		mkdir(ROOT, 0775);
		ESP_LOGW(TAG, "card formatted");
	}
	return e;
}

const char *sd_card_name(void) { return card ? card->cid.name : ""; }

/* ------------------------------------------------------------------ locked entry points, and the USB hand-over */

esp_err_t sd_save_jpeg(const uint8_t *buf, size_t len, const char *trigger, char *name_out, size_t name_n)
{
	sd_lock();
	esp_err_t e = sd_save_jpeg_u(buf, len, trigger, name_out, name_n);
	sd_unlock();
	return e;
}

cJSON *sd_list_days(void)
{
	sd_lock();
	cJSON *a = sd_list_days_u();
	sd_unlock();
	return a;
}

cJSON *sd_list_files(const char *day)
{
	sd_lock();
	cJSON *a = sd_list_files_u(day);
	sd_unlock();
	return a;
}

esp_err_t sd_serve(httpd_req_t *req, const char *day, const char *file, bool download)
{
	sd_lock();
	esp_err_t e = sd_serve_u(req, day, file, download);
	sd_unlock();
	return e;
}

esp_err_t sd_delete(const char *day, const char *file)
{
	sd_lock();
	esp_err_t e = sd_delete_u(day, file);
	sd_unlock();
	return e;
}

esp_err_t sd_format(void)
{
	sd_lock();
	esp_err_t e = sd_format_u();
	sd_unlock();
	return e;
}

/* The computer takes the card: unmount the file system, keep the card initialised for raw sector access */
void *sd_usb_lend(void)
{
	if (!lock) {
		return NULL;
	}
	sd_lock();
	void *r = NULL;
	if (mounted) {
		esp_vfs_fat_sdcard_unmount(MOUNT, card); /* also stops the SDMMC host */
		mounted = false;
		card = NULL;
		sdmmc_host_t host;
		sdmmc_slot_config_t slot;
		sd_config(&host, &slot);
		memset(&raw_card, 0, sizeof(raw_card));
		if (sdmmc_host_init() == ESP_OK && sdmmc_host_init_slot(host.slot, &slot) == ESP_OK &&
			sdmmc_card_init(&host, &raw_card) == ESP_OK) {
			lent = true;
			r = &raw_card;
		} else {
			ESP_LOGW(TAG, "card hand-over to USB failed: kept by the camera");
			sdmmc_host_deinit();
			sd_mount();
		}
	}
	sd_unlock();
	return r;
}

/* The computer let go (eject, unplug): the camera mounts the card again */
void sd_usb_return(void)
{
	if (!lock) {
		return;
	}
	sd_lock();
	if (lent) {
		sdmmc_host_deinit();
		lent = false;
		sd_mount();
	}
	sd_unlock();
}


#else /* no SD support in this build */

bool sd_present(void) { return false; }
bool sd_mount(void) { return false; }
void sd_unmount(void) {}
bool sd_usage(uint64_t *total, uint64_t *free_bytes) { *total = *free_bytes = 0; return false; }
esp_err_t sd_save_jpeg(const uint8_t *buf, size_t len, const char *trigger, char *name_out, size_t name_n) { return ESP_ERR_NOT_SUPPORTED; }
cJSON *sd_list_days(void) { return cJSON_CreateArray(); }
cJSON *sd_list_files(const char *day) { return cJSON_CreateArray(); }
esp_err_t sd_serve(httpd_req_t *req, const char *day, const char *file, bool download) { return web_send_error(req, "404 Not Found", "NOT_SUPPORTED", NULL); }
esp_err_t sd_delete(const char *day, const char *file) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sd_format(void) { return ESP_ERR_NOT_SUPPORTED; }
const char *sd_card_name(void) { return ""; }
void *sd_usb_lend(void) { return NULL; }
void sd_usb_return(void) {}

#endif
