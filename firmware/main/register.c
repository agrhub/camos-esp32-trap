/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Zero-touch self-registration (flybot-aws docs/CONTRACTS.md 2a.2): a camera with no certificate makes an ECDSA P-256
 * key (hardware RNG; the key is stored in NVS and never leaves it), a CSR with CN = serial, and calls
 * POST {apiBase}/api/device/register. The answer's certificate, device token, thing, KVS channel and claim URL are
 * stored and the MQTT agent starts. 429 / 5xx / no answer: retried with back-off (30 s doubling to 1 h, +-20 %).
 * 409 serial_taken: red LED, the portal and console show it, and the camera asks again after 1 h, then every 2, 4 and
 * at most 6 h (the owner resets the camera in FlyBot, or adds it again with the verify code); console `reg start`
 * asks at once.
 * A pre-provisioned certificate (tools/factory.*, the portal, provision.sh) skips all of this.
 *
 * Removed in FlyBot: deleting a camera there removes its IoT thing and certificate and drops its device token. A 401
 * from the device API, or IoT refusing the connection again and again, makes the camera ask FlyBot whether its token
 * still counts (GET /api/device/ota, authenticated). Only a 401 to that question drops the registration (certificate,
 * token, thing; serial, verify code and key stay: store_drop_registration) and restarts into self-registration, at
 * most once an hour; anything else (an answer, no network) leaves the identity alone.
 */
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_csr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"
#include "reg_parse.h"

static const char *TAG = "register";
static volatile bool pending;
static TaskHandle_t task;

bool register_pending(void) { return pending; }

static int rng(void *ctx, unsigned char *out, size_t len)
{
	esp_fill_random(out, len);
	return 0;
}

/* The device key: the stored one, else a new P-256 key written to NVS first (a retry reuses it) */
static int load_or_make_key(mbedtls_pk_context *pk)
{
	mbedtls_pk_init(pk);
	if (g_cfg.key && mbedtls_pk_parse_key(pk, (const unsigned char *)g_cfg.key, strlen(g_cfg.key) + 1, NULL, 0, rng, NULL) == 0) {
		return 0;
	}
	mbedtls_pk_free(pk);
	mbedtls_pk_init(pk);
	int r = mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
	if (r == 0) {
		r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), rng, NULL);
	}
	if (r != 0) {
		ESP_LOGE(TAG, "key generation failed -0x%04x", -r);
		return r;
	}
	unsigned char *pem = calloc(1, 800);
	if (!pem) {
		return -1;
	}
	r = mbedtls_pk_write_key_pem(pk, pem, 800);
	if (r == 0 && !store_set_key((const char *)pem)) {
		r = -1;
	}
	free(pem);
	if (r == 0) {
		ESP_LOGI(TAG, "new ECDSA P-256 device key stored in NVS");
	}
	return r;
}

char *register_csr_pem(void)
{
	mbedtls_pk_context pk;
	if (load_or_make_key(&pk) != 0) {
		mbedtls_pk_free(&pk);
		return NULL;
	}
	mbedtls_x509write_csr req;
	mbedtls_x509write_csr_init(&req);
	mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);
	mbedtls_x509write_csr_set_key(&req, &pk);
	char subject[48];
	snprintf(subject, sizeof(subject), "CN=%s", g_cfg.serial);
	char *pem = NULL;
	int r = mbedtls_x509write_csr_set_subject_name(&req, subject);
	if (r == 0) {
		pem = calloc(1, 1200);
		if (pem && (r = mbedtls_x509write_csr_pem(&req, (unsigned char *)pem, 1200, rng, NULL)) != 0) {
			free(pem);
			pem = NULL;
		}
	}
	if (r != 0) {
		ESP_LOGE(TAG, "CSR failed -0x%04x", -r);
	}
	mbedtls_x509write_csr_free(&req);
	mbedtls_pk_free(&pk);
	return pem;
}

