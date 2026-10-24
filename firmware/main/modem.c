/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * SIMCom A7680C (LTE Cat.1) over UART with esp_modem. The A76xx AT set is the SIM7600 set for everything this
 * firmware needs (ATE0, AT+CPIN?, AT+CSQ, AT+CGDCONT, ATD*99#, AT+CMUX, AT+CPOF), so the SIM7600 DCE profile is
 * used; what actually answered on hardware is to be recorded in docs/WIRING.md once a module is on the bench.
 *
 *   power on   PWRKEY asserted 1.5 s (datasheet: >= 1 s), then AT every second for CAMOS_MODEM_BOOT_WAIT_S
 *   data       CMUX first (AT + PPP on one UART, so CSQ can still be read), plain PPP when CMUX fails
 *   power off  AT+CPOF (esp_modem_power_down), then PWRKEY 3 s if it still answers, then the rail switch
 *
 * A breakout with neither PWRKEY nor a rail switch wired (setting modemPwrkey and POWER_GPIO -1, e.g. the TDM2309 A7680C board,
 * which powers up by itself) could not be switched on again after AT+CPOF: there "power off" is AT+CFUN=0 (radio
 * off, the module keeps answering AT) and the start turns the radio back on with AT+CFUN=1.
 *
 * Without CAMOS_MODEM (ESP32-CAM preset) every function is a stub and the network is Wi-Fi.
 */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"
#include "apn.h"
#include "nmea.h"
#include <sys/time.h>

static const char *TAG = "modem";

#if CAMOS_MODEM
#include "esp_modem_api.h"
#include "esp_netif_ppp.h"

static esp_modem_dce_t *dce;
static esp_netif_t *ppp_netif;
static volatile bool s_up;
static bool cmux_mode;
static int last_csq = 99;
static char imei[32], iccid[32], oper[40];
/* nothing wired can switch the module on again: never AT+CPOF it (see the top of the file) */
#define NO_POWER_CONTROL (g_cfg.modem_pwrkey < 0 && CONFIG_CAMOS_MODEM_POWER_GPIO < 0)
static bool registered;

static void gpio_out(int pin, int level)
{
	if (pin < 0) {
		return;
	}
	gpio_hold_dis(pin);
	gpio_set_direction(pin, GPIO_MODE_OUTPUT);
	gpio_set_level(pin, level);
}

static void rail(bool on)
{
#if CONFIG_CAMOS_MODEM_POWER_GPIO >= 0
	int active_low =
#ifdef CONFIG_CAMOS_MODEM_POWER_ACTIVE_LOW
		1;
#else
		0;
#endif
	gpio_out(CONFIG_CAMOS_MODEM_POWER_GPIO, on != active_low);
	if (on) {
		vTaskDelay(pdMS_TO_TICKS(200)); /* the 4 V rail and its bulk capacitor */
	}
#endif
	(void)on;
}

static void pwrkey(int ms)
{
	if (g_cfg.modem_pwrkey < 0) {
		return;
	}
	int asserted =
#ifdef CONFIG_CAMOS_MODEM_PWRKEY_ACTIVE_HIGH
		1;
#else
		0;
#endif
	gpio_out(g_cfg.modem_pwrkey, asserted);
	vTaskDelay(pdMS_TO_TICKS(ms));
	gpio_set_level(g_cfg.modem_pwrkey, !asserted);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (id == IP_EVENT_PPP_GOT_IP) {
		ip_event_got_ip_t *ev = data;
		ESP_LOGI(TAG, "PPP up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
		s_up = true;
	} else if (id == IP_EVENT_PPP_LOST_IP) {
		ESP_LOGW(TAG, "PPP lost IP");
		s_up = false;
	}
}

static void on_ppp_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	/* NETIF_PPP_ERRORNONE (0) comes with a successful connection, around IP_EVENT_PPP_GOT_IP (seen after it on an
	 * A7680C, 22 Oct 2026): only the other codes end the link */
	if (id > NETIF_PPP_ERRORNONE && id < NETIF_PP_PHASE_OFFSET) {
		ESP_LOGW(TAG, "PPP status %d", (int)id); /* NETIF_PPP_ERRORUSER = a clean exit */
		s_up = false;
	}
}

/* 4G is tried when the build has the modem, 4G is on (defaults: on, APN auto) and no earlier boot found the module
 * missing ("modem_absent", cleared when someone sets the APN / 4G in the portal or console) */
bool modem_available(void)
{
	return CONFIG_CAMOS_MODEM_TX_GPIO >= 0 && CONFIG_CAMOS_MODEM_RX_GPIO >= 0 && g_cfg.cellular &&
		   !store_get_flag("modem_absent");
}

static modem_fail_t last_fail;
modem_fail_t modem_last_failure(void) { return last_fail; }
static char apn_used[64];
const char *modem_apn(void) { return apn_used; }

bool modem_up(void) { return s_up; }

/* The 4G cell's position from SIMCom's location service: AT+CLBS=1,<cid> answers OK, then
 * "+CLBS: <code>,<lat>,<lng>,<accuracy m>" a few seconds later (code 0 = found). Older SIMCom modules give the
 * longitude first, so a first value outside -90..90 is taken as the longitude. Needs the data context up. */
bool modem_cell_location(double *lat, double *lng, int *acc_m)
{
	if (!dce || !s_up) {
		return false;
	}
	char out[CONFIG_ESP_MODEM_C_API_STR_MAX] = "";
	if (esp_modem_at_raw(dce, "AT+CLBS=1,1\r", out, "+CLBS:", "ERROR", 30000) != ESP_OK) {
		ESP_LOGW(TAG, "AT+CLBS: no answer");
		return false;
	}
	/* the pass phrase ends the read; the URC comes as one line, so the numbers are normally there */
	const char *p = strstr(out, "+CLBS:");
	int code = -1, acc = 0;
	double a = 0, b = 0;
	if (!p || sscanf(p, "+CLBS: %d,%lf,%lf,%d", &code, &a, &b, &acc) < 3) {
		ESP_LOGW(TAG, "AT+CLBS: unexpected answer \"%s\"", out);
		return false;
	}
	if (code != 0) {
		ESP_LOGW(TAG, "AT+CLBS: no position (code %d)", code);
		return false;
	}
	if (fabs(a) > 90) {
		double t = a;
		a = b;
		b = t;
	}
	*lat = a;
	*lng = b;
	*acc_m = acc;
	return true;
}

int modem_rssi(void)
{
	return last_csq >= 0 && last_csq <= 31 ? -113 + 2 * last_csq : 0;
}

void modem_ip(char *out, size_t n)
{
	esp_netif_ip_info_t info;
	if (ppp_netif && esp_netif_get_ip_info(ppp_netif, &info) == ESP_OK) {
		snprintf(out, n, IPSTR, IP2STR(&info.ip));
	} else {
		out[0] = 0;
	}
}

/* IMEI and ICCID kept in NVS (blob "cellids"), so the state carries them while the camera is on Wi-Fi too */
typedef struct {
	char imei[24];
	char iccid[24];
} cell_ids_t;

static void save_ids(void)
{
	cell_ids_t now = { 0 }, saved = { 0 };
	strlcpy(now.imei, imei, sizeof(now.imei));
	strlcpy(now.iccid, iccid, sizeof(now.iccid));
	if (!now.imei[0] && !now.iccid[0]) {
		return;
	}
	if (!store_get_blob("cellids", &saved, sizeof(saved)) || memcmp(&saved, &now, sizeof(now)) != 0) {
		store_set_blob("cellids", &now, sizeof(now));
	}
}

void modem_saved_ids(char *out_imei, size_t in, char *out_iccid, size_t cn)
{
	cell_ids_t saved = { 0 };
	if (imei[0] || iccid[0]) {
		strlcpy(saved.imei, imei, sizeof(saved.imei));
		strlcpy(saved.iccid, iccid, sizeof(saved.iccid));
	} else if (!store_get_blob("cellids", &saved, sizeof(saved))) {
		memset(&saved, 0, sizeof(saved));
	}
	saved.imei[sizeof(saved.imei) - 1] = saved.iccid[sizeof(saved.iccid) - 1] = 0;
	strlcpy(out_imei, saved.imei, in);
	strlcpy(out_iccid, saved.iccid, cn);
}

void modem_cell_json(cJSON *net)
{
	cJSON *cell = cJSON_AddObjectToObject(net, "cell"); /* protocol 4 */
	cJSON_AddStringToObject(cell, "imei", imei);
	cJSON_AddStringToObject(cell, "iccid", iccid);
	cJSON_AddStringToObject(cell, "operator", oper);
	cJSON_AddNumberToObject(cell, "csq", last_csq);
	cJSON_AddBoolToObject(cell, "registered", registered);
}

/* Reads the signal quality; in CMUX mode this works while PPP is up */
static void read_csq(void)
{
	int rssi = 99, ber = 99;
	if (dce && esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
		last_csq = rssi;
	}
}

static volatile int start_gen; /* one csq_task per successful modem_start (a recovery starts the modem again) */

static void csq_task(void *arg)
{
	while (dce && cmux_mode && (int)(intptr_t)arg == start_gen) {
		vTaskDelay(pdMS_TO_TICKS(60000));
		read_csq();
	}
	vTaskDelete(NULL);
}

static bool wait_at(int seconds)
{
	for (int i = 0; i < seconds; i++) {
		if (esp_modem_sync(dce) == ESP_OK) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	return false;
}

/* ---------------------------------------------------------------- UART rate */

#define N_RATES 3
static int uart_baud = CONFIG_CAMOS_MODEM_BAUD; /* the rate the ESP32 side runs at */

/* The rates above the configured one, fastest first: modemFastBaud, then half and a quarter of it (the TDM2309 A7680C
 * board level-shifts RXD/TXD with transistors and did not carry 921600 on 22 Oct 2026); 0 = none */
static int fast_rate(int i)
{
	int b = g_cfg.modem_fast_baud > 0 ? g_cfg.modem_fast_baud >> i : 0;
	return b > CONFIG_CAMOS_MODEM_BAUD ? b : 0;
}

static void set_uart_baud(int baud)
{
	if (baud > 0 && baud != uart_baud) {
		uart_set_baudrate(CONFIG_CAMOS_MODEM_UART_NUM, baud);
		uart_baud = baud;
	}
}

/* AT at the configured rate or one of the fast ones: the module keeps AT+IPR from an earlier boot */
static bool find_at(int seconds)
{
	for (int i = 0; i < seconds; i++) {
		int slot = i % (N_RATES + 1);
		int b = slot ? fast_rate(slot - 1) : CONFIG_CAMOS_MODEM_BAUD;
		if (b) {
			set_uart_baud(b);
		}
		if (esp_modem_sync(dce) == ESP_OK) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	return false;
}

/* Move the module and the UART to the fastest rate the wires carry (AT+IPR), else stay at the configured one */
static void go_fast(void)
{
	if (uart_baud != CONFIG_CAMOS_MODEM_BAUD) {
		return; /* found at a fast rate the module kept from an earlier boot */
	}
	char cmd[24];
	for (int i = 0; i < N_RATES; i++) {
		int b = fast_rate(i);
		if (!b) {
			continue;
		}
		snprintf(cmd, sizeof(cmd), "AT+IPR=%d", b);
		if (esp_modem_at(dce, cmd, NULL, 2000) != ESP_OK) {
			ESP_LOGW(TAG, "%s refused: staying at %d baud", cmd, uart_baud);
			return;
		}
		int was = uart_baud;
		set_uart_baud(b);
		vTaskDelay(pdMS_TO_TICKS(100));
		if (wait_at(2)) {
			ESP_LOGI(TAG, "UART at %d baud", b);
			return;
		}
		ESP_LOGW(TAG, "no answer at %d baud: back to %d", b, was);
		snprintf(cmd, sizeof(cmd), "AT+IPR=%d", was);
		esp_modem_at(dce, cmd, NULL, 1000); /* sent at the fast rate, which may still get through */
		set_uart_baud(was);
		if (!wait_at(3)) {
			return; /* lost: find_at at the next start */
		}
	}
}

/* The ESP32 was reset (crash, watchdog, flashing, the reset button) while the module kept its power: it is still in
 * CMUX or PPP data mode and ignores plain AT (seen on the A7680C after a flash, 22 Oct 2026). Find the mode, at
 * any of the rates, and go back to commands; true when AT answers again. */
static bool resume_command_mode(void)
{
	for (int r = 0; r <= N_RATES; r++) {
		int b = r ? fast_rate(r - 1) : CONFIG_CAMOS_MODEM_BAUD;
		if (!b) {
			continue;
		}
		set_uart_baud(b);
		if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT) != ESP_OK) {
			continue;
		}
		esp_modem_dce_mode_t m = esp_modem_get_mode(dce);
		ESP_LOGI(TAG, "module found in mode %d at %d baud: back to commands", (int)m, b);
		if (m == ESP_MODEM_MODE_CMUX_MANUAL || m == ESP_MODEM_MODE_CMUX) {
			esp_modem_set_mode(dce, ESP_MODEM_MODE_CMUX_MANUAL_EXIT);
		} else if (m == ESP_MODEM_MODE_DATA) {
			esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
		}
		return wait_at(3);
	}
	return false;
}

/* 4G is on but MQTT stays down: the signal went, the module rebooted (a supply dip during a long transmit, seen
 * 22 Oct 2026 on a board without its 18650 cell) or hangs. In place, without restarting the camera:
 *  1. the Wi-Fi is set and in range again: restart onto it (Wi-Fi first at boot);
 *  2. first time: radio off and on (modem_stop / modem_start: AT+CFUN=0, AT+CFUN=1, registration, PPP);
 *  3. second time: the module reset (AT+CRESET), then the same start;
 *  4. still nothing: restart the camera (the module may not answer AT any more).
 * A working connection clears the count. */
#define NET_STUCK_S 180

static void recover(bool reset)
{
	if (reset && dce) {
		if (s_up || cmux_mode) {
			esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		esp_modem_at(dce, "AT+CRESET", NULL, 3000);
		esp_modem_destroy(dce);
		dce = NULL;
		s_up = false;
		cmux_mode = false;
		vTaskDelay(pdMS_TO_TICKS(5000)); /* the module goes down before it boots again */
	} else {
		modem_stop();
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	if (modem_start()) {
		net_4g_restarted();
	}
}

/* The radio off on purpose (a GPS fix): the watchdog below does not count it as a 4G that carries no data */
static volatile bool s_paused;

void modem_radio_pause(void)
{
	s_paused = true;
	ESP_LOGI(TAG, "4G radio off for a GPS fix");
	modem_stop();
}

bool modem_radio_resume(void)
{
	bool ok = modem_start();
	if (ok) {
		net_4g_restarted();
	}
	s_paused = false;
	ESP_LOGI(TAG, "4G radio back after the GPS fix%s", ok ? "" : ": no network, the watchdog takes over");
	return ok;
}

static void watchdog_task(void *arg)
{
	int down = 0, tries = 0;
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(10000));
		if (s_paused || net_type() != NET_4G || !proto_provisioned() || proto_connected()) {
			down = 0;
			if (proto_connected()) {
				tries = 0;
			}
			continue;
		}
		down += 10;
		if (down < NET_STUCK_S) {
			continue;
		}
		down = 0;
		int rssi = 0;
		if (g_cfg.wifi_ssid[0] && wifi_probe(&rssi)) {
			ESP_LOGE(TAG, "4G carries no data and Wi-Fi %s is in range (%d dBm): restarting onto it", g_cfg.wifi_ssid, rssi);
			events_note("net", "4G stopped carrying data, Wi-Fi in range: restarting onto it");
			vTaskDelay(pdMS_TO_TICKS(1000));
			esp_restart();
		}
		if (++tries > 2) {
			ESP_LOGE(TAG, "4G still carries no data after a radio restart and a module reset: restarting the camera");
			events_note("net", "4G stopped carrying data: restarting");
			vTaskDelay(pdMS_TO_TICKS(1000));
			esp_restart();
		}
		ESP_LOGE(TAG, "4G on but no MQTT for %d s: %s", NET_STUCK_S,
				 tries == 1 ? "radio off and on, dialling again" : "resetting the module (AT+CRESET)");
		events_note("net", tries == 1 ? "4G stopped carrying data: radio off and on" : "4G stopped carrying data: module reset");
		recover(tries == 2);
	}
}

/* A7680C answers "+ICCID: 8984..." to AT+CICCID (SIMCom); not part of the esp_modem command set */
static void read_iccid(void)
{
	char out[96] = "";
	if (esp_modem_at(dce, "AT+CICCID", out, 2000) == ESP_OK) {
		char *p = strstr(out, "ICCID:");
		if (p) {
			p += 6;
			while (*p == ' ') {
				p++;
			}
			int i = 0;
			while (*p && *p != '\r' && *p != '\n' && i < (int)sizeof(iccid) - 1) {
				iccid[i++] = *p++;
			}
			iccid[i] = 0;
		}
	}
}

/* The UART driver and the DCE at the configured rate; modem_start sets the APN later */
static esp_modem_dce_t *new_dce(void)
{
	if (!ppp_netif) {
		esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_PPP();
		ppp_netif = esp_netif_new(&ncfg);
	}
	esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
	dte.uart_config.port_num = CONFIG_CAMOS_MODEM_UART_NUM;
	dte.uart_config.tx_io_num = CONFIG_CAMOS_MODEM_TX_GPIO;
	dte.uart_config.rx_io_num = CONFIG_CAMOS_MODEM_RX_GPIO;
	dte.uart_config.rts_io_num = -1;
	dte.uart_config.cts_io_num = -1;
	dte.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
	dte.uart_config.baud_rate = CONFIG_CAMOS_MODEM_BAUD;
	dte.uart_config.rx_buffer_size = 16384; /* 921600 baud is about 90 KB/s */
	dte.uart_config.tx_buffer_size = 1024;
	dte.dte_buffer_size = 2048;
	dte.task_stack_size = 6144;
	esp_modem_dce_config_t dcfg = ESP_MODEM_DCE_DEFAULT_CONFIG(apn_used);
	esp_modem_dce_t *d = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dcfg, ppp_netif);
	uart_baud = CONFIG_CAMOS_MODEM_BAUD; /* the driver set the UART up at this rate */
	if (!d) {
		ESP_LOGE(TAG, "esp_modem init failed (UART %d, tx %d, rx %d)", CONFIG_CAMOS_MODEM_UART_NUM,
				 CONFIG_CAMOS_MODEM_TX_GPIO, CONFIG_CAMOS_MODEM_RX_GPIO);
	}
	return d;
}

