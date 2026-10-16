/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * QR codes for the claim link (CONTRACTS 2a: the portal and the console show a QR of claimUrl) and the label text,
 * as SVG built on the device (espressif/qrcode = Nayuki's qrcodegen), so the pages need no JavaScript QR library.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h> /* qrcode.h uses bool without it */
#include "qrcode.h"
#include "esp_log.h"
#include "camos.h"

typedef struct {
	char *svg;
	size_t len, cap;
} svg_buf_t;

static void put(svg_buf_t *b, const char *s)
{
	size_t n = strlen(s);
	if (!b->svg || b->len + n + 1 > b->cap) {
		size_t cap = (b->cap ? b->cap * 2 : 4096) + n;
		char *p = realloc(b->svg, cap);
		if (!p) {
			return;
		}
		b->svg = p;
		b->cap = cap;
	}
	memcpy(b->svg + b->len, s, n + 1);
	b->len += n;
}

/* One <path> of 1x1 squares per dark module, run-length joined per row: small and crisp at any size */
static void to_svg(esp_qrcode_handle_t q, void *user)
{
	svg_buf_t *b = user;
	int n = esp_qrcode_get_size(q);
	char tmp[160];
	snprintf(tmp, sizeof(tmp), "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-2 -2 %d %d\" shape-rendering=\"crispEdges\">", n + 4, n + 4);
	put(b, tmp);
	put(b, "<rect x=\"-2\" y=\"-2\" width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"");
	for (int y = 0; y < n; y++) {
		for (int x = 0; x < n;) {
			if (!esp_qrcode_get_module(q, x, y)) {
				x++;
				continue;
			}
			int run = 1;
			while (x + run < n && esp_qrcode_get_module(q, x + run, y)) {
				run++;
			}
			snprintf(tmp, sizeof(tmp), "M%d %dh%dv1h-%dz", x, y, run, run);
			put(b, tmp);
			x += run;
		}
	}
	put(b, "\"/></svg>");
}

char *qr_svg(const char *text)
{
	svg_buf_t b = { 0 };
	esp_qrcode_config_t cfg = {
		.display_func_with_cb = to_svg,
		.max_qrcode_version = 10,
		.qrcode_ecc_level = ESP_QRCODE_ECC_MED,
		.user_data = &b,
	};
	if (esp_qrcode_generate(&cfg, text) != ESP_OK) {
		free(b.svg);
		return NULL;
	}
	return b.svg;
}

void qr_print(const char *text)
{
	esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
	esp_qrcode_generate(&cfg, text);
}

/* GET /api/qr.svg?what=claim|label (portal and console, no secret in either) */
esp_err_t web_qr_get(httpd_req_t *req)
{
	char q[24] = "", v[8] = "claim", text[200];
	if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
		httpd_query_key_value(q, "what", v, sizeof(v));
	}
	if (!strcmp(v, "label")) {
		snprintf(text, sizeof(text), "CAMOS:1;SN:%s;VC:%s;M:%s;;", g_cfg.serial, g_cfg.verify_code, CONFIG_CAMOS_MODEL);
	} else {
		store_claim_url(text, sizeof(text));
	}
	char *svg = qr_svg(text);
	if (!svg) {
		return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "qr");
	}
	httpd_resp_set_type(req, "image/svg+xml");
	httpd_resp_set_hdr(req, "Cache-Control", "no-store");
	esp_err_t e = httpd_resp_sendstr(req, svg);
	free(svg);
	return e;
}
