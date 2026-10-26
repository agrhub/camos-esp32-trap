/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Serial console (UART0, 115200): every module can be exercised on the bench without the cloud.
 *
 *   help                      list
 *   info                      serial, verify code, label QR text, network, battery, clock, power mode
 *   cfg                       the configuration (secrets shown as set / not set)
 *   set <key> <value>         any portal key: ssid password apn simPin apiBase iotHost thingName kvsChannel kvsRegion
 *                             roleAlias credHost timezone intervalMin powerMode provisionToken token apPassword
 *   cert / key                paste a PEM, end with a line "END" (the console reads until it)
 *   net                       bring the network up (4G when an APN is set, else Wi-Fi) and show the state
 *   pair                      POST /api/device/pair with the stored provisioning token
 *   time                      SNTP / FlyBot time state
 *   snap                      capture + upload now (trigger "rpc"), prints the meta JSON and the result
 *   meta                      print the snapshot meta JSON only
 *   batt                      read the battery / solar ADCs, print the power decision
 *   mode auto|battery|powered power.mode (portal value; the shadow wins when it set one)
 *   gps [quiet] [timeout s]   take a fix now; quiet: camera, Wi-Fi and 4G off for it, then a restart
 *   at <command>              send an AT command to the modem (command mode or CMUX only)
 *   live [status|start [min]|stop] cloud live view (ESP32-S3): state, viewer, fps, bitrate
 *   ota                       GET /api/device/ota and apply it when offered
 *   awake <s>                 no deep sleep for s seconds (default 600)
 *   sleep <s>                 deep sleep now for s seconds (modem off first)
 *   portal                    restart into the setup portal
 *   factory                   erase everything but the serial and verify code, restart
 *   restart
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

#if CONFIG_CAMOS_CONSOLE

