/*
 * dhcp_client.c
 * Minimal DHCPv4 over Ethernet, sent on the USB RNDIS path (not via utun).
 * utun이 아닌 USB RNDIS 경로로 보내는 최소 DHCPv4 클라이언트.
 *
 * RFC 2131: Discover/Request then T1 Renew (unicast) and T2 Rebind (broadcast).
 * RFC 2131: Discover/Request 이후 T1 유니캐스트 Renew, T2 브로드캐스트 Rebind.
 */
#include "dhcp_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/time.h>
#include <time.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2
#define DHCP_HTYPE_ETH      1
#define DHCP_COOKIE         0x63825363u
#define DHCP_OPT_PAD        0
#define DHCP_OPT_SUBNET     1
#define DHCP_OPT_ROUTER     3
#define DHCP_OPT_DNS        6
#define DHCP_OPT_REQ_IP     50
#define DHCP_OPT_LEASE      51
#define DHCP_OPT_TYPE       53
#define DHCP_OPT_SERVER     54
#define DHCP_OPT_PARAM_REQ  55
#define DHCP_OPT_T1         58
#define DHCP_OPT_T2         59
#define DHCP_OPT_END        255
#define DHCP_DISCOVER       1
#define DHCP_OFFER          2
#define DHCP_REQUEST        3
#define DHCP_ACK            5
#define DHCP_NAK            6
#define DHCP_LEASE_INFINITE 0xFFFFFFFFu
#define DHCP_DEFAULT_LEASE  3600u

#define DHCP_FIXED 240 /* op..cookie inclusive */

enum {
    ST_INIT = 0,
    ST_SELECTING,
    ST_REQUESTING,
    ST_BOUND,
    ST_RENEWING,
    ST_REBINDING
};

struct at_dhcp {
    uint8_t mac[AT_MAC_LEN];
    uint32_t xid;
    int state;
    uint32_t yiaddr;
    uint32_t server;
    uint32_t netmask;
    uint32_t router;
    uint32_t dns[2];
    int dns_n;
    uint32_t lease_secs;
    uint32_t t1_secs;
    uint32_t t2_secs;
    uint64_t bound_ms;
    struct timeval last_tx;
    int backoff_ms;
};

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

static uint64_t tv_ms(const struct timeval *tv) {
    return (uint64_t)tv->tv_sec * 1000ull + (uint64_t)tv->tv_usec / 1000ull;
}

static void ip4_str(uint32_t addr, char *out, size_t n) {
    struct in_addr a;
    a.s_addr = addr;
    inet_ntop(AF_INET, &a, out, (socklen_t)n);
}

static int lease_active(const at_dhcp_t *d) {
    return d && (d->state == ST_BOUND || d->state == ST_RENEWING || d->state == ST_REBINDING);
}

static void reset_select(at_dhcp_t *d) {
    d->state = ST_INIT;
    d->yiaddr = 0;
    d->server = 0;
    d->lease_secs = 0;
    d->t1_secs = 0;
    d->t2_secs = 0;
    d->bound_ms = 0;
    d->backoff_ms = 500;
    memset(&d->last_tx, 0, sizeof(d->last_tx));
}

/* Fill T1/T2 from options or RFC defaults (50% / 87.5%). Infinite lease skips renew. */
/* 옵션 또는 RFC 기본값(50%/87.5%)으로 T1/T2를 채운다. 무한 임대는 갱신하지 않는다. */
static void apply_lease_timers(at_dhcp_t *d) {
    d->bound_ms = now_ms();
    if (d->lease_secs == 0) d->lease_secs = DHCP_DEFAULT_LEASE;
    if (d->lease_secs == DHCP_LEASE_INFINITE) {
        d->t1_secs = 0;
        d->t2_secs = 0;
        return;
    }
    if (d->t1_secs == 0 || d->t1_secs >= d->lease_secs)
        d->t1_secs = d->lease_secs / 2u;
    if (d->t2_secs == 0 || d->t2_secs <= d->t1_secs || d->t2_secs >= d->lease_secs)
        d->t2_secs = d->lease_secs - d->lease_secs / 8u;
    if (d->t1_secs == 0) d->t1_secs = 1;
    if (d->t2_secs <= d->t1_secs) d->t2_secs = d->t1_secs + 1;
}

