/*
 * test_rndis.c
 * Host-side unit tests for RNDIS framing (no USB hardware).
 * USB 하드웨어 없이 RNDIS 프레이밍을 검증한다.
 */
#include "rndis.h"
#include <assert.h>
#include <stdio.h>

static int g_frames = 0;
static uint8_t g_last[64];
static size_t g_last_len = 0;

static void on_frame(const uint8_t *frame, size_t len, void *user) {
    (void)user;
    g_frames++;
    g_last_len = len < sizeof(g_last) ? len : sizeof(g_last);
    memcpy(g_last, frame, g_last_len);
}

static void expect_eq_u32(const char *name, uint32_t a, uint32_t b) {
    if (a != b) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", name, a, b);
        exit(1);
    }
}

int main(void) {
    uint8_t buf[256];

    /* Initialize message layout / 초기화 메시지 레이아웃 */
    size_t n = rndis_build_initialize(buf, sizeof(buf), 7, 16384);
    assert(n == 24);
    expect_eq_u32("type", at_rd32le(buf), RNDIS_MSG_INITIALIZE);
    expect_eq_u32("len", at_rd32le(buf + 4), 24);
    expect_eq_u32("xid", at_rd32le(buf + 8), 7);
    expect_eq_u32("major", at_rd32le(buf + 12), 1);
    expect_eq_u32("max", at_rd32le(buf + 20), 16384);

    /* Wrap then unwrap a 64-byte Ethernet frame / 64바이트 프레임 왕복 */
    uint8_t eth[64];
    memset(eth, 0xAB, sizeof(eth));
    eth[0] = 0xFF; eth[1] = 0xFF; eth[2] = 0xFF; eth[3] = 0xFF; eth[4] = 0xFF; eth[5] = 0xFF;
    uint8_t wrapped[128];
    size_t w = rndis_wrap_ethernet(wrapped, sizeof(wrapped), eth, sizeof(eth));
    assert(w >= 44 + 64);
    expect_eq_u32("pkt type", at_rd32le(wrapped), RNDIS_MSG_PACKET);
    expect_eq_u32("data off", at_rd32le(wrapped + 8), 36);
    expect_eq_u32("data len", at_rd32le(wrapped + 12), 64);

    g_frames = 0;
    int got = rndis_unwrap_bulk(wrapped, w, on_frame, NULL);
    assert(got == 1);
    assert(g_frames == 1);
    assert(g_last_len == 64);
    assert(g_last[0] == 0xFF && g_last[12] == 0xAB);

    /* Two packets in one bulk transfer / 한 전송에 패킷 두 개 */
    uint8_t batch[256];
    uint8_t eth2[60];
    memset(eth2, 0x11, sizeof(eth2));
    size_t w1 = rndis_wrap_ethernet(batch, sizeof(batch), eth, sizeof(eth));
    size_t w2 = rndis_wrap_ethernet(batch + w1, sizeof(batch) - w1, eth2, sizeof(eth2));
    g_frames = 0;
    got = rndis_unwrap_bulk(batch, w1 + w2, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);

    /* Device PacketAlignmentFactor=4 means 16-byte records in one URB. */
    /* 기기의 PacketAlignmentFactor=4 는 URB 안 레코드가 16바이트 정렬임을 뜻한다. */
    uint8_t batch16[512];
    size_t a1 = rndis_wrap_ethernet_aligned(batch16, sizeof(batch16), eth, sizeof(eth), 4);
    size_t a2 = rndis_wrap_ethernet_aligned(batch16 + a1, sizeof(batch16) - a1, eth2, sizeof(eth2), 4);
    assert(a1 > 0 && (a1 % 16) == 0);
    assert(a2 > 0 && (a2 % 16) == 0);
    g_frames = 0;
    got = rndis_unwrap_bulk_aligned(batch16, a1 + a2, 4, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);

    /* Samsung: MessageLength is 4-byte aligned, records padded to 16 bytes. */
    /* 삼성: MessageLength는 4바이트 정렬, 레코드는 16바이트까지 패딩. */
    uint8_t samsung[512];
    memset(samsung, 0, sizeof(samsung));
    size_t s1 = rndis_wrap_ethernet(samsung, sizeof(samsung), eth, sizeof(eth));
    size_t s1pad = (s1 + 15u) & ~15u;
    assert(s1pad > s1);
    size_t s2 = rndis_wrap_ethernet(samsung + s1pad, sizeof(samsung) - s1pad, eth2, sizeof(eth2));
    g_frames = 0;
    got = rndis_unwrap_bulk(samsung, s1pad + s2, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);
    g_frames = 0;
    got = rndis_unwrap_bulk_aligned(samsung, s1pad + s2, 4, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);

    /* MessageLength of the first record covers the whole URB; still find both. */
    /* 첫 레코드 MessageLength가 URB 전체를 덮어도 둘 다 찾아야 한다. */
    uint8_t swallow[512];
    memset(swallow, 0, sizeof(swallow));
    size_t t1 = rndis_wrap_ethernet_aligned(swallow, sizeof(swallow), eth, sizeof(eth), 4);
    size_t t2 = rndis_wrap_ethernet_aligned(swallow + t1, sizeof(swallow) - t1, eth2, sizeof(eth2), 4);
    at_wr32le(swallow + 4, (uint32_t)(t1 + t2)); /* lie: whole URB / URB 전체라고 거짓 기록 */
    g_frames = 0;
    got = rndis_unwrap_bulk_aligned(swallow, t1 + t2, 4, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);
    /* Next PACKET sits on a 4-byte boundary, not 16. 16-byte walk skipped it. */
    /* 다음 PACKET 이 16이 아니라 4바이트 경계에 있다. 16바이트 순회는 이를 건너뛴다. */
    uint8_t pad4[512];
    memset(pad4, 0, sizeof(pad4));
    size_t p1 = rndis_wrap_ethernet(pad4, sizeof(pad4), eth, sizeof(eth));
    size_t p1a = (p1 + 3u) & ~3u;
    assert(p1a % 16 != 0 || p1 % 16 != 0);
    size_t p2 = rndis_wrap_ethernet(pad4 + p1a, sizeof(pad4) - p1a, eth2, sizeof(eth2));
    g_frames = 0;
    got = rndis_unwrap_bulk_aligned(pad4, p1a + p2, 4, on_frame, NULL);
    assert(got == 2);
    assert(g_frames == 2);

    /* Query completion: offset is relative to byte 8 / 쿼리 완료 오프셋은 바이트 8 기준 */
    uint8_t q[32];
    memset(q, 0, sizeof(q));
    at_wr32le(q + 0, RNDIS_MSG_QUERY_CMPLT);
    at_wr32le(q + 4, 30);
    at_wr32le(q + 8, 3);
    at_wr32le(q + 12, RNDIS_STATUS_SUCCESS);
    at_wr32le(q + 16, 6);
    at_wr32le(q + 20, 16); /* offset from byte 8 => payload at 24 */
    q[24] = 0x0A; q[25] = 0x11; q[26] = 0x22; q[27] = 0x33; q[28] = 0x44; q[29] = 0x55;
    const uint8_t *info = NULL;
    uint32_t ilen = 0;
    assert(rndis_parse_query_cmplt(q, 30, 3, &info, &ilen) == 0);
    assert(ilen == 6);
    assert(info[0] == 0x0A && info[5] == 0x55);

    /* Initialize complete / 초기화 완료 파싱 */
    uint8_t ic[52];
    memset(ic, 0, sizeof(ic));
    at_wr32le(ic + 0, RNDIS_MSG_INITIALIZE_CMPLT);
    at_wr32le(ic + 4, 52);
    at_wr32le(ic + 8, 1);
    at_wr32le(ic + 12, 0);
    at_wr32le(ic + 36, 8192);
    at_wr32le(ic + 40, 2);
    rndis_init_result_t ir;
    assert(rndis_parse_initialize_cmplt(ic, 52, 1, &ir) == 0);
    assert(ir.max_transfer_size == 8192);
    assert(ir.packet_alignment == 2);

    /* Set packet filter message / 패킷 필터 SET */
    uint8_t filter[4];
    at_wr32le(filter, RNDIS_DEFAULT_FILTER);
    n = rndis_build_set(buf, sizeof(buf), 9, RNDIS_OID_GEN_CURRENT_PACKET_FILTER, filter, 4);
    assert(n == 32);
    expect_eq_u32("set oid", at_rd32le(buf + 12), RNDIS_OID_GEN_CURRENT_PACKET_FILTER);
    expect_eq_u32("set off", at_rd32le(buf + 20), 20);
    expect_eq_u32("filter", at_rd32le(buf + 28), RNDIS_DEFAULT_FILTER);

    /* utun leaves L4 checksum 0; we must fill it before USB. */
    /* utun은 L4 체크섬을 0으로 둔다. USB로 보내기 전에 채워야 한다. */
    uint8_t pkt[40];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x45;
    pkt[2] = 0;
    pkt[3] = 40;
    pkt[8] = 64;
    pkt[9] = 6;
    pkt[12] = 10; pkt[13] = 11; pkt[14] = 226; pkt[15] = 2;
    pkt[16] = 10; pkt[17] = 11; pkt[18] = 226; pkt[19] = 11;
    pkt[20] = 0x04; pkt[21] = 0xd2;
    pkt[22] = 0x00; pkt[23] = 80;
    pkt[32] = 0x50;
    pkt[33] = 0x02;
    assert(at_ip4_fix_checksums(pkt, 40) == 0);
    assert(!(pkt[10] == 0 && pkt[11] == 0));
    assert(!(pkt[36] == 0 && pkt[37] == 0));
    /* Second pass must stay stable / 두 번째 계산도 같은 값이어야 한다. */
    uint8_t ipcs0 = pkt[10], ipcs1 = pkt[11], tcs0 = pkt[36], tcs1 = pkt[37];
    assert(at_ip4_fix_checksums(pkt, 40) == 0);
    assert(pkt[10] == ipcs0 && pkt[11] == ipcs1);
    assert(pkt[36] == tcs0 && pkt[37] == tcs1);

    printf("test_rndis: all checks passed\n");
    return 0;
}