static int cmd_info(int argc, char **argv)
{
	char ts[25] = "unset", ip[16];
	if (clock_synced()) {
		iso8601(time(NULL), ts);
	}
	net_ip(ip, sizeof(ip));
	printf("serial %s  verify code %s  model %s  fw %s\n", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL, fw_version());
	printf("label QR: CAMOS:1;SN:%s;VC:%s;M:%s;;\n", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL);
	printf("paired %s  thing %s  iot %s\n", store_paired() ? "yes" : "no", g_cfg.thing_name, g_cfg.mqtt_host);
	printf("net %s %s ip %s rssi %d  mqtt %s\n", net_type_name(), net_up() ? "up" : "down", ip, net_rssi(),
		   proto_connected() ? "connected" : "down");
	printf("local name %s.local  (web console http://%s.local/ on the same Wi-Fi)\n", local_hostname(), local_hostname());
	printf("clock %s  uptime %lld s  free heap %u (min %u, largest block %u)  free PSRAM %u\n", ts,
		   (long long)(esp_timer_get_time() / 1000000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
		   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
	printf("timezone %s (from %s), clock from %s, public IP %s, claimed %s\n", tz_effective_iana(), tz_effective_source(),
		   clock_source(), g_public_ip[0] ? g_public_ip : "?", shadow_claimed() ? "yes" : "no");
	printf("battery %d mV %d %%  solar %d mV  charging %s  power.mode %s -> effective %s\n", battery_mv(), battery_pct(),
		   solar_mv(), battery_charging() ? "yes" : "no", power_mode_name(shadow_power_mode()), power_mode_name(power_effective()));
	time_t next = sched_next_capture();
	if (next) {
		iso8601(next, ts);
		printf("next capture %s (interval %d min)\n", ts, shadow_interval_min());
	}
	gps_fix_t f;
	if (gps_last(&f)) {
		iso8601(f.fix_at, ts);
		printf("location %.6f, %.6f (%s)\n", f.lat, f.lng, ts);
	}
	return 0;
}

static int cmd_cfg(int argc, char **argv)
{
	cJSON *o = cJSON_CreateObject();
	store_config_json(o, false);
	char *s = cJSON_Print(o);
	printf("%s\n", s);
	free(s);
	cJSON_Delete(o);
	return 0;
}

static int cmd_set(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: set <key> [value]\n");
		return 1;
	}
	/* values with spaces: join the rest */
	char value[256] = "";
	for (int i = 2; i < argc; i++) {
		if (i > 2) {
			strlcat(value, " ", sizeof(value));
		}
		strlcat(value, argv[i], sizeof(value));
	}
	if (!store_apply_kv(argv[1], value)) {
		printf("unknown key or bad value: %s\n", argv[1]);
		return 1;
	}
	printf("ok\n");
	return 0;
}

/* Reads lines from stdin until "END"; the REPL is paused meanwhile (linenoise is line based, so this reads raw lines) */
static char *read_pem(void)
{
	char *buf = calloc(1, 6000);
	if (!buf) {
		return NULL;
	}
	printf("paste the PEM, then a line END\n");
	char line[128];
	while (fgets(line, sizeof(line), stdin)) {
		if (!strncmp(line, "END", 3) && (line[3] == '\n' || line[3] == '\r' || line[3] == 0)) {
			break;
		}
		if (strlen(buf) + strlen(line) >= 5999) {
			break;
		}
		strcat(buf, line);
	}
	return buf;
}

static char *pending_cert;

static int cmd_cert(int argc, char **argv)
{
	free(pending_cert);
	pending_cert = read_pem();
	printf("certificate read (%u bytes); now `key`\n", pending_cert ? (unsigned)strlen(pending_cert) : 0);
	return 0;
}

static int cmd_key(int argc, char **argv)
{
	char *key = read_pem();
	bool ok = pending_cert && key && store_set_pem(pending_cert, key);
	printf("%s\n", ok ? "certificate and key stored" : "failed: run `cert` first, both must be PEM");
	free(key);
	free(pending_cert);
	pending_cert = NULL;
	return ok ? 0 : 1;
}

static int cmd_net(int argc, char **argv)
{
	net_ensure();
	for (int i = 0; i < 60 && !net_up(); i++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	char ip[16];
	net_ip(ip, sizeof(ip));
	printf("net %s %s ip %s rssi %d\n", net_type_name(), net_up() ? "up" : "down", ip, net_rssi());
	cJSON *o = cJSON_CreateObject();
	net_cell_json(o);
	char *s = cJSON_PrintUnformatted(o);
	printf("%s\n", s);
	free(s);
	cJSON_Delete(o);
	return 0;
}

static int cmd_pair(int argc, char **argv)
{
	if (argc > 1) {
		store_apply_kv("provisionToken", argv[1]);
	}
	net_ensure();
	if (!net_wait_up(90)) {
		printf("no network\n");
		return 1;
	}
	char err[96];
	esp_err_t e = flybot_pair(err, sizeof(err));
	if (e == ESP_OK) {
		printf("paired: thing %s, iot %s, kvs %s\n", g_cfg.thing_name, g_cfg.mqtt_host, g_cfg.kvs_channel);
	} else {
		printf("pair failed: %s\n", err);
	}
	return e == ESP_OK ? 0 : 1;
}

static int cmd_time(int argc, char **argv)
{
	char ts[25] = "unset";
	if (clock_synced()) {
		iso8601(time(NULL), ts);
	}
	printf("clock %s\n", ts);
	if (argc > 1 && !strcmp(argv[1], "flybot")) {
		printf("GET /api/device/time: %s\n", flybot_time(true) == ESP_OK ? "ok" : "failed");
	}
	return 0;
}

static int cmd_meta(int argc, char **argv)
{
	cJSON *m = flybot_meta_json("rpc", time(NULL));
	char *s = cJSON_Print(m);
	printf("%s\n", s);
	free(s);
	cJSON_Delete(m);
	return 0;
}

static int cmd_snap(int argc, char **argv)
{
	sched_keep_awake(180);
	upload_t up;
	esp_err_t e = capture_now("rpc", &up);
	if (e == ESP_OK) {
		printf("snapshot %s %dx%d %u bytes\n", up.key, up.w, up.h, (unsigned)up.bytes);
	} else {
		printf("failed: %s (JPEG %dx%d %u bytes)\n", up.error, up.w, up.h, (unsigned)up.bytes);
	}
	return e == ESP_OK ? 0 : 1;
}

/* frame [w h [quality]]: one JPEG at the given size through the live-view stream path, size and timing printed */
static int cmd_frame(int argc, char **argv)
{
	int w = argc > 2 ? atoi(argv[1]) : 640, h = argc > 2 ? atoi(argv[2]) : 480, q = argc > 3 ? atoi(argv[3]) : 12;
	int64_t t0 = esp_timer_get_time();
	esp_err_t e = cam_stream_start(w, h, q);
	if (e != ESP_OK) {
		printf("camera init failed 0x%x\n", e);
		return 1;
	}
	int64_t t1 = esp_timer_get_time();
	for (int i = 0; i < 3; i++) { /* let exposure settle */
		camera_fb_t *f = esp_camera_fb_get();
		if (f) {
			esp_camera_fb_return(f);
		}
	}
	int64_t t2 = esp_timer_get_time();
	camera_fb_t *fb = esp_camera_fb_get();
	int64_t t3 = esp_timer_get_time();
	if (!fb) {
		printf("no frame\n");
	} else {
		int fw = 0, fh = 0;
		jpeg_dims(fb->buf, fb->len, &fw, &fh);
		printf("frame %dx%d q%d: %u bytes; init %lld ms, 3 warm-up frames %lld ms, frame %lld ms; free heap %u, free PSRAM %u\n",
			   fw, fh, q, (unsigned)fb->len, (long long)((t1 - t0) / 1000), (long long)((t2 - t1) / 1000),
			   (long long)((t3 - t2) / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
			   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
		esp_camera_fb_return(fb);
	}
	cam_stream_stop();
	return fb ? 0 : 1;
}

/* cam regs | cam night on|off|auto | cam reg <hex addr> [hex value]: the sensor's state, for diagnosis */
static int cmd_cam(int argc, char **argv)
{
	if (argc >= 3 && !strcmp(argv[1], "night")) {
		cam_set_night_override(!strcmp(argv[2], "on") ? 1 : !strcmp(argv[2], "off") ? 0 : -1);
		printf("night mode %s for the next stream start\n", argv[2]);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "reg")) {
		int addr = (int)strtol(argv[2], NULL, 16), val = argc > 3 ? (int)strtol(argv[3], NULL, 16) : -1;
		int r = cam_reg(addr, val);
		if (r < 0) {
			printf("camera off\n");
			return 1;
		}
		printf("0x%04x = 0x%02x\n", addr, r);
		return 0;
	}
	cam_print_regs();
	return 0;
}

static int cmd_batt(int argc, char **argv)
{
	battery_sample();
	cJSON *o = cJSON_CreateObject();
	battery_json(o);
	power_json(o);
	char *s = cJSON_PrintUnformatted(o);
	printf("%s\n", s);
	free(s);
	cJSON_Delete(o);
	return 0;
}

static int cmd_mode(int argc, char **argv)
{
	if (argc < 2 || !store_apply_kv("powerMode", argv[1])) {
		printf("usage: mode auto|battery|powered (now %s, effective %s)\n", power_mode_name(shadow_power_mode()),
			   power_mode_name(power_effective()));
		return 1;
	}
	printf("power.mode %s, effective %s\n", power_mode_name(shadow_power_mode()), power_mode_name(power_effective()));
	sched_changed();
	return 0;
}

/* usb [auto|storage|webcam|off]: the native USB port's mode (applied at the next boot) */
static int cmd_usb(int argc, char **argv)
{
	if (argc < 2) {
		cJSON *o = cJSON_CreateObject();
		usbdev_json(o);
		char *j = cJSON_PrintUnformatted(cJSON_GetObjectItem(o, "usb"));
		printf("usb %s\n", j ? j : "{}");
		cJSON_free(j);
		cJSON_Delete(o);
		return 0;
	}
	if (!usb_mode_valid(argv[1]) || !store_apply_kv("usbMode", argv[1])) {
		printf("usage: usb auto|storage|webcam|off\n");
		return 1;
	}
	printf("usbMode %s: restart to apply (command restart)\n", g_cfg.usb_mode);
	return 0;
}

static int cmd_gps(int argc, char **argv)
{
	bool quiet = argc > 1 && !strcmp(argv[1], "quiet");
	int timeout = argc > 1 + quiet ? atoi(argv[1 + quiet]) : CONFIG_CAMOS_GPS_FIX_TIMEOUT_S;
	sched_keep_awake(timeout + 10);
	if (quiet) {
		/* nothing else on the air or on the board's clocks: the GPS hears only the sky (and the supply) */
		printf("quiet fix: live view, motion, Wi-Fi and 4G off; the camera restarts after it\n");
		gps_set_quiet(true);
		live_stop();
		wifi_stop();
		modem_stop();
		vTaskDelay(pdMS_TO_TICKS(2000)); /* motion lets the camera go */
	}
	gps_fix_t f;
	bool ok = gps_fix(timeout, &f);
	if (ok) {
		char ts[25];
		iso8601(f.fix_at, ts);
		printf("fix %.6f, %.6f, %d sats, %s\n", f.lat, f.lng, f.sats, ts);
	} else {
		printf("no fix\n");
	}
	if (quiet) {
		printf("restarting to bring the network back\n");
		vTaskDelay(pdMS_TO_TICKS(3000));
		esp_restart();
	}
	return ok ? 0 : 1;
}

static int cmd_at(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: at <AT command>\n");
		return 1;
	}
	char cmd[128] = "", out[256];
	for (int i = 1; i < argc; i++) {
		strlcat(cmd, argv[i], sizeof(cmd));
	}
	int r = modem_at(cmd, out, sizeof(out), 3000);
	if (r == -1) {
		printf("modem not started (run `net` with an APN set)\n");
	} else if (r == -2) {
		printf("UART busy with PPP (no CMUX): stop the network first\n");
	} else {
		printf("%s\n%s\n", out, r == 0 ? "OK" : "ERROR");
	}
	return r == 0 ? 0 : 1;
}

#if CONFIG_CAMOS_LIVE_BENCH_CREDS
esp_err_t live_kvs_bench_key(const char *field, const char *value);
#endif

static int cmd_live(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "status")) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "supported", live_supported());
		cJSON_AddBoolToObject(o, "window", live_active());
		cJSON_AddNumberToObject(o, "remainingS", live_remaining_s());
		live_status_json(o);
		char *t = cJSON_PrintUnformatted(o);
		printf("%s\n", t ? t : "{}");
		free(t);
		cJSON_Delete(o);
		return 0;
	}
	if (!strcmp(argv[1], "stop")) {
		live_stop();
		printf("stopped\n");
		return 0;
	}
