/* Host unit tests: registration response / body (reg_parse.c, CONTRACTS 2a.2), APN table (apn.c), AT+CCLK? (nmea.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/main/reg_parse.h"
#include "../firmware/main/apn.h"
#include "../firmware/main/nmea.h"
#include "cJSON.h"

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

static const char *good =
	"{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\\nMIIB...\\n-----END CERTIFICATE-----\\n\","
	"\"deviceToken\":\"dt_abc123\",\"iot\":{\"endpoint\":\"a38gyfo6dhqxf4-ats.iot.ap-southeast-2.amazonaws.com\","
	"\"thingName\":\"ESPF85B1B92877C\"},\"kvs\":{\"region\":\"ap-southeast-2\",\"channelName\":\"cam-ESPF85B1B92877C\"},"
	"\"claimUrl\":\"https://flybot.agrhub.com/claim?sn=ESPF85B1B92877C\",\"claimed\":false}";

int main(void)
{
	reg_result_t *r = malloc(sizeof(*r));
	char err[64];

	/* the contract example */
	CHECK(reg_parse(good, r, err, sizeof(err)));
	CHECK(!strncmp(r->cert, "-----BEGIN CERTIFICATE-----\nMIIB", 32));
	CHECK(!strcmp(r->token, "dt_abc123"));
	CHECK(!strcmp(r->endpoint, "a38gyfo6dhqxf4-ats.iot.ap-southeast-2.amazonaws.com"));
	CHECK(!strcmp(r->thing, "ESPF85B1B92877C"));
	CHECK(!strcmp(r->kvs_region, "ap-southeast-2"));
	CHECK(!strcmp(r->kvs_channel, "cam-ESPF85B1B92877C"));
	CHECK(!strcmp(r->claim_url, "https://flybot.agrhub.com/claim?sn=ESPF85B1B92877C"));
	CHECK(r->claimed == 0);

	/* required fields */
	CHECK(!reg_parse("{\"ok\":false}", r, err, sizeof(err)) && !strcmp(err, "ok is not true"));
	CHECK(!reg_parse("{\"ok\":true,\"deviceToken\":\"x\",\"iot\":{\"endpoint\":\"e\"}}", r, err, sizeof(err)) && !strcmp(err, "no certificatePem"));
	CHECK(!reg_parse("{\"ok\":true,\"certificatePem\":\"not a pem\",\"deviceToken\":\"x\",\"iot\":{\"endpoint\":\"e\"}}", r, err, sizeof(err)));
	CHECK(!reg_parse("{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\",\"iot\":{\"endpoint\":\"e\"}}", r, err, sizeof(err)) && !strcmp(err, "no deviceToken"));
	CHECK(!reg_parse("{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\",\"deviceToken\":\"x\"}", r, err, sizeof(err)) && !strcmp(err, "no iot.endpoint"));
	CHECK(!reg_parse("<html>502</html>", r, err, sizeof(err)) && !strcmp(err, "not JSON"));
	CHECK(!reg_parse("", r, err, sizeof(err)));

	/* optional fields absent; claimed true */
	CHECK(reg_parse("{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\\n\",\"deviceToken\":\"t\",\"iot\":{\"endpoint\":\"e\"},\"claimed\":true}", r, err, sizeof(err)));
	CHECK(r->thing[0] == 0 && r->kvs_channel[0] == 0 && r->claim_url[0] == 0 && r->claimed == 1);
	CHECK(reg_parse("{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\\n\",\"deviceToken\":\"t\",\"iot\":{\"endpoint\":\"e\"}}", r, err, sizeof(err)));
	CHECK(r->claimed == -1);

	/* a too long token is refused, not truncated */
	char big[400];
	snprintf(big, sizeof(big), "{\"ok\":true,\"certificatePem\":\"-----BEGIN CERTIFICATE-----\",\"deviceToken\":\"%0200d\",\"iot\":{\"endpoint\":\"e\"}}", 0);
	CHECK(!reg_parse(big, r, err, sizeof(err)));

	/* request body */
	char *b = reg_body("ESPF85B1B92877C", "9WBYWY", "AIC-FREENOVE-S3CAM", "0.3.0", "-----BEGIN CERTIFICATE REQUEST-----\nX\n-----END CERTIFICATE REQUEST-----\n");
	cJSON *j = cJSON_Parse(b);
	CHECK(j && cJSON_GetObjectItem(j, "v")->valueint == 1);
	CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "serial")), "ESPF85B1B92877C"));
	CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "verifyCode")), "9WBYWY"));
	CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "model")), "AIC-FREENOVE-S3CAM"));
	CHECK(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(j, "fw")), "0.3.0"));
	CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(j, "csr")), "BEGIN CERTIFICATE REQUEST") != NULL);
	cJSON_Delete(j);
	free(b);

	/* APN from the IMSI */
	CHECK(!strcmp(apn_from_imsi("452040123456789")->apn, "v-internet"));
	CHECK(!strcmp(apn_from_imsi("452020123456789")->apn, "m3-world"));
	CHECK(!strcmp(apn_from_imsi("452010123456789")->apn, "m-wap"));
	CHECK(!strcmp(apn_from_imsi("452050123456789")->apn, "internet"));
	CHECK(!strcmp(apn_from_imsi("\r\n452040123456789\r\n")->operator_name, "Viettel"));
	CHECK(apn_from_imsi("452990000000000") == NULL);
	CHECK(apn_from_imsi("310260000000000") == NULL);
	CHECK(apn_from_imsi("4520") == NULL);
	CHECK(apn_from_imsi(NULL) == NULL);
	CHECK(!apn_from_imsi("452040123456789")->verified); /* no operator page confirmed it (docs/APN.md) */

	/* AT+CCLK? -> UTC: 2026-10-16 15:00:00 at +07:00 (28 quarters) is 08:00:00Z = 1792137600 */
	CHECK(cclk_to_epoch("+CCLK: \"26/10/16,15:00:00+28\"") == 1792137600);
	CHECK(cclk_to_epoch("\r\n+CCLK: \"26/10/16,08:00:00+00\"\r\n\r\nOK") == 1792137600);
	CHECK(cclk_to_epoch("+CCLK: \"26/10/16,04:30:00-14\"") == 1792137600);  /* -03:30 */
	CHECK(cclk_to_epoch("+CCLK: \"26/10/16,08:00:00\"") == 1792137600);     /* no zone */
	CHECK(cclk_to_epoch("+CCLK: \"80/01/06,00:00:12+00\"") == 0);           /* no network time yet */
	CHECK(cclk_to_epoch("+CCLK: \"04/01/01,00:00:00+00\"") == 0);
	CHECK(cclk_to_epoch("ERROR") == 0);
	CHECK(cclk_to_epoch(NULL) == 0);

	free(r);
	printf("%s: %d failure(s)\n", __FILE__, fails);
	return fails ? 1 : 0;
}
