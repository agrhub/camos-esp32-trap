/* SPDX-License-Identifier: AGPL-3.0-only */
/* esp_peer's UDP send (src/transport/udp.c defines it weak), with a longer wait when the Wi-Fi has no TX buffer left.
 *
 * esp_peer gives up after about 40 ms on ENOBUFS and returns -200; mbedTLS then keeps the DTLS record as pending output
 * and every mbedtls_ssl_read answers MBEDTLS_ERR_SSL_WANT_WRITE ("mbedtls_ssl_read error: -26752"), as with FullHD and
 * 2K live frames (100 to 400 KB each) on 27 Oct 2026. A live experiment ("udpWaitMs", live_dc.h) can make the sender wait
 * longer for the queue to drain; by default it waits as esp_peer does. The full-queue count is logged either way. The
 * struct mirrors esp_peer 1.5.7's udp_socket_t. */
#include "sdkconfig.h"
#if CONFIG_CAMOS_LIVE
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_peer_types.h"

#define TAG "peer_udp"
#define PEER_UDP_WAIT_DEFAULT_MS 40 /* esp_peer's own: two tries 20 ms apart */

typedef struct {
	int fd;
	int ipv6_fd;
	uint32_t sin6_scope_id;
	esp_peer_addr_t bind_addr;
	long long int timeout_sec;
	long int timeout_usec;
	atomic_int user_count;
} udp_socket_t;

static volatile int wait_ms;
static uint32_t full_waits, full_gave_up;

void peer_udp_set_wait(int ms)
{
	wait_ms = ms > 0 ? ms : 0;
}
static int64_t last_report_us;

static void report(void)
{
	int64_t now = esp_timer_get_time();
	if ((full_waits || full_gave_up) && now - last_report_us > 5000000) {
		ESP_LOGW(TAG, "Wi-Fi TX queue full %u times in the last %d s, %u packets given up", (unsigned)full_waits,
				 (int)((now - last_report_us) / 1000000), (unsigned)full_gave_up);
		full_waits = full_gave_up = 0;
		last_report_us = now;
	}
}

int udp_socket_sendto(udp_socket_t *udp_socket, esp_peer_addr_t *addr, const uint8_t *buf, int len)
{
	struct sockaddr_storage ss;
	socklen_t ss_len;
	int fd;
	memset(&ss, 0, sizeof(ss));
	if (addr->family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
		sin->sin_family = AF_INET;
		memcpy(&sin->sin_addr.s_addr, addr->ipv4, 4);
		sin->sin_port = htons(addr->port);
		ss_len = sizeof(*sin);
		fd = udp_socket->fd;
	} else if (addr->family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
		sin6->sin6_family = AF_INET6;
		memcpy(&sin6->sin6_addr, addr->ipv6, 16);
		sin6->sin6_port = htons(addr->port);
		sin6->sin6_scope_id = udp_socket->sin6_scope_id;
		ss_len = sizeof(*sin6);
		fd = udp_socket->ipv6_fd;
	} else {
		return -1;
	}
	if (fd < 0) {
		return -1;
	}
	atomic_fetch_add(&udp_socket->user_count, 1);
	int64_t t0 = 0;
	int delay_ms = 2;
	int ret;
	for (;;) {
		ret = sendto(fd, buf, len, 0, (struct sockaddr *)&ss, ss_len);
		if (ret >= 0 || (errno != ENOBUFS && errno != ENOMEM)) {
			break;
		}
		int64_t now = esp_timer_get_time();
		if (!t0) {
			t0 = now;
			full_waits++;
		} else if (now - t0 >= (wait_ms ? wait_ms : PEER_UDP_WAIT_DEFAULT_MS) * 1000LL) {
			full_gave_up++;
			ret = -200; /* esp_peer's "may retry" */
			break;
		}
		vTaskDelay(pdMS_TO_TICKS(delay_ms) ? pdMS_TO_TICKS(delay_ms) : 1);
		delay_ms = delay_ms < 16 ? delay_ms * 2 : 16;
	}
	if (ret < 0 && ret != -200) {
		ESP_LOGE(TAG, "sendto: %d %s", errno, strerror(errno));
		ret = -1;
	}
	atomic_fetch_sub(&udp_socket->user_count, 1);
	report();
	return ret;
}
#endif
