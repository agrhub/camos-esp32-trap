/*
 * MQTT agent: CamOS device protocol v1 over TLS to AWS IoT Core (protocol 1, 2, 4, 5, 7, 8, 9) plus the FlyBot
 * additions (CONTRACTS section 3: RPC snapshot.now, live.start, live.stop, gps.fix, ota.apply; goodbye wakeAt).
 *
 *   camera -> cloud  state (retained), event, telemetry, shadow/reported (retained), rpc/res
 *   cloud -> camera  shadow/desired, rpc/req
 *
 * Mutual TLS with the device certificate and key from NVS; the broker is verified against the embedded Amazon Root CA 1.
 * The client id is the serial (the AWS IoT policy is scoped to camos/{thing}/...). SNI is sent by esp-tls.
 * The shadow part is in shadow.c, captures in sched.c; this file only moves messages.
 */
#include <string.h>
#include <stdlib.h>
#include "mqtt_client.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "camos.h"

static const char *TAG = "proto";
extern const char root_ca_pem[] asm("_binary_AmazonRootCA1_pem_start");

static esp_mqtt_client_handle_t client;
static volatile bool connected;
static char t_state[64], t_event[64], t_telemetry[64], t_desired[64], t_reported[64], t_req[64], t_res[64];
static char uri[160];
static int64_t down_since_ms;
static volatile int64_t connected_at_ms;
static volatile int rpc_busy; /* queued + running */

#define RPC_QUEUE_LEN 8
typedef struct {
	char *json;
	TickType_t received;
} rpc_item_t;
static QueueHandle_t rpc_queue;

bool proto_connected(void) { return connected; }
bool proto_provisioned(void) { return client != NULL; }
bool proto_idle(void) { return rpc_busy == 0; }

int proto_connected_for_s(void)
{
	return connected ? (int)((esp_timer_get_time() / 1000 - connected_at_ms) / 1000) : -1;
}

/* ------------------------------------------------------------------ publishing */

static void pub(const char *topic, const char *json, int qos, bool retain)
{
	if (!client) {
		return; /* not provisioned */
	}
	/* QoS 1 goes through the outbox, so it survives short disconnects; QoS 0 is only sent while connected */
	if (qos > 0) {
		esp_mqtt_client_enqueue(client, topic, json, 0, qos, retain, true);
	} else if (connected) {
		esp_mqtt_client_publish(client, topic, json, 0, 0, 0);
	}
}

cJSON *evt_new(const char *type, char id_out[27])
{
	cJSON *e = cJSON_CreateObject();
	new_ulid(id_out);
	cJSON_AddNumberToObject(e, "v", CAMOS_PROTOCOL);
	cJSON_AddStringToObject(e, "id", id_out);
	cJSON_AddNumberToObject(e, "ts", (double)now_ms());
	cJSON_AddStringToObject(e, "clock", clock_synced() ? "utc" : "uptime"); /* same extension as the AMB82 firmware */
	cJSON_AddStringToObject(e, "type", type);
	return e;
}

void proto_publish_event(cJSON *evt)
{
	/* the console's Events page keeps a short line of every event */
	const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "type"));
	const char *trig = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "trigger"));
	const char *err = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "imageError"));
	const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "snapshotId"));
	const char *det = cJSON_GetStringValue(cJSON_GetObjectItem(evt, "detail"));
	char line[96];
	snprintf(line, sizeof(line), "%s%s%s%s", trig ? trig : "", err ? ": upload failed, " : sid ? ": uploaded " : "",
			 err ? err : sid ? sid : "", det ? det : "");
	events_note(type ? type : "event", line);
	char *s = cJSON_PrintUnformatted(evt);
	cJSON_Delete(evt);
	if (s) {
		pub(t_event, s, 1, false);
		free(s);
	}
}

void proto_publish_reported(const char *json)
{
	pub(t_reported, json, 1, true);
}

void proto_publish_reported_now(void)
{
	if (!client) {
		return;
	}
	char *r = shadow_reported_json();
	if (r) {
		proto_publish_reported(r);
		free(r);
	}
}

