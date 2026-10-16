/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Configuration (docs/dev/CONFIG.md). Three layers, the later wins:
 *
 *   1. build-time defaults   firmware/defaults.example.json (public demo values) or defaults.local.json (git-ignored,
 *                            your bench Wi-Fi), compiled into camos_defaults.h by main/CMakeLists.txt
 *   2. factory partition     "fctry" (read-only NVS written by tools/factory.*): per-device certificate, key, thing
 *   3. runtime NVS           "nvs": the portal, the console, pairing / registration
 *
 * Factory reset erases layer 3 except the identity (serial, verify code, certificate, key, device token, thing, IoT
 * endpoint, KVS channel, claim state: CONTRACTS 2a, protocol 9 "keeps certificates"), so the camera returns to the
 * build defaults and stays registered. Nothing secret is compiled in: certificates and keys are per device.
 *
 * NVS keys (namespace "camos"): serial vcode wifi_ssid wifi_pass apn sim_pin api_base token prov_token mqtt_host
 * thing kvs_chan kvs_region role_alias cred_host tz ap_pass claim_url wifi_err reg_err (strings); interval power_mode
 * led cellular gps claimed wifi_new portal boots modem_absent (i32); cert key (PEM); settings gps_fix (blobs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "camos.h"
#include "camos_defaults.h"

static const char *TAG = "store";
cfg_t g_cfg;
char g_wifi_error[33]; /* SSID the camera could not join after the portal (shown there), "" none */
char g_reg_error[64];  /* registration error (409 serial_taken ...), "" none */
char g_public_ip[48];  /* publicIp from /api/device/time */
#define FCTRY_PART "fctry"

/* ------------------------------------------------------------------ NVS primitives */

static bool get_str_h(nvs_handle_t h, const char *key, char *out, size_t n)
{
	size_t len = n;
	return nvs_get_str(h, key, out, &len) == ESP_OK;
}

static char *get_pem(nvs_handle_t h, const char *key)
{
	size_t len = 0;
	if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len < 32) {
		return NULL;
	}
	char *s = malloc(len);
	if (s && nvs_get_str(h, key, s, &len) != ESP_OK) {
		free(s);
		return NULL;
	}
	return s;
}

static void put_str(const char *key, const char *value)
{
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READWRITE, &h) == ESP_OK) {
		if (value && value[0]) {
			nvs_set_str(h, key, value);
		} else {
			nvs_erase_key(h, key);
		}
		nvs_commit(h);
		nvs_close(h);
	}
}

void store_set_str(const char *key, const char *value) { put_str(key, value); }

/* Forgot the web console password: the setup button clears it (the next visit activates again) */
void store_clear_admin(void)
{
	put_str("admin_hash", "");
	put_str("admin_salt", "");
	ESP_LOGW(TAG, "web console admin password cleared");
}

void store_set_flag(const char *name, int value)
{
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READWRITE, &h) == ESP_OK) {
		nvs_set_i32(h, name, value);
		nvs_commit(h);
		nvs_close(h);
	}
}

int store_get_flag(const char *name)
{
	return store_get_flag_default(name, 0);
}

int store_get_flag_default(const char *name, int dflt)
{
	nvs_handle_t h;
	int32_t v = dflt;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		if (nvs_get_i32(h, name, &v) != ESP_OK) {
			v = dflt;
		}
		nvs_close(h);
	}
	return v;
}

bool store_get_blob(const char *name, void *out, size_t len)
{
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) != ESP_OK) {
		return false;
	}
	size_t got = len;
	bool ok = nvs_get_blob(h, name, out, &got) == ESP_OK && got == len;
	nvs_close(h);
	return ok;
}

void store_set_blob(const char *name, const void *data, size_t len)
{
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READWRITE, &h) == ESP_OK) {
		nvs_set_blob(h, name, data, len);
		nvs_commit(h);
		nvs_close(h);
	}
}

static void trim_slash(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && s[n - 1] == '/') {
		s[--n] = 0;
	}
}

/* CONTRACTS 2a.1: 6 characters, Crockford base32 from the hardware RNG, made once */
static void make_verify_code(char out[12])
{
	static const char alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
	for (int i = 0; i < 6; i++) {
		out[i] = alphabet[esp_random() & 31];
	}
	out[6] = 0;
}

