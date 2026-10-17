/* SPDX-License-Identifier: AGPL-3.0-only */
/* The camera's local name: flybot-<last 6 letters/digits of the serial, lowercase>. It is sent to the router as the
 * DHCP host name and answered over mDNS as <name>.local, with the web console announced as an _http._tcp service, so
 * the console opens at http://flybot-xxxxxx.local/ on the same network (Windows, macOS, iOS, Linux with Avahi;
 * Android browsers often do not resolve .local, hence the IP stays on the pages too). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "camos.h"

static const char *TAG = "mdns";
static char s_name[24];
static bool s_mdns;

const char *local_hostname(void)
{
	if (!s_name[0]) {
		char tail[7] = { 0 };
		int k = 0;
		for (int i = (int)strlen(g_cfg.serial) - 1; i >= 0 && k < 6; i--) {
			unsigned char c = (unsigned char)g_cfg.serial[i];
			if (isalnum(c)) {
				memmove(tail + 1, tail, (size_t)k);
				tail[0] = (char)tolower(c);
				k++;
			}
		}
		snprintf(s_name, sizeof(s_name), "flybot-%s", k ? tail : "camera");
	}
	return s_name;
}

void local_hostname_url(char *out, size_t n)
{
	snprintf(out, n, "http://%s.local/", local_hostname());
}

void local_hostname_apply(esp_netif_t *sta)
{
	if (!sta) {
		return;
	}
	esp_err_t err = esp_netif_set_hostname(sta, local_hostname());
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "DHCP host name not set: %s", esp_err_to_name(err));
	}
}

void local_mdns_start(void)
{
	if (s_mdns) {
		return;
	}
	esp_err_t err = mdns_init();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "mDNS not started: %s", esp_err_to_name(err));
		return;
	}
	mdns_hostname_set(local_hostname());
	char instance[48];
	snprintf(instance, sizeof(instance), "FlyBot camera %s", g_cfg.serial);
	mdns_instance_name_set(instance);
	mdns_txt_item_t txt[] = { { "model", CONFIG_CAMOS_MODEL }, { "serial", g_cfg.serial }, { "path", "/" } };
	err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "mDNS service not added: %s", esp_err_to_name(err));
	}
	s_mdns = true;
	ESP_LOGI(TAG, "local name %s.local", local_hostname());
}