at_dhcp_t *at_dhcp_create(const uint8_t mac[AT_MAC_LEN]) {
    at_dhcp_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (mac) memcpy(d->mac, mac, AT_MAC_LEN);
    d->xid = ((uint32_t)getpid() << 16) ^ (uint32_t)time(NULL) ^ 0xA5A5u;
    d->state = ST_INIT;
    d->backoff_ms = 500;
    d->netmask = htonl(0xffffff00u);
    return d;
}

void at_dhcp_destroy(at_dhcp_t *d) { free(d); }

int at_dhcp_bound(const at_dhcp_t *d) { return lease_active(d); }

void at_dhcp_lease(const at_dhcp_t *d, at_net_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!lease_active(d)) return;
    ip4_str(d->yiaddr, out->ip, sizeof(out->ip));
    ip4_str(d->netmask, out->netmask, sizeof(out->netmask));
    if (d->router) ip4_str(d->router, out->gateway, sizeof(out->gateway));
    if (d->dns_n >= 1) {
        ip4_str(d->dns[0], out->dns, sizeof(out->dns));
        if (d->dns_n >= 2) {
            char second[48];
            ip4_str(d->dns[1], second, sizeof(second));
            size_t used = strlen(out->dns);
            if (used + 1 + strlen(second) < sizeof(out->dns)) {
                out->dns[used] = ' ';
                memcpy(out->dns + used + 1, second, strlen(second) + 1);
            }
        }
    }
    out->ready = 1;
}

#ifdef AT_DHCP_TEST
void at_dhcp_debug_age_ms(at_dhcp_t *d, uint64_t age_ms) {
    if (!d) return;
    uint64_t n = now_ms();
    d->bound_ms = n > age_ms ? n - age_ms : 0;
    memset(&d->last_tx, 0, sizeof(d->last_tx));
}

uint32_t at_dhcp_debug_lease_secs(const at_dhcp_t *d) {
    return d ? d->lease_secs : 0;
}

uint32_t at_dhcp_debug_t1_secs(const at_dhcp_t *d) {
    return d ? d->t1_secs : 0;
}

uint32_t at_dhcp_debug_t2_secs(const at_dhcp_t *d) {
    return d ? d->t2_secs : 0;
}
#endif

static size_t append_opt(uint8_t *o, size_t off, size_t cap, uint8_t t,
                         const void *v, uint8_t n) {
    if (off + 2u + n >= cap) return off;
    o[off++] = t;
    o[off++] = n;
    if (n && v) memcpy(o + off, v, n);
    return off + n;
}

