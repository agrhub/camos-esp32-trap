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
 *   gps [timeout s]           take a fix now
 *   at <command>              send an AT command to the modem (command mode or CMUX only)
 *   live start [min] | stop   live view (ESP32-S3)
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
	printf("clock %s  uptime %lld s  free heap %u (min %u)  free PSRAM %u\n", ts, (long long)(esp_timer_get_time() / 1000000),
		   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
		   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
		printf("GET /api/device/time: %s\n", flybot_time() == ESP_OK ? "ok" : "failed");
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

static int cmd_gps(int argc, char **argv)
{
	int timeout = argc > 1 ? atoi(argv[1]) : CONFIG_CAMOS_GPS_FIX_TIMEOUT_S;
	sched_keep_awake(timeout + 10);
	gps_fix_t f;
	if (!gps_fix(timeout, &f)) {
		printf("no fix\n");
		return 1;
	}
	char ts[25];
	iso8601(f.fix_at, ts);
	printf("fix %.6f, %.6f, %d sats, %s\n", f.lat, f.lng, f.sats, ts);
	return 0;
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

static int cmd_live(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "stop")) {
		live_stop();
		printf("stopped\n");
		return 0;
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
	esp_err_t e = ota_apply(url, sha, err, sizeof(err));
	if (e != ESP_OK) {
		printf("failed: %s\n", err);
		return 1;
	}
	printf("installed, restarting\n");
	vTaskDelay(pdMS_TO_TICKS(500));
	esp_restart();
	return 0;
}

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
	esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
	esp_deep_sleep_start();
	return 0;
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

static int cmd_restart(int argc, char **argv)
{
	esp_restart();
	return 0;
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
		{ .command = "frame", .help = "frame [w h [q]]: one JPEG at that size (default 640 480 12), size and timings", .func = cmd_frame },
		{ .command = "batt", .help = "read battery / solar, print the power decision", .func = cmd_batt },
		{ .command = "mode", .help = "mode auto|battery|powered", .func = cmd_mode },
		{ .command = "gps", .help = "gps [timeout]: take a fix", .func = cmd_gps },
		{ .command = "at", .help = "at <command>: send to the modem", .func = cmd_at },
		{ .command = "live", .help = "live start [min] | stop", .func = cmd_live },
		{ .command = "ota", .help = "check GET /api/device/ota and apply", .func = cmd_ota },
		{ .command = "awake", .help = "awake [s]: no deep sleep for a while", .func = cmd_awake },
		{ .command = "sleep", .help = "sleep [s]: deep sleep now", .func = cmd_sleep },
		{ .command = "portal", .help = "restart into the setup portal", .func = cmd_portal },
		{ .command = "factory", .help = "erase the configuration (keeps serial and verify code)", .func = cmd_factory },
		{ .command = "restart", .help = "restart", .func = cmd_restart },
	};
	for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
		esp_console_cmd_register(&cmds[i]);
	}
	esp_console_start_repl(repl);
}

#else
void console_start(void) {}
#endif