/* AT from a module that is already on (always-on wiring, or a warm restart). One the ESP32 left in CMUX is back in
 * command mode after resume_command_mode, but still holds its old CMUX session (it refuses AT+CMUX and ATD*99#,
 * seen on the A7680C) and this driver its terminal state: reset the module (AT+CRESET, like a power cycle) and start
 * a fresh driver, as at a cold boot. False when nothing answers (dce may then be NULL). */
static bool reach_at(void)
{
	if (find_at(2)) {
		return true;
	}
	if (!resume_command_mode()) {
		return false;
	}
	ESP_LOGI(TAG, "resetting the module (AT+CRESET)");
	esp_modem_at(dce, "AT+CRESET", NULL, 3000);
	esp_modem_destroy(dce);
	vTaskDelay(pdMS_TO_TICKS(5000)); /* the module goes down before it boots again */
	dce = new_dce();
	return dce && find_at(CONFIG_CAMOS_MODEM_BOOT_WAIT_S);
}

/* An always-on module (no PWRKEY, no rail switch) registers by itself at power-up and would stay on the network all
 * the time, through every deep sleep, while Wi-Fi carries the traffic. Its radio goes off (AT+CFUN=0) once per
 * power-up: the flag lives in RTC memory, which a deep sleep keeps; modem_start turns the radio on again. */