static size_t build_bootp(at_dhcp_t *d, uint8_t msg_type, uint8_t *bootp, size_t cap) {
    if (cap < 300) return 0;
    memset(bootp, 0, cap);
    bootp[0] = DHCP_OP_BOOTREQUEST;
    bootp[1] = DHCP_HTYPE_ETH;
    bootp[2] = 6;
    memcpy(bootp + 4, &d->xid, 4);
    /* Renew is unicast (ciaddr set, no broadcast flag). Rebind/Discover broadcast. */
    /* Renew는 유니캐스트(ciaddr, 브로드캐스트 플래그 없음). Rebind/Discover는 브로드캐스트. */
    int renewing = (d->state == ST_RENEWING);
    int rebinding = (d->state == ST_REBINDING);
    if (renewing || rebinding)
        memcpy(bootp + 12, &d->yiaddr, 4); /* ciaddr / 현재 주소 */
    if (!renewing) bootp[10] = 0x80;
    memcpy(bootp + 28, d->mac, AT_MAC_LEN);
    uint32_t cookie = htonl(DHCP_COOKIE);
    memcpy(bootp + 236, &cookie, 4);

    uint8_t *opt = bootp + DHCP_FIXED;
    size_t oc = cap - DHCP_FIXED;
    size_t off = 0;
    uint8_t typ = msg_type;
    off = append_opt(opt, off, oc, DHCP_OPT_TYPE, &typ, 1);
    /* subnet, router, dns, broadcast, lease, T1, T2 */
    off = append_opt(opt, off, oc, DHCP_OPT_PARAM_REQ, "\x01\x03\x06\x1c\x33\x3a\x3b", 7);
    /* Selecting Request: option 50 + 54. Renew/Rebind: ciaddr only (RFC 2131). */
    /* 선택 Request: 옵션 50+54. Renew/Rebind: ciaddr만 (RFC 2131). */
    if (msg_type == DHCP_REQUEST && d->yiaddr && !renewing && !rebinding) {
        off = append_opt(opt, off, oc, DHCP_OPT_REQ_IP, &d->yiaddr, 4);
        if (d->server)
            off = append_opt(opt, off, oc, DHCP_OPT_SERVER, &d->server, 4);
    }
    if (off < oc) opt[off++] = DHCP_OPT_END;
    size_t bootp_len = DHCP_FIXED + off;
    if (bootp_len < 300) bootp_len = 300;
    return bootp_len;
}

static size_t encapsulate_udp_ip_eth(const uint8_t mac[AT_MAC_LEN],
                                     uint32_t src_ip, uint32_t dst_ip,
                                     const uint8_t *bootp, size_t bootp_len,
                                     uint8_t *eth, size_t cap) {
    size_t udp_len = 8 + bootp_len;
    size_t ip_len = 20 + udp_len;
    size_t total = 14 + ip_len;
    if (cap < total) return 0;
    memset(eth, 0xff, 6);
    memcpy(eth + 6, mac, AT_MAC_LEN);
    eth[12] = 0x08;
    eth[13] = 0x00;

    uint8_t *ip = eth + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)ip_len;
    ip[8] = 64;
    ip[9] = IPPROTO_UDP;
    memcpy(ip + 12, &src_ip, 4);
    memcpy(ip + 16, &dst_ip, 4);
    uint16_t csum = at_ip_checksum(ip, 20);
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)csum;

    uint8_t *udp = ip + 20;
    udp[0] = (uint8_t)(DHCP_CLIENT_PORT >> 8);
    udp[1] = (uint8_t)DHCP_CLIENT_PORT;
    udp[2] = (uint8_t)(DHCP_SERVER_PORT >> 8);
    udp[3] = (uint8_t)DHCP_SERVER_PORT;
    udp[4] = (uint8_t)(udp_len >> 8);
    udp[5] = (uint8_t)udp_len;
    udp[6] = 0;
    udp[7] = 0;
    memcpy(udp + 8, bootp, bootp_len);

    /* IPv4 UDP checksum; 0 is also legal. / IPv4 UDP 체크섬. 0 도 허용된다. */
    uint8_t pseudo[12 + 8 + 1024];
    if (8 + bootp_len <= 1024) {
        memset(pseudo, 0, sizeof(pseudo));
        memcpy(pseudo + 0, ip + 12, 4);
        memcpy(pseudo + 4, ip + 16, 4);
        pseudo[9] = IPPROTO_UDP;
        pseudo[10] = (uint8_t)(udp_len >> 8);
        pseudo[11] = (uint8_t)udp_len;
        memcpy(pseudo + 12, udp, udp_len);
        uint16_t u = at_ip_checksum(pseudo, 12 + udp_len);
        if (u == 0) u = 0xffff;
        udp[6] = (uint8_t)(u >> 8);
        udp[7] = (uint8_t)u;
    }
    return total;
}