/* ------------------------------------------------------------------ the settings table */

typedef struct {
	const char *key;   /* portal JSON / console name */
	const char *nvs;
	char *dst;
	size_t n;
	bool secret;
	const char *dflt;  /* build-time default, "" none */
	bool identity;     /* kept by factory reset */
} str_field_t;

static const str_field_t fields[] = {
	{ "ssid", "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), false, CAMOS_DEFAULT_SSID, false },
	{ "password", "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), true, CAMOS_DEFAULT_PASSWORD, false },
	{ "apn", "apn", g_cfg.apn, sizeof(g_cfg.apn), false, CAMOS_DEFAULT_APN, false },
	{ "simPin", "sim_pin", g_cfg.sim_pin, sizeof(g_cfg.sim_pin), true, "", false },
	{ "apiBase", "api_base", g_cfg.api_base, sizeof(g_cfg.api_base), false, CAMOS_DEFAULT_API_BASE, false },
	{ "token", "token", g_cfg.token, sizeof(g_cfg.token), true, "", true },
	{ "provisionToken", "prov_token", g_cfg.prov_token, sizeof(g_cfg.prov_token), true, "", false },
	{ "iotHost", "mqtt_host", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), false, CAMOS_DEFAULT_IOT_HOST, true },
	{ "thingName", "thing", g_cfg.thing_name, sizeof(g_cfg.thing_name), false, "", true },
	{ "kvsChannel", "kvs_chan", g_cfg.kvs_channel, sizeof(g_cfg.kvs_channel), false, "", true },
	{ "kvsRegion", "kvs_region", g_cfg.kvs_region, sizeof(g_cfg.kvs_region), false, CAMOS_DEFAULT_KVS_REGION, true },
	{ "roleAlias", "role_alias", g_cfg.role_alias, sizeof(g_cfg.role_alias), false, "", true },
	{ "credHost", "cred_host", g_cfg.cred_host, sizeof(g_cfg.cred_host), false, "", true },
	{ "claimUrl", "claim_url", g_cfg.claim_url, sizeof(g_cfg.claim_url), false, "", true },
	{ "timezone", "tz", g_cfg.tz, sizeof(g_cfg.tz), false, CAMOS_DEFAULT_TIMEZONE, false },
	{ "apPassword", "ap_pass", g_cfg.ap_pass, sizeof(g_cfg.ap_pass), true, "", false },
	{ "verifyCode", "vcode", g_cfg.verify_code, sizeof(g_cfg.verify_code), false, "", true },
	{ "uiLang", "ui_lang", g_cfg.ui_lang, sizeof(g_cfg.ui_lang), false, "en", false },
	{ "motionRoi", "mo_roi", g_cfg.motion_roi, sizeof(g_cfg.motion_roi), false, "0,0,100,100", false },
	{ "staticIp", "st_ip", g_cfg.static_ip, sizeof(g_cfg.static_ip), false, "", false },
	{ "staticMask", "st_mask", g_cfg.static_mask, sizeof(g_cfg.static_mask), false, "255.255.255.0", false },
	{ "staticGateway", "st_gw", g_cfg.static_gw, sizeof(g_cfg.static_gw), false, "", false },
	{ "staticDns", "st_dns", g_cfg.static_dns, sizeof(g_cfg.static_dns), false, "", false },
};
#define N_FIELDS (sizeof(fields) / sizeof(fields[0]))

typedef struct {
	const char *key, *nvs;
	int *dst;
	int dflt, lo, hi;
	bool identity;
} int_field_t;

static const int_field_t ints[] = {
	{ "intervalMin", "interval", &g_cfg.interval_min, CAMOS_DEFAULT_INTERVAL_MIN, 0, 1440, false },
	{ "powerMode", "power_mode", &g_cfg.power_mode, CAMOS_DEFAULT_POWER_MODE, 0, 2, false },
	{ "cellular", "cellular", &g_cfg.cellular, CAMOS_DEFAULT_CELLULAR, 0, 1, false },
	{ "gps", "gps", &g_cfg.gps, CAMOS_DEFAULT_GPS, 0, 1, false },
	{ "ledEnabled", "led", &g_cfg.led, CAMOS_DEFAULT_LED, 0, 1, false },
	{ "batteryMonitor", "batt_mon", &g_cfg.batt_mon, CAMOS_DEFAULT_BATTERY_MONITOR, 0, 1, false },
	{ "claimed", "claimed", &g_cfg.claimed, 0, 0, 1, true }, /* factory image and owner pairing set 1 */
	{ "sdSave", "sd_save", &g_cfg.sd_save, 1, 0, 1, false },
	{ "schedFrom", "sched_from", &g_cfg.sched_from, 0, 0, 1439, false },
	{ "schedTo", "sched_to", &g_cfg.sched_to, 1439, 0, 1439, false },
	{ "motion", "motion", &g_cfg.motion, 0, 0, 1, false },
	{ "motionSens", "mo_sens", &g_cfg.motion_sens, 50, 1, 100, false },
	{ "motionCooldown", "mo_cool", &g_cfg.motion_cool, 60, 5, 3600, false },
	{ "motionUpload", "mo_upload", &g_cfg.motion_upload, 0, 0, 1, false },
	{ "motionSd", "mo_sd", &g_cfg.motion_sd, 1, 0, 1, false },
	{ "ipMode", "ip_mode", &g_cfg.ip_mode, 0, 0, 1, false },
};
#define N_INTS (sizeof(ints) / sizeof(ints[0]))

/* where each value comes from: 'd' build default, 'f' factory partition, 'n' runtime NVS, 0 none */
static char src_str[N_FIELDS], src_int[N_INTS];

static void load_layer(nvs_handle_t h, char tag)
{
	for (size_t i = 0; i < N_FIELDS; i++) {
		char buf[160];
		if (get_str_h(h, fields[i].nvs, buf, sizeof(buf)) && strlen(buf) < fields[i].n) {
			strcpy(fields[i].dst, buf);
			src_str[i] = tag;
		}
	}
	for (size_t i = 0; i < N_INTS; i++) {
		int32_t v;
		if (nvs_get_i32(h, ints[i].nvs, &v) == ESP_OK && v >= ints[i].lo && v <= ints[i].hi) {
			*ints[i].dst = v;
			src_int[i] = tag;
		}
	}
	get_str_h(h, "serial", g_cfg.serial, sizeof(g_cfg.serial));
	char *c = get_pem(h, "cert"), *k = get_pem(h, "key");
	if (c) {
		free(g_cfg.cert);
		g_cfg.cert = c;
	}
	if (k) {
		free(g_cfg.key);
		g_cfg.key = k;
	}
}

void store_load(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	memset(src_str, 0, sizeof(src_str));
	memset(src_int, 0, sizeof(src_int));
	for (size_t i = 0; i < N_FIELDS; i++) {
		strlcpy(fields[i].dst, fields[i].dflt, fields[i].n);
		src_str[i] = fields[i].dflt[0] ? 'd' : 0;
	}
	for (size_t i = 0; i < N_INTS; i++) {
		*ints[i].dst = ints[i].dflt;
		src_int[i] = 'd';
	}
	/* layer 2: the factory partition, when tools/factory.* wrote one */
	nvs_handle_t h;
	bool factory = false;
	if (nvs_flash_init_partition(FCTRY_PART) == ESP_OK &&
		nvs_open_from_partition(FCTRY_PART, CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		load_layer(h, 'f');
		nvs_close(h);
		factory = true;
	}
	/* layer 3: runtime NVS */
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		load_layer(h, 'n');
		get_str_h(h, "wifi_err", g_wifi_error, sizeof(g_wifi_error));
		get_str_h(h, "reg_err", g_reg_error, sizeof(g_reg_error));
		nvs_close(h);
	}
	if (!g_cfg.serial[0]) {
		/* Same shape as the AMB82 serials (AIC + 12 hex): ESP + MAC, 15 characters */
		uint8_t mac[6];
		esp_read_mac(mac, ESP_MAC_WIFI_STA);
		snprintf(g_cfg.serial, sizeof(g_cfg.serial), "ESP%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
				 mac[4], mac[5]);
	}
	if (!g_cfg.verify_code[0]) {
		make_verify_code(g_cfg.verify_code);
		put_str("vcode", g_cfg.verify_code);
	}
	trim_slash(g_cfg.api_base);
	ESP_LOGI(TAG, "defaults from %s%s", CAMOS_DEFAULTS_FILE, factory ? ", factory partition present" : "");
	ESP_LOGI(TAG, "serial %s, verify code %s, wifi %s, 4G %s (apn %s), api %s, iot %s, cert %s, key %s, token %s, thing %s",
			 g_cfg.serial, g_cfg.verify_code, g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(none)", g_cfg.cellular ? "on" : "off",
			 g_cfg.apn[0] ? g_cfg.apn : "auto", g_cfg.api_base[0] ? g_cfg.api_base : "(none)",
			 g_cfg.mqtt_host[0] ? g_cfg.mqtt_host : "(none)", g_cfg.cert ? "yes" : "NO", g_cfg.key ? "yes" : "NO",
			 g_cfg.token[0] ? "yes" : "no", g_cfg.thing_name[0] ? g_cfg.thing_name : "(none)");
}

void store_set_wifi(const char *ssid, const char *pass)
{
	store_apply_kv("ssid", ssid);
	store_apply_kv("password", pass ? pass : "");
}

static bool pem_ok(const char *s, const char *what)
{
	return s && strstr(s, "-----BEGIN ") && strstr(s, what) && strstr(s, "-----END ") && strlen(s) < 6000;
}

bool store_set_pem(const char *cert, const char *key)
{
	/* "CERTIFICATE" and "PRIVATE KEY" (EC or PKCS#8); esp-tls / mbedtls parse the rest */
	if (!pem_ok(cert, "CERTIFICATE") || !pem_ok(key, "PRIVATE KEY")) {
		return false;
	}
	put_str("cert", cert);
	put_str("key", key);
	free(g_cfg.cert);
	free(g_cfg.key);
	g_cfg.cert = strdup(cert);
	g_cfg.key = strdup(key);
	return g_cfg.cert && g_cfg.key;
}

bool store_set_key(const char *key_pem)
{
	if (!pem_ok(key_pem, "PRIVATE KEY")) {
		return false;
	}
	put_str("key", key_pem);
	free(g_cfg.key);
	g_cfg.key = strdup(key_pem);
	return g_cfg.key != NULL;
}

void store_erase_all(void)
{
	/* keep the identity (serial, verify code, certificate, key, token, thing ...), drop everything else */
	char *keep_cert = g_cfg.cert ? strdup(g_cfg.cert) : NULL, *keep_key = g_cfg.key ? strdup(g_cfg.key) : NULL;
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READWRITE, &h) == ESP_OK) {
		nvs_erase_all(h);
		nvs_set_str(h, "serial", g_cfg.serial);
		for (size_t i = 0; i < N_FIELDS; i++) {
			if (fields[i].identity && src_str[i] == 'n' && fields[i].dst[0]) {
				nvs_set_str(h, fields[i].nvs, fields[i].dst);
			}
		}
		for (size_t i = 0; i < N_INTS; i++) {
			if (ints[i].identity && src_int[i] == 'n') {
				nvs_set_i32(h, ints[i].nvs, *ints[i].dst);
			}
		}
		if (keep_cert) {
			nvs_set_str(h, "cert", keep_cert);
		}
		if (keep_key) {
			nvs_set_str(h, "key", keep_key);
		}
		nvs_commit(h);
		nvs_close(h);
	}
	free(keep_cert);
	free(keep_key);
	ESP_LOGW(TAG, "factory reset: settings back to the build defaults, identity kept");
}