static RTC_DATA_ATTR bool s_parked;

void modem_park(void)
{
	if (!NO_POWER_CONTROL || dce || s_parked || CONFIG_CAMOS_MODEM_TX_GPIO < 0 || CONFIG_CAMOS_MODEM_RX_GPIO < 0 ||
		store_get_flag("modem_absent")) {
		return;
	}
	dce = new_dce();
	if (dce && reach_at() && esp_modem_at(dce, "AT+CFUN=0", NULL, 10000) == ESP_OK) {
		s_parked = true;
		ESP_LOGI(TAG, "4G radio off while Wi-Fi is in use (AT+CFUN=0)");
	} else {
		ESP_LOGW(TAG, "could not switch the 4G radio off (no answer)");
	}
	if (dce) {
		esp_modem_destroy(dce);
		dce = NULL;
	}
}

bool modem_start(void)
{
	if (dce) {
		return s_up;
	}
	static bool handlers;
	if (!handlers) {
		esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL);
		esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, on_ppp_event, NULL);
		handlers = true;
	}
	rail(true);
	last_fail = MODEM_OK;
	strlcpy(apn_used, g_cfg.apn, sizeof(apn_used)); /* "" = auto from the SIM, decided below */
	dce = new_dce();
	if (!dce) {
		return false;
	}
	/* Already on (always-on wiring, or a warm restart)? Else PWRKEY and wait for the boot */
	bool at_ok = reach_at();
	if (!dce) {
		goto fail;
	}
	if (!at_ok) {
		ESP_LOGI(TAG, "no answer, PWRKEY");
		pwrkey(1500);
		if (!find_at(CONFIG_CAMOS_MODEM_BOOT_WAIT_S)) {
			ESP_LOGE(TAG, "modem does not answer AT after %d s (no module, wiring, or baud rate)", CONFIG_CAMOS_MODEM_BOOT_WAIT_S);
			last_fail = MODEM_NOT_FITTED;
			store_set_flag("modem_absent", 1); /* next boots go straight to Wi-Fi / the portal */
			goto fail;
		}
	}
	esp_modem_at(dce, "ATE0", NULL, 1000);
	go_fast();
	if (NO_POWER_CONTROL) {
		esp_modem_at(dce, "AT+CFUN=1", NULL, 10000); /* the radio, off since the last modem_stop or modem_park */
		s_parked = false;
	}
	/* the SIM answers "busy" for a few seconds after power-up or AT+CFUN=1 */
	bool pin_ok = false;
	esp_err_t pin_err = ESP_FAIL;
	for (int i = 0; i < 5 && (pin_err = esp_modem_read_pin(dce, &pin_ok)) != ESP_OK; i++) {
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	if (pin_err != ESP_OK) {
		ESP_LOGE(TAG, "no SIM (AT+CPIN? failed)");
		last_fail = MODEM_NO_SIM;
		goto fail;
	}
	if (!pin_ok) {
		if (!g_cfg.sim_pin[0] || esp_modem_set_pin(dce, g_cfg.sim_pin) != ESP_OK) {
			ESP_LOGE(TAG, "SIM PIN required or wrong");
			goto fail;
		}
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	esp_modem_get_imei(dce, imei);
	read_iccid();
	save_ids();
	/* network time zone updates (NITZ) into the RTC of the module: AT+CCLK? then gives local time + zone */
	esp_modem_at(dce, "AT+CTZU=1", NULL, 1000);
	/* APN: the user's when set, else from the IMSI (MCC-MNC table, apn.c) */
	if (!apn_used[0]) {
		char imsi[32] = "";
		if (esp_modem_get_imsi(dce, imsi) == ESP_OK) {
			const apn_entry_t *a = apn_from_imsi(imsi);
			if (a) {
				strlcpy(apn_used, a->apn, sizeof(apn_used));
				ESP_LOGI(TAG, "APN auto: %.5s %s -> \"%s\" (%s)", imsi, a->operator_name, a->apn,
						 a->verified ? "verified" : "unverified, docs/APN.md");
			} else {
				ESP_LOGW(TAG, "APN auto: network %.5s not in the table: empty APN (network default bearer); set one in the portal if data fails", imsi);
			}
		} else {
			ESP_LOGW(TAG, "APN auto: AT+CIMI failed, empty APN");
		}
		esp_modem_set_apn(dce, apn_used);
	}
	/* Registration: up to 60 s (CREG/CEREG are inside esp_modem's attachment query) */
	for (int i = 0; i < 60; i++) {
		int state = 0;
		read_csq();
		if (esp_modem_get_network_attachment_state(dce, &state) == ESP_OK && state) {
			registered = true;
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	if (!registered) {
		ESP_LOGE(TAG, "not registered on the network after 60 s (csq %d)", last_csq);
		last_fail = MODEM_NO_NETWORK;
		goto fail;
	}
	int act = 0;
	esp_modem_get_operator_name(dce, oper, &act);
	ESP_LOGI(TAG, "IMEI %s, ICCID %s, operator %s, csq %d, APN \"%s\"%s", imei, iccid, oper, last_csq, apn_used,
			 g_cfg.apn[0] ? "" : " (auto)");
	/* clock chain, step 2: the network time the module got by NITZ (CONTRACTS 2a.6) */
	if (!clock_synced() || !strcmp(clock_source(), "rtc")) {
		char cclk[64] = "";
		if (esp_modem_at(dce, "AT+CCLK?", cclk, 2000) == ESP_OK) {
			int64_t t = cclk_to_epoch(cclk);
			if (t) {
				struct timeval tv = { .tv_sec = (time_t)t };
				settimeofday(&tv, NULL);
				clock_set_source("modem");
			} else {
				ESP_LOGI(TAG, "AT+CCLK? %s: no network time yet", cclk);
			}
		}
	}

	esp_err_t err = ESP_FAIL;
#ifdef CONFIG_CAMOS_MODEM_CMUX
	err = esp_modem_set_mode(dce, ESP_MODEM_MODE_CMUX);
	cmux_mode = err == ESP_OK;
	if (!cmux_mode) {
		ESP_LOGW(TAG, "CMUX not accepted (%s), plain PPP", esp_err_to_name(err));
	}
#endif
	if (!cmux_mode) {
		err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "PPP start failed: %s", esp_err_to_name(err));
		goto fail;
	}
	for (int i = 0; i < 30 && !s_up; i++) {
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
	if (!s_up) {
		ESP_LOGE(TAG, "no IP from PPP after 30 s (APN \"%s\"?)", apn_used);
		last_fail = MODEM_NO_DATA;
		goto fail;
	}
	start_gen++;
	if (cmux_mode) {
		xTaskCreate(csq_task, "csq", 3072, (void *)(intptr_t)start_gen, 2, NULL);
	}
	static bool watching;
	if (!watching) {
		/* runs modem_start for a recovery: the same stack as the 4G bring-up */
		watching = xTaskCreate(watchdog_task, "modem-wd", 6144, NULL, 2, NULL) == pdPASS;
	}
	return true;
fail:
	modem_stop();
	return false;
}

void modem_stop(void)
{
	if (dce) {
		if (s_up || cmux_mode) {
			esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
			vTaskDelay(pdMS_TO_TICKS(500));
		}
		if (NO_POWER_CONTROL) {
			s_parked = esp_modem_at(dce, "AT+CFUN=0", NULL, 10000) == ESP_OK; /* radio off; AT+CPOF could not be undone */
		} else {
			if (esp_modem_power_down(dce) != ESP_OK) { /* AT+CPOF */
				pwrkey(3000);                          /* datasheet: >= 2.5 s for power off */
			}
			vTaskDelay(pdMS_TO_TICKS(1500)); /* let the module finish its shutdown before the rail goes */
		}
		esp_modem_destroy(dce);
		dce = NULL;
	}
	s_up = false;
	cmux_mode = false;
	rail(false);
#if CONFIG_CAMOS_MODEM_POWER_GPIO >= 0
	gpio_hold_en(CONFIG_CAMOS_MODEM_POWER_GPIO);
#endif
	if (g_cfg.modem_pwrkey >= 0) {
		gpio_hold_en(g_cfg.modem_pwrkey);
	}
}

int modem_at(const char *cmd, char *out, size_t n, int timeout_ms)
{
	if (!dce) {
		return -1;
	}
	if (s_up && !cmux_mode) {
		return -2; /* plain PPP owns the UART */
	}
	char buf[256] = "";
	esp_err_t err = esp_modem_at(dce, cmd, buf, timeout_ms);
	strlcpy(out, buf, n);
	return err == ESP_OK ? 0 : 1;
}

#else /* no modem in this build */

bool modem_available(void) { return false; }
modem_fail_t modem_last_failure(void) { return MODEM_NOT_FITTED; }
const char *modem_apn(void) { return ""; }
bool modem_start(void) { ESP_LOGW(TAG, "no modem in this build (CAMOS_MODEM)"); return false; }
void modem_stop(void) {}
void modem_park(void) {}
void modem_radio_pause(void) {}
bool modem_radio_resume(void) { return false; }
bool modem_up(void) { return false; }
bool modem_cell_location(double *lat, double *lng, int *acc_m) { (void)lat; (void)lng; (void)acc_m; return false; }
int modem_rssi(void) { return 0; }
void modem_cell_json(cJSON *net) { (void)net; }
void modem_saved_ids(char *imei, size_t in, char *iccid, size_t cn) { if (in) imei[0] = 0; if (cn) iccid[0] = 0; }
int modem_at(const char *cmd, char *out, size_t n, int timeout_ms) { (void)cmd; (void)timeout_ms; if (n) out[0] = 0; return -1; }
void modem_ip(char *out, size_t n) { if (n) out[0] = 0; }

#endif
