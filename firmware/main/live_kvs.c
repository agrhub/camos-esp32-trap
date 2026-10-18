/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Cloud live view: Kinesis Video Streams WebRTC MASTER on the camera's signaling channel (CONTRACTS section 4).
 *
 * One task ("kvs") owns everything but the encoder:
 *   credentials   AWS IoT credential provider (device certificate + role alias, GET /role-aliases/<alias>/credentials,
 *                 1 h keys), or bench keys typed on the serial console (CAMOS_LIVE_BENCH_CREDS, RAM only)
 *   channel       DescribeSignalingChannel -> ARN, GetSignalingChannelEndpoint (MASTER) -> WSS + HTTPS endpoints
 *   signaling     WebSocket to the presigned WSS URL (SigV4, sigv4.c); SDP_OFFER / ICE_CANDIDATE from a VIEWER in,
 *                 SDP_ANSWER out (the answer carries the camera's candidates)
 *   peer          esp_peer (ICE, DTLS-SRTP, RTP) opened for each viewer with the channel's STUN / TURN servers
 *                 (GetIceServerConfig), closed when the viewer leaves; one viewer at a time, the newest one wins
 * The encoder task ("venc") starts with a viewer's offer and ends with its session: it takes the camera from the
 * scheduler (camera_take), keeps the sensor up in YUV422 and, once the viewer is connected, sends software H.264
 * (esp_h264, openh264 baseline) on the video track. When the viewer opens the "camos" data channel and asks for JPEG
 * (live_dc.h), it sends the sensor's own JPEG frames over that channel instead, in chunks with an ack window.
 *
 * The task joins the channel while a live.start / live.requested window is open, and all the time in powered mode
 * (CAMOS_LIVE_STANDBY: a viewer finds the camera at once); it leaves when neither holds.
 */
#include "sdkconfig.h"
#if CONFIG_CAMOS_LIVE
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_h264_alloc.h"
#include "camos.h"
#include "sigv4.h"
#include "live_dc.h"

static const char *TAG = "kvs";

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
/* esp_peer's prebuilt library is compiled against ESP-IDF 5.5, whose ESP_LOGx macros call esp_log(config, tag,
 * format, ...) with the level in the low 3 bits of config and the same pre-formatted "X (time) tag: " format as
 * 5.4's esp_log_write: route it there */
void esp_log(uint32_t config, const char *tag, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	esp_log_writev((esp_log_level_t)(config & 0x7), tag, format, ap);
	va_end(ap);
}
#endif

#define LIVE_W CONFIG_CAMOS_LIVE_WIDTH
#define LIVE_H CONFIG_CAMOS_LIVE_HEIGHT
#define LIVE_FPS CONFIG_CAMOS_LIVE_FPS
#define SERVICE "kinesisvideo"
#define RESP_MAX 4096
#define MAX_TURN 1          /* esp_peer gathers from one server: a KVS TURN server also answers STUN */
#define ICE_TTL_S 240      /* TURN credentials last 300 s */
#define ANSWER_WAIT_S 30   /* an offer that does not end connected within this is dropped */
#define LIVE_MIN_INTERNAL (16 * 1024) /* JPEG frames wait while less internal RAM is free */

/* ------------------------------------------------------------------ state */

typedef enum { EV_OFFER, EV_CANDIDATE, EV_WS_UP, EV_WS_DOWN, EV_GO_AWAY } ev_kind_t;
typedef struct {
	ev_kind_t kind;
	char sender[72];
	char *data; /* SDP or candidate, heap; NULL for the others */
} ev_t;

static struct {
	TaskHandle_t task;
	QueueHandle_t q;
	/* credentials */
	char akid[32], secret[64];
	char *token;
	int64_t cred_expires; /* epoch s, 0 = unknown (bench keys) */
	bool bench;
	/* channel */
	char arn[192], wss_host[128], https_host[128];
	/* ICE */
	char stun[96], turn_url[MAX_TURN][160], turn_user[MAX_TURN][256], turn_pass[MAX_TURN][128];
	esp_peer_ice_server_cfg_t ice[MAX_TURN];
	int n_ice;
	int64_t ice_at;
	/* signaling */
	esp_websocket_client_handle_t ws;
	volatile bool ws_up;
	char *rx;
	int rx_len, rx_cap;
	/* peer */
	esp_peer_handle_t peer;
	char viewer[72];
	int64_t offer_at;
	volatile bool viewer_up, close_req;
	volatile bool pause_req, paused; /* a capture borrows the camera (live_pause) */
	volatile int64_t last_ack_ms;     /* JPEG: the viewer's last ack (the weak-link fallback) */
	bool viewer_chose;                /* the viewer asked for a mode: no automatic JPEG start */
	int pause_ms;                     /* the last pause, request to the stream running again */
	TaskHandle_t venc;
	volatile bool venc_run, venc_done;
	/* data channel "camos" (live_dc.h): JPEG frames instead of H.264 when the viewer asks */
	volatile bool dc_open, dc_hello, dc_err_pending, req_pending;
	uint16_t dc_stream;
	char dc_err[64];
	live_quality_t req, cur;
	live_dc_flow_t flow;
	uint8_t *dc_buf;           /* one chunk: header + payload */
	int32_t push_ms;           /* time to hand the last JPEG frame to the channel */
	uint32_t dc_drops, dc_waits, frame_bytes;
	/* status */
	char state[16], err[96];
	int64_t retry_at;
	float fps, kbps, enc_ms, cap_ms;
	uint32_t frames, idr, sessions;
	int setup_ms;
} k;

static void set_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(k.err, sizeof(k.err), fmt, ap);
	va_end(ap);
	ESP_LOGW(TAG, "%s", k.err);
}

static bool provider_configured(void)
{
	return g_cfg.cred_host[0] && g_cfg.role_alias[0] && g_cfg.cert && g_cfg.key;
}

bool live_kvs_creds_source(void)
{
	return provider_configured() || (k.bench && k.akid[0] && k.secret[0]);
}

bool live_streaming(void) { return k.viewer_up; }

/* ------------------------------------------------------------------ HTTPS */

typedef struct {
	const char *name, *value;
} hdr_t;

/* Returns the HTTP status (resp holds the body, NUL-terminated) or -1 */
static int https(const char *url, esp_http_client_method_t m, const hdr_t *hdrs, int nh, const char *body, bool mtls,
				 char *resp, size_t respn)
{
	esp_http_client_config_t cfg = {
		.url = url,
		.method = m,
		.timeout_ms = 10000,
		.buffer_size = 2048,
		.buffer_size_tx = 4096, /* the session token is one header */
		.crt_bundle_attach = esp_crt_bundle_attach,
	};
	if (mtls) {
		cfg.client_cert_pem = g_cfg.cert;
		cfg.client_key_pem = g_cfg.key;
	}
	esp_http_client_handle_t h = esp_http_client_init(&cfg);
	if (!h) {
		return -1;
	}
	for (int i = 0; i < nh; i++) {
		esp_http_client_set_header(h, hdrs[i].name, hdrs[i].value);
	}
	int status = -1, blen = body ? (int)strlen(body) : 0;
	if (esp_http_client_open(h, blen) == ESP_OK && (blen == 0 || esp_http_client_write(h, body, blen) == blen) &&
		esp_http_client_fetch_headers(h) >= 0) {
		status = esp_http_client_get_status_code(h);
		int n = 0, r;
		while (n < (int)respn - 1 && (r = esp_http_client_read(h, resp + n, respn - 1 - n)) > 0) {
			n += r;
		}
		resp[n] = 0;
	}
	esp_http_client_close(h);
	esp_http_client_cleanup(h);
	return status;
}

