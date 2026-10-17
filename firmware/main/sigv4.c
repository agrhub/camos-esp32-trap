/* SPDX-License-Identifier: AGPL-3.0-only */
/* AWS Signature Version 4 (see sigv4.h). SHA-256 follows FIPS 180-4. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sigv4.h"

/* ------------------------------------------------------------------ SHA-256 */

typedef struct {
	uint32_t h[8];
	uint64_t len; /* bytes */
	uint8_t buf[64];
	size_t n;
} sha256_t;

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha256_t *s, const uint8_t *p)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void sha_init(sha256_t *s)
{
	static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	memcpy(s->h, iv, sizeof(iv));
	s->len = 0;
	s->n = 0;
}

static void sha_update(sha256_t *s, const void *data, size_t len)
{
	const uint8_t *p = data;
	s->len += len;
	while (len) {
		size_t take = 64 - s->n < len ? 64 - s->n : len;
		memcpy(s->buf + s->n, p, take);
		s->n += take, p += take, len -= take;
		if (s->n == 64) {
			sha_block(s, s->buf);
			s->n = 0;
		}
	}
}

static void sha_final(sha256_t *s, uint8_t out[32])
{
	uint64_t bits = s->len * 8;
	uint8_t pad = 0x80, zero = 0;
	sha_update(s, &pad, 1);
	while (s->n != 56) {
		sha_update(s, &zero, 1);
	}
	uint8_t l[8];
	for (int i = 0; i < 8; i++) {
		l[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha_update(s, l, 8);
	for (int i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8), out[4 * i + 3] = (uint8_t)s->h[i];
	}
}

void sigv4_sha256(const void *data, size_t len, uint8_t out[32])
{
	sha256_t s;
	sha_init(&s);
	sha_update(&s, data, len);
	sha_final(&s, out);
}

static void hex(const uint8_t *in, size_t n, char *out)
{
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = d[in[i] >> 4], out[2 * i + 1] = d[in[i] & 15];
	}
	out[2 * n] = 0;
}

void sigv4_sha256_hex(const void *data, size_t len, char out[65])
{
	uint8_t h[32];
	sigv4_sha256(data, len, h);
	hex(h, 32, out);
}

void sigv4_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[32])
{
	uint8_t k[64] = { 0 }, pad[64], inner[32];
	if (key_len > 64) {
		sigv4_sha256(key, key_len, k);
	} else {
		memcpy(k, key, key_len);
	}
	sha256_t s;
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x36;
	}
	sha_init(&s);
	sha_update(&s, pad, 64);
	sha_update(&s, data, len);
	sha_final(&s, inner);
	for (int i = 0; i < 64; i++) {
		pad[i] = k[i] ^ 0x5c;
	}
	sha_init(&s);
	sha_update(&s, pad, 64);
	sha_update(&s, inner, 32);
	sha_final(&s, out);
}

/* ------------------------------------------------------------------ SigV4 */

int sigv4_uri_encode(const char *in, char *out, size_t outn)
{
	static const char d[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
		int keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
				   *p == '_' || *p == '.' || *p == '~';
		if (o + (keep ? 1 : 3) >= outn) {
			return -1;
		}
		if (keep) {
			out[o++] = (char)*p;
		} else {
			out[o++] = '%', out[o++] = d[*p >> 4], out[o++] = d[*p & 15];
		}
	}
	if (o >= outn) {
		return -1;
	}
	out[o] = 0;
	return (int)o;
}

void sigv4_signing_key(const char *secret, const char *date, const char *region, const char *service, uint8_t out[32])
{
	char k0[128];
	int n = snprintf(k0, sizeof(k0), "AWS4%s", secret);
	uint8_t a[32], b[32];
	sigv4_hmac_sha256((const uint8_t *)k0, (size_t)n, date, strlen(date), a);
	sigv4_hmac_sha256(a, 32, region, strlen(region), b);
	sigv4_hmac_sha256(b, 32, service, strlen(service), a);
	sigv4_hmac_sha256(a, 32, "aws4_request", 12, out);
	memset(k0, 0, sizeof(k0));
}

/* signature hex of string-to-sign(canonical request) */
static void sign(const sigv4_creds_t *c, const char *region, const char *service, const char *amz_date, const char *canonical,
				 char sig[65])
{
	char date[9], creq_hash[65];
	memcpy(date, amz_date, 8);
	date[8] = 0;
	sigv4_sha256_hex(canonical, strlen(canonical), creq_hash);
	char sts[256];
	snprintf(sts, sizeof(sts), "AWS4-HMAC-SHA256\n%s\n%s/%s/%s/aws4_request\n%s", amz_date, date, region, service, creq_hash);
	uint8_t key[32], mac[32];
	sigv4_signing_key(c->secret, date, region, service, key);
	sigv4_hmac_sha256(key, 32, sts, strlen(sts), mac);
	hex(mac, 32, sig);
}