/* Advance BOUND -> RENEWING (T1) -> REBINDING (T2) -> INIT (expiry). */
/* BOUND에서 T1 Renew, T2 Rebind, 만료 시 INIT으로 되돌린다. */
static void advance_renew_state(at_dhcp_t *d, uint64_t now) {
    if (!lease_active(d)) return;
    if (d->lease_secs == DHCP_LEASE_INFINITE) return;

    uint64_t age = now >= d->bound_ms ? now - d->bound_ms : 0;
    uint64_t lease_ms = (uint64_t)d->lease_secs * 1000ull;
    uint64_t t1_ms = (uint64_t)d->t1_secs * 1000ull;
    uint64_t t2_ms = (uint64_t)d->t2_secs * 1000ull;

    if (lease_ms && age >= lease_ms) {
        at_log(AT_LOG_WARN, "DHCP lease expired — restarting from Discover");
        reset_select(d);
        return;
    }
    if (d->state != ST_REBINDING && t2_ms && age >= t2_ms) {
        at_log(AT_LOG_INFO, "DHCP T2 rebind (xid=0x%08x)", ntohl(d->xid));
        d->state = ST_REBINDING;
        d->backoff_ms = 1000;
        memset(&d->last_tx, 0, sizeof(d->last_tx));
        return;
    }
    if (d->state == ST_BOUND && t1_ms && age >= t1_ms) {
        at_log(AT_LOG_INFO, "DHCP T1 renew (xid=0x%08x)", ntohl(d->xid));
        d->state = ST_RENEWING;
        d->backoff_ms = 1000;
        memset(&d->last_tx, 0, sizeof(d->last_tx));
    }
}

size_t at_dhcp_want_tx(at_dhcp_t *d, uint8_t *eth, size_t cap) {
    if (!d || !eth) return 0;
    uint64_t now = now_ms();
    advance_renew_state(d, now);

    if (d->state == ST_BOUND) return 0;
    if (d->lease_secs == DHCP_LEASE_INFINITE && lease_active(d)) return 0;

    if (d->last_tx.tv_sec || d->last_tx.tv_usec) {
        if (now < tv_ms(&d->last_tx) + (uint64_t)d->backoff_ms) return 0;
    }

    uint8_t msg = DHCP_DISCOVER;
    if (d->state == ST_REQUESTING || d->state == ST_RENEWING || d->state == ST_REBINDING)
        msg = DHCP_REQUEST;

    uint8_t bootp[548];
    size_t blen = build_bootp(d, msg, bootp, sizeof(bootp));
    if (!blen) return 0;

    uint32_t src = 0;
    uint32_t dst = 0xffffffffu; /* 255.255.255.255 */
    if (d->state == ST_RENEWING || d->state == ST_REBINDING) src = d->yiaddr;
    if (d->state == ST_RENEWING && d->server) dst = d->server;

    size_t n = encapsulate_udp_ip_eth(d->mac, src, dst, bootp, blen, eth, cap);
    if (!n) return 0;
    gettimeofday(&d->last_tx, NULL);
    if (d->state == ST_INIT) d->state = ST_SELECTING;
    d->backoff_ms = d->backoff_ms < 4000 ? d->backoff_ms * 2 : 8000;

    const char *name = "DISCOVER";
    if (msg == DHCP_REQUEST) {
        if (d->state == ST_RENEWING) name = "REQUEST(renew)";
        else if (d->state == ST_REBINDING) name = "REQUEST(rebind)";
        else name = "REQUEST";
    }
    at_log(AT_LOG_INFO, "DHCP %s sent (xid=0x%08x)", name, ntohl(d->xid));
    return n;
}

static uint32_t rd_opt_u32(const uint8_t *v) {
    uint32_t n;
    memcpy(&n, v, 4);
    return ntohl(n);
}