#if CONFIG_CAMOS_LIVE_BENCH_CREDS
	if (!strcmp(argv[1], "key") && argc >= 3) {
		esp_err_t e = live_kvs_bench_key(argv[2], argc > 3 ? argv[3] : NULL);
		printf("%s\n", e == ESP_OK ? "ok" : esp_err_to_name(e)); /* the value is never echoed */
		return e == ESP_OK ? 0 : 1;
	}
#endif
	if (strcmp(argv[1], "start")) {
		printf("usage: live [status] | start [min] | stop\n");
		return 1;
	}
	char err[96];
	int minutes = argc > 2 ? atoi(argv[2]) : 5;
	esp_err_t e = live_start(minutes, err, sizeof(err));
	if (e == ESP_OK) {
		printf("live for %d min\n", minutes);
	} else {
		printf("failed: %s\n", err);
	}
	return e == ESP_OK ? 0 : 1;
}

static void ota_progress_print(int pct, void *ctx)
{
	(void)ctx;
	printf("downloaded %d %%\n", pct);
}

static int cmd_ota(int argc, char **argv)
{
	char fw[32], url[256], sha[80], err[96];
	net_ensure();
	if (!net_wait_up(90)) {
		printf("no network\n");
		return 1;
	}
	if (!flybot_ota_check(fw, sizeof(fw), url, sizeof(url), sha, sizeof(sha))) {
		printf("no update offered\n");
		return 0;
	}
	printf("offered %s from %s\n", fw, url);
	sched_keep_awake(600);
	esp_err_t e = ota_apply(url, sha, err, sizeof(err), ota_progress_print, NULL);
	if (e != ESP_OK) {
		printf("failed: %s\n", err);
		return 1;
	}
	printf("installed, restarting\n");
	vTaskDelay(pdMS_TO_TICKS(500));
	remember_reboot_reason("ota"); /* the boot event says why, as after ota.apply */
	esp_restart();
	return 0;
}

