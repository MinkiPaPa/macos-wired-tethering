/*
 * rndis.c
 * Pure RNDIS message builders/parsers. No USB, no macOS.
 * 순수 RNDIS 메시지 생성/파서. USB/macOS 의존 없음.
 */
#include "rndis.h"

/* Split a RNDIS data buffer that may hold more than one Ethernet frame. */
/* 이더넷 프레임이 여러 개 들어 있는 RNDIS 데이터 버퍼를 나눈다. */
static int emit_eth_payload(const uint8_t *p, size_t n, rndis_eth_cb cb, void *user) {
    if (!p || n < 14) return 0;
    if (n <= AT_ETH_MAX) {
        cb(p, n, user);
        return 1;
    }
    int frames = 0;
    size_t i = 0;
    while (i + 14 <= n) {
        uint16_t et = (uint16_t)(((uint16_t)p[i + 12] << 8) | p[i + 13]);
        size_t flen = 0;
        if (et == 0x0800 && i + 14 + 20 <= n) {
            unsigned ihl = (unsigned)(p[i + 14] & 0x0f) * 4u;
            size_t iplen = ((size_t)p[i + 16] << 8) | p[i + 17];
            if (ihl >= 20 && iplen >= ihl) flen = 14 + iplen;
        } else if (et == 0x0806) {
            flen = 42;
        } else if (et == 0x86DD && i + 14 + 40 <= n) {
            size_t plen = ((size_t)p[i + 18] << 8) | p[i + 19];
            flen = 14 + 40 + plen;
        }
        if (flen < 14 || i + flen > n) {
            cb(p + i, n - i, user);
            frames++;
            break;
        }
        cb(p + i, flen, user);
        frames++;
        i += flen;
    }
    return frames;
}

/* PacketAlignmentFactor is a shift count: 2 => 4 bytes, 4 => 16 bytes. */
/* PacketAlignmentFactor 는 시프트 값이다. 2면 4바이트, 4면 16바이트. */
static size_t align_pow2(size_t n, uint32_t shift) {
    if (shift < 2) shift = 2;
    if (shift > 8) shift = 8;
    size_t a = (size_t)1u << shift;
    return (n + a - 1u) & ~(a - 1u);
}

size_t rndis_build_initialize(uint8_t *buf, size_t buf_len, uint32_t request_id,
                              uint32_t max_xfer) {
    if (!buf || buf_len < RNDIS_INITIALIZE_LEN) return 0;
    memset(buf, 0, RNDIS_INITIALIZE_LEN);
    at_wr32le(buf + 0, RNDIS_MSG_INITIALIZE);
    at_wr32le(buf + 4, RNDIS_INITIALIZE_LEN);
    at_wr32le(buf + 8, request_id);
    at_wr32le(buf + 12, 1); /* MajorVersion 1.0 / 메이저 버전 */
    at_wr32le(buf + 16, 0); /* MinorVersion */
    at_wr32le(buf + 20, max_xfer);
    return RNDIS_INITIALIZE_LEN;
}

size_t rndis_build_halt(uint8_t *buf, size_t buf_len, uint32_t request_id) {
    if (!buf || buf_len < 12) return 0;
    memset(buf, 0, 12);
    at_wr32le(buf + 0, RNDIS_MSG_HALT);
    at_wr32le(buf + 4, 12);
    at_wr32le(buf + 8, request_id);
    return 12;
}

size_t rndis_build_keepalive(uint8_t *buf, size_t buf_len, uint32_t request_id) {
    if (!buf || buf_len < 16) return 0;
    memset(buf, 0, 16);
    at_wr32le(buf + 0, RNDIS_MSG_KEEPALIVE);
    at_wr32le(buf + 4, 16);
    at_wr32le(buf + 8, request_id);
    return 16;
}

size_t rndis_build_keepalive_cmplt(uint8_t *buf, size_t buf_len, uint32_t request_id) {
    if (!buf || buf_len < 16) return 0;
    memset(buf, 0, 16);
    at_wr32le(buf + 0, RNDIS_MSG_KEEPALIVE_CMPLT);
    at_wr32le(buf + 4, 16);
    at_wr32le(buf + 8, request_id);
    at_wr32le(buf + 12, RNDIS_STATUS_SUCCESS);
    return 16;
}

