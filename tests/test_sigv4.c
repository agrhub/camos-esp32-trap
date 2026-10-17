/* SPDX-License-Identifier: AGPL-3.0-only */
/* SigV4 (firmware/main/sigv4.c) against published vectors: FIPS 180-2 SHA-256, RFC 4231 HMAC, the AWS signing-key
 * example and the AWS SigV4 test suite "get-vanilla" request. */
#include <stdio.h>
#include <string.h>
#include "../firmware/main/sigv4.h"

static int fails;

static void expect(const char *what, const char *got, const char *want)
{
	if (strcmp(got, want)) {
		printf("FAIL %s\n  got  %s\n  want %s\n", what, got, want);
		fails++;
	}
}

static void hexs(const uint8_t *p, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", p[i]);
	}
}

int main(void)
{
	char h[65];
	sigv4_sha256_hex("abc", 3, h);
	expect("sha256 abc", h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	sigv4_sha256_hex("", 0, h);
	expect("sha256 empty", h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	sigv4_sha256_hex(two, strlen(two), h);
	expect("sha256 448 bits", h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	uint8_t mac[32];
	const char *msg = "what do ya want for nothing?";
	sigv4_hmac_sha256((const uint8_t *)"Jefe", 4, msg, strlen(msg), mac);
	hexs(mac, 32, h);
	expect("hmac rfc4231 #2", h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
	uint8_t key131[131];
	memset(key131, 0xaa, sizeof(key131));
	const char *big = "Test Using Larger Than Block-Size Key - Hash Key First";
	sigv4_hmac_sha256(key131, sizeof(key131), big, strlen(big), mac);
	hexs(mac, 32, h);
	expect("hmac rfc4231 #6", h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

	/* AWS docs, "Examples of how to derive a signing key for Signature Version 4" */
	sigv4_signing_key("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "20120215", "us-east-1", "iam", mac);
	hexs(mac, 32, h);
	expect("signing key", h, "f4780e2d9f65fa895f9c67b32ce1baf0b0d8a43505a000a1a9e090d414db404d");

	/* AWS SigV4 test suite: get-vanilla */
	sigv4_creds_t c = { "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", NULL };
	char auth[256];
	if (sigv4_auth_header(&c, "us-east-1", "service", "20150830T123600Z", "GET", "example.amazonaws.com", "/", NULL, NULL, 0,
						  auth, sizeof(auth))) {
		printf("FAIL auth header returned an error\n");
		fails++;
	}
	expect("get-vanilla", auth,
		   "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date, "
		   "Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31");

	char enc[64];
	sigv4_uri_encode("arn:aws:kinesisvideo:a/b c~", enc, sizeof(enc));
	expect("uri encode", enc, "arn%3Aaws%3Akinesisvideo%3Aa%2Fb%20c~");
	if (sigv4_uri_encode("abcdef", enc, 4) != -1) {
		printf("FAIL uri encode overflow not reported\n");
		fails++;
	}

	/* Presigned URL: query sorted by name, the token and the channel ARN encoded, the signature last */
	sigv4_creds_t t = { "AKIDEXAMPLE", "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY", "tok/en=" };
	char url[1024];
	if (sigv4_presign_url(&t, "ap-southeast-2", "kinesisvideo", "20261017T010203Z", 299, "wss", "m-1.kinesisvideo.example",
						  "/", "X-Amz-ChannelARN", "arn:aws:kinesisvideo:ap-southeast-2:1:channel/cam-ESP32S3-0001/1", url,
						  sizeof(url))) {
		printf("FAIL presign returned an error\n");
		fails++;
	}
	const char *prefix = "wss://m-1.kinesisvideo.example/?X-Amz-Algorithm=AWS4-HMAC-SHA256"
						 "&X-Amz-ChannelARN=arn%3Aaws%3Akinesisvideo%3Aap-southeast-2%3A1%3Achannel%2Fcam-ESP32S3-0001%2F1"
						 "&X-Amz-Credential=AKIDEXAMPLE%2F20261017%2Fap-southeast-2%2Fkinesisvideo%2Faws4_request"
						 "&X-Amz-Date=20261017T010203Z&X-Amz-Expires=299&X-Amz-Security-Token=tok%2Fen%3D"
						 "&X-Amz-SignedHeaders=host&X-Amz-Signature=";
	if (strncmp(url, prefix, strlen(prefix)) || strlen(url) != strlen(prefix) + 64) {
		printf("FAIL presign layout\n  got  %s\n", url);
		fails++;
	}
	if (sigv4_presign_url(&t, "r", "s", "20261017T010203Z", 299, "wss", "h", "/", NULL, NULL, url, 40) != -1) {
		printf("FAIL presign overflow not reported\n");
		fails++;
	}

	printf("sigv4: %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
