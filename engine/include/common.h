/*
 * common.h
 * Shared types, logging, and byte-order helpers.
 * 공통 타입, 로그, 바이트 오더 헬퍼.
 */
#ifndef AT_COMMON_H
#define AT_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AT_VERSION
#define AT_VERSION "1.0.0"
#endif

#define AT_MAC_LEN 6
#define AT_IFNAMSIZ 16
#define AT_MAX_NAME 128
#define AT_MAX_SERIAL 64
#define AT_MAX_DEVICES 32
#define AT_MAX_TRANSFER 16384
#define AT_ETH_MIN 60
#define AT_ETH_MAX 1518
#define AT_UTUN_MTU 1500

typedef enum {
    AT_LOG_DEBUG = 0,
    AT_LOG_INFO  = 1,
    AT_LOG_WARN  = 2,
    AT_LOG_ERROR = 3
} at_log_level_t;

typedef void (*at_log_fn)(at_log_level_t level, const char *message, void *user);

void at_set_logger(at_log_fn fn, void *user);
void at_log(at_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static inline uint32_t at_rd32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static inline void at_wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void at_mac_format(const uint8_t mac[AT_MAC_LEN], char *out, size_t out_len);
/* 0 if the whole string fit. -1 if args are bad or the output was truncated. */
/* 전체가 들어가면 0. 인자가 없거나 잘리면 -1. */
int at_json_escape(const char *src, char *dst, size_t dst_len);

/* Snapshot two MACs under mu so a reader cannot observe a torn write. */
/* mu 아래에서 MAC 두 개를 스냅샷해, 읽기가 찢어진 쓰기를 보지 않게 한다. */
void at_mac_snapshot_pair(pthread_mutex_t *mu,
                          const uint8_t a[AT_MAC_LEN],
                          const uint8_t b[AT_MAC_LEN],
                          uint8_t out_a[AT_MAC_LEN],
                          uint8_t out_b[AT_MAC_LEN]);
/* Store one MAC under mu. Writers that already hold mu should memcpy instead. */
/* mu 아래에서 MAC 하나를 저장한다. 이미 mu 를 가진 쪽은 memcpy 를 쓴다. */
void at_mac_store(pthread_mutex_t *mu, uint8_t dst[AT_MAC_LEN],
                  const uint8_t src[AT_MAC_LEN]);

/* 1 if the frame is an ARP reply or IPv4 packet from gw_ip. Copies src MAC to out. */
/* ARP 응답이거나 IPv4 송신지가 gw_ip 이면 1. 송신 MAC 을 out 에 복사한다. */
int at_eth_learn_gw_mac(const uint8_t *frame, size_t len,
                        const uint8_t host_mac[AT_MAC_LEN],
                        uint32_t gw_ip,
                        uint8_t out[AT_MAC_LEN]);

/* Internet checksum (RFC 1071). Result is host-order 16-bit ones-complement. */
/* RFC 1071 인터넷 체크섬. 호스트 오더 16비트 1의 보수. */
uint16_t at_ip_checksum(const void *data, size_t len);

/* Fill IPv4 header + TCP/UDP/ICMP checksums. utun often leaves them 0 (offload). */
/* IPv4 헤더와 TCP/UDP/ICMP 체크섬을 채운다. utun은 오프로드로 0인 경우가 많다. */
int at_ip4_fix_checksums(uint8_t *ip, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AT_COMMON_H */
