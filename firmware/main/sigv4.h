/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * AWS Signature Version 4 for the KVS signaling calls of the live view (live_kvs.c): SHA-256 / HMAC-SHA256, the
 * Authorization header of a JSON POST, and a presigned URL (the WSS connect of the signaling channel). Portable C with
 * no ESP-IDF dependency, so tests/test_sigv4.c checks it on the host against the AWS reference vectors.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
	const char *akid;   /* access key id */
	const char *secret; /* secret access key */
	const char *token;  /* session token, NULL or "" for long-term keys */
} sigv4_creds_t;

void sigv4_sha256(const void *data, size_t len, uint8_t out[32]);
void sigv4_sha256_hex(const void *data, size_t len, char out[65]);
void sigv4_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[32]);

/* RFC 3986 percent-encoding as SigV4 wants it: A-Z a-z 0-9 - _ . ~ stay, everything else (also '/') is %XX.
 * Returns the length written, or -1 when out is too small. */
int sigv4_uri_encode(const char *in, char *out, size_t outn);

/* kSigning = HMAC(HMAC(HMAC(HMAC("AWS4" + secret, date), region), service), "aws4_request"); date is YYYYMMDD */
void sigv4_signing_key(const char *secret, const char *date, const char *region, const char *service, uint8_t out[32]);

/*
 * Authorization header value for a request with the headers [content-type,] host, x-amz-date [, x-amz-security-token]
 * (content_type NULL: not signed; the caller sends exactly these headers). amz_date is YYYYMMDDTHHMMSSZ, path the
 * canonical URI ("/describeSignalingChannel"), no query string. Returns 0, or -1 when out is too small.
 */
int sigv4_auth_header(const sigv4_creds_t *c, const char *region, const char *service, const char *amz_date,
					  const char *method, const char *host, const char *path, const char *content_type, const void *body,
					  size_t body_len, char *out, size_t outn);

/*
 * Presigned GET URL "<scheme>://<host><path>?<query>&X-Amz-Signature=..." with the extra query parameter
 * extra_name=extra_value (raw, encoded here; NULL for none; KVS: X-Amz-ChannelARN), signed header host only, empty
 * payload, the query sorted by name. Returns 0, or -1 when out is too small.
 */
int sigv4_presign_url(const sigv4_creds_t *c, const char *region, const char *service, const char *amz_date, int expires_s,
					  const char *scheme, const char *host, const char *path, const char *extra_name, const char *extra_value,
					  char *out, size_t outn);
