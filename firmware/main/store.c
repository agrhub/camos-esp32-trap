/*
 * NVS (namespace "camos"): everything the portal, the console and tools/provision.sh write, plus small flags and the
 * persisted shadow. Nothing but the board pin preset and the Amazon Root CA is fixed at build time.
 *
 *   serial      ESP + 12 hex of the MAC (generated when absent)     vcode      label verify code (generated)
 *   wifi_ssid   wifi_pass                                            apn        sim_pin
 *   api_base    FlyBot base URL                                      token      device token (pairing)
 *   prov_token  provisioning token (consumed by pairing)             mqtt_host  AWS IoT endpoint
 *   thing       kvs_chan  kvs_region  role_alias  cred_host          tz         IANA zone
 *   interval    capture interval (min, i32)                          power_mode 0 auto 1 battery 2 powered (i32)
 *   cert  key   PEM                                                  ap_pass    portal Wi-Fi password
 *   settings    persisted shadow (blob)   portal (i32 flag)   boots (i32)   gps_fix (blob)
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

static const char *TAG = "store";
cfg_t g_cfg;

static void get_str(nvs_handle_t h, const char *key, char *out, size_t n)
{
	size_t len = n;
	if (nvs_get_str(h, key, out, &len) != ESP_OK) {
		out[0] = 0;
	}
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

static void trim_slash(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && s[n - 1] == '/') {
		s[--n] = 0;
	}
}

/* Protocol 12.1: the label carries SN and VC. These boards have no label, so the firmware makes the code once and
 * shows it on the portal page and the console (tools/label.py prints the QR). 6 characters, no 0/O/1/I. */
