/*
 * SIMCom A7680C (LTE Cat.1) over UART with esp_modem. The A76xx AT set is the SIM7600 set for everything this
 * firmware needs (ATE0, AT+CPIN?, AT+CSQ, AT+CGDCONT, ATD*99#, AT+CMUX, AT+CPOF), so the SIM7600 DCE profile is
 * used; what actually answered on hardware is to be recorded in docs/WIRING.md once a module is on the bench.
 *
 *   power on   PWRKEY asserted 1.5 s (datasheet: >= 1 s), then AT every second for CAMOS_MODEM_BOOT_WAIT_S
 *   data       CMUX first (AT + PPP on one UART, so CSQ can still be read), plain PPP when CMUX fails
 *   power off  AT+CPOF (esp_modem_power_down), then PWRKEY 3 s if it still answers, then the rail switch
 *
 * Without CAMOS_MODEM (ESP32-CAM preset) every function is a stub and the network is Wi-Fi.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camos.h"

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
#if CONFIG_CAMOS_MODEM_PWRKEY_GPIO >= 0
	int asserted =
#ifdef CONFIG_CAMOS_MODEM_PWRKEY_ACTIVE_HIGH
		1;
#else
		0;
#endif
	gpio_out(CONFIG_CAMOS_MODEM_PWRKEY_GPIO, asserted);
	vTaskDelay(pdMS_TO_TICKS(ms));
	gpio_set_level(CONFIG_CAMOS_MODEM_PWRKEY_GPIO, !asserted);
#endif
	(void)ms;
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
	if (id < NETIF_PP_PHASE_OFFSET) {
		ESP_LOGW(TAG, "PPP status %d", (int)id); /* NETIF_PPP_ERRORUSER = a clean exit */
		s_up = false;
	}
}

bool modem_available(void)
{
	return CONFIG_CAMOS_MODEM_TX_GPIO >= 0 && CONFIG_CAMOS_MODEM_RX_GPIO >= 0 && g_cfg.apn[0];
}

bool modem_up(void) { return s_up; }

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

static void csq_task(void *arg)
{
	while (dce && cmux_mode) {
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
	dte.uart_config.rx_buffer_size = 4096;
	dte.uart_config.tx_buffer_size = 1024;
	dte.dte_buffer_size = 1024;
	dte.task_stack_size = 6144;
	esp_modem_dce_config_t dcfg = ESP_MODEM_DCE_DEFAULT_CONFIG(g_cfg.apn);
	dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dcfg, ppp_netif);
	if (!dce) {
		ESP_LOGE(TAG, "esp_modem init failed (UART %d, tx %d, rx %d)", CONFIG_CAMOS_MODEM_UART_NUM,
				 CONFIG_CAMOS_MODEM_TX_GPIO, CONFIG_CAMOS_MODEM_RX_GPIO);
		return false;
	}
	/* Already on (always-on wiring, or a warm restart)? Else PWRKEY and wait for the boot */
	if (!wait_at(2)) {
		ESP_LOGI(TAG, "no answer, PWRKEY");
		pwrkey(1500);
		if (!wait_at(CONFIG_CAMOS_MODEM_BOOT_WAIT_S)) {
			ESP_LOGE(TAG, "modem does not answer AT after %d s (no module, wiring, or baud rate)", CONFIG_CAMOS_MODEM_BOOT_WAIT_S);
			goto fail;
		}
	}
	esp_modem_at(dce, "ATE0", NULL, 1000);
	bool pin_ok = false;
	if (esp_modem_read_pin(dce, &pin_ok) != ESP_OK) {
		ESP_LOGE(TAG, "no SIM (AT+CPIN? failed)");
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
		goto fail;
	}
	int act = 0;
	esp_modem_get_operator_name(dce, oper, &act);
	ESP_LOGI(TAG, "IMEI %s, ICCID %s, operator %s, csq %d, APN %s", imei, iccid, oper, last_csq, g_cfg.apn);

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
		ESP_LOGE(TAG, "no IP from PPP after 30 s (APN %s?)", g_cfg.apn);
		goto fail;
	}
	if (cmux_mode) {
		xTaskCreate(csq_task, "csq", 3072, NULL, 2, NULL);
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
		if (esp_modem_power_down(dce) != ESP_OK) { /* AT+CPOF */
			pwrkey(3000);                          /* datasheet: >= 2.5 s for power off */
		}
		vTaskDelay(pdMS_TO_TICKS(1500)); /* let the module finish its shutdown before the rail goes */
		esp_modem_destroy(dce);
		dce = NULL;
	}
	s_up = false;
	cmux_mode = false;
	rail(false);
#if CONFIG_CAMOS_MODEM_POWER_GPIO >= 0
	gpio_hold_en(CONFIG_CAMOS_MODEM_POWER_GPIO);
#endif
#if CONFIG_CAMOS_MODEM_PWRKEY_GPIO >= 0
	gpio_hold_en(CONFIG_CAMOS_MODEM_PWRKEY_GPIO);
#endif
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
bool modem_start(void) { ESP_LOGW(TAG, "no modem in this build (CAMOS_MODEM)"); return false; }
void modem_stop(void) {}
bool modem_up(void) { return false; }
int modem_rssi(void) { return 0; }
void modem_cell_json(cJSON *net) { (void)net; }
int modem_at(const char *cmd, char *out, size_t n, int timeout_ms) { (void)cmd; (void)timeout_ms; if (n) out[0] = 0; return -1; }
void modem_ip(char *out, size_t n) { if (n) out[0] = 0; }

#endif