int sigv4_auth_header(const sigv4_creds_t *c, const char *region, const char *service, const char *amz_date,
					  const char *method, const char *host, const char *path, const char *content_type, const void *body,
					  size_t body_len, char *out, size_t outn)
{
	bool tok = c->token && c->token[0];
	char body_hash[65];
	sigv4_sha256_hex(body ? body : "", body ? body_len : 0, body_hash);
	char signed_headers[96];
	snprintf(signed_headers, sizeof(signed_headers), "%shost;x-amz-date%s", content_type ? "content-type;" : "",
			 tok ? ";x-amz-security-token" : "");
	size_t cap = 512 + strlen(host) + strlen(path) + (tok ? strlen(c->token) : 0) + (content_type ? strlen(content_type) : 0);
	char *creq = malloc(cap);
	if (!creq) {
		return -1;
	}
	int n = snprintf(creq, cap, "%s\n%s\n\n", method, path);
	if (content_type) {
		n += snprintf(creq + n, cap - n, "content-type:%s\n", content_type);
	}
	n += snprintf(creq + n, cap - n, "host:%s\nx-amz-date:%s\n", host, amz_date);
	if (tok) {
		n += snprintf(creq + n, cap - n, "x-amz-security-token:%s\n", c->token);
	}
	snprintf(creq + n, cap - n, "\n%s\n%s", signed_headers, body_hash);
	char sig[65];
	sign(c, region, service, amz_date, creq, sig);
	free(creq);
	int w = snprintf(out, outn, "AWS4-HMAC-SHA256 Credential=%s/%.8s/%s/%s/aws4_request, SignedHeaders=%s, Signature=%s",
					 c->akid, amz_date, region, service, signed_headers, sig);
	return (w < 0 || (size_t)w >= outn) ? -1 : 0;
}

typedef struct {
	const char *name;
	char *value; /* encoded */
} qparam_t;

static int qcmp(const void *a, const void *b)
{
	return strcmp(((const qparam_t *)a)->name, ((const qparam_t *)b)->name);
}

static char *enc_dup(const char *raw)
{
	size_t cap = strlen(raw) * 3 + 1;
	char *e = malloc(cap);
	if (e && sigv4_uri_encode(raw, e, cap) < 0) {
		free(e);
		e = NULL;
	}
	return e;
}

int sigv4_presign_url(const sigv4_creds_t *c, const char *region, const char *service, const char *amz_date, int expires_s,
					  const char *scheme, const char *host, const char *path, const char *extra_name, const char *extra_value,
					  char *out, size_t outn)
{
	bool tok = c->token && c->token[0];
	char credential[160], expires[12];
	snprintf(credential, sizeof(credential), "%s/%.8s/%s/%s/aws4_request", c->akid, amz_date, region, service);
	snprintf(expires, sizeof(expires), "%d", expires_s);
	qparam_t q[8];
	int nq = 0;
	q[nq++] = (qparam_t){ "X-Amz-Algorithm", enc_dup("AWS4-HMAC-SHA256") };
	q[nq++] = (qparam_t){ "X-Amz-Credential", enc_dup(credential) };
	q[nq++] = (qparam_t){ "X-Amz-Date", enc_dup(amz_date) };
	q[nq++] = (qparam_t){ "X-Amz-Expires", enc_dup(expires) };
	q[nq++] = (qparam_t){ "X-Amz-SignedHeaders", enc_dup("host") };
	if (tok) {
		q[nq++] = (qparam_t){ "X-Amz-Security-Token", enc_dup(c->token) };
	}
	if (extra_name && extra_value) {
		q[nq++] = (qparam_t){ extra_name, enc_dup(extra_value) };
	}
	int ret = -1;
	size_t qlen = 0;
	for (int i = 0; i < nq; i++) {
		if (!q[i].value) {
			goto done;
		}
		qlen += strlen(q[i].name) + strlen(q[i].value) + 2;
	}
	qsort(q, nq, sizeof(q[0]), qcmp);
	char *query = malloc(qlen + 1);
	if (!query) {
		goto done;
	}
	size_t o = 0;
	for (int i = 0; i < nq; i++) {
		o += sprintf(query + o, "%s%s=%s", i ? "&" : "", q[i].name, q[i].value);
	}
	size_t cap = qlen + strlen(host) + strlen(path) + 200;
	char *creq = malloc(cap);
	if (creq) {
		char empty_hash[65];
		sigv4_sha256_hex("", 0, empty_hash);
		snprintf(creq, cap, "GET\n%s\n%s\nhost:%s\n\nhost\n%s", path, query, host, empty_hash);
		char sig[65];
		sign(c, region, service, amz_date, creq, sig);
		free(creq);
		int w = snprintf(out, outn, "%s://%s%s?%s&X-Amz-Signature=%s", scheme, host, path, query, sig);
		ret = (w < 0 || (size_t)w >= outn) ? -1 : 0;
	}
	free(query);
done:
	for (int i = 0; i < nq; i++) {
		free(q[i].value);
	}
	return ret;
}