#if CONFIG_CAMOS_CONSOLE_CRASH_TEST
/* Test builds: crash on purpose, to check the crash report FlyBot receives after the restart */
static int cmd_crash(int argc, char **argv)
{
	ESP_LOGE("console", "crash test: abort() now");
	vTaskDelay(pdMS_TO_TICKS(200));
	abort();
	return 0;
}
#endif

static int cmd_awake(int argc, char **argv)
{
	int s = argc > 1 ? atoi(argv[1]) : 600;
	sched_keep_awake(s);
	printf("no deep sleep for %d s\n", s);
	return 0;
}

static int cmd_sleep(int argc, char **argv)
{
	int s = argc > 1 ? atoi(argv[1]) : 60;
	printf("deep sleep %d s\n", s);
	proto_goodbye(now_ms() + (uint64_t)s * 1000);
	proto_flush(3000);
	net_sleep_prepare();
	cam_sleep_prepare();
	gps_sleep_prepare();
	esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
	esp_deep_sleep_start();
	return 0;
}

/* Bench: the portal's Connect check from the console (no phone needed); without arguments the stored network */
static int cmd_connect(int argc, char **argv)
{
	if (!portal_active()) {
		printf("only in the setup portal (command `portal`)\n");
		return 1;
	}
	const char *ssid = argc > 1 ? argv[1] : g_cfg.wifi_ssid;
	const char *pass = argc > 2 ? argv[2] : (strcmp(ssid, g_cfg.wifi_ssid) ? "" : g_cfg.wifi_pass);
	esp_err_t e = portal_try(ssid, pass);
	printf(e == ESP_OK ? "checking \"%s\" (GET /api/connect, or watch the log)\n" : "busy (%s)\n", e == ESP_OK ? ssid : esp_err_to_name(e));
	return e == ESP_OK ? 0 : 1;
}

