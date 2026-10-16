/* SPDX-License-Identifier: AGPL-3.0-only */
/* POST /api/device/register response (CONTRACTS 2a.2), parsed without ESP-IDF so it is host-tested (tests/test_reg.c) */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	char cert[4096];
	char token[130];
	char endpoint[128];
	char thing[64];
	char kvs_region[24];
	char kvs_channel[64];
	char claim_url[160];
	int claimed;          /* 0 / 1, -1 when absent */
} reg_result_t;

/* true and *out filled when the JSON has ok, a CERTIFICATE PEM, a device token and iot.endpoint; else false + err */
bool reg_parse(const char *json, reg_result_t *out, char *err, size_t errn);
/* The request body: {"v":1,"serial","verifyCode","model","fw","csr"}; caller frees */
char *reg_body(const char *serial, const char *code, const char *model, const char *fw, const char *csr_pem);