static void amz_now(char out[17])
{
	time_t now = time(NULL);
	struct tm t;
	gmtime_r(&now, &t);
	strftime(out, 17, "%Y%m%dT%H%M%SZ", &t);
}

static sigv4_creds_t creds(void)
{
	return (sigv4_creds_t){ k.akid, k.secret, k.token };
}

/* SigV4-signed JSON POST to a KVS API (service kinesisvideo) */
static int kvs_post(const char *host, const char *path, const char *body, char *resp, size_t respn)
{
	char amz[17], auth[420], url[200];
	amz_now(amz);
	sigv4_creds_t c = creds();
	if (sigv4_auth_header(&c, g_cfg.kvs_region, SERVICE, amz, "POST", host, path, "application/json", body, strlen(body),
						  auth, sizeof(auth))) {
		return -1;
	}
	snprintf(url, sizeof(url), "https://%s%s", host, path);
	hdr_t h[] = { { "Content-Type", "application/json" }, { "X-Amz-Date", amz }, { "Authorization", auth },
				  { "X-Amz-Security-Token", k.token ? k.token : "" } };
	int st = https(url, HTTP_METHOD_POST, h, k.token && k.token[0] ? 4 : 3, body, false, resp, respn);
	if (st != 200) {
		ESP_LOGW(TAG, "POST %s: http %d %.160s", path, st, st > 0 ? resp : "");
	}
	return st;
}

/* ------------------------------------------------------------------ credentials */

static void creds_clear(void)
{
	memset(k.akid, 0, sizeof(k.akid));
	memset(k.secret, 0, sizeof(k.secret));
	if (k.token) {
		memset(k.token, 0, strlen(k.token));
		free(k.token);
		k.token = NULL;
	}
	k.cred_expires = 0;
}

static bool creds_valid(void)
{
	if (!k.akid[0] || !k.secret[0]) {
		return false;
	}
	return k.bench || k.cred_expires == 0 || k.cred_expires - time(NULL) > 300;
}

static time_t parse_iso(const char *s)
{
	struct tm t = { 0 };
	if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) {
		return 0;
	}
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	/* timegm: the clock runs in UTC unless TZ is set; compute the offset of mktime once */
	time_t local = mktime(&t);
	struct tm g;
	gmtime_r(&local, &g);
	g.tm_isdst = 0;
	return local + (local - mktime(&g));
}

/* AWS IoT credential provider: temporary keys for the role behind g_cfg.role_alias, authenticated by the device
 * certificate (the IoT policy must allow iot:AssumeRoleWithCertificate on the role alias) */
static bool creds_fetch(void)
{
	if (k.bench) {
		return k.akid[0] && k.secret[0];
	}
	if (!provider_configured()) {
		set_err("no credentials: credHost / roleAlias not set (pairing gives them)");
		return false;
	}
	char url[256];
	snprintf(url, sizeof(url), "https://%s/role-aliases/%s/credentials", g_cfg.cred_host, g_cfg.role_alias);
	hdr_t h[] = { { "x-amzn-iot-thingname", g_cfg.thing_name[0] ? g_cfg.thing_name : g_cfg.serial } };
	char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
	if (!resp) {
		return false;
	}
	int st = https(url, HTTP_METHOD_GET, h, 1, NULL, true, resp, RESP_MAX);
	bool ok = false;
	if (st == 200) {
		cJSON *j = cJSON_Parse(resp), *c = cJSON_GetObjectItem(j, "credentials");
		const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "accessKeyId"));
		const char *sec = cJSON_GetStringValue(cJSON_GetObjectItem(c, "secretAccessKey"));
		const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionToken"));
		const char *exp = cJSON_GetStringValue(cJSON_GetObjectItem(c, "expiration"));
		if (id && sec && tok && strlen(id) < sizeof(k.akid) && strlen(sec) < sizeof(k.secret)) {
			creds_clear();
			strlcpy(k.akid, id, sizeof(k.akid));
			strlcpy(k.secret, sec, sizeof(k.secret));
			k.token = strdup(tok);
			k.cred_expires = parse_iso(exp);
			ok = k.token != NULL;
			ESP_LOGI(TAG, "credentials from the IoT credential provider, valid %lld s",
					 (long long)(k.cred_expires ? k.cred_expires - time(NULL) : -1));
		}
		cJSON_Delete(j);
		memset(resp, 0, RESP_MAX);
	} else {
		/* 403 "Access Denied" / 404 "Role alias does not exist": the AWS side lacks the role alias (LIVE_VIEW.md) */
		set_err("credential provider: http %d %.48s", st, st > 0 ? resp : "no answer");
	}
	free(resp);
	return ok;
}

#if CONFIG_CAMOS_LIVE_BENCH_CREDS
/* Console `live key id|secret|token|clear [value]`: temporary keys for a bench test without the credential provider.
 * RAM only, never logged or stored; `token` appends (the console line is 256 characters). */
esp_err_t live_kvs_bench_key(const char *field, const char *value)
{
	if (!strcmp(field, "clear")) {
		creds_clear();
		k.bench = false;
		return ESP_OK;
	}
	if (!value) {
		return ESP_ERR_INVALID_ARG;
	}
	k.retry_at = esp_timer_get_time() + 3LL * 1000000; /* the token comes in pieces: no join before the last one */
	if (!k.bench) {
		creds_clear();
		k.bench = true;
	}
	if (!strcmp(field, "id")) {
		strlcpy(k.akid, value, sizeof(k.akid));
	} else if (!strcmp(field, "secret")) {
		strlcpy(k.secret, value, sizeof(k.secret));
	} else if (!strcmp(field, "token")) {
		size_t old = k.token ? strlen(k.token) : 0, add = strlen(value);
		char *t = realloc(k.token, old + add + 1);
		if (!t) {
			return ESP_ERR_NO_MEM;
		}
		memcpy(t + old, value, add + 1);
		k.token = t;
	} else {
		return ESP_ERR_INVALID_ARG;
	}
	return ESP_OK;
}
#endif

/* ------------------------------------------------------------------ channel, endpoints, ICE servers */

static bool channel_resolve(void)
{
	char host[96], body[320];
	snprintf(host, sizeof(host), "kinesisvideo.%s.amazonaws.com", g_cfg.kvs_region);
	char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
	if (!resp) {
		return false;
	}
	bool ok = false;
	if (!k.arn[0]) {
		snprintf(body, sizeof(body), "{\"ChannelName\":\"%s\"}", g_cfg.kvs_channel);
		if (kvs_post(host, "/describeSignalingChannel", body, resp, RESP_MAX) == 200) {
			cJSON *j = cJSON_Parse(resp);
			const char *arn = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "ChannelInfo"), "ChannelARN"));
			if (arn) {
				strlcpy(k.arn, arn, sizeof(k.arn));
			}
			cJSON_Delete(j);
		}
		if (!k.arn[0]) {
			set_err("DescribeSignalingChannel %s failed", g_cfg.kvs_channel);
			goto out;
		}
	}
	snprintf(body, sizeof(body),
			 "{\"ChannelARN\":\"%s\",\"SingleMasterChannelEndpointConfiguration\":{\"Protocols\":[\"WSS\",\"HTTPS\"],"
			 "\"Role\":\"MASTER\"}}",
			 k.arn);
	if (kvs_post(host, "/getSignalingChannelEndpoint", body, resp, RESP_MAX) == 200) {
		cJSON *j = cJSON_Parse(resp), *e;
		cJSON_ArrayForEach(e, cJSON_GetObjectItem(j, "ResourceEndpointList"))
		{
			const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(e, "Protocol"));
			const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(e, "ResourceEndpoint"));
			if (p && u && !strcmp(p, "WSS") && !strncmp(u, "wss://", 6)) {
				strlcpy(k.wss_host, u + 6, sizeof(k.wss_host));
			} else if (p && u && !strcmp(p, "HTTPS") && !strncmp(u, "https://", 8)) {
				strlcpy(k.https_host, u + 8, sizeof(k.https_host));
			}
		}
		cJSON_Delete(j);
	}
	ok = k.wss_host[0] && k.https_host[0];
	if (!ok) {
		set_err("GetSignalingChannelEndpoint failed");
	} else {
		ESP_LOGI(TAG, "channel %s: %s", g_cfg.kvs_channel, k.wss_host);
	}