static void make_verify_code(char out[12])
{
	static const char alphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
	for (int i = 0; i < 6; i++) {
		out[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
	}
	out[6] = 0;
}

void store_load(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.power_mode = CONFIG_CAMOS_POWER_MODE_DEFAULT;
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		get_str(h, "serial", g_cfg.serial, sizeof(g_cfg.serial));
		get_str(h, "vcode", g_cfg.verify_code, sizeof(g_cfg.verify_code));
		get_str(h, "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid));
		get_str(h, "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass));
		get_str(h, "apn", g_cfg.apn, sizeof(g_cfg.apn));
		get_str(h, "sim_pin", g_cfg.sim_pin, sizeof(g_cfg.sim_pin));
		get_str(h, "api_base", g_cfg.api_base, sizeof(g_cfg.api_base));
		get_str(h, "token", g_cfg.token, sizeof(g_cfg.token));
		get_str(h, "prov_token", g_cfg.prov_token, sizeof(g_cfg.prov_token));
		get_str(h, "mqtt_host", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host));
		get_str(h, "thing", g_cfg.thing_name, sizeof(g_cfg.thing_name));
		get_str(h, "kvs_chan", g_cfg.kvs_channel, sizeof(g_cfg.kvs_channel));
		get_str(h, "kvs_region", g_cfg.kvs_region, sizeof(g_cfg.kvs_region));
		get_str(h, "role_alias", g_cfg.role_alias, sizeof(g_cfg.role_alias));
		get_str(h, "cred_host", g_cfg.cred_host, sizeof(g_cfg.cred_host));
		get_str(h, "tz", g_cfg.tz, sizeof(g_cfg.tz));
		get_str(h, "ap_pass", g_cfg.ap_pass, sizeof(g_cfg.ap_pass));
		int32_t v;
		if (nvs_get_i32(h, "interval", &v) == ESP_OK) {
			g_cfg.interval_min = v;
		}
		if (nvs_get_i32(h, "power_mode", &v) == ESP_OK && v >= 0 && v <= 2) {
			g_cfg.power_mode = v;
		}
		g_cfg.cert = get_pem(h, "cert");
		g_cfg.key = get_pem(h, "key");
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
	ESP_LOGI(TAG, "serial %s, verify code %s, wifi %s, apn %s, api %s, iot %s, cert %s, key %s, token %s, thing %s",
			 g_cfg.serial, g_cfg.verify_code, g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(none)",
			 g_cfg.apn[0] ? g_cfg.apn : "(none)", g_cfg.api_base[0] ? g_cfg.api_base : "(none)",
			 g_cfg.mqtt_host[0] ? g_cfg.mqtt_host : "(none)", g_cfg.cert ? "yes" : "NO", g_cfg.key ? "yes" : "NO",
			 g_cfg.token[0] ? "yes" : "no", g_cfg.thing_name[0] ? g_cfg.thing_name : "(none)");
}

void store_set_wifi(const char *ssid, const char *pass)
{
	strlcpy(g_cfg.wifi_ssid, ssid, sizeof(g_cfg.wifi_ssid));
	strlcpy(g_cfg.wifi_pass, pass ? pass : "", sizeof(g_cfg.wifi_pass));
	put_str("wifi_ssid", g_cfg.wifi_ssid);
	put_str("wifi_pass", g_cfg.wifi_pass);
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
	nvs_handle_t h;
	int32_t v = 0;
	if (nvs_open(CAMOS_NS, NVS_READONLY, &h) == ESP_OK) {
		nvs_get_i32(h, name, &v);
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

void store_erase_all(void)
{
	char serial[24], vcode[12];
	strlcpy(serial, g_cfg.serial, sizeof(serial));
	strlcpy(vcode, g_cfg.verify_code, sizeof(vcode));
	nvs_handle_t h;
	if (nvs_open(CAMOS_NS, NVS_READWRITE, &h) == ESP_OK) {
		nvs_erase_all(h);
		nvs_set_str(h, "serial", serial);
		nvs_set_str(h, "vcode", vcode);
		nvs_commit(h);
		nvs_close(h);
	}
}

bool store_paired(void)
{
	return g_cfg.token[0] && g_cfg.mqtt_host[0];
}

/* ------------------------------------------------------------------ portal / console keys */

typedef struct {
	const char *key;   /* portal JSON / console name */
	const char *nvs;
	char *dst;
	size_t n;
	bool secret;
} str_field_t;

static const str_field_t fields[] = {
	{ "ssid", "wifi_ssid", g_cfg.wifi_ssid, sizeof(g_cfg.wifi_ssid), false },
	{ "password", "wifi_pass", g_cfg.wifi_pass, sizeof(g_cfg.wifi_pass), true },
	{ "apn", "apn", g_cfg.apn, sizeof(g_cfg.apn), false },
	{ "simPin", "sim_pin", g_cfg.sim_pin, sizeof(g_cfg.sim_pin), true },
	{ "apiBase", "api_base", g_cfg.api_base, sizeof(g_cfg.api_base), false },
	{ "token", "token", g_cfg.token, sizeof(g_cfg.token), true },
	{ "provisionToken", "prov_token", g_cfg.prov_token, sizeof(g_cfg.prov_token), true },
	{ "iotHost", "mqtt_host", g_cfg.mqtt_host, sizeof(g_cfg.mqtt_host), false },
	{ "thingName", "thing", g_cfg.thing_name, sizeof(g_cfg.thing_name), false },
	{ "kvsChannel", "kvs_chan", g_cfg.kvs_channel, sizeof(g_cfg.kvs_channel), false },
	{ "kvsRegion", "kvs_region", g_cfg.kvs_region, sizeof(g_cfg.kvs_region), false },
	{ "roleAlias", "role_alias", g_cfg.role_alias, sizeof(g_cfg.role_alias), false },
	{ "credHost", "cred_host", g_cfg.cred_host, sizeof(g_cfg.cred_host), false },
	{ "timezone", "tz", g_cfg.tz, sizeof(g_cfg.tz), false },
	{ "apPassword", "ap_pass", g_cfg.ap_pass, sizeof(g_cfg.ap_pass), true },
	{ "verifyCode", "vcode", g_cfg.verify_code, sizeof(g_cfg.verify_code), false },
};

bool store_apply_kv(const char *key, const char *value)
{
	if (!key || !value) {
		return false;
	}
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
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
		strlcpy(f->dst, value, f->n);
		if (!strcmp(key, "apiBase")) {
			trim_slash(f->dst);
		}
		put_str(f->nvs, f->dst);
		return true;
	}
	if (!strcmp(key, "intervalMin")) {
		int v = atoi(value);
		if (v < 0 || v > 1440 || (!isdigit((unsigned char)value[0]) && value[0] != '0')) {
			return false;
		}
		g_cfg.interval_min = v;
		store_set_flag("interval", v);
		return true;
	}
	if (!strcmp(key, "powerMode")) {
		power_mode_t m;
		if (!power_mode_parse(value, &m)) {
			return false;
		}
		g_cfg.power_mode = m;
		store_set_flag("power_mode", m);
		return true;
	}
	return false;
}

void store_config_json(cJSON *o, bool secrets)
{
	cJSON_AddStringToObject(o, "serial", g_cfg.serial);
	cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "fw", fw_version());
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		const str_field_t *f = &fields[i];
		if (f->secret && !secrets) {
			cJSON_AddBoolToObject(o, f->key, f->dst[0] != 0); /* set or not, never the value */
		} else {
			cJSON_AddStringToObject(o, f->key, f->dst);
		}
	}
	cJSON_AddNumberToObject(o, "intervalMin", g_cfg.interval_min);
	cJSON_AddStringToObject(o, "powerMode", power_mode_name(g_cfg.power_mode));
	cJSON_AddBoolToObject(o, "cert", g_cfg.cert != NULL);
	cJSON_AddBoolToObject(o, "key", g_cfg.key != NULL);
	cJSON_AddBoolToObject(o, "paired", store_paired());
	cJSON_AddBoolToObject(o, "modem", CAMOS_MODEM);
	cJSON_AddBoolToObject(o, "gps", CAMOS_GPS);
	cJSON_AddBoolToObject(o, "live", CAMOS_LIVE);
	char qr[96];
	snprintf(qr, sizeof(qr), "CAMOS:1;SN:%s;VC:%s;M:%s;;", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL);
	cJSON_AddStringToObject(o, "labelQr", qr); /* protocol 12.1 */
}
