/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * OTA (RPC `ota.apply`): GET /api/device/ota gives {fw, url, sha256}; the image is streamed into the other OTA slot
 * with esp_ota_write while a SHA-256 runs over the bytes; only when the digest matches is the slot made bootable.
 * A wrong hash leaves the running firmware untouched (esp_ota_abort). The new image must then mark itself valid
 * (esp_ota_mark_app_valid_cancel_rollback) after its first successful MQTT connect; the bootloader rolls back
 * otherwise when CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on (sdkconfig.defaults).
 * `progress` (may be NULL) is called at 25, 50 and 75 % when the server gives the length (ota_progress events).
 */
#include <string.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

static const char *TAG = "ota";

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* An image is downloading: motion detection pauses (its photos took the 4G uplink and the download stalled twice,
 * 24 Oct 2026), and the live view stops and cannot start (its encoder holds the internal RAM the TLS download
 * needs: "esp-aes: Failed to allocate memory", DOWNLOAD_FAILED at 0 %, 25 Oct 2026) */
static volatile bool running;

bool ota_running(void) { return running; }

static esp_err_t apply(const char *url, const char *sha256_hex, char *err, size_t errn, ota_progress_cb progress,
					   void *ctx);

esp_err_t ota_apply(const char *url, const char *sha256_hex, char *err, size_t errn, ota_progress_cb progress, void *ctx)
{
	running = true;
	live_stop();
	esp_err_t e = apply(url, sha256_hex, err, errn, progress, ctx);
	running = false;
	return e;
}

static esp_err_t apply(const char *url, const char *sha256_hex, char *err, size_t errn, ota_progress_cb progress,
					   void *ctx)
{
	err[0] = 0;
	uint8_t expect[32];
	if (!sha256_hex || strlen(sha256_hex) != 64) {
		strlcpy(err, "sha256 must be 64 hex characters", errn);
		return ESP_ERR_INVALID_ARG;
	}
	for (int i = 0; i < 32; i++) {
		int h = hexval(sha256_hex[2 * i]), l = hexval(sha256_hex[2 * i + 1]);
		if (h < 0 || l < 0) {
			strlcpy(err, "sha256 is not hex", errn);
			return ESP_ERR_INVALID_ARG;
		}
		expect[i] = (uint8_t)(h * 16 + l);
	}
	const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
	if (!part) {
		strlcpy(err, "no OTA partition", errn);
		return ESP_ERR_NOT_FOUND;
	}
	esp_http_client_config_t cfg = {
		.url = url,
		.timeout_ms = 30000,
		.buffer_size = 4096,
		.keep_alive_enable = true,
	};
	if (strncmp(url, "https://", 8) == 0) {
		cfg.crt_bundle_attach = esp_crt_bundle_attach;
	}
	esp_http_client_handle_t h = esp_http_client_init(&cfg);
	if (!h) {
		strlcpy(err, "http init", errn);
		return ESP_FAIL;
	}
	if (strncmp(url, g_cfg.api_base, strlen(g_cfg.api_base)) == 0 && g_cfg.token[0]) {
		esp_http_client_set_header(h, "X-Device-Token", g_cfg.token); /* only under apiBase (protocol 11.4) */
		esp_http_client_set_header(h, "X-Device-Serial", g_cfg.serial);
	}
	esp_err_t e = esp_http_client_open(h, 0);
	if (e != ESP_OK) {
		snprintf(err, errn, "connect: %s", esp_err_to_name(e));
		esp_http_client_cleanup(h);
		return e;
	}
	int64_t total = esp_http_client_fetch_headers(h);
	int status = esp_http_client_get_status_code(h);
	if (status != 200) {
		snprintf(err, errn, "http %d", status);
		esp_http_client_cleanup(h);
		return ESP_FAIL;
	}
	if (total > (int64_t)part->size) {
		snprintf(err, errn, "image %lld bytes > slot %lu", (long long)total, (unsigned long)part->size);
		esp_http_client_cleanup(h);
		return ESP_ERR_INVALID_SIZE;
	}
	ESP_LOGI(TAG, "downloading %lld bytes into %s", (long long)total, part->label);
	esp_ota_handle_t oh;
	led_state_t led_before = led_get();
	led_set(LED_OTA);
	e = esp_ota_begin(part, total > 0 ? (size_t)total : OTA_SIZE_UNKNOWN, &oh);
	if (e != ESP_OK) {
		led_set(led_before);
	}
	if (e != ESP_OK) {
		snprintf(err, errn, "ota begin: %s", esp_err_to_name(e));
		esp_http_client_cleanup(h);
		return e;
	}
	mbedtls_sha256_context sha;
	mbedtls_sha256_init(&sha);
	mbedtls_sha256_starts(&sha, 0);
	char *buf = malloc(4096);
	size_t got = 0;
	int reported = 0; /* last ota_progress milestone */
	bool ok = buf != NULL;
	while (ok) {
		int n = esp_http_client_read(h, buf, 4096);
		if (n < 0) {
			strlcpy(err, "read", errn);
			ok = false;
		} else if (n == 0) {
			break;
		} else {
			mbedtls_sha256_update(&sha, (const uint8_t *)buf, n);
			if ((e = esp_ota_write(oh, buf, n)) != ESP_OK) {
				snprintf(err, errn, "ota write: %s", esp_err_to_name(e));
				ok = false;
			}
			got += n;
			if ((got & 0x3ffff) == 0) {
				ESP_LOGI(TAG, "%u bytes", (unsigned)got);
			}
			int pct = ota_progress_next((long long)got, (long long)total, reported, 25);
			if (pct > 0) {
				reported = pct;
				if (progress) {
					progress(pct, ctx);
				}
			}
		}
	}
	free(buf);
	esp_http_client_cleanup(h);
	uint8_t digest[32];
	mbedtls_sha256_finish(&sha, digest);
	mbedtls_sha256_free(&sha);
	if (ok && total > 0 && (int64_t)got != total) {
		snprintf(err, errn, "short download %u of %lld", (unsigned)got, (long long)total);
		ok = false;
	}
	if (ok && memcmp(digest, expect, 32) != 0) {
		strlcpy(err, "sha256 mismatch: image discarded", errn);
		ok = false;
	}
	if (!ok) {
		esp_ota_abort(oh);
		led_set(led_before);
		led_flash(LED_FLASH_ERROR);
		return ESP_FAIL;
	}
	e = esp_ota_end(oh); /* validates the image header and signature chain of the slot */
	if (e != ESP_OK) {
		snprintf(err, errn, "ota end: %s", esp_err_to_name(e));
		return e;
	}
	e = esp_ota_set_boot_partition(part);
	if (e != ESP_OK) {
		snprintf(err, errn, "set boot: %s", esp_err_to_name(e));
		return e;
	}
	ESP_LOGI(TAG, "verified %u bytes, sha256 ok, boot partition %s: restarting", (unsigned)got, part->label);
	return ESP_OK;
}