bool register_apply_response(const char *json, char *err, size_t errn)
{
	reg_result_t *r = malloc(sizeof(*r));
	if (!r) {
		snprintf(err, errn, "no memory");
		return false;
	}
	bool parsed = reg_parse(json, r, err, errn);
	bool ok = parsed && store_set_pem(r->cert, g_cfg.key);
	if (ok) {
		store_apply_kv("token", r->token);
		store_apply_kv("iotHost", r->endpoint);
		if (r->thing[0]) {
			store_apply_kv("thingName", r->thing);
		}
		if (r->kvs_region[0]) {
			store_apply_kv("kvsRegion", r->kvs_region);
		}
		if (r->kvs_channel[0]) {
			store_apply_kv("kvsChannel", r->kvs_channel);
		}
		if (r->cred_host[0] && r->role_alias[0]) {
			store_apply_kv("credHost", r->cred_host);
			store_apply_kv("roleAlias", r->role_alias);
		}
		if (r->claim_url[0]) {
			strlcpy(g_cfg.claim_url, r->claim_url, sizeof(g_cfg.claim_url));
			store_set_str("claim_url", r->claim_url);
		}
		store_apply_kv("claimed", r->claimed == 1 ? "1" : "0");
	} else if (parsed) {
		snprintf(err, errn, "certificate or key not storable");
	}
	free(r);
	return ok;
}

#define TAKEN_FIRST_S 3600
#define TAKEN_MAX_S (6 * 3600)

/* After a 409 the next question waits (NVS "reg_retry", epoch s, so a battery camera asks at the first wake after it);
 * the camera stays free to sleep meanwhile */
static void wait_for_retry(void)
{
	for (;;) {
		int at = store_get_flag_default("reg_retry", 0);
		time_t now = time(NULL);
		if (!strcmp(g_reg_error, "") || !clock_synced() || at <= 0 || now >= at) {
			return;
		}
		pending = false;
		int left = (int)(at - now);
		ESP_LOGW(TAG, "registration was refused (serial_taken): asking again in %d min", (left + 59) / 60);
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)(left > 3600 ? 3600 : left) * 1000)); /* reg start wakes it */
		pending = true;
	}
}

static void register_task(void *arg)
{
	int backoff_s = 30;
	for (;;) {
		sched_keep_awake(300);
		net_ensure();
		if (!net_wait_up(120)) {
			ESP_LOGW(TAG, "no network (or no clock) yet");
			continue;
		}
		if (!strcmp(g_reg_error, "serial_taken")) {
			wait_for_retry();
		}
		char *csr = register_csr_pem();
		char *body = csr ? reg_body(g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL, fw_version(), csr) : NULL;
		free(csr);
		char *resp = calloc(1, 6144);
		int status = body && resp ? flybot_request("/api/device/register", HTTP_METHOD_POST, body, resp, 6144, 30000) : -1;
		free(body);
		char err[40] = "";
		if ((status == 200 || status == 201) && register_apply_response(resp, err, sizeof(err))) {
			free(resp);
			ESP_LOGI(TAG, "registered: thing %s, IoT %s, KVS %s, %s, claim at %s", g_cfg.thing_name, g_cfg.mqtt_host,
					 g_cfg.kvs_channel, g_cfg.claimed ? "claimed" : "unclaimed", g_cfg.claim_url);
			g_reg_error[0] = 0;
			store_set_str("reg_err", "");
			store_set_flag("reg_retry", 0);
			store_set_flag("reg_taken_s", 0);
			pending = false;
			proto_fast_retry(180); /* IoT Core may refuse the brand-new certificate for a few seconds */
			proto_start();         /* the certificate is there now: connect to IoT Core */
			break;
		}
		free(resp);
		if (status == 409) {
			int taken_s = store_get_flag_default("reg_taken_s", 0);
			taken_s = taken_s <= 0 ? TAKEN_FIRST_S : (taken_s * 2 > TAKEN_MAX_S ? TAKEN_MAX_S : taken_s * 2);
			ESP_LOGE(TAG, "409 serial_taken: %s is registered elsewhere; reset or add the camera in FlyBot (asking again in %d min)",
					 g_cfg.serial, taken_s / 60);
			strlcpy(g_reg_error, "serial_taken", sizeof(g_reg_error));
			store_set_str("reg_err", g_reg_error);
			store_set_flag("reg_taken_s", taken_s);
			store_set_flag("reg_retry", (int)(time(NULL) + taken_s));
			led_set(LED_ERROR);
			continue; /* wait_for_retry */
		}
		if (status == 200 || status == 201) {
			snprintf(g_reg_error, sizeof(g_reg_error), "bad answer: %s", err);
		} else if (status < 0) {
			snprintf(g_reg_error, sizeof(g_reg_error), "no answer from FlyBot");
		} else {
			snprintf(g_reg_error, sizeof(g_reg_error), "http %d", status);
		}
		int jitter = (int)(esp_random() % 41) - 20;
		int wait = backoff_s * (100 + jitter) / 100;
		ESP_LOGW(TAG, "register failed (%s): retry in %d s", g_reg_error, wait);
		vTaskDelay(pdMS_TO_TICKS(wait * 1000));
		backoff_s = backoff_s * 2 > 3600 ? 3600 : backoff_s * 2;
	}
	task = NULL;
	vTaskDelete(NULL);
}

