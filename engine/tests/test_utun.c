/*
 * test_utun.c
 * utun read drop policy (no kernel utun required).
 * utun 읽기 폐기 정책. 커널 utun 은 필요 없다.
 */
#include "utun_if.h"

#include <stdio.h>
#include <stdlib.h>

static int fails;

static void expect(int cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", name);
        fails++;
    }
}

int main(void) {
    /* Short / timeout-sized reads are not this helper's job. */
    /* 짧은 읽기·타임아웃은 이 헬퍼의 일이 아니다. */
    expect(!at_utun_read_is_drop(4, 4096, 2048), "short");
    expect(!at_utun_read_is_drop(-1, 4096, 2048), "io-err");

    /* Typical IPv4 + 4-byte family header under MTU. */
    /* MTU 아래의 일반 IPv4 + 4바이트 family 헤더. */
    expect(!at_utun_read_is_drop(4 + 1500, 4096, 2048), "mtu");
    expect(!at_utun_read_is_drop(4 + 20, 4096, 2048), "tiny-ip");

    /* Full buffer: kernel datagram may have been truncated. */
    /* 버퍼가 가득 참: 커널 데이터그램이 잘렸을 수 있다. */
    expect(at_utun_read_is_drop(4096, 4096, 2048), "full-buf");
    expect(at_utun_read_is_drop(2048, 2048, 2048), "old-buf-full");

    /* Payload larger than the caller dest. */
    /* 호출 쪽 목적지보다 큰 페이로드. */
    expect(at_utun_read_is_drop(4 + 3000, 4096, 2048), "over-cap");
    expect(!at_utun_read_is_drop(4 + 2048, 4096, 2048), "exact-cap");
    expect(at_utun_read_is_drop(4 + 2049, 4096, 2048), "cap-plus-one");

    if (fails) {
        fprintf(stderr, "test_utun: %d failed\n", fails);
        return 1;
    }
    printf("test_utun: all checks passed\n");
    return 0;
}