out:
	free(resp);
	return ok;
}

static void ice_fetch(void)
{
	snprintf(k.stun, sizeof(k.stun), "stun:stun.kinesisvideo.%s.amazonaws.com:443", g_cfg.kvs_region);
	k.ice[0] = (esp_peer_ice_server_cfg_t){ .stun_url = k.stun };
	k.n_ice = 1;
	char body[300];
	snprintf(body, sizeof(body), "{\"ChannelARN\":\"%s\",\"ClientId\":\"%s\",\"Service\":\"TURN\"}", k.arn, g_cfg.serial);
	char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
	if (!resp) {
		return;
	}
	if (kvs_post(k.https_host, "/v1/get-ice-server-config", body, resp, RESP_MAX) == 200) {
		cJSON *j = cJSON_Parse(resp), *s;
		int n = 0;
		cJSON_ArrayForEach(s, cJSON_GetObjectItem(j, "IceServerList"))
		{
			if (n >= MAX_TURN) {
				break;
			}
			const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(s, "Username"));
			const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(s, "Password"));
			const char *pick = NULL;
			cJSON *u;
			cJSON_ArrayForEach(u, cJSON_GetObjectItem(s, "Uris"))
			{
				const char *v = cJSON_GetStringValue(u);
				if (v && !strncmp(v, "turn:", 5) && strstr(v, "transport=udp")) {
					pick = v; /* TURN over UDP: the cheapest relay for esp_peer */
					break;
				}
			}
			if (pick && user && pass) {
				strlcpy(k.turn_url[n], pick, sizeof(k.turn_url[n]));
				char *q = strchr(k.turn_url[n], '?');
				if (q) {
					*q = 0; /* "turn:host:443?transport=udp": esp_peer wants host:port only */
				}
				strlcpy(k.turn_user[n], user, sizeof(k.turn_user[n]));
				strlcpy(k.turn_pass[n], pass, sizeof(k.turn_pass[n]));
				k.ice[n] = (esp_peer_ice_server_cfg_t){ k.turn_url[n], k.turn_user[n], k.turn_pass[n] };
				n++;
			}
		}
		k.n_ice = n ? n : 1; /* no TURN server: STUN alone (host and server-reflexive candidates) */
		k.ice_at = esp_timer_get_time();
		cJSON_Delete(j);
	}
	memset(resp, 0, RESP_MAX);
	free(resp);
	ESP_LOGI(TAG, "ICE server: %s (user %u chars)", k.ice[0].stun_url, (unsigned)(k.ice[0].user ? strlen(k.ice[0].user) : 0));
}

/* ------------------------------------------------------------------ signaling (WebSocket) */

static void post(ev_kind_t kind, const char *sender, char *data)
{
	ev_t e = { .kind = kind, .data = data };
	strlcpy(e.sender, sender ? sender : "", sizeof(e.sender));
	if (xQueueSend(k.q, &e, pdMS_TO_TICKS(100)) != pdTRUE) {
		free(data);
	}
}

/* One complete text message from KVS: {"senderClientId", "messageType", "messagePayload": base64(JSON)} */
static void on_ws_message(const char *text, int len)
{
	cJSON *j = cJSON_ParseWithLength(text, len);
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "messageType"));
	const char *sender = cJSON_GetStringValue(cJSON_GetObjectItem(j, "senderClientId"));
	const char *payload = cJSON_GetStringValue(cJSON_GetObjectItem(j, "messagePayload"));
	if (!type || !strcmp(type, "STATUS_RESPONSE")) {
		if (len > 2) {
			ESP_LOGI(TAG, "signaling: %.*s", len > 200 ? 200 : len, text); /* STATUS_RESPONSE and the like */
		}
		cJSON_Delete(j);
		return;
	}
	if (!strcmp(type, "GO_AWAY") || !strcmp(type, "RECONNECT_ICE_SERVER")) {
		post(EV_GO_AWAY, NULL, NULL);
		cJSON_Delete(j);
		return;
	}
	size_t plen = payload ? strlen(payload) : 0, dlen = 0;
	unsigned char *dec = plen ? heap_caps_malloc(plen + 1, MALLOC_CAP_SPIRAM) : NULL;
	if (dec && mbedtls_base64_decode(dec, plen, &dlen, (const unsigned char *)payload, plen) == 0) {
		dec[dlen] = 0;
		cJSON *p = cJSON_Parse((const char *)dec);
		if (!strcmp(type, "SDP_OFFER")) {
			const char *sdp = cJSON_GetStringValue(cJSON_GetObjectItem(p, "sdp"));
			if (sdp) {
				post(EV_OFFER, sender, strdup(sdp));
			}
		} else if (!strcmp(type, "ICE_CANDIDATE")) {
			const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(p, "candidate"));
			if (c && c[0]) {
				post(EV_CANDIDATE, sender, strdup(c));
			}
		}
		cJSON_Delete(p);
	}
	free(dec);
	cJSON_Delete(j);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	esp_websocket_event_data_t *d = data;
	switch (id) {
	case WEBSOCKET_EVENT_CONNECTED:
		k.ws_up = true;
		post(EV_WS_UP, NULL, NULL);
		break;
	case WEBSOCKET_EVENT_DISCONNECTED:
	case WEBSOCKET_EVENT_CLOSED:
		if (k.ws_up || id == WEBSOCKET_EVENT_DISCONNECTED) {
			k.ws_up = false;
			post(EV_WS_DOWN, NULL, NULL);
		}
		break;
	case WEBSOCKET_EVENT_ERROR:
		ESP_LOGW(TAG, "signaling error: handshake http %d, tls 0x%x, errno %d", d->error_handle.esp_ws_handshake_status_code,
				 d->error_handle.esp_tls_last_esp_err, d->error_handle.esp_transport_sock_errno);
		break;
	case WEBSOCKET_EVENT_DATA:
		if (d->op_code != 0x1 && d->op_code != 0x0) {
			break; /* ping / pong / close */
		}
		if (d->payload_offset == 0) {
			k.rx_len = 0;
		}
		if (d->payload_len > 64 * 1024) {
			break;
		}
		if (d->payload_len + 1 > k.rx_cap) {
			char *n = heap_caps_realloc(k.rx, d->payload_len + 1, MALLOC_CAP_SPIRAM);
			if (!n) {
				break;
			}
			k.rx = n, k.rx_cap = d->payload_len + 1;
		}
		if (k.rx_len + d->data_len <= d->payload_len) {
			memcpy(k.rx + k.rx_len, d->data_ptr, d->data_len);
			k.rx_len += d->data_len;
		}
		if (k.rx_len == d->payload_len && d->fin) {
			k.rx[k.rx_len] = 0;
			on_ws_message(k.rx, k.rx_len);
			k.rx_len = 0;
		}
		break;
	default:
		break;
	}
}