static int cmd_portal(int argc, char **argv)
{
	store_set_flag("portal", 1);
	esp_restart();
	return 0;
}

static int cmd_factory(int argc, char **argv)
{
	store_erase_all();
	esp_restart();
	return 0;
}

/* led [portal|connecting|online|error|ota|off|capture|uploaderr|lowbatt|on|disable]: show a state to check the colours */
static int cmd_led(int argc, char **argv)
{
	if (argc < 2) {
		printf("led %s, state %s\n", led_enabled() ? "enabled" : "disabled", led_state_name(led_get()));
		return 0;
	}
	const char *a = argv[1];
	static const struct { const char *name; int state; } states[] = {
		{ "off", LED_OFF }, { "portal", LED_PORTAL }, { "connecting", LED_CONNECTING }, { "online", LED_ONLINE },
		{ "error", LED_ERROR }, { "ota", LED_OTA },
	};
	for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		if (!strcmp(a, states[i].name)) {
			led_set((led_state_t)states[i].state);
			return 0;
		}
	}
	if (!strcmp(a, "capture")) {
		led_flash(LED_FLASH_CAPTURE);
	} else if (!strcmp(a, "uploaderr")) {
		led_flash(LED_FLASH_ERROR);
	} else if (!strcmp(a, "lowbatt")) {
		led_flash(LED_FLASH_LOWBATT);
	} else if (!strcmp(a, "on")) {
		store_apply_kv("ledEnabled", "1");
	} else if (!strcmp(a, "disable")) {
		store_apply_kv("ledEnabled", "0");
	} else {
		printf("usage: led [portal|connecting|online|error|ota|off|capture|uploaderr|lowbatt|on|disable]\n");
		return 1;
	}
	return 0;
}

