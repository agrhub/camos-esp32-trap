/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Zero-touch self-registration (flybot-aws docs/CONTRACTS.md 2a.2): a camera with no certificate makes an ECDSA P-256
 * key (hardware RNG; the key is stored in NVS and never leaves it), a CSR with CN = serial, and calls
 * POST {apiBase}/api/device/register. The answer's certificate, device token, thing, KVS channel and claim URL are
 * stored and the MQTT agent starts. 429 / 5xx / no answer: retried with back-off (30 s doubling to 1 h, +-20 %).
 * 409 serial_taken: stop, red LED, the portal and console show it (the owner resets the camera in FlyBot).
 * A pre-provisioned certificate (tools/factory.*, the portal, provision.sh) skips all of this.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
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
			pending = false;
			proto_start(); /* the certificate is there now: connect to IoT Core */
			break;
		}
		free(resp);
		if (status == 409) {
			ESP_LOGE(TAG, "409 serial_taken: %s is registered with another verify code; reset the camera in FlyBot",
					 g_cfg.serial);
			strlcpy(g_reg_error, "serial_taken", sizeof(g_reg_error));
			store_set_str("reg_err", g_reg_error);
			led_set(LED_ERROR);
			pending = false;
			break;
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
	if (task || (g_cfg.cert && g_cfg.key)) {
		return;
	}
	if (!g_cfg.api_base[0]) {
		ESP_LOGW(TAG, "no FlyBot URL: cannot self-register");
		return;
	}
	if (!strcmp(g_reg_error, "serial_taken")) {
		ESP_LOGE(TAG, "registration refused earlier (serial_taken): not retrying; reset the camera in FlyBot first");
		led_set(LED_ERROR);
		return;
	}
	pending = true;
	led_set(LED_CONNECTING);
	ESP_LOGI(TAG, "no certificate: self-registering at %s/api/device/register as %s (verify code %s)", g_cfg.api_base,
			 g_cfg.serial, g_cfg.verify_code);
	xTaskCreate(register_task, "register", 8192, NULL, 4, &task);
}