/* Protocol 4: retained state, published at connect and after notable changes */
void proto_publish_state(bool online)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", CAMOS_PROTOCOL);
	if (!online) {
		cJSON_AddFalseToObject(o, "online"); /* the Last Will payload, sent by hand before a deep sleep */
	} else {
		cJSON_AddNumberToObject(o, "ts", (double)now_ms());
		cJSON_AddStringToObject(o, "clock", clock_synced() ? "utc" : "uptime");
		cJSON_AddTrueToObject(o, "online");
		cJSON_AddStringToObject(o, "model", CONFIG_CAMOS_MODEL);
		cJSON_AddStringToObject(o, "fw", fw_version());
		cJSON_AddNumberToObject(o, "protocol", CAMOS_PROTOCOL);
		cJSON *net = cJSON_AddObjectToObject(o, "net"); /* no sd on these boards */
		char ip[16];
		net_ip(ip, sizeof(ip));
		cJSON_AddStringToObject(net, "type", net_type_name());
		cJSON_AddStringToObject(net, "ip", ip);
		cJSON_AddNumberToObject(net, "rssi", net_rssi());
		net_cell_json(net);
		gps_state_json(o);
		cJSON_AddStringToObject(o, "thingName", g_cfg.thing_name);
		cJSON_AddNumberToObject(o, "shadowVersion", shadow_version());
		shadow_image_json(o);     /* video.image and capture, like firmware 0.25 */
		shadow_additions_json(o); /* power, live, kvs, location, net, battery (CONTRACTS section 3) */
	}
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (s) {
		pub(t_state, s, 1, true);
		free(s);
	}
}

/* Before a deep sleep a clean close sends no Last Will, so say it ourselves: the Last Will payload plus two additive
 * fields telling the cloud this is a planned sleep and when the camera is back (ms, UTC) */
void proto_goodbye(uint64_t wake_at_ms)
{
	char s[96];
	snprintf(s, sizeof(s), "{\"v\":1,\"online\":false,\"sleeping\":true,\"wakeAt\":%llu}", (unsigned long long)wake_at_ms);
	pub(t_state, s, 1, true);
}

bool proto_flush(int timeout_ms)
{
	for (int t = 0; client && t < timeout_ms; t += 100) {
		if (esp_mqtt_client_get_outbox_size(client) == 0) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(100));
	}
	return false;
}

/* Protocol 7 */
static void publish_telemetry(void)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", CAMOS_PROTOCOL);
	cJSON_AddNumberToObject(o, "ts", (double)now_ms());
	cJSON_AddNumberToObject(o, "uptimeS", (double)(esp_timer_get_time() / 1000000));
	cJSON_AddNumberToObject(o, "heapFree", (double)esp_get_free_heap_size());
	cJSON_AddNumberToObject(o, "psramFree", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	cJSON_AddNumberToObject(o, "rssi", net_rssi());
	cJSON_AddStringToObject(o, "net", net_type_name());
	cJSON_AddNumberToObject(o, "rebootCount", reboot_count());
	battery_sample();
	battery_json(o);
	power_json(o);
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (s) {
		pub(t_telemetry, s, 0, false);
		free(s);
	}
}

/* ------------------------------------------------------------------ RPC (protocol 9) */

static void rpc_reply(const char *id, cJSON *result, const char *code, const char *message)
{
	cJSON *o = cJSON_CreateObject();
	cJSON_AddNumberToObject(o, "v", CAMOS_PROTOCOL);
	cJSON_AddStringToObject(o, "id", id);
	if (code) {
		cJSON_AddFalseToObject(o, "ok");
		cJSON *e = cJSON_AddObjectToObject(o, "error");
		cJSON_AddStringToObject(e, "code", code);
		cJSON_AddStringToObject(e, "message", message ? message : code);
		cJSON_Delete(result);
	} else {
		cJSON_AddTrueToObject(o, "ok");
		cJSON_AddItemToObject(o, "result", result ? result : cJSON_CreateObject());
	}
	char *s = cJSON_PrintUnformatted(o);
	cJSON_Delete(o);
	if (s) {
		pub(t_res, s, 1, false);
		free(s);
	}
}

static cJSON *image_result(const upload_t *up)
{
	cJSON *r = cJSON_CreateObject();
	cJSON *img = cJSON_AddObjectToObject(r, "image"); /* protocol 9 `capture` shape, key = snapshotId */
	cJSON_AddStringToObject(img, "key", up->key);
	cJSON_AddNumberToObject(img, "w", up->w);
	cJSON_AddNumberToObject(img, "h", up->h);
	cJSON_AddNumberToObject(img, "bytes", (double)up->bytes);
	cJSON_AddNumberToObject(r, "bytes", (double)up->bytes);
	cJSON_AddStringToObject(r, "snapshotId", up->key); /* CONTRACTS */
	return r;
}