static int cmd_scan(int argc, char **argv)
{
	wifi_net_t nets[WIFI_SCAN_MAX];
	int n = wifi_scan(nets, WIFI_SCAN_MAX);
	if (n < 0) {
		printf("Wi-Fi not running (4G mode?)\n");
		return 1;
	}
	web_scan_cache_set(nets, n);
	for (int i = 0; i < n; i++) {
		printf("%2d  %-32s %4d dBm  ch %2d  %s\n", i + 1, nets[i].ssid, nets[i].rssi, nets[i].channel, nets[i].secure ? "secure" : "open");
	}
	printf("%d networks\n", n);
	return 0;
}

static int cmd_qr(int argc, char **argv)
{
	char text[200];
	if (argc > 1 && !strcmp(argv[1], "label")) {
		snprintf(text, sizeof(text), "CAMOS:1;SN:%s;VC:%s;M:%s;;", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL);
	} else {
		store_claim_url(text, sizeof(text));
	}
	printf("%s\n", text);
	qr_print(text);
	char *svg = qr_svg(text);
	printf("svg %u bytes\n", svg ? (unsigned)strlen(svg) : 0);
	free(svg);
	return 0;
}

/* csr: the CSR registration would send (makes the device key once if there is none), for openssl req -verify */
static int cmd_csr(int argc, char **argv)
{
	char *pem = register_csr_pem();
	if (!pem) {
		printf("CSR failed\n");
		return 1;
	}
	printf("%s", pem);
	free(pem);
	return 0;
}

static int cmd_reg(int argc, char **argv)
{
	printf("certificate %s, key %s, token %s, claimed %s, registration %s%s%s\n", g_cfg.cert ? "yes" : "no", g_cfg.key ? "yes" : "no",
		   g_cfg.token[0] ? "yes" : "no", shadow_claimed() ? "yes" : "no", register_pending() ? "pending" : "idle",
		   g_reg_error[0] ? ", last error: " : "", g_reg_error);
	char url[200];
	store_claim_url(url, sizeof(url));
	printf("claim at %s (serial %s, verify code %s)\n", url, g_cfg.serial, g_cfg.verify_code);
	if (argc > 1 && !strcmp(argv[1], "start")) {
		if (!strcmp(g_reg_error, "serial_taken")) {
			g_reg_error[0] = 0; /* ask now, not after the wait a refusal set */
			store_set_str("reg_err", "");
			store_set_flag("reg_retry", 0);
			store_set_flag("reg_taken_s", 0); /* and a new refusal waits 1 h again */
		}
		register_start();
	}
	return 0;
}

static int cmd_restart(int argc, char **argv)
{
	esp_restart();
	return 0;
}

/* Automatic light sleep (main.c power_setup) stops UART0 between Wi-Fi beacons, and characters typed then are lost.
 * UART0 wakes the chip (the first character of a burst may be lost), and the chip stays out of light sleep for
 * CONSOLE_AWAKE_S after boot and after every command, so an interactive session is reliable. */
#define CONSOLE_AWAKE_S 300
static esp_pm_lock_handle_t pm_lock;
static esp_timer_handle_t pm_timer;
static bool pm_held;

static void pm_release(void *arg)
{
	if (pm_held) {
		esp_pm_lock_release(pm_lock);
		pm_held = false;
	}
}

static void console_touch(void)
{
	if (!pm_lock) {
		return;
	}
	if (!pm_held) {
		esp_pm_lock_acquire(pm_lock);
		pm_held = true;
	}
	esp_timer_stop(pm_timer);
	esp_timer_start_once(pm_timer, (uint64_t)CONSOLE_AWAKE_S * 1000000ULL);
}

static int dispatch(void *ctx, int argc, char **argv)
{
	console_touch();
	return ((esp_console_cmd_func_t)ctx)(argc, argv);
}

static void console_pm_setup(void)
{
#if CONFIG_PM_ENABLE
	if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "console", &pm_lock) != ESP_OK) {
		pm_lock = NULL;
		return;
	}
	const esp_timer_create_args_t a = { .callback = pm_release, .name = "console-pm" };
	esp_timer_create(&a, &pm_timer);
	uart_set_wakeup_threshold(UART_NUM_0, 3);
	esp_sleep_enable_uart_wakeup(UART_NUM_0);
	console_touch(); /* the first minutes after boot are the bench session */
#endif
}

