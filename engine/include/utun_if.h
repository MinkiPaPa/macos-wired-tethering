/*
 * utun_if.h
 * macOS utun (layer-3 tunnel) used as the host-facing interface.
 * 호스트가 보는 인터페이스로 macOS utun(L3 터널)을 사용한다.
 */
#ifndef AT_UTUN_IF_H
#define AT_UTUN_IF_H

#include "common.h"
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct at_utun at_utun_t;

at_utun_t *at_utun_open(void);
void at_utun_close(at_utun_t *u);

const char *at_utun_name(const at_utun_t *u);

/* Read result besides 0 (ok), 1 (timeout/empty), -1 (io error). */
/* 0(성공), 1(타임아웃/빈 값), -1(입출력 오류) 외의 읽기 결과. */
#define AT_UTUN_READ_DROP (-2)

/* True if a utun read of `n` bytes from a `bufsz` buffer must be dropped. */
/* `bufsz` 버퍼에서 `n` 바이트를 읽었을 때 버려야 하면 참. */
int at_utun_read_is_drop(ssize_t n, size_t bufsz, size_t cap);

/* Read one IPv4/IPv6 datagram (without the 4-byte family header).
 * timeout_ms=0 polls without waiting so the caller can drain a burst.
 * Returns AT_UTUN_READ_DROP when the datagram was truncated or larger than cap. */
/* 4바이트 family 헤더를 제외한 IPv4/IPv6 데이터그램 하나를 읽는다.
 * timeout_ms=0 이면 대기 없이 poll 하여 버스트로 비울 수 있다.
 * 잘리거나 cap 보다 크면 AT_UTUN_READ_DROP. */
int at_utun_read(at_utun_t *u, uint8_t *ip, size_t cap, size_t *len, int *family,
                 int timeout_ms);

/* Write one IP datagram. family is AF_INET or AF_INET6. */
/* IP 데이터그램 하나를 쓴다. family 는 AF_INET 또는 AF_INET6. */
int at_utun_write(at_utun_t *u, const uint8_t *ip, size_t len, int family);

#ifdef __cplusplus
}
#endif

#endif /* AT_UTUN_IF_H */