static void rpc_capture(const char *id)
{
	upload_t up;
	esp_err_t err = capture_now("rpc", &up);
	if (err == ESP_OK) {
		rpc_reply(id, image_result(&up), NULL, NULL);
	} else if (up.bytes) {
		rpc_reply(id, NULL, "UPLOAD_FAILED", up.error);
	} else if (err == ESP_ERR_INVALID_STATE) {
		rpc_reply(id, NULL, "BUSY", up.error);
	} else {
		rpc_reply(id, NULL, "INTERNAL", up.error[0] ? up.error : "camera");
	}
}

static void rpc_ota(const char *id)
{
	char fw[32], url[256], sha[80], err[96];
	if (!flybot_ota_check(fw, sizeof(fw), url, sizeof(url), sha, sizeof(sha))) {
		rpc_reply(id, NULL, "NOT_FOUND", "no update offered by GET /api/device/ota");
		return;
	}
	if (!strcmp(fw, fw_version())) {
		rpc_reply(id, NULL, "OLD_VERSION", fw);
		return;
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "version", fw);
	rpc_reply(id, r, NULL, NULL); /* protocol 9: a heavy command answers at once, the result is an event */
	proto_flush(3000);
	sched_keep_awake(600);
	esp_err_t e = ota_apply(url, sha, err, sizeof(err));
	char eid[27];
	cJSON *ev = evt_new("ota_result", eid);
	cJSON_AddStringToObject(ev, "target", "firmware");
	cJSON_AddStringToObject(ev, "version", fw);
	cJSON_AddBoolToObject(ev, "ok", e == ESP_OK);
	cJSON_AddStringToObject(ev, "stage", e == ESP_OK ? "installed" : "failed");
	if (e != ESP_OK) {
		cJSON_AddStringToObject(ev, "error", strstr(err, "sha256") ? "SIGNATURE_INVALID" : "DOWNLOAD_FAILED");
		cJSON_AddStringToObject(ev, "detail", err);
	}
	proto_publish_event(ev);
	if (e == ESP_OK) {
		proto_flush(3000);
		remember_reboot_reason("ota");
		esp_restart();
	}
}