static void ws_close(void)
{
	if (k.ws) {
		esp_websocket_client_stop(k.ws);
		esp_websocket_client_destroy(k.ws);
		k.ws = NULL;
	}
	k.ws_up = false;
}

static bool ws_open(void)
{
	size_t cap = 1024 + (k.token ? strlen(k.token) * 3 : 0);
	char *url = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM), amz[17];
	if (!url) {
		return false;
	}
	amz_now(amz);
	sigv4_creds_t c = creds();
	if (sigv4_presign_url(&c, g_cfg.kvs_region, SERVICE, amz, 299, "wss", k.wss_host, "/", "X-Amz-ChannelARN", k.arn, url,
						  cap)) {
		free(url);
		return false;
	}
	esp_websocket_client_config_t cfg = {
		.uri = url,
		.crt_bundle_attach = esp_crt_bundle_attach,
		.buffer_size = 4096,
		.task_stack = 6144,
		.task_prio = 5,
		.disable_auto_reconnect = true, /* a reconnect needs a freshly signed URL: done here */
		.network_timeout_ms = 10000,
		.ping_interval_sec = 30,
		.pingpong_timeout_sec = 90,
	};
	k.ws = esp_websocket_client_init(&cfg);
	memset(url, 0, cap);
	free(url);
	if (!k.ws) {
		return false;
	}
	esp_websocket_register_events(k.ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
	if (esp_websocket_client_start(k.ws) != ESP_OK) {
		ws_close();
		return false;
	}
	return true;
}

static bool ws_send(const char *action, const char *recipient, cJSON *payload)
{
	char *json = cJSON_PrintUnformatted(payload);
	if (!json || !k.ws || !k.ws_up) {
		free(json);
		return false;
	}
	size_t jl = strlen(json), b64 = 4 * ((jl + 2) / 3) + 1, cap = b64 + 160 + strlen(recipient), olen = 0;
	char *msg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
	int sent = -1;
	if (msg) {
		int n = snprintf(msg, cap, "{\"action\":\"%s\",\"recipientClientId\":\"%s\",\"messagePayload\":\"", action, recipient);
		mbedtls_base64_encode((unsigned char *)msg + n, cap - n, &olen, (const unsigned char *)json, jl);
		strlcpy(msg + n + olen, "\"}", cap - n - olen);
		/* the client's lock can be held a while by its own task on a poor link: try three times */
		for (int i = 0; i < 3 && sent < 0 && k.ws_up; i++) {
			sent = esp_websocket_client_send_text(k.ws, msg, strlen(msg), pdMS_TO_TICKS(3000));
		}
		if (sent < 0) {
			ESP_LOGW(TAG, "%s to the viewer not sent", action);
		}
		free(msg);
	}
	free(json);
	return sent >= 0;
}

/* ------------------------------------------------------------------ data channel (live_dc.h) */

static int64_t clock_ms(void) { return esp_timer_get_time() / 1000; }

/* Data channel messages go out from the encoder task only, like the video frames (esp_peer's main loop runs in the
 * kvs task meanwhile, as in Espressif's esp_webrtc); a send waits while the channel's cache is full */
static int dc_send(esp_peer_data_channel_type_t type, const void *data, int size)
{
	if (!k.peer || !k.dc_open) {
		return ESP_PEER_ERR_WRONG_STATE;
	}
	esp_peer_data_frame_t f = { .type = type, .stream_id = k.dc_stream, .data = (uint8_t *)data, .size = size };
	return esp_peer_send_data(k.peer, &f);
}

static void dc_send_json(cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (s) {
		dc_send(ESP_PEER_DATA_CHANNEL_STRING, s, (int)strlen(s));
		free(s);
	}
}

/* One JPEG in chunks of LIVE_DC_CHUNK bytes behind a 20-byte header; false when the channel refused a chunk (the
 * viewer drops the incomplete frame when the next one starts) */
static bool dc_send_jpeg(const uint8_t *jpg, uint32_t len, uint32_t seq, uint32_t ts_ms)
{
	if (!k.dc_buf) {
		k.dc_buf = heap_caps_malloc(LIVE_DC_HDR_LEN + LIVE_DC_CHUNK, MALLOC_CAP_SPIRAM);
		if (!k.dc_buf) {
			return false;
		}
	}
	for (uint32_t off = 0; off < len; off += LIVE_DC_CHUNK) {
		uint32_t n = len - off > LIVE_DC_CHUNK ? LIVE_DC_CHUNK : len - off;
		live_dc_header(k.dc_buf, seq, len, off, ts_ms);
		memcpy(k.dc_buf + LIVE_DC_HDR_LEN, jpg + off, n);
		if (dc_send(ESP_PEER_DATA_CHANNEL_DATA, k.dc_buf, (int)(LIVE_DC_HDR_LEN + n)) != ESP_PEER_ERR_NONE) {
			return false;
		}
	}
	return true;
}

static const char *mode_name(live_mode_t m) { return m == LIVE_MODE_JPEG ? "jpeg" : "h264"; }

static cJSON *quality_json(const char *t, const live_quality_t *q)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "t", t);
	cJSON_AddStringToObject(o, "mode", mode_name(q->mode));
	cJSON_AddNumberToObject(o, "w", q->w);
	cJSON_AddNumberToObject(o, "h", q->h);
	cJSON_AddNumberToObject(o, "q", q->q);
	cJSON_AddNumberToObject(o, "fps", q->fps);
	return o;
}

static void dc_send_hello(const live_quality_t *cur)
{
	cJSON *o = quality_json("hello", cur);
	cJSON_AddNumberToObject(o, "v", LIVE_DC_VERSION);
	cJSON *m = cJSON_AddArrayToObject(o, "modes");
	cJSON_AddItemToArray(m, cJSON_CreateString("h264"));
	cJSON_AddItemToArray(m, cJSON_CreateString("jpeg"));
	cJSON *sz = cJSON_AddArrayToObject(o, "sizes"); /* the sensor's live sizes (cam.c) */
	const int(*sizes)[2];
	int n = live_dc_sizes(&sizes);
	for (int i = 0; i < n; i++) {
		cJSON_AddItemToArray(sz, cJSON_CreateIntArray(sizes[i], 2));
	}
	cJSON *qr = cJSON_AddArrayToObject(o, "qRange"); /* "q" is the running quality, as in every message */
	cJSON_AddItemToArray(qr, cJSON_CreateNumber(LIVE_DC_Q_MIN));
	cJSON_AddItemToArray(qr, cJSON_CreateNumber(LIVE_DC_Q_MAX));
	cJSON_AddNumberToObject(o, "chunk", LIVE_DC_CHUNK);
	dc_send_json(o);
}

static void dc_send_error(const char *msg)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddStringToObject(o, "t", "error");
	cJSON_AddStringToObject(o, "msg", msg);
	dc_send_json(o);
}

static int on_dc_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
	ESP_LOGI(TAG, "data channel \"%s\" (stream %u) open", ch->label ? ch->label : "", (unsigned)ch->stream_id);
	if (ch->label && !strcmp(ch->label, LIVE_DC_LABEL)) {
		k.dc_stream = ch->stream_id;
		k.dc_open = true;
		k.dc_hello = true;
	}
	return 0;
}