void register_start(void)
{
	if (task) {
		xTaskNotifyGive(task); /* waiting after a refusal: ask now */
		return;
	}
	if (g_cfg.cert && g_cfg.key) {
		return;
	}
	if (!g_cfg.api_base[0]) {
		ESP_LOGW(TAG, "no FlyBot URL: cannot self-register");
		return;
	}
	pending = true;
	led_set(LED_CONNECTING);
	ESP_LOGI(TAG, "no certificate: self-registering at %s/api/device/register as %s", g_cfg.api_base, g_cfg.serial);
	xTaskCreate(register_task, "register", 8192, NULL, 4, &task);
}

/* ------------------------------------------------------------------ still registered? */

#define IDENTITY_CHECK_EVERY_S 600
#define REREGISTER_EVERY_S 3600
static volatile bool checking;
static int64_t last_check_us = -(int64_t)IDENTITY_CHECK_EVERY_S * 1000000;
static char check_why[48];

static void identity_task(void *arg)
{
	char *resp = calloc(1, 512);
	int status = -1;
	if (resp && net_wait_up(60)) {
		status = flybot_request("/api/device/ota", HTTP_METHOD_GET, NULL, resp, 512, 20000);
	}
	free(resp);
	if (status == 401) {
		time_t now = time(NULL);
		int last = store_get_flag_default("rereg_at", 0);
		if (clock_synced() && last > 0 && now - last < REREGISTER_EVERY_S) {
			ESP_LOGE(TAG, "FlyBot no longer knows this camera (%s), but it registered again %d s ago: waiting", check_why,
					 (int)(now - last));
		} else {
			ESP_LOGW(TAG, "FlyBot no longer knows this camera (%s, device API 401): registering again", check_why);
			events_note("register", "removed in FlyBot: registering again");
			if (clock_synced()) {
				store_set_flag("rereg_at", (int)now);
			}
			if (store_drop_registration()) {
				vTaskDelay(pdMS_TO_TICKS(500));
				remember_reboot_reason("reregister");
				esp_restart();
			}
		}
	} else if (status > 0) {
		ESP_LOGW(TAG, "%s, but FlyBot still accepts the device token (http %d): identity kept", check_why, status);
	} else {
		ESP_LOGW(TAG, "%s: FlyBot did not answer, identity kept", check_why);
	}
	checking = false;
	vTaskDelete(NULL);
}

void register_check_identity(const char *why)
{
	int64_t now = esp_timer_get_time();
	if (checking || pending || !g_cfg.token[0] || now - last_check_us < (int64_t)IDENTITY_CHECK_EVERY_S * 1000000) {
		return;
	}
	last_check_us = now;
	strlcpy(check_why, why, sizeof(check_why));
	checking = true;
	if (xTaskCreate(identity_task, "identity", 6144, NULL, 3, NULL) != pdPASS) {
		checking = false;
	}
}