static void rpc_handle(const char *json, TickType_t received)
{
	cJSON *req = cJSON_Parse(json);
	const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(req, "id"));
	const char *method = cJSON_GetStringValue(cJSON_GetObjectItem(req, "method"));
	cJSON *params = cJSON_GetObjectItem(req, "params");
	if (!id || !method) {
		cJSON_Delete(req);
		return;
	}
	/* The broker may deliver a command twice (QoS 1): answer each id once */
	static char recent[8][32];
	static int next;
	for (int i = 0; i < 8; i++) {
		if (!strcmp(recent[i], id)) {
			ESP_LOGI(TAG, "rpc %s (%s) already handled", method, id);
			cJSON_Delete(req);
			return;
		}
	}
	strlcpy(recent[next], id, sizeof(recent[0]));
	next = (next + 1) % 8;

	/* "The camera skips commands older than timeoutS, counted from receipt" (protocol 9) */
	cJSON *to = cJSON_GetObjectItem(req, "timeoutS");
	if (cJSON_IsNumber(to) && to->valuedouble > 0 && (xTaskGetTickCount() - received) * portTICK_PERIOD_MS > to->valuedouble * 1000) {
		ESP_LOGW(TAG, "rpc %s (%s) timed out in the queue, dropped", method, id);
		cJSON_Delete(req);
		return;
	}
	ESP_LOGI(TAG, "rpc %s (%s)", method, id);

	if (!strcmp(method, "ping")) {
		cJSON *r = cJSON_CreateObject();
		cJSON_AddNumberToObject(r, "uptimeS", (double)(esp_timer_get_time() / 1000000));
		rpc_reply(id, r, NULL, NULL);
	} else if (!strcmp(method, "state")) {
		proto_publish_state(true);
		rpc_reply(id, NULL, NULL, NULL);
	} else if (!strcmp(method, "capture") || !strcmp(method, "snapshot.now")) {
		/* params.detectionId (run the detection on the image) is ignored: counting is done in the cloud */
		rpc_capture(id);
	} else if (!strcmp(method, "live.start")) {
		cJSON *m = cJSON_GetObjectItem(params, "minutes");
		int minutes = cJSON_IsNumber(m) ? (int)m->valuedouble : 5;
		if (minutes < 1 || minutes > 60) {
			rpc_reply(id, NULL, "BAD_PARAMS", "minutes 1..60");
		} else {
			char err[96];
			esp_err_t e = live_start(minutes, err, sizeof(err));
			if (e == ESP_OK) {
				cJSON *r = cJSON_CreateObject();
				cJSON_AddStringToObject(r, "channelName", g_cfg.kvs_channel);
				cJSON_AddNumberToObject(r, "seconds", live_remaining_s());
				rpc_reply(id, r, NULL, NULL);
			} else {
				rpc_reply(id, NULL, e == ESP_ERR_NOT_SUPPORTED ? "NOT_SUPPORTED" : e == ESP_ERR_INVALID_STATE ? "BUSY" : "INTERNAL", err);
			}
		}
	} else if (!strcmp(method, "live.stop")) {
		live_stop();
		rpc_reply(id, NULL, NULL, NULL);
	} else if (!strcmp(method, "gps.fix")) {
		if (!gps_available()) {
			rpc_reply(id, NULL, "NOT_SUPPORTED", "no GPS on this board");
		} else {
			cJSON *tmo = cJSON_GetObjectItem(params, "timeoutS");
			int timeout = cJSON_IsNumber(tmo) ? (int)tmo->valuedouble : CONFIG_CAMOS_GPS_FIX_TIMEOUT_S;
			sched_keep_awake(timeout + 10);
			gps_fix_t f;
			if (gps_fix(timeout, &f)) {
				cJSON *r = cJSON_CreateObject();
				gps_json(r);
				cJSON_AddNumberToObject(r, "sats", f.sats);
				rpc_reply(id, r, NULL, NULL);
				proto_publish_reported_now();
			} else {
				rpc_reply(id, NULL, "TIMEOUT", "no fix");
			}
		}
	} else if (!strcmp(method, "ota.apply")) {
		rpc_ota(id);
	} else if (!strcmp(method, "reboot")) {
		rpc_reply(id, NULL, NULL, NULL);
		proto_flush(3000);
		remember_reboot_reason("rpc");
		esp_restart();
	} else if (!strcmp(method, "net.set")) {
		/* protocol 9: wifi {ssid, password}, cellular {apn} -> NVS; takes effect at the next boot */
		cJSON *wifi = cJSON_GetObjectItem(params, "wifi"), *cell = cJSON_GetObjectItem(params, "cellular");
		const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "ssid"));
		const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(wifi, "password"));
		const char *apn = cJSON_GetStringValue(cJSON_GetObjectItem(cell, "apn"));
		bool ok = true;
		if (ssid) {
			ok = store_apply_kv("ssid", ssid) && store_apply_kv("password", pass ? pass : "");
		}
		if (apn) {
			ok = ok && store_apply_kv("apn", apn);
		}
		rpc_reply(id, NULL, ok ? NULL : "BAD_PARAMS", ok ? NULL : "ssid/password/apn");
	} else if (!strcmp(method, "factory_reset")) {
		rpc_reply(id, NULL, NULL, NULL);
		proto_flush(3000);
		store_erase_all();
		esp_restart();
	} else {
		rpc_reply(id, NULL, "NOT_SUPPORTED", method); /* records, speaker, app, format_sd, ota.start ... */
	}
	cJSON_Delete(req);
}

static void rpc_task(void *arg)
{
	rpc_item_t it;
	for (;;) {
		if (xQueueReceive(rpc_queue, &it, portMAX_DELAY)) {
			rpc_handle(it.json, it.received); /* one at a time, in the order received */
			rpc_busy--;
			free(it.json);
		}
	}
}

/* ------------------------------------------------------------------ MQTT events */

static char *rx_buf;
static int rx_len, rx_total;
static char rx_topic[80];