static int on_dc_close(esp_peer_data_channel_info_t *ch, void *ctx)
{
	if (k.dc_open && ch->stream_id == k.dc_stream) {
		ESP_LOGI(TAG, "data channel closed: back to H.264");
		k.dc_open = false;
	}
	return 0;
}

/* Control messages from the viewer (main loop of the kvs task): applied by the encoder task */
static int on_dc_data(esp_peer_data_frame_t *f, void *ctx)
{
	if (f->type != ESP_PEER_DATA_CHANNEL_STRING || f->stream_id != k.dc_stream || f->size <= 0 || f->size > 512) {
		return 0;
	}
	live_dc_msg_t m;
	switch (live_dc_parse((const char *)f->data, (size_t)f->size, (const live_quality_t *)&k.cur, LIVE_W, LIVE_H, &m)) {
	case LIVE_DC_QUALITY:
		k.req = m.quality;
		k.req_pending = true;
		k.viewer_chose = true;
		break;
	case LIVE_DC_ACK:
		live_dc_on_ack(&k.flow, m.seq, clock_ms());
		k.last_ack_ms = clock_ms();
		break;
	case LIVE_DC_BAD:
		strlcpy(k.dc_err, m.error, sizeof(k.dc_err));
		k.dc_err_pending = true;
		break;
	default:
		break;
	}
	return 0;
}

/* ------------------------------------------------------------------ encoder task */

typedef struct {
	esp_h264_enc_handle_t enc;
	uint8_t *out;
	uint32_t out_cap;
	bool cam_on;
} pipe_t;

static void pipe_stop(pipe_t *p)
{
	if (p->enc) {
		esp_h264_enc_close(p->enc);
		esp_h264_enc_del(p->enc);
	}
	if (p->out) {
		esp_h264_free(p->out);
	}
	if (p->cam_on) {
		cam_stream_stop();
	}
	memset(p, 0, sizeof(*p));
}

/* H.264: raw YUV422 + the software encoder; JPEG: the sensor's own encoder at the asked size and quality */
static bool pipe_start(pipe_t *p, const live_quality_t *q)
{
	memset(p, 0, sizeof(*p));
	if (q->mode == LIVE_MODE_JPEG) {
		if (cam_stream_start_live_jpeg(q->w, q->h, q->q) != ESP_OK) {
			set_err("camera start (JPEG %dx%d) failed", q->w, q->h);
			return false;
		}
		p->cam_on = true;
		return true;
	}
	if (cam_stream_start_yuv(LIVE_W, LIVE_H) != ESP_OK) {
		set_err("camera start (YUV422) failed");
		return false;
	}
	p->cam_on = true;
	esp_h264_enc_cfg_sw_t cfg = {
		.pic_type = ESP_H264_RAW_FMT_YUYV,
		.gop = (uint8_t)(q->fps * 2),
		.fps = (uint8_t)q->fps,
		.res = { .width = LIVE_W, .height = LIVE_H },
		.rc = { .bitrate = CONFIG_CAMOS_LIVE_KBPS * 1000, .qp_min = 22, .qp_max = 40 },
	};
	if (esp_h264_enc_sw_new(&cfg, &p->enc) != ESP_H264_ERR_OK || esp_h264_enc_open(p->enc) != ESP_H264_ERR_OK) {
		set_err("H.264 encoder init failed");
		pipe_stop(p);
		return false;
	}
	p->out = esp_h264_aligned_calloc(16, 1, LIVE_W * LIVE_H * 3 / 2, &p->out_cap, ESP_H264_MEM_SPIRAM);
	if (!p->out) {
		set_err("no memory for the H.264 output");
		pipe_stop(p);
		return false;
	}
	return true;
}

static const live_quality_t default_quality = { LIVE_MODE_H264, LIVE_W, LIVE_H, 12, LIVE_FPS };
/* The normal mode once the viewer's data channel is open: JPEG at VGA, the web console stream's standard quality */
static const live_quality_t jpeg_quality = { LIVE_MODE_JPEG, 640, 480, 12, 15 };
#define WEAK_LINK_MS 6000 /* JPEG frames go out but no ack comes back for this long: H.264 on the video track */

/* A capture wants the camera: stop the pipeline, give the camera lock to the scheduler, wait until the capture is
 * done with the sensor, then start again in the same mode. False when the camera does not come back. */