bool store_paired(void)
{
	return g_cfg.token[0] && g_cfg.mqtt_host[0];
}

/* ------------------------------------------------------------------ portal / console keys */

/* "x,y,w,h" in percent, inside 0..100, w and h at least 5 */
bool roi_parse(const char *s, int out[4])
{
	int v[4];
	char tail;
	if (!s || sscanf(s, "%d,%d,%d,%d%c", &v[0], &v[1], &v[2], &v[3], &tail) != 4) {
		return false;
	}
	if (v[0] < 0 || v[1] < 0 || v[2] < 5 || v[3] < 5 || v[0] + v[2] > 100 || v[1] + v[3] > 100) {
		return false;
	}
	if (out) {
		memcpy(out, v, sizeof(v));
	}
	return true;
}

static bool ip4_ok(const char *s)
{
	int a, b, c, d;
	char tail;
	return sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 && a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 &&
		   c <= 255 && d >= 0 && d <= 255;
}

bool store_apply_kv(const char *key, const char *value)
{
	if (!key || !value) {
		return false;
	}
	if (!strcmp(key, "verifyCode") || !strcmp(key, "claimUrl")) {
		return false; /* made on the device / given by registration, never typed */
	}
	for (size_t i = 0; i < N_FIELDS; i++) {
		const str_field_t *f = &fields[i];
		if (strcmp(f->key, key) != 0) {
			continue;
		}
		if (strlen(value) >= f->n) {
			return false;
		}
		if (!strcmp(key, "timezone") && value[0] && !tz_lookup(value)) {
			return false; /* unknown zone: add it to tz.c or give a POSIX string */
		}
		if (!strcmp(key, "apPassword") && value[0] && strlen(value) < 8) {
			return false;
		}
		if (!strcmp(key, "uiLang") && value[0] && strcmp(value, "en") && strcmp(value, "vi")) {
			return false;
		}
		if (!strcmp(key, "motionRoi") && value[0] && !roi_parse(value, NULL)) {
			return false;
		}
		if (!strncmp(key, "static", 6) && value[0] && !ip4_ok(value)) {
			return false;
		}
		bool changed = strcmp(f->dst, value) != 0;
		/* an empty value means "back to the default" */
		strlcpy(f->dst, value[0] ? value : f->dflt, f->n);
		if (!strcmp(key, "apiBase")) {
			trim_slash(f->dst);
		}
		put_str(f->nvs, value[0] ? f->dst : "");
		src_str[i] = value[0] ? 'n' : (f->dflt[0] ? 'd' : 0);
		if (!strcmp(key, "motionRoi")) {
			motion_config_changed();
		}
		if ((!strcmp(key, "ssid") || !strcmp(key, "password")) && (changed || g_wifi_error[0])) {
			/* a new network: the next boot falls back to the portal if it cannot join (wifi.c) */
			store_set_flag("wifi_new", 1);
			put_str("wifi_err", "");
			g_wifi_error[0] = 0;
		}
		if (!strcmp(key, "apn") || !strcmp(key, "simPin")) {
			store_set_flag("modem_absent", 0); /* someone is setting 4G up: probe the modem again */
		}
		return true;
	}
	for (size_t i = 0; i < N_INTS; i++) {
		const int_field_t *f = &ints[i];
		if (strcmp(f->key, key) != 0) {
			continue;
		}
		int v;
		if (!strcmp(key, "powerMode")) {
			power_mode_t m;
			if (!power_mode_parse(value, &m)) {
				return false;
			}
			v = m;
		} else if (!strcmp(value, "true") || !strcmp(value, "on")) {
			v = 1;
		} else if (!strcmp(value, "false") || !strcmp(value, "off")) {
			v = 0;
		} else {
			char *end;
			long l = strtol(value, &end, 10);
			if (!value[0] || *end || l < f->lo || l > f->hi) {
				return false;
			}
			v = (int)l;
		}
		*f->dst = v;
		store_set_flag(f->nvs, v);
		src_int[i] = 'n';
		if (!strcmp(key, "ledEnabled")) {
			led_enable(v != 0);
		}
		if (!strcmp(key, "cellular")) {
			store_set_flag("modem_absent", 0);
		}
		if (!strcmp(key, "gps") && v) {
			store_set_flag("gps_absent", 0); /* switched on again: probe the module */
		}
		if (!strncmp(key, "motion", 6)) {
			motion_config_changed();
		}
		if (!strncmp(key, "sched", 5)) {
			sched_changed();
		}
		return true;
	}
	return false;
}

