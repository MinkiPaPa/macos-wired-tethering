/*
 * test_mac_sync.c
 * Concurrent gw/host MAC snapshot vs store (TX vs ARP/USB learning).
 * TX 와 ARP/USB 학습이 동시에 MAC 을 읽고 쓸 때 찢어지지 않는지 검사한다.
 */
#include "common.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>

#define AT_SYNC_ITERS 20000
#define AT_SYNC_READERS 3

static const uint8_t k_mac_a[AT_MAC_LEN] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t k_mac_b[AT_MAC_LEN] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
static const uint8_t k_host[AT_MAC_LEN]  = { 0x82, 0x5f, 0x77, 0xbe, 0x99, 0xb3 };

struct sync_ctx {
    pthread_mutex_t mu;
    uint8_t gw[AT_MAC_LEN];
    uint8_t host[AT_MAC_LEN];
    atomic_int stop;
    atomic_int torn;
    atomic_int samples;
};

/* True if mac is exactly one of the two published gateway values. */
/* mac 이 게시된 게이트웨이 값 두 개 중 하나와 정확히 같으면 참. */
static int mac_is_published(const uint8_t mac[AT_MAC_LEN]) {
    return memcmp(mac, k_mac_a, AT_MAC_LEN) == 0 ||
           memcmp(mac, k_mac_b, AT_MAC_LEN) == 0;
}

static void *writer_fn(void *arg) {
    struct sync_ctx *c = arg;
    for (int i = 0; i < AT_SYNC_ITERS; i++) {
        at_mac_store(&c->mu, c->gw, (i & 1) ? k_mac_b : k_mac_a);
    }
    atomic_store(&c->stop, 1);
    return NULL;
}

static void *reader_fn(void *arg) {
    struct sync_ctx *c = arg;
    uint8_t gw[AT_MAC_LEN];
    uint8_t host[AT_MAC_LEN];
    while (!atomic_load(&c->stop) || atomic_load(&c->samples) < 64) {
        at_mac_snapshot_pair(&c->mu, c->gw, c->host, gw, host);
        atomic_fetch_add(&c->samples, 1);
        if (!mac_is_published(gw) || memcmp(host, k_host, AT_MAC_LEN) != 0) {
            atomic_store(&c->torn, 1);
            break;
        }
        if (atomic_load(&c->samples) > AT_SYNC_ITERS * 4) break;
    }
    return NULL;
}

static void expect_mac(const char *name, const uint8_t *got, const uint8_t *want) {
    if (memcmp(got, want, AT_MAC_LEN) == 0) return;
    fprintf(stderr, "FAIL %s: torn or unexpected MAC\n", name);
    exit(1);
}