static bool pause_cycle(pipe_t *p, live_quality_t *cur, bool *cam_held)
{
	int64_t t0 = esp_timer_get_time();
	pipe_stop(p);
	if (*cam_held) {
		camera_give();
		*cam_held = false;
	}
	k.paused = true;
	ESP_LOGI(TAG, "live stream paused for a capture");
	while (k.pause_req && k.venc_run) {
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	k.paused = false;
	if (!k.venc_run || !camera_take(30000)) {
		return false;
	}
	*cam_held = true;
	if (!pipe_start(p, cur)) {
		*cur = default_quality;
		if (!pipe_start(p, cur)) {
			return false;
		}
		k.cur = *cur;
		dc_send_json(quality_json("mode", cur));
	}
	live_dc_flow_reset(&k.flow);
	k.last_ack_ms = clock_ms(); /* no acks during the pause: not a weak link */
	k.pause_ms = (int)((esp_timer_get_time() - t0) / 1000);
	ESP_LOGI(TAG, "live stream back after %d ms", k.pause_ms);
	return true;
}

static void venc_task(void *arg)
{
	pipe_t p = { 0 };
	bool cam_held = false, wifi = net_type() == NET_WIFI;
	live_quality_t cur = default_quality;
	k.cur = cur;
	stream_stop(); /* a LAN MJPEG viewer gives way */
	if (!camera_take(15000)) {
		set_err("camera busy (capture in progress)");
		goto done;
	}
	cam_held = true;
	if (!pipe_start(&p, &cur)) {
		goto done;
	}
	if (wifi) {
		esp_wifi_set_ps(WIFI_PS_NONE); /* modem sleep between beacons costs frames */
	}
	/* Started with the offer: the camera's DMA buffer and the encoder are allocated before the DTLS session takes its
	 * share of internal RAM, and the exposure settles while ICE runs */
	while (k.venc_run && !k.viewer_up) {
		if (k.pause_req && !pause_cycle(&p, &cur, &cam_held)) {
			goto done;
		}
		camera_fb_t *f = esp_camera_fb_get();
		if (f) {
			esp_camera_fb_return(f);
		}
	}
	ESP_LOGI(TAG, "streaming H.264 %dx%d at %d fps, %d kbit/s; free internal %u, PSRAM %u", LIVE_W, LIVE_H, cur.fps,
			 CONFIG_CAMOS_LIVE_KBPS, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
			 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	int64_t t0 = esp_timer_get_time(), next = t0, win = t0, last_log = t0, last_stats = t0;
	uint32_t win_frames = 0, seq = 0;
	bool waiting = false;
	uint64_t win_bytes = 0, win_enc_us = 0, win_cap_us = 0;
	k.frames = k.idr = k.dc_drops = k.dc_waits = 0;
	live_dc_flow_reset(&k.flow);
	while (k.venc_run) {
		if (k.dc_hello) {
			k.dc_hello = false;
			dc_send_hello(&cur);
			if (!k.req_pending && !k.viewer_chose) {
				k.req = jpeg_quality; /* JPEG by default; the viewer's own choice, when it sends one, wins */
				k.req_pending = true;
			}
		}
		if (k.dc_err_pending) {
			k.dc_err_pending = false;
			dc_send_error(k.dc_err);
		}
		if (k.pause_req) {
			if (!pause_cycle(&p, &cur, &cam_held)) {
				set_err("camera not back after a capture");
				break;
			}
			next = win = esp_timer_get_time(), win_frames = 0, win_bytes = 0, win_enc_us = 0, win_cap_us = 0;
			continue;
		}
		live_quality_t want = cur;
		bool asked = false;
		const char *why = NULL;
		if (k.req_pending) {
			k.req_pending = false;
			want = k.req;
			asked = true;
		}
		if (want.mode == LIVE_MODE_JPEG && !k.dc_open) {
			want = default_quality; /* the data channel is gone: H.264 on the video track */
		} else if (capture_busy()) {
			k.last_ack_ms = clock_ms(); /* a photo upload fills the uplink and acks get lost: not a weak link */
		} else if (cur.mode == LIVE_MODE_JPEG && want.mode == LIVE_MODE_JPEG && seq > 0 &&
				   clock_ms() - k.last_ack_ms > WEAK_LINK_MS) {
			want = default_quality; /* nothing arrives: the small H.264 stream copes with a weak link */
			why = "weak_link";
			ESP_LOGW(TAG, "no JPEG ack for %d s: H.264 %dx%d", WEAK_LINK_MS / 1000, LIVE_W, LIVE_H);
		}
		if (memcmp(&want, &cur, sizeof(want))) {
			pipe_stop(&p);
			if (!pipe_start(&p, &want)) {
				dc_send_error(k.err); /* stay with what ran, else the default */
				want = cur;
				if (!pipe_start(&p, &want)) {
					want = default_quality;
					if (!pipe_start(&p, &want)) {
						break;
					}
				}
			}
			cur = want;
			k.cur = cur;
			live_dc_flow_reset(&k.flow);
			k.last_ack_ms = clock_ms();
			win = next = esp_timer_get_time(), win_frames = 0, win_bytes = 0, win_enc_us = 0, win_cap_us = 0;
			ESP_LOGI(TAG, "live view now %s %dx%d q%d at %d fps", mode_name(cur.mode), cur.w, cur.h, cur.q, cur.fps);
			cJSON *mo = quality_json("mode", &cur);
			if (why) {
				cJSON_AddStringToObject(mo, "reason", why);
			}
			dc_send_json(mo);
		} else if (asked) {
			dc_send_json(quality_json("mode", &cur)); /* already running: confirm it */
		}
		int64_t c0 = esp_timer_get_time();
		camera_fb_t *fb = esp_camera_fb_get();
		win_cap_us += (uint64_t)(esp_timer_get_time() - c0);
		if (!fb) {
			continue;
		}
		int64_t e0 = esp_timer_get_time();
		uint32_t pts = (uint32_t)((e0 - t0) / 1000), bytes = 0;
		if (cur.mode == LIVE_MODE_H264) {
			if (fb->format != PIXFORMAT_YUV422 || fb->len < (size_t)LIVE_W * LIVE_H * 2) {
				esp_camera_fb_return(fb);
				set_err("unexpected camera frame");
				break;
			}
			esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = fb->buf, .len = fb->len }, .pts = pts };
			esp_h264_enc_out_frame_t of = { .raw_data = { .buffer = p.out, .len = p.out_cap } };
			esp_h264_err_t r = esp_h264_enc_process(p.enc, &in, &of);
			esp_camera_fb_return(fb);
			if (r != ESP_H264_ERR_OK) {
				ESP_LOGW(TAG, "encode error %d", r);
				continue;
			}
			esp_peer_video_frame_t vf = { .pts = pts, .data = p.out, .size = (int)of.length };
			if (k.peer) {
				esp_peer_send_video(k.peer, &vf);
			}
			k.idr += of.frame_type == ESP_H264_FRAME_TYPE_IDR;
			bytes = of.length;
		} else {
			/* flow control: no frame while LIVE_DC_WINDOW frames wait for their ack (the newest frame is taken next) */
			/* ... and none while internal RAM runs low: Wi-Fi needs it to carry what is already queued */
			if (!live_dc_may_send(&k.flow, clock_ms()) || heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < LIVE_MIN_INTERNAL) {
				esp_camera_fb_return(fb);
				if (!waiting) {
					k.dc_waits++; /* frames the channel could not take in time */
					waiting = true;
				}
				vTaskDelay(pdMS_TO_TICKS(5));
				continue;
			}
			waiting = false;
			int64_t s0 = esp_timer_get_time();
			bool ok = fb->format == PIXFORMAT_JPEG && dc_send_jpeg(fb->buf, fb->len, seq + 1, pts);
			bytes = fb->len;
			esp_camera_fb_return(fb);
			k.push_ms = (int32_t)((esp_timer_get_time() - s0) / 1000);
			if (!ok) {
				k.dc_drops++;
				vTaskDelay(pdMS_TO_TICKS(20)); /* the channel refused a chunk: let it drain */
				continue;
			}
			live_dc_on_sent(&k.flow, ++seq, clock_ms());
		}
		int64_t e1 = esp_timer_get_time();
		k.frames++;
		win_frames++, win_bytes += bytes, win_enc_us += (uint64_t)(e1 - e0);
		if (e1 - win >= 2000000) {
			k.fps = win_frames * 1e6f / (float)(e1 - win);
			k.kbps = win_bytes * 8 / 1000.0f / ((e1 - win) / 1e6f);
			k.enc_ms = win_enc_us / 1000.0f / win_frames;
			k.cap_ms = win_cap_us / 1000.0f / win_frames;
			k.frame_bytes = (uint32_t)(win_bytes / win_frames);
			win = e1, win_frames = 0, win_bytes = 0, win_enc_us = 0, win_cap_us = 0;
		}
		if (e1 - last_stats >= 2000000 && k.dc_open) {
			cJSON *o = quality_json("stats", &cur);
			cJSON_AddNumberToObject(o, "rate", (int)(k.fps * 10) / 10.0);
			cJSON_AddNumberToObject(o, "kbps", (int)k.kbps);
			cJSON_AddNumberToObject(o, "bytes", k.frame_bytes);
			cJSON_AddNumberToObject(o, "drops", k.dc_drops);
			cJSON_AddNumberToObject(o, "waits", k.dc_waits);
			if (cur.mode == LIVE_MODE_JPEG) {
				cJSON_AddNumberToObject(o, "ackMs", k.flow.last_rtt_ms);
				cJSON_AddNumberToObject(o, "pushMs", k.push_ms);
			}
			dc_send_json(o);
			last_stats = e1;
		}
		if (e1 - last_log >= 10000000) {
			ESP_LOGI(TAG, "%s %dx%d: %.1f fps, %.0f kbit/s, %u bytes/frame, camera wait %.1f ms, %s %.1f ms, ack %d ms, push %d ms, "
					 "drops %u, waits %u, free internal %u (largest %u), PSRAM %u",
					 mode_name(cur.mode), cur.w, cur.h, k.fps, k.kbps, (unsigned)k.frame_bytes, k.cap_ms,
					 cur.mode == LIVE_MODE_JPEG ? "send" : "encode", k.enc_ms, (int)k.flow.last_rtt_ms, (int)k.push_ms,
					 (unsigned)k.dc_drops, (unsigned)k.dc_waits, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
					 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
					 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
			last_log = e1;
		}
		const int64_t period = 1000000 / cur.fps;
		next += period;
		int64_t now = esp_timer_get_time();
		if (next > now) {
			vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
		} else if (now - next > period) {
			next = now; /* behind: no burst to catch up */
			vTaskDelay(1);  /* let the network tasks of this core run */
		}
	}
done:
	pipe_stop(&p);
	if (cam_held) {
		camera_give();
	}
	if (wifi) {
		esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	}
	k.fps = 0;
	ESP_LOGI(TAG, "encoder stopped after %u frames", (unsigned)k.frames);
	k.venc_done = true;
	vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ peer */

static int on_peer_state(esp_peer_state_t st, void *ctx)
{
	ESP_LOGI(TAG, "peer state %d", st);
	if (st == ESP_PEER_STATE_CONNECTED) {
		k.viewer_up = true;
		k.setup_ms = (int)((esp_timer_get_time() - k.offer_at) / 1000);
	} else if (st == ESP_PEER_STATE_DISCONNECTED || st == ESP_PEER_STATE_CLOSED) {
		k.close_req = true;
	} else if (st == ESP_PEER_STATE_CONNECT_FAILED && k.viewer_up) {
		k.close_req = true; /* before the first connection the viewer's later candidates (and its checks, which
							 * give peer-reflexive ones) still can make it: ANSWER_WAIT_S decides */
	}
	return 0;
}

static int on_peer_msg(esp_peer_msg_t *m, void *ctx)
{
	if (m->type != ESP_PEER_MSG_TYPE_SDP || !k.viewer[0]) {
		return 0; /* local candidates travel inside the answer */
	}
	cJSON *p = cJSON_CreateObject();
	cJSON_AddStringToObject(p, "type", "answer");
	cJSON_AddStringToObject(p, "sdp", (const char *)m->data);
	if (ws_send("SDP_ANSWER", k.viewer, p)) {
		ESP_LOGI(TAG, "answer sent to %s", k.viewer);
	}
	cJSON_Delete(p);
	return 0;
}

static void venc_stop(void)
{
	if (k.venc) {
		k.venc_run = false;
		for (int i = 0; i < 300 && !k.venc_done; i++) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
		k.venc = NULL;
	}
}

static void peer_close(void)
{
	bool was = k.viewer_up;
	k.viewer_up = false;
	venc_stop();
	if (k.peer) {
		esp_peer_disconnect(k.peer);
		esp_peer_close(k.peer);
		k.peer = NULL;
	}
	free(k.dc_buf); /* the encoder task has stopped */
	k.dc_buf = NULL;
	k.close_req = false;
	if (was || k.viewer[0]) {
		ESP_LOGI(TAG, "viewer %s gone", k.viewer);
	}
	k.viewer[0] = 0;
}

static bool peer_open(void)
{
	esp_peer_default_cfg_t d = {
		.agent_recv_timeout = 500,
		.keep_role = true,          /* the master always answers */
		.alive_binding_retries = 3, /* a viewer that vanished is noticed after about 20 s (default 5: 30 s) */
		.rtp_cfg = { .send_pool_size = 256 * 1024, .send_queue_num = 256 },
		/* a few JPEG frames of the largest size in flight (PSRAM) */
		.data_ch_cfg = { .cache_timeout = 2000, .send_cache_size = 512 * 1024, .recv_cache_size = 16 * 1024 },
	};
	esp_peer_cfg_t cfg = {
		.server_lists = k.ice,
		.server_num = (uint8_t)k.n_ice,
		.role = ESP_PEER_ROLE_CONTROLLED,
		.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
		/* the viewers offer audio too (recvonly): an audio line that never carries a frame keeps the answer valid */
		.audio_info = { .codec = ESP_PEER_AUDIO_CODEC_G711A, .sample_rate = 8000, .channel = 1 },
		.audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
		.video_info = { .codec = ESP_PEER_VIDEO_CODEC_H264, .width = LIVE_W, .height = LIVE_H, .fps = LIVE_FPS },
		.video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
		/* the viewer opens "camos"; an offer without an application line gets an answer without one */
		.enable_data_channel = true,
		.manual_ch_create = true,
		.on_channel_open = on_dc_open,
		.on_channel_close = on_dc_close,
		.on_data = on_dc_data,
		.on_state = on_peer_state,
		.on_msg = on_peer_msg,
		.extra_cfg = &d,
		.extra_size = sizeof(d),
	};
	int r = esp_peer_open(&cfg, esp_peer_get_default_impl(), &k.peer);
	if (r != ESP_PEER_ERR_NONE) {
		k.peer = NULL;
		set_err("esp_peer_open %d", r);
		return false;
	}
	return true;
}

static void handle_offer(ev_t *e)
{
	peer_close(); /* one viewer at a time: the newest one wins */
	strlcpy(k.viewer, e->sender, sizeof(k.viewer));
	k.offer_at = esp_timer_get_time();
	k.dc_open = k.dc_hello = k.dc_err_pending = k.req_pending = k.viewer_chose = false;
	int sizes[LIVE_DC_MAX_SIZES][2];
	live_dc_set_sizes(sizes, cam_sizes(true, sizes, LIVE_DC_MAX_SIZES)); /* the sensor's live sizes */
	ESP_LOGI(TAG, "offer from viewer %s (%u bytes)", k.viewer, (unsigned)strlen(e->data));
	k.venc_run = true, k.venc_done = false;
	if (xTaskCreatePinnedToCore(venc_task, "venc", 16384, NULL, 4, &k.venc, 1) != pdPASS) {
		k.venc = NULL;
		set_err("no memory for the encoder task");
		k.viewer[0] = 0;
		return;
	}
	if (!k.ice_at || esp_timer_get_time() - k.ice_at > (int64_t)ICE_TTL_S * 1000000) {
		ice_fetch();
	}
	if (!peer_open()) {
		venc_stop();
		k.viewer[0] = 0;
		return;
	}
	k.sessions++;
	esp_peer_new_connection(k.peer);
	esp_peer_msg_t m = { .type = ESP_PEER_MSG_TYPE_SDP, .data = (uint8_t *)e->data, .size = (int)strlen(e->data) };
	esp_peer_send_msg(k.peer, &m);
}

/* ------------------------------------------------------------------ supervisor */

static bool wanted(void)
{
	if (!live_supported()) {
		return false;
	}
#if CONFIG_CAMOS_LIVE_STANDBY
	if (power_effective() == POWER_POWERED) {
		return true;
	}
#endif
	return live_active(); /* the window's end also ends a viewer's session (live.start again extends it) */
}

static void leave(void)
{
	peer_close();
	ws_close();
	if (strcmp(k.state, "off")) {
		ESP_LOGI(TAG, "left the channel");
	}
	strlcpy(k.state, "off", sizeof(k.state));
}

static void backoff(int s)
{
	k.retry_at = esp_timer_get_time() + (int64_t)s * 1000000;
	strlcpy(k.state, "retry", sizeof(k.state));
}

static void kvs_task(void *arg)
{
	bool cert_ready = false;
	int fails = 0;
	for (;;) {
		ev_t e;
		/* JPEG over the data channel: the main loop moves the SCTP data, so it runs more often */
		int wait_ms = k.peer ? (k.dc_open && k.cur.mode == LIVE_MODE_JPEG ? 2 : 10) : (k.ws ? 500 : 2000);
		bool got = xQueueReceive(k.q, &e, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
		if (got) {
			if (e.kind == EV_OFFER && k.ws_up) {
				handle_offer(&e);
			} else if (e.kind == EV_CANDIDATE && k.peer && !strcmp(e.sender, k.viewer) && !strstr(e.data, ".local ")) {
				/* mDNS host candidates (browsers hide their LAN address) cannot be resolved here: the viewer's checks
				 * reach the camera's host candidate anyway and become peer-reflexive */
				esp_peer_msg_t m = { .type = ESP_PEER_MSG_TYPE_CANDIDATE, .data = (uint8_t *)e.data, .size = (int)strlen(e.data) };
				esp_peer_send_msg(k.peer, &m);
			} else if (e.kind == EV_WS_UP) {
				ESP_LOGI(TAG, "joined %s as MASTER", g_cfg.kvs_channel);
				strlcpy(k.state, "joined", sizeof(k.state));
				k.err[0] = 0;
				fails = 0;
				if (!k.peer && (!k.ice_at || esp_timer_get_time() - k.ice_at > (int64_t)ICE_TTL_S * 1000000)) {
					ice_fetch(); /* ready before an offer: the answer goes out sooner */
				}
			} else if (e.kind == EV_WS_DOWN || e.kind == EV_GO_AWAY) {
				if (k.ws) {
					ESP_LOGI(TAG, "signaling %s", e.kind == EV_GO_AWAY ? "asks to reconnect" : "closed");
					ws_close(); /* reconnected below; a connected viewer keeps its media path */
					k.retry_at = esp_timer_get_time() + 2LL * 1000000;
				}
			}
			free(e.data);
		}
		if (k.peer) {
			esp_peer_main_loop(k.peer);
			if (k.viewer_up && strcmp(k.state, "viewer")) {
				strlcpy(k.state, "viewer", sizeof(k.state));
				ESP_LOGI(TAG, "viewer %s connected after %d ms", k.viewer, k.setup_ms);
			}
			if (k.venc && k.venc_done) {
				k.close_req = true; /* the encoder gave up (camera) */
			}
			if (k.close_req || (!k.viewer_up && esp_timer_get_time() - k.offer_at > (int64_t)ANSWER_WAIT_S * 1000000)) {
				peer_close();
				strlcpy(k.state, k.ws_up ? "joined" : "joining", sizeof(k.state));
			}
			if (got) {
				continue; /* drain the queue before the slower checks */
			}
		}
		if (!wanted()) {
			if (k.ws || k.peer) {
				leave();
			}
			strlcpy(k.state, "off", sizeof(k.state));
			continue;
		}
		int64_t now = esp_timer_get_time();
		if (k.ws) {
			if (!k.ws_up && now > k.retry_at) {
				set_err("signaling did not connect (clock, credentials or network)");
				ws_close();
				backoff(fails++ < 3 ? 5 : 60);
			} else if (k.ws_up && !k.peer && !creds_valid()) {
				ws_close(); /* keys about to expire: rejoin with fresh ones */
			}
			continue;
		}
		if (now < k.retry_at) {
			continue;
		}
		if (!net_up() || !clock_synced()) {
			strlcpy(k.state, "waiting", sizeof(k.state));
			continue;
		}
		strlcpy(k.state, "joining", sizeof(k.state));
		if (!cert_ready) {
			esp_peer_pre_generate_cert(); /* DTLS certificate once: later sessions start faster */
			cert_ready = true;
		}
		if (!creds_valid() && !creds_fetch()) {
			backoff(fails++ < 3 ? 15 : 120);
			continue;
		}
		if ((!k.arn[0] || !k.wss_host[0]) && !channel_resolve()) {
			k.arn[0] = 0; /* the channel may be recreated: resolve again */
			backoff(fails++ < 3 ? 15 : 120);
			continue;
		}
		if (!ws_open()) {
			set_err("signaling connect failed");
			backoff(fails++ < 3 ? 5 : 60);
			continue;
		}
		k.retry_at = esp_timer_get_time() + 15LL * 1000000; /* no CONNECTED within 15 s: start over */
	}
}

/* ------------------------------------------------------------------ API for live.c / console / status */

void live_kvs_init(void)
{
	if (k.task) {
		return;
	}
	strlcpy(k.state, "off", sizeof(k.state));
	k.q = xQueueCreate(16, sizeof(ev_t));
	esp_log_level_set("AGENT", ESP_LOG_WARN); /* esp_peer's ICE agent logs every allocate and the TURN password */
	/* the stack in PSRAM: internal RAM is for the camera DMA, Wi-Fi and the encoder (this task never writes flash) */
	if (!k.q || xTaskCreatePinnedToCoreWithCaps(kvs_task, "kvs", 12288, NULL, 5, &k.task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
		ESP_LOGE(TAG, "live view task not started");
		k.task = NULL;
	}
}

esp_err_t live_kvs_start(void)
{
	if (!k.task) {
		return ESP_ERR_INVALID_STATE;
	}
	k.retry_at = 0; /* a new request retries at once */
	return ESP_OK;
}

int live_pause(int timeout_ms)
{
	if (!k.venc || k.venc_done) {
		return 0;
	}
	k.pause_req = true;
	for (int t = 0; t < timeout_ms; t += 10) {
		if (k.paused || k.venc_done) {
			return 1;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	k.pause_req = false;
	ESP_LOGW(TAG, "the encoder did not give the camera up within %d ms", timeout_ms);
	return -1;
}

void live_resume(void)
{
	k.pause_req = false;
}

void live_kvs_stop(void)
{
	k.close_req = true; /* the viewer is dropped; the channel is left unless powered standby keeps it */
}

void live_status_json(cJSON *o)
{
	cJSON_AddStringToObject(o, "state", k.state);
	cJSON_AddStringToObject(o, "channel", g_cfg.kvs_channel);
	cJSON_AddStringToObject(o, "credentials", k.bench ? "bench" : provider_configured() ? "iot" : "none");
	cJSON_AddBoolToObject(o, "viewer", k.viewer_up);
	cJSON_AddNumberToObject(o, "sessions", k.sessions);
	if (k.viewer_up) {
		cJSON_AddStringToObject(o, "mode", mode_name(k.cur.mode));
		cJSON_AddBoolToObject(o, "dataChannel", k.dc_open);
		cJSON_AddNumberToObject(o, "width", k.cur.w);
		cJSON_AddNumberToObject(o, "height", k.cur.h);
		cJSON_AddNumberToObject(o, "frameBytes", k.frame_bytes);
		cJSON_AddNumberToObject(o, "drops", k.dc_drops);
		cJSON_AddNumberToObject(o, "waits", k.dc_waits);
		cJSON_AddNumberToObject(o, "fps", (int)(k.fps * 10) / 10.0);
		cJSON_AddNumberToObject(o, "kbps", (int)k.kbps);
		cJSON_AddNumberToObject(o, "encodeMs", (int)(k.enc_ms * 10) / 10.0);
		cJSON_AddNumberToObject(o, "cameraMs", (int)(k.cap_ms * 10) / 10.0);
		cJSON_AddNumberToObject(o, "frames", k.frames);
		cJSON_AddNumberToObject(o, "setupMs", k.setup_ms);
		if (k.pause_ms) {
			cJSON_AddNumberToObject(o, "lastPauseMs", k.pause_ms);
		}
	}
	if (k.err[0]) {
		cJSON_AddStringToObject(o, "error", k.err);
	}
}

#endif /* CONFIG_CAMOS_LIVE */
