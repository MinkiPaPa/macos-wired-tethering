/*
 * test_dhcp.c
 * DHCP Discover / Request / Renew / Rebind framing checks.
 * DHCP Discover·Request·Renew·Rebind 프레이밍 검사.
 */
#include "dhcp_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#define DHCP_FIXED 240
#define DHCP_COOKIE 0x63825363u

static size_t append_opt(uint8_t *o, size_t off, uint8_t t, const void *v, uint8_t n) {
    o[off++] = t;
    o[off++] = n;
    if (n && v) memcpy(o + off, v, n);
    return off + n;
}

/* Build a BOOTP reply Ethernet frame for the client's xid. */
/* 클라이언트의 xid 에 맞는 BOOTP 응답 이더넷 프레임을 만든다. */
static size_t build_reply(const uint8_t *discover_eth, size_t discover_len,
                          uint8_t msg_type, uint32_t yiaddr_nbo, uint32_t server_nbo,
                          uint32_t lease_secs, uint8_t *out, size_t cap) {
    assert(discover_len > 14 + 20 + 8 + DHCP_FIXED);
    const uint8_t *req_bootp = discover_eth + 14 + 20 + 8;
    uint8_t bootp[548];
    memset(bootp, 0, sizeof(bootp));
    bootp[0] = 2;
    bootp[1] = 1;
    bootp[2] = 6;
    memcpy(bootp + 4, req_bootp + 4, 4);
    memcpy(bootp + 16, &yiaddr_nbo, 4);
    memcpy(bootp + 20, &server_nbo, 4);
    memcpy(bootp + 28, req_bootp + 28, 6);
    uint32_t cookie = htonl(DHCP_COOKIE);
    memcpy(bootp + 236, &cookie, 4);

    uint8_t *opt = bootp + DHCP_FIXED;
    size_t off = 0;
    off = append_opt(opt, off, 53, &msg_type, 1);
    off = append_opt(opt, off, 54, &server_nbo, 4);
    uint32_t mask = htonl(0xffffff00u);
    off = append_opt(opt, off, 1, &mask, 4);
    off = append_opt(opt, off, 3, &server_nbo, 4);
    uint32_t lease = htonl(lease_secs);
    off = append_opt(opt, off, 51, &lease, 4);
    opt[off++] = 255;
    size_t blen = DHCP_FIXED + off;
    if (blen < 300) blen = 300;

    size_t total = 14 + 20 + 8 + blen;
    assert(cap >= total);
    memset(out, 0, total);
    memset(out, 0xff, 6);
    memcpy(out + 6, "\x02\x00\x00\x00\x00\x01", 6);
    out[12] = 0x08;
    out[13] = 0x00;
    uint8_t *ip = out + 14;
    ip[0] = 0x45;
    size_t ip_len = 20 + 8 + blen;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)ip_len;
    ip[8] = 64;
    ip[9] = 17;
    memcpy(ip + 12, &server_nbo, 4);
    memset(ip + 16, 0xff, 4);
    uint8_t *udp = ip + 20;
    udp[0] = 0;
    udp[1] = 67;
    udp[2] = 0;
    udp[3] = 68;
    size_t udp_len = 8 + blen;
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)udp_len;
    memcpy(udp + 8, bootp, blen);
    return total;
}

static int bootp_has_type(const uint8_t *eth, size_t n, uint8_t want) {
    if (n < 14 + 20 + 8 + DHCP_FIXED + 3) return 0;
    const uint8_t *bootp = eth + 14 + 20 + 8;
    const uint8_t *opt = bootp + DHCP_FIXED;
    size_t left = n - (size_t)(opt - eth);
    size_t i = 0;
    while (i < left) {
        uint8_t t = opt[i++];
        if (t == 0) continue;
        if (t == 255) break;
        if (i >= left) break;
        uint8_t ln = opt[i++];
        if (i + ln > left) break;
        if (t == 53 && ln >= 1 && opt[i] == want) return 1;
        i += ln;
    }
    return 0;
}

