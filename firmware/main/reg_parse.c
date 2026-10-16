/* See reg_parse.h. Only cJSON and libc. */
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "reg_parse.h"

static bool copy(char *dst, size_t n, const char *src)
{
	if (!src || strlen(src) >= n) {
		return false;
	}
	strcpy(dst, src);
	return true;
}

bool reg_parse(const char *json, reg_result_t *out, char *err, size_t errn)
{
	memset(out, 0, sizeof(*out));
	out->claimed = -1;
	err[0] = 0;
	cJSON *j = cJSON_Parse(json);
	if (!cJSON_IsObject(j)) {
		snprintf(err, errn, "not JSON");
		cJSON_Delete(j);
		return false;
	}
	cJSON *iot = cJSON_GetObjectItem(j, "iot"), *kvs = cJSON_GetObjectItem(j, "kvs");
	const char *cert = cJSON_GetStringValue(cJSON_GetObjectItem(j, "certificatePem"));
	bool ok = cJSON_IsTrue(cJSON_GetObjectItem(j, "ok"));
	if (!ok) {
		snprintf(err, errn, "ok is not true");
	} else if (!cert || !strstr(cert, "-----BEGIN CERTIFICATE-----") || !copy(out->cert, sizeof(out->cert), cert)) {
		snprintf(err, errn, "no certificatePem");
		ok = false;
	} else if (!copy(out->token, sizeof(out->token), cJSON_GetStringValue(cJSON_GetObjectItem(j, "deviceToken"))) ||
			   !out->token[0]) {
		snprintf(err, errn, "no deviceToken");
		ok = false;
	} else if (!copy(out->endpoint, sizeof(out->endpoint), cJSON_GetStringValue(cJSON_GetObjectItem(iot, "endpoint"))) ||
			   !out->endpoint[0]) {
		snprintf(err, errn, "no iot.endpoint");
		ok = false;
	}
	if (ok) {
		/* optional fields: absent or too long ones stay empty */
		copy(out->thing, sizeof(out->thing), cJSON_GetStringValue(cJSON_GetObjectItem(iot, "thingName")));
		copy(out->kvs_region, sizeof(out->kvs_region), cJSON_GetStringValue(cJSON_GetObjectItem(kvs, "region")));
		copy(out->kvs_channel, sizeof(out->kvs_channel), cJSON_GetStringValue(cJSON_GetObjectItem(kvs, "channelName")));
		copy(out->claim_url, sizeof(out->claim_url), cJSON_GetStringValue(cJSON_GetObjectItem(j, "claimUrl")));
		cJSON *c = cJSON_GetObjectItem(j, "claimed");
		out->claimed = cJSON_IsBool(c) ? cJSON_IsTrue(c) : -1;
	}
	cJSON_Delete(j);
	return ok;
}

char *reg_body(const char *serial, const char *code, const char *model, const char *fw, const char *csr_pem)
{
	cJSON *b = cJSON_CreateObject();
	cJSON_AddNumberToObject(b, "v", 1);
	cJSON_AddStringToObject(b, "serial", serial);
	cJSON_AddStringToObject(b, "verifyCode", code);
	cJSON_AddStringToObject(b, "model", model);
	cJSON_AddStringToObject(b, "fw", fw);
	cJSON_AddStringToObject(b, "csr", csr_pem);
	char *s = cJSON_PrintUnformatted(b);
	cJSON_Delete(b);
	return s;
}