static void on_message(const char *topic, const char *data, int len)
{
	if (!strcmp(topic, t_desired)) {
		char *reported = shadow_apply(data, len, true);
		if (reported) {
			proto_publish_reported(reported);
			free(reported);
		}
		settings_t s;
		shadow_get(&s);
		if (s.live_requested && live_supported() && !live_active()) {
			char err[96];
			if (live_start(5, err, sizeof(err)) != ESP_OK) {
				ESP_LOGW(TAG, "live.requested: %s", err);
			}
		}
	} else if (!strcmp(topic, t_req)) {
		rpc_item_t it = { .json = strndup(data, len), .received = xTaskGetTickCount() };
		rpc_busy++; /* before the send: the worker may finish first */
		if (!it.json || xQueueSend(rpc_queue, &it, 0) != pdTRUE) {
			rpc_busy--;
			cJSON *r = cJSON_Parse(data);
			const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(r, "id"));
			if (id) {
				rpc_reply(id, NULL, "BUSY", "queue full");
			}
			cJSON_Delete(r);
			free(it.json);
		}
	}
}

static void on_connected(void)
{
	led_set(LED_ONLINE);
	connected = true;
	down_since_ms = 0;
	connected_at_ms = esp_timer_get_time() / 1000;
	ESP_LOGI(TAG, "connected to %s", uri);
	esp_mqtt_client_subscribe_single(client, t_desired, 1);
	esp_mqtt_client_subscribe_single(client, t_req, 1);
	proto_publish_state(true);
	if (shadow_version() > 0) {
		proto_publish_reported_now();
	}
	if (battery_low_due()) {
		char id[27];
		cJSON *e = evt_new("low_power", id);
		cJSON_AddNumberToObject(e, "voltage", battery_mv() / 1000.0); /* volts */
		proto_publish_event(e);
	}
	static bool booted;
	if (!booted) {
		booted = true;
		esp_ota_mark_app_valid_cancel_rollback(); /* a new OTA image proves itself by reaching the broker */
		const char *reason = boot_reason_name();
		if (reason) {
			char id[27];
			cJSON *e = evt_new("boot", id);
			cJSON_AddStringToObject(e, "reason", reason);
			cJSON_AddStringToObject(e, "fw", fw_version());
			proto_publish_event(e);
		}
	}
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	esp_mqtt_event_handle_t ev = data;
	switch ((esp_mqtt_event_id_t)id) {
	case MQTT_EVENT_CONNECTED:
		on_connected();
		break;
	case MQTT_EVENT_DISCONNECTED:
		if (led_get() == LED_ONLINE) {
			led_set(LED_CONNECTING);
		}
		if (connected) {
			ESP_LOGW(TAG, "disconnected");
			down_since_ms = esp_timer_get_time() / 1000;
		}
		connected = false;
		break;
	case MQTT_EVENT_DATA:
		/* Long messages arrive in pieces: the topic only in the first one */
		if (ev->current_data_offset == 0) {
			free(rx_buf);
			rx_buf = NULL;
			rx_total = ev->total_data_len;
			rx_len = 0;
			if (rx_total > 16384 || ev->topic_len >= (int)sizeof(rx_topic) || !(rx_buf = malloc(rx_total + 1))) {
				rx_buf = NULL;
				break;
			}
			memcpy(rx_topic, ev->topic, ev->topic_len);
			rx_topic[ev->topic_len] = 0;
		}
		if (rx_buf && rx_len + ev->data_len <= rx_total) {
			memcpy(rx_buf + rx_len, ev->data, ev->data_len);
			rx_len += ev->data_len;
			if (rx_len == rx_total) {
				rx_buf[rx_len] = 0;
				on_message(rx_topic, rx_buf, rx_len);
				free(rx_buf);
				rx_buf = NULL;
			}
		}
		break;
	case MQTT_EVENT_ERROR:
		if (ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
			/* esp-tls and mbedtls codes: a wrong certificate shows up here (0x2700 range = X.509 verification) */
			ESP_LOGE(TAG, "transport error: esp-tls 0x%x, tls stack 0x%x, errno %d", ev->error_handle->esp_tls_last_esp_err,
					 ev->error_handle->esp_tls_stack_err, ev->error_handle->esp_transport_sock_errno);
		} else if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
			ESP_LOGE(TAG, "connection refused, code %d (policy or certificate not registered?)",
					 ev->error_handle->connect_return_code);
		}
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ supervisor: reconnect, telemetry, network watchdog */

/* Protocol 1: reconnect after 1, 2, 4 ... 300 s with +-20 % jitter. Not before the clock is set: the
 * server certificate would look "not yet valid". Telemetry every 5 min on Wi-Fi, 30 min on 4G (protocol 7). */
static void supervisor_task(void *arg)
{
	int backoff_s = 1;
	int64_t next_try = 0, next_telemetry = 0;
	bool started = false;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(1000));
		int64_t now = esp_timer_get_time() / 1000;
		if (connected) {
			backoff_s = 1;
			if (now >= next_telemetry) {
				publish_telemetry();
				next_telemetry = now + (net_type() == NET_4G ? 30 : 5) * 60 * 1000;
			}
			continue;
		}
		if (CONFIG_CAMOS_NET_WATCHDOG_MIN > 0 && net_started()) {
			if (!down_since_ms) {
				down_since_ms = now;
			}
			if (now - down_since_ms > (int64_t)CONFIG_CAMOS_NET_WATCHDOG_MIN * 60000) {
				ESP_LOGE(TAG, "no MQTT for %d minutes, restarting", CONFIG_CAMOS_NET_WATCHDOG_MIN);
				remember_reboot_reason("watchdog");
				esp_restart();
			}
		}
		if (!net_up() || !clock_synced()) {
			continue;
		}
		if (!started) {
			esp_mqtt_client_start(client);
			started = true;
			next_try = now + 15000;
		} else if (now >= next_try) {
			esp_mqtt_client_reconnect(client);
			int jitter = (int)(esp_random() % 41) - 20;
			next_try = now + (int64_t)backoff_s * 1000 * (100 + jitter) / 100;
			backoff_s = backoff_s * 2 > 300 ? 300 : backoff_s * 2;
		}
	}
}