int main(void) {
    uint8_t mac[6] = { 0x82, 0x5f, 0x77, 0xbe, 0x99, 0xb3 };
    at_dhcp_t *d = at_dhcp_create(mac);
    assert(d);
    uint8_t eth[1518];
    size_t n = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(n > 14 + 20 + 8 + 240);
    assert(eth[12] == 0x08 && eth[13] == 0x00);
    assert(memcmp(eth + 6, mac, 6) == 0);
    const uint8_t *bootp = eth + 14 + 20 + 8;
    assert(bootp[0] == 1);
    assert(bootp[1] == 1);
    assert(bootp[2] == 6);
    uint32_t cookie;
    memcpy(&cookie, bootp + 236, 4);
    assert(ntohl(cookie) == DHCP_COOKIE);
    assert(bootp_has_type(eth, n, 1)); /* Discover */

    uint32_t yi = inet_addr("10.11.226.2");
    uint32_t srv = inet_addr("10.11.226.1");
    uint8_t offer[1518];
    size_t on = build_reply(eth, n, 2, yi, srv, 3600, offer, sizeof(offer));
    assert(at_dhcp_on_frame(d, offer, on) == 0);
    assert(!at_dhcp_bound(d));

    size_t rn = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(rn > 0);
    assert(bootp_has_type(eth, rn, 3)); /* Request */

    uint8_t ack[1518];
    size_t an = build_reply(eth, rn, 5, yi, srv, 3600, ack, sizeof(ack));
    assert(at_dhcp_on_frame(d, ack, an) == 1);
    assert(at_dhcp_bound(d));
    assert(at_dhcp_debug_lease_secs(d) == 3600);
    assert(at_dhcp_debug_t1_secs(d) == 1800);
    assert(at_dhcp_debug_t2_secs(d) == 3150);

    at_net_info_t lease;
    at_dhcp_lease(d, &lease);
    assert(strcmp(lease.ip, "10.11.226.2") == 0);
    assert(lease.ready);

    /* Before T1, no TX. / T1 전에는 송신 없음. */
    assert(at_dhcp_want_tx(d, eth, sizeof(eth)) == 0);

    /* T1: unicast Renew — ciaddr set, no broadcast flag, dest = server. */
    /* T1: 유니캐스트 Renew. ciaddr 설정, 브로드캐스트 플래그 없음, 목적지는 서버. */
    at_dhcp_debug_age_ms(d, 1800ull * 1000ull + 50ull);
    size_t rnw = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(rnw > 0);
    assert(bootp_has_type(eth, rnw, 3));
    const uint8_t *rb = eth + 14 + 20 + 8;
    assert(rb[10] == 0 && rb[11] == 0); /* no broadcast flag */
    uint32_t ciaddr;
    memcpy(&ciaddr, rb + 12, 4);
    assert(ciaddr == yi);
    uint32_t ip_dst;
    memcpy(&ip_dst, eth + 14 + 16, 4);
    assert(ip_dst == srv);
    uint32_t ip_src;
    memcpy(&ip_src, eth + 14 + 12, 4);
    assert(ip_src == yi);
    assert(at_dhcp_bound(d));

    /* Renew ACK keeps us bound and does not look like a new bind. */
    /* Renew ACK 는 바인드를 유지하고 신규 바인드로 치지 않는다. */
    uint8_t rack[1518];
    size_t ran = build_reply(eth, rnw, 5, yi, srv, 3600, rack, sizeof(rack));
    assert(at_dhcp_on_frame(d, rack, ran) == 0);
    assert(at_dhcp_bound(d));
    assert(at_dhcp_want_tx(d, eth, sizeof(eth)) == 0);

    /* T2: broadcast Rebind. / T2: 브로드캐스트 Rebind. */
    at_dhcp_debug_age_ms(d, 3150ull * 1000ull + 50ull);
    size_t rbnd = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(rbnd > 0);
    assert(bootp_has_type(eth, rbnd, 3));
    const uint8_t *bb = eth + 14 + 20 + 8;
    assert(bb[10] == 0x80);
    memcpy(&ciaddr, bb + 12, 4);
    assert(ciaddr == yi);
    memcpy(&ip_dst, eth + 14 + 16, 4);
    assert(ip_dst == 0xffffffffu);

    /* Expiry returns to Discover. / 만료 시 Discover 로 돌아간다. */
    at_dhcp_debug_age_ms(d, 3600ull * 1000ull + 50ull);
    size_t exp = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(exp > 0);
    assert(bootp_has_type(eth, exp, 1));
    assert(!at_dhcp_bound(d));

    /* NAK after Requesting also drops the lease. / Requesting 중 NAK 도 임대를 버린다. */
    uint8_t offer2[1518];
    size_t o2 = build_reply(eth, exp, 2, yi, srv, 3600, offer2, sizeof(offer2));
    assert(at_dhcp_on_frame(d, offer2, o2) == 0);
    size_t req2 = at_dhcp_want_tx(d, eth, sizeof(eth));
    assert(req2 > 0);
    uint8_t nak[1518];
    size_t nk = build_reply(eth, req2, 6, yi, srv, 3600, nak, sizeof(nak));
    assert(at_dhcp_on_frame(d, nak, nk) == -1);
    assert(!at_dhcp_bound(d));

    at_dhcp_destroy(d);
    printf("test_dhcp: all checks passed\n");
    return 0;
}