size_t rndis_build_query(uint8_t *buf, size_t buf_len, uint32_t request_id,
                         uint32_t oid, const uint8_t *in, uint32_t in_len) {
    size_t total = RNDIS_QUERY_SET_HDR_LEN + in_len;
    if (!buf || buf_len < total) return 0;
    memset(buf, 0, total);
    at_wr32le(buf + 0, RNDIS_MSG_QUERY);
    at_wr32le(buf + 4, (uint32_t)total);
    at_wr32le(buf + 8, request_id);
    at_wr32le(buf + 12, oid);
    at_wr32le(buf + 16, in_len);
    /* Offset is from byte 8. Header after RequestId is 20 bytes. */
    /* 오프셋은 바이트 8부터 센다. RequestId 이후 헤더가 20바이트. */
    at_wr32le(buf + 20, in_len ? 20 : 0);
    at_wr32le(buf + 24, 0);
    if (in && in_len) memcpy(buf + RNDIS_QUERY_SET_HDR_LEN, in, in_len);
    return total;
}

size_t rndis_build_set(uint8_t *buf, size_t buf_len, uint32_t request_id,
                       uint32_t oid, const uint8_t *value, uint32_t value_len) {
    size_t total = RNDIS_QUERY_SET_HDR_LEN + value_len;
    if (!buf || buf_len < total || !value) return 0;
    memset(buf, 0, total);
    at_wr32le(buf + 0, RNDIS_MSG_SET);
    at_wr32le(buf + 4, (uint32_t)total);
    at_wr32le(buf + 8, request_id);
    at_wr32le(buf + 12, oid);
    at_wr32le(buf + 16, value_len);
    at_wr32le(buf + 20, 20);
    at_wr32le(buf + 24, 0);
    memcpy(buf + RNDIS_QUERY_SET_HDR_LEN, value, value_len);
    return total;
}

size_t rndis_wrap_ethernet(uint8_t *out, size_t out_len,
                           const uint8_t *eth, size_t eth_len) {
    return rndis_wrap_ethernet_aligned(out, out_len, eth, eth_len, 2);
}

size_t rndis_wrap_ethernet_aligned(uint8_t *out, size_t out_len,
                                   const uint8_t *eth, size_t eth_len,
                                   uint32_t align_shift) {
    if (!out || !eth || eth_len == 0) return 0;
    size_t payload = eth_len;
    /* Pad short frames to Ethernet minimum (without FCS). */
    /* FCS를 제외한 이더넷 최소 길이까지 패딩한다. */
    if (payload < AT_ETH_MIN) payload = AT_ETH_MIN;
    size_t total = align_pow2(RNDIS_PACKET_HDR_LEN + payload, align_shift);
    if (out_len < total) return 0;
    memset(out, 0, total);
    at_wr32le(out + 0, RNDIS_MSG_PACKET);
    at_wr32le(out + 4, (uint32_t)total);
    at_wr32le(out + 8, 36); /* DataOffset = 44 - 8 */
    at_wr32le(out + 12, (uint32_t)payload);
    memcpy(out + RNDIS_PACKET_HDR_LEN, eth, eth_len);
    return total;
}

static int check_cmplt_header(const uint8_t *buf, size_t len, uint32_t expect_type,
                              uint32_t expect_id, uint32_t *status_out) {
    if (!buf || len < 16) return -1;
    uint32_t type = at_rd32le(buf + 0);
    uint32_t msglen = at_rd32le(buf + 4);
    uint32_t rid = at_rd32le(buf + 8);
    uint32_t status = at_rd32le(buf + 12);
    if (type != expect_type) return -2;
    if (msglen > len) return -3;
    if (rid != expect_id) return -4;
    if (status_out) *status_out = status;
    if (status != RNDIS_STATUS_SUCCESS) return -5;
    return 0;
}

int rndis_parse_initialize_cmplt(const uint8_t *buf, size_t len,
                                 uint32_t expect_id, rndis_init_result_t *out) {
    uint32_t status = 0;
    int rc = check_cmplt_header(buf, len, RNDIS_MSG_INITIALIZE_CMPLT, expect_id, &status);
    if (rc != 0) return rc;
    if (len < 52) return -6;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->device_flags = at_rd32le(buf + 24);
        out->medium = at_rd32le(buf + 28);
        out->max_packets_per_xfer = at_rd32le(buf + 32);
        out->max_transfer_size = at_rd32le(buf + 36);
        out->packet_alignment = at_rd32le(buf + 40);
    }
    return 0;
}