static int parse_options(at_dhcp_t *d, const uint8_t *opt, size_t len,
                         uint8_t *msg_type) {
    *msg_type = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t t = opt[i++];
        if (t == DHCP_OPT_PAD) continue;
        if (t == DHCP_OPT_END) break;
        if (i >= len) break;
        uint8_t n = opt[i++];
        if (i + n > len) break;
        const uint8_t *v = opt + i;
        if (t == DHCP_OPT_TYPE && n >= 1) *msg_type = v[0];
        else if (t == DHCP_OPT_SERVER && n >= 4) memcpy(&d->server, v, 4);
        else if (t == DHCP_OPT_SUBNET && n >= 4) memcpy(&d->netmask, v, 4);
        else if (t == DHCP_OPT_ROUTER && n >= 4) memcpy(&d->router, v, 4);
        else if (t == DHCP_OPT_LEASE && n >= 4) d->lease_secs = rd_opt_u32(v);
        else if (t == DHCP_OPT_T1 && n >= 4) d->t1_secs = rd_opt_u32(v);
        else if (t == DHCP_OPT_T2 && n >= 4) d->t2_secs = rd_opt_u32(v);
        else if (t == DHCP_OPT_DNS && n >= 4) {
            d->dns_n = 0;
            if (n >= 4) {
                memcpy(&d->dns[0], v, 4);
                d->dns_n = 1;
            }
            if (n >= 8) {
                memcpy(&d->dns[1], v + 4, 4);
                d->dns_n = 2;
            }
        }
        i += n;
    }
    return 0;
}

int at_dhcp_on_frame(at_dhcp_t *d, const uint8_t *eth, size_t len) {
    if (!d || !eth) return 0;
    if (len < 14 + 20 + 8 + DHCP_FIXED) return 0;
    if (eth[12] != 0x08 || eth[13] != 0x00) return 0;
    const uint8_t *ip = eth + 14;
    if ((ip[0] >> 4) != 4) return 0;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || 14 + ihl + 8 + DHCP_FIXED > len) return 0;
    if (ip[9] != IPPROTO_UDP) return 0;
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
    if (dport != DHCP_CLIENT_PORT) return 0;
    const uint8_t *bootp = udp + 8;
    size_t blen = len - (size_t)(bootp - eth);
    if (blen < DHCP_FIXED) return 0;
    if (bootp[0] != DHCP_OP_BOOTREPLY) return 0;
    uint32_t xid;
    memcpy(&xid, bootp + 4, 4);
    if (xid != d->xid) return 0;
    uint32_t cookie;
    memcpy(&cookie, bootp + 236, 4);
    if (ntohl(cookie) != DHCP_COOKIE) return 0;

    uint8_t msg = 0;
    parse_options(d, bootp + DHCP_FIXED, blen - DHCP_FIXED, &msg);
    uint32_t new_yi = 0;
    memcpy(&new_yi, bootp + 16, 4);

    if (msg == DHCP_OFFER && new_yi) {
        if (lease_active(d)) return 0;
        memcpy(&d->yiaddr, &new_yi, 4);
        char ipstr[48];
        ip4_str(d->yiaddr, ipstr, sizeof(ipstr));
        at_log(AT_LOG_INFO, "DHCP OFFER %s", ipstr);
        d->state = ST_REQUESTING;
        d->backoff_ms = 200;
        memset(&d->last_tx, 0, sizeof(d->last_tx));
        return 0;
    }
    if (msg == DHCP_ACK && new_yi) {
        int was_active = lease_active(d);
        uint32_t old_ip = d->yiaddr;
        memcpy(&d->yiaddr, &new_yi, 4);
        char ipstr[48];
        ip4_str(d->yiaddr, ipstr, sizeof(ipstr));
        at_log(AT_LOG_INFO, was_active ? "DHCP ACK (renew) %s" : "DHCP ACK %s", ipstr);
        if (d->dns_n == 0 && d->router) {
            d->dns[0] = d->router;
            d->dns_n = 1;
        }
        if (!d->router && d->server) d->router = d->server;
        apply_lease_timers(d);
        d->state = ST_BOUND;
        d->backoff_ms = 500;
        memset(&d->last_tx, 0, sizeof(d->last_tx));
        if (!was_active) return 1;
        return (old_ip != d->yiaddr) ? 1 : 0;
    }
    if (msg == DHCP_NAK) {
        int had = lease_active(d) || d->state == ST_REQUESTING;
        at_log(AT_LOG_WARN, "DHCP NAK — restarting from Discover");
        reset_select(d);
        return had ? -1 : 0;
    }
    return 0;
}
