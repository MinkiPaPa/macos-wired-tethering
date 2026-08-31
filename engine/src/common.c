/*
 * common.c
 * Logging and small helpers.
 * 로그와 작은 헬퍼 함수.
 */
#include "common.h"

#include <stdarg.h>

static at_log_fn g_log_fn = NULL;
static void *g_log_user = NULL;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

void at_set_logger(at_log_fn fn, void *user) {
    pthread_mutex_lock(&g_log_mu);
    g_log_fn = fn;
    g_log_user = user;
    pthread_mutex_unlock(&g_log_mu);
}

void at_log(at_log_level_t level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_mu);
    at_log_fn fn = g_log_fn;
    void *user = g_log_user;
    pthread_mutex_unlock(&g_log_mu);

    if (fn) {
        fn(level, buf, user);
        return;
    }

    const char *tag = "INFO";
    if (level == AT_LOG_DEBUG) tag = "DEBUG";
    else if (level == AT_LOG_WARN) tag = "WARN";
    else if (level == AT_LOG_ERROR) tag = "ERROR";
    fprintf(stderr, "[%s] %s\n", tag, buf);
}

void at_mac_format(const uint8_t mac[AT_MAC_LEN], char *out, size_t out_len) {
    if (!out || out_len < 18) return;
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void at_mac_snapshot_pair(pthread_mutex_t *mu,
                          const uint8_t a[AT_MAC_LEN],
                          const uint8_t b[AT_MAC_LEN],
                          uint8_t out_a[AT_MAC_LEN],
                          uint8_t out_b[AT_MAC_LEN]) {
    if (!mu || !a || !b || !out_a || !out_b) return;
    pthread_mutex_lock(mu);
    memcpy(out_a, a, AT_MAC_LEN);
    memcpy(out_b, b, AT_MAC_LEN);
    pthread_mutex_unlock(mu);
}

void at_mac_store(pthread_mutex_t *mu, uint8_t dst[AT_MAC_LEN],
                  const uint8_t src[AT_MAC_LEN]) {
    if (!mu || !dst || !src) return;
    pthread_mutex_lock(mu);
    memcpy(dst, src, AT_MAC_LEN);
    pthread_mutex_unlock(mu);
}

int at_eth_learn_gw_mac(const uint8_t *frame, size_t len,
                        const uint8_t host_mac[AT_MAC_LEN],
                        uint32_t gw_ip,
                        uint8_t out[AT_MAC_LEN]) {
    if (!frame || !host_mac || !out || gw_ip == 0 || len < 14) return 0;
    if (memcmp(frame + 6, host_mac, AT_MAC_LEN) == 0) return 0;
    uint16_t et = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);
    if (et == 0x0806) {
        if (len < 42) return 0;
        uint16_t op = (uint16_t)(((uint16_t)frame[20] << 8) | frame[21]);
        uint32_t spa;
        memcpy(&spa, frame + 28, 4);
        if (op != 2 || spa != gw_ip) return 0;
        memcpy(out, frame + 22, AT_MAC_LEN);
        return 1;
    }
    if (et == 0x0800) {
        if (len < 14 + 20) return 0;
        if ((frame[14] >> 4) != 4) return 0;
        uint32_t src;
        memcpy(&src, frame + 14 + 12, 4);
        if (src != gw_ip) return 0;
        memcpy(out, frame + 6, AT_MAC_LEN);
        return 1;
    }
    return 0;
}

int at_json_escape(const char *src, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return -1;
    size_t o = 0;
    if (!src) src = "";
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        const char *rep = NULL;
        char tmp[8];
        if (*p == '"') rep = "\\\"";
        else if (*p == '\\') rep = "\\\\";
        else if (*p == '\n') rep = "\\n";
        else if (*p == '\r') rep = "\\r";
        else if (*p == '\t') rep = "\\t";
        else if (*p < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
            rep = tmp;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n + 1 >= dst_len) {
                dst[o] = '\0';
                return -1;
            }
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            if (o + 2 >= dst_len) {
                dst[o] = '\0';
                return -1;
            }
            dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
    return 0;
}

uint16_t at_ip_checksum(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t s = 0;
    while (len > 1) {
        s += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xffffu) + (s >> 16);
    return (uint16_t)~s;
}

int at_ip4_fix_checksums(uint8_t *ip, size_t len) {
    if (!ip || len < 20) return -1;
    if ((ip[0] >> 4) != 4) return -1;
    unsigned ihl = (unsigned)(ip[0] & 0x0f) * 4u;
    if (ihl < 20 || len < ihl) return -1;

    /* Skip L4 on fragments; still fix the IP header checksum. */
    /* 조각이면 L4는 건너뛰고 IP 헤더 체크섬만 고친다. */
    uint16_t frag = (uint16_t)(((uint16_t)ip[6] << 8) | ip[7]);
    int is_frag = (frag & 0x1fff) != 0;

    uint8_t proto = ip[9];
    uint8_t *l4 = ip + ihl;
    size_t l4len = len - ihl;
    int csum_off = -1;
    if (!is_frag && l4len >= 4) {
        if (proto == 6 && l4len >= 20) csum_off = 16;       /* TCP */
        else if (proto == 17 && l4len >= 8) csum_off = 6;    /* UDP */
        else if (proto == 1 && l4len >= 4) csum_off = 2;     /* ICMP */
    }
    int ip_missing = (ip[10] | ip[11]) == 0;
    int l4_missing = (csum_off >= 0) && ((l4[csum_off] | l4[csum_off + 1]) == 0);
    /* Already filled (no utun offload): skip the expensive recompute. */
    /* 이미 채워져 있으면(오프로드 아님) 비싼 재계산을 건너뛴다. */
    if (!ip_missing && !l4_missing) return 0;

    if (ip_missing) {
        ip[10] = 0;
        ip[11] = 0;
        uint16_t ipcs = at_ip_checksum(ip, ihl);
        ip[10] = (uint8_t)(ipcs >> 8);
        ip[11] = (uint8_t)(ipcs);
    }

    if (is_frag || csum_off < 0 || !l4_missing) return 0;

    l4[csum_off] = 0;
    l4[csum_off + 1] = 0;

    uint32_t s = 0;
    if (proto == 6 || proto == 17) {
        /* IPv4 pseudo-header: src, dst, zero+proto, L4 length. */
        /* IPv4 의사 헤더: src, dst, 0+프로토콜, L4 길이. */
        s += ((uint32_t)ip[12] << 8) | ip[13];
        s += ((uint32_t)ip[14] << 8) | ip[15];
        s += ((uint32_t)ip[16] << 8) | ip[17];
        s += ((uint32_t)ip[18] << 8) | ip[19];
        s += proto;
        s += (uint32_t)l4len;
    }
    const uint8_t *p = l4;
    size_t n = l4len;
    while (n > 1) {
        s += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        n -= 2;
    }
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xffffu) + (s >> 16);
    uint16_t csum = (uint16_t)~s;
    /* UDP checksum 0 means "no checksum"; store 0xffff instead. */
    /* UDP 체크섬 0은 '없음'이므로 0xffff 로 저장한다. */
    if (proto == 17 && csum == 0) csum = 0xffff;
    l4[csum_off] = (uint8_t)(csum >> 8);
    l4[csum_off + 1] = (uint8_t)csum;
    return 0;
}