int main(void) {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    uint8_t gw[AT_MAC_LEN];
    uint8_t host[AT_MAC_LEN];
    uint8_t out_gw[AT_MAC_LEN];
    uint8_t out_host[AT_MAC_LEN];

    /* Sequential store then snapshot must be a full 6-byte copy. */
    /* 순차 store 후 스냅샷은 6바이트 전체가 복사되어야 한다. */
    memset(gw, 0, sizeof(gw));
    memcpy(host, k_host, AT_MAC_LEN);
    at_mac_store(&mu, gw, k_mac_a);
    at_mac_snapshot_pair(&mu, gw, host, out_gw, out_host);
    expect_mac("seq gw", out_gw, k_mac_a);
    expect_mac("seq host", out_host, k_host);
    at_mac_store(&mu, gw, k_mac_b);
    at_mac_snapshot_pair(&mu, gw, host, out_gw, out_host);
    expect_mac("seq gw2", out_gw, k_mac_b);

    struct sync_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mu, NULL);
    memcpy(ctx.gw, k_mac_a, AT_MAC_LEN);
    memcpy(ctx.host, k_host, AT_MAC_LEN);
    atomic_init(&ctx.stop, 0);
    atomic_init(&ctx.torn, 0);
    atomic_init(&ctx.samples, 0);

    pthread_t wr;
    pthread_t rd[AT_SYNC_READERS];
    assert(pthread_create(&wr, NULL, writer_fn, &ctx) == 0);
    for (int i = 0; i < AT_SYNC_READERS; i++)
        assert(pthread_create(&rd[i], NULL, reader_fn, &ctx) == 0);
    pthread_join(wr, NULL);
    for (int i = 0; i < AT_SYNC_READERS; i++)
        pthread_join(rd[i], NULL);

    if (atomic_load(&ctx.torn)) {
        fprintf(stderr, "FAIL concurrent snapshot saw a torn MAC\n");
        exit(1);
    }
    if (atomic_load(&ctx.samples) < 64) {
        fprintf(stderr, "FAIL too few snapshot samples (%d)\n",
                atomic_load(&ctx.samples));
        exit(1);
    }
    pthread_mutex_destroy(&ctx.mu);

    printf("test_mac_sync: all checks passed (%d snapshots)\n",
           atomic_load(&ctx.samples));

    /* Gateway MAC is learned only from ARP replies / IPv4 src == gw_ip. */
    /* 게이트웨이 MAC 은 ARP 응답·IPv4 송신지 == gw_ip 에서만 배운다. */
    uint32_t gw_ip = 0x02000a0a; /* 10.10.0.2, memcpy into the IPv4 src field */
    uint8_t frame[64];
    uint8_t learned[AT_MAC_LEN];
    uint8_t gw_mac[AT_MAC_LEN] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 6, gw_mac, AT_MAC_LEN);
    frame[12] = 0x08;
    frame[13] = 0x00;
    frame[14] = 0x45;
    memcpy(frame + 14 + 12, &gw_ip, 4);
    if (!at_eth_learn_gw_mac(frame, 34, k_host, gw_ip, learned) ||
        memcmp(learned, gw_mac, AT_MAC_LEN) != 0) {
        fprintf(stderr, "FAIL learn from IPv4 gw src\n");
        return 1;
    }
    uint32_t other = 0x01000a0a;
    memcpy(frame + 14 + 12, &other, 4);
    if (at_eth_learn_gw_mac(frame, 34, k_host, gw_ip, learned)) {
        fprintf(stderr, "FAIL must not learn from other IPv4 src\n");
        return 1;
    }
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 6, gw_mac, AT_MAC_LEN);
    frame[12] = 0x08;
    frame[13] = 0x06;
    frame[21] = 2;
    memcpy(frame + 22, gw_mac, AT_MAC_LEN);
    memcpy(frame + 28, &gw_ip, 4);
    if (!at_eth_learn_gw_mac(frame, 42, k_host, gw_ip, learned) ||
        memcmp(learned, gw_mac, AT_MAC_LEN) != 0) {
        fprintf(stderr, "FAIL learn from ARP reply\n");
        return 1;
    }
    frame[21] = 1;
    if (at_eth_learn_gw_mac(frame, 42, k_host, gw_ip, learned)) {
        fprintf(stderr, "FAIL must not learn from ARP request\n");
        return 1;
    }
    memcpy(frame + 6, k_host, AT_MAC_LEN);
    frame[12] = 0x08;
    frame[13] = 0x00;
    frame[14] = 0x45;
    memcpy(frame + 14 + 12, &gw_ip, 4);
    if (at_eth_learn_gw_mac(frame, 34, k_host, gw_ip, learned)) {
        fprintf(stderr, "FAIL must not learn host MAC\n");
        return 1;
    }
    if (at_eth_learn_gw_mac(frame, 34, k_host, 0, learned)) {
        fprintf(stderr, "FAIL must not learn with gw_ip 0\n");
        return 1;
    }

    char esc[16];
    if (at_json_escape("ab", esc, sizeof(esc)) != 0 || strcmp(esc, "ab") != 0) {
        fprintf(stderr, "FAIL json plain\n");
        return 1;
    }
    if (at_json_escape("a\"b", esc, sizeof(esc)) != 0 || strcmp(esc, "a\\\"b") != 0) {
        fprintf(stderr, "FAIL json quote\n");
        return 1;
    }
    if (at_json_escape(NULL, esc, sizeof(esc)) != 0 || esc[0] != '\0') {
        fprintf(stderr, "FAIL json null src\n");
        return 1;
    }
    memset(esc, 'Z', sizeof(esc));
    if (at_json_escape("hello", esc, 3) != -1 || strcmp(esc, "h") != 0) {
        fprintf(stderr, "FAIL json truncate still a valid C string\n");
        return 1;
    }
    memset(esc, 'Z', sizeof(esc));
    if (at_json_escape("\"", esc, 3) != -1 || esc[0] != '\0') {
        fprintf(stderr, "FAIL json must not cut mid-escape\n");
        return 1;
    }
    if (at_json_escape("x", NULL, 4) != -1) {
        fprintf(stderr, "FAIL json null dst\n");
        return 1;
    }
    return 0;
}