void store_config_json(cJSON *o, bool secrets)
{
	cJSON_AddStringToObject(o, "serial", g_cfg.serial);
	cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "fw", fw_version());
	cJSON *dflt = cJSON_CreateArray(); /* keys whose value is the build default (the portal shows a badge) */
	for (size_t i = 0; i < N_FIELDS; i++) {
		const str_field_t *f = &fields[i];
		if (f->secret && !secrets) {
			cJSON_AddBoolToObject(o, f->key, f->dst[0] != 0); /* set or not, never the value */
		} else {
			cJSON_AddStringToObject(o, f->key, f->dst);
		}
		if (src_str[i] == 'd') {
			cJSON_AddItemToArray(dflt, cJSON_CreateString(f->key));
		}
	}
	for (size_t i = 0; i < N_INTS; i++) {
		if (!strcmp(ints[i].key, "powerMode")) {
			cJSON_AddStringToObject(o, "powerMode", power_mode_name(g_cfg.power_mode));
		} else if (ints[i].hi == 1) {
			cJSON_AddBoolToObject(o, ints[i].key, *ints[i].dst != 0);
		} else {
			cJSON_AddNumberToObject(o, ints[i].key, *ints[i].dst);
		}
		if (src_int[i] == 'd') {
			cJSON_AddItemToArray(dflt, cJSON_CreateString(ints[i].key));
		}
	}
	cJSON_AddItemToObject(o, "defaults", dflt);
	cJSON_AddStringToObject(o, "defaultsFile", CAMOS_DEFAULTS_FILE);
	cJSON_AddBoolToObject(o, "cert", g_cfg.cert != NULL);
	cJSON_AddBoolToObject(o, "key", g_cfg.key != NULL);
	cJSON_AddBoolToObject(o, "paired", store_paired());
	cJSON_AddBoolToObject(o, "registered", g_cfg.cert != NULL && g_cfg.token[0]);
	cJSON_AddBoolToObject(o, "hasModem", CAMOS_MODEM);
	cJSON_AddBoolToObject(o, "hasGps", CAMOS_GPS);
	cJSON_AddBoolToObject(o, "hasLive", CAMOS_LIVE);
	cJSON_AddStringToObject(o, "apSsid", portal_ssid());
	cJSON_AddStringToObject(o, "wifiError", g_wifi_error);
	cJSON_AddStringToObject(o, "regError", g_reg_error);
	char qr[96];
	snprintf(qr, sizeof(qr), "CAMOS:1;SN:%s;VC:%s;M:%s;;", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "labelQr", qr); /* protocol 12.1 */
	char url[200];
	store_claim_url(url, sizeof(url));
	cJSON_DeleteItemFromObject(o, "claimUrl"); /* the effective URL, not only a stored one */
	cJSON_AddStringToObject(o, "claimUrl", url);
}