void console_start(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	rc.prompt = "camos>";
	rc.max_cmdline_length = 256;
	rc.task_stack_size = 12288; /* snap / pair run inside the console task */
	esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
	if (esp_console_new_repl_uart(&uc, &rc, &repl) != ESP_OK) {
		return;
	}
	esp_console_register_help_command();
	const esp_console_cmd_t cmds[] = {
		{ .command = "info", .help = "serial, network, battery, clock, power mode", .func = cmd_info },
		{ .command = "cfg", .help = "configuration (secrets as set / not set)", .func = cmd_cfg },
		{ .command = "set", .help = "set <key> <value>: ssid password apn apiBase iotHost intervalMin powerMode timezone ...", .func = cmd_set },
		{ .command = "cert", .help = "paste the device certificate PEM, end with END", .func = cmd_cert },
		{ .command = "key", .help = "paste the device private key PEM, end with END", .func = cmd_key },
		{ .command = "net", .help = "bring the network up and show it", .func = cmd_net },
		{ .command = "pair", .help = "pair [provisionToken]: POST /api/device/pair", .func = cmd_pair },
		{ .command = "time", .help = "time [flybot]: clock state, or sync from FlyBot", .func = cmd_time },
		{ .command = "snap", .help = "capture and upload now", .func = cmd_snap },
		{ .command = "meta", .help = "print the snapshot meta JSON", .func = cmd_meta },
		{ .command = "cam", .help = "cam [regs] | cam night on|off|auto | cam reg <hex addr> [hex value]: sensor state", .func = cmd_cam },
		{ .command = "frame", .help = "frame [w h [q]]: one JPEG at that size (default 640 480 12), size and timings", .func = cmd_frame },
		{ .command = "batt", .help = "read battery / solar, print the power decision", .func = cmd_batt },
		{ .command = "mode", .help = "mode auto|battery|powered", .func = cmd_mode },
		{ .command = "usb", .help = "usb [auto|storage|webcam|off]: native USB port mode (at the next boot)", .func = cmd_usb },
		{ .command = "gps", .help = "gps [quiet] [timeout]: take a fix; quiet: camera, Wi-Fi and 4G off, then restart", .func = cmd_gps },
		{ .command = "at", .help = "at <command>: send to the modem", .func = cmd_at },
		{ .command = "live", .help = "live [status] | start [min] | stop", .func = cmd_live },
		{ .command = "ota", .help = "check GET /api/device/ota and apply", .func = cmd_ota },
		{ .command = "awake", .help = "awake [s]: no deep sleep for a while", .func = cmd_awake },
#if CONFIG_CAMOS_CONSOLE_CRASH_TEST
		{ .command = "crash", .help = "crash: abort on purpose (test builds only)", .func = cmd_crash },
#endif
		{ .command = "sleep", .help = "sleep [s]: deep sleep now", .func = cmd_sleep },
		{ .command = "portal", .help = "restart into the setup portal", .func = cmd_portal },
		{ .command = "connect", .help = "connect [ssid [password]]: in the portal, check a network like the page's Connect", .func = cmd_connect },
		{ .command = "factory", .help = "erase the configuration (keeps serial and verify code)", .func = cmd_factory },
		{ .command = "led", .help = "led [state|capture|uploaderr|lowbatt|on|disable]: show an LED state", .func = cmd_led },
		{ .command = "scan", .help = "Wi-Fi scan (APSTA in the portal: the SoftAP stays up)", .func = cmd_scan },
		{ .command = "qr", .help = "qr [label]: the claim URL (or label) QR on the console", .func = cmd_qr },
		{ .command = "csr", .help = "print the registration CSR (makes the device key once)", .func = cmd_csr },
		{ .command = "reg", .help = "reg [start]: registration / claim state", .func = cmd_reg },
		{ .command = "restart", .help = "restart", .func = cmd_restart },
	};
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		esp_console_cmd_t c = cmds[i];
		c.func_w_context = dispatch; /* every command keeps the chip awake for a while (light sleep loses input) */
		c.context = (void *)cmds[i].func;
		c.func = NULL;
		esp_console_cmd_register(&c);
	}
	console_pm_setup();
	esp_console_start_repl(repl);
}

#else
void console_start(void) {}
#endif