int rndis_parse_set_cmplt(const uint8_t *buf, size_t len, uint32_t expect_id) {
    uint32_t status = 0;
    return check_cmplt_header(buf, len, RNDIS_MSG_SET_CMPLT, expect_id, &status);
}

int rndis_parse_query_cmplt(const uint8_t *buf, size_t len, uint32_t expect_id,
                            const uint8_t **info, uint32_t *info_len) {
    uint32_t status = 0;
    int rc = check_cmplt_header(buf, len, RNDIS_MSG_QUERY_CMPLT, expect_id, &status);
    if (rc != 0) return rc;
    if (len < 24) return -6;
    uint32_t ilen = at_rd32le(buf + 16);
    uint32_t ioff = at_rd32le(buf + 20);
    /* Information buffer starts at RequestId + offset (byte 8 + ioff). */
    /* 정보 버퍼는 RequestId + offset (바이트 8 + ioff)에서 시작한다. */
    size_t start = (size_t)RNDIS_OFFSET_BASE + ioff;
    if (start + ilen > len) return -7;
    if (info) *info = buf + start;
    if (info_len) *info_len = ilen;
    return 0;
}

int rndis_parse_status(const uint8_t *buf, size_t len, uint32_t *status_out) {
    if (!buf || len < 16) return -1;
    if (at_rd32le(buf + 0) != RNDIS_MSG_INDICATE_STATUS) return -2;
    if (status_out) *status_out = at_rd32le(buf + 12);
    return 0;
}

static int looks_like_rndis_packet(const uint8_t *buf, size_t len, size_t s,
                                   uint32_t *data_off_out, uint32_t *data_len_out) {
    if (s + RNDIS_PACKET_HDR_LEN > len) return 0;
    if (at_rd32le(buf + s) != RNDIS_MSG_PACKET) return 0;
    uint32_t data_off = at_rd32le(buf + s + 8);
    uint32_t data_len = at_rd32le(buf + s + 12);
    /* Spec DataOffset is 36 (payload at +44). Reject other values to avoid
     * matching 01 00 00 00 inside an Ethernet payload. */
    /* 스펙 DataOffset 은 36 (페이로드는 +44). 이더넷 안의 01 00 00 00 과
     * 구분하려고 다른 값은 거절한다. */
    if (data_off != 36) return 0;
    if (data_len < 14 || data_len > 4096) return 0;
    size_t payload_at = s + RNDIS_OFFSET_BASE + data_off;
    if (payload_at + data_len > len) return 0;
    if (data_off_out) *data_off_out = data_off;
    if (data_len_out) *data_len_out = data_len;
    return 1;
}

int rndis_unwrap_bulk_aligned(const uint8_t *buf, size_t len, uint32_t align_shift,
                              rndis_eth_cb cb, void *user) {
    if (!buf || !cb) return -1;
    (void)align_shift;
    size_t off = 0;
    int frames = 0;
    /* Do not walk by MessageLength or 16-byte alignment. Samsung often places
     * the next PACKET_MSG at a 4-byte boundary (or immediately after the
     * payload). Jumping to 16 bytes skipped those headers (4524B frames=1). */
    /* MessageLength 나 16바이트 정렬로 걷지 않는다. 삼성은 다음 PACKET_MSG를
     * 4바이트 경계(또는 페이로드 바로 뒤)에 두는 경우가 많다. 16바이트로
     * 점프하면 그 헤더를 건너뛴다 (4524B frames=1). */
    while (off + RNDIS_PACKET_HDR_LEN <= len) {
        size_t found = (size_t)-1;
        uint32_t data_off = 0, data_len = 0;
        for (size_t s = off; s + RNDIS_PACKET_HDR_LEN <= len; s++) {
            if (looks_like_rndis_packet(buf, len, s, &data_off, &data_len)) {
                found = s;
                break;
            }
        }
        if (found == (size_t)-1) break;
        size_t payload_at = found + RNDIS_OFFSET_BASE + data_off;
        frames += emit_eth_payload(buf + payload_at, data_len, cb, user);
        size_t next = payload_at + data_len;
        if (next <= off) break;
        off = next;
    }
    return frames;
}

int rndis_unwrap_bulk(const uint8_t *buf, size_t len, rndis_eth_cb cb, void *user) {
    return rndis_unwrap_bulk_aligned(buf, len, 2, cb, user);
}