/* Protocol 1: after a network change (Wi-Fi back) reconnect right away */
static void on_net_up(void)
{
	if (client && !connected) {
		esp_mqtt_client_reconnect(client);
	}
}

void proto_start(void)
{
	if (!g_cfg.cert || !g_cfg.key || !g_cfg.mqtt_host[0]) {
		ESP_LOGE(TAG, "not provisioned (certificate, key or IoT host missing): use the portal or tools/provision.sh. MQTT stays off");
		return;
	}
	snprintf(t_state, sizeof(t_state), "camos/%s/state", g_cfg.serial);
	snprintf(t_event, sizeof(t_event), "camos/%s/event", g_cfg.serial);
	snprintf(t_telemetry, sizeof(t_telemetry), "camos/%s/telemetry", g_cfg.serial);
	snprintf(t_desired, sizeof(t_desired), "camos/%s/shadow/desired", g_cfg.serial);
	snprintf(t_reported, sizeof(t_reported), "camos/%s/shadow/reported", g_cfg.serial);
	snprintf(t_req, sizeof(t_req), "camos/%s/rpc/req", g_cfg.serial);
	snprintf(t_res, sizeof(t_res), "camos/%s/rpc/res", g_cfg.serial);
	snprintf(uri, sizeof(uri), strchr(g_cfg.mqtt_host, ':') ? "mqtts://%s" : "mqtts://%s:8883", g_cfg.mqtt_host);

	esp_mqtt_client_config_t mc = {
		.broker.address.uri = uri,
		.broker.verification.certificate = root_ca_pem, /* Amazon Root CA 1 */
		.credentials.client_id = g_cfg.serial,
		.credentials.authentication.certificate = g_cfg.cert,
		.credentials.authentication.key = g_cfg.key,
		.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
		.session.keepalive = 60,
		.session.disable_clean_session = true, /* clean session false: keep subscriptions and queued QoS 1 commands */
		.session.last_will = { .topic = t_state, .msg = "{\"v\":1,\"online\":false}", .qos = 1, .retain = 1 },
		.network.disable_auto_reconnect = true, /* the supervisor does the back-off */
		.network.timeout_ms = 20000,
		.buffer.size = 4096,
		.buffer.out_size = 2048,
	};
	client = esp_mqtt_client_init(&mc);
	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
	rpc_queue = xQueueCreate(RPC_QUEUE_LEN, sizeof(rpc_item_t));
	xTaskCreate(rpc_task, "rpc", 12288, NULL, 5, NULL);
	xTaskCreate(supervisor_task, "mqtt-sup", 4096, NULL, 4, NULL);
	net_set_up_cb(on_net_up);
}