void store_claim_url(char *out, size_t n)
{
	if (g_cfg.claim_url[0]) {
		strlcpy(out, g_cfg.claim_url, n);
	} else {
		snprintf(out, n, "%s/claim?sn=%s", g_cfg.api_base[0] ? g_cfg.api_base : CAMOS_DEFAULT_API_BASE, g_cfg.serial);
	}
}

bool store_overridden(const char *key)
{
	for (size_t i = 0; i < N_FIELDS; i++) {
		if (!strcmp(fields[i].key, key)) {
			return src_str[i] == 'n';
		}
	}
	for (size_t i = 0; i < N_INTS; i++) {
		if (!strcmp(ints[i].key, key)) {
			return src_int[i] == 'n';
		}
	}
	return false;
}

/* The timezone /api/device/time gave (CONTRACTS 2a.6), kept across boots and factory reset is fine to lose */
void nvs_tz_read(char *iana, size_t in, char *posix, size_t pn, char *src, size_t sn)
{
	iana[0] = posix[0] = src[0] = 0;
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		get_str_h(h, "tz_auto", iana, in);
		get_str_h(h, "tz_posix", posix, pn);
		get_str_h(h, "tz_src", src, sn);
		nvs_close(h);
	}
}

void nvs_tz_write(const char *iana, const char *posix, const char *src)
{
	put_str("tz_auto", iana);
	put_str("tz_posix", posix);
	put_str("tz_src", src);
}
