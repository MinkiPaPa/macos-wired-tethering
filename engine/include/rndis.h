/*
 * rndis.h
 * Host-side Remote NDIS (RNDIS) framing.
 * 호스트 측 Remote NDIS 프레이밍. USB 하드웨어와 독립적이다.
 *
 * Spec notes / 스펙 메모:
 *  - Every multi-byte field is little-endian.
 *  - Buffer offsets are measured from byte 8 (RequestId), not byte 0.
 *  - One bulk transfer may contain several PACKET_MSG records.
 */
#ifndef AT_RNDIS_H
#define AT_RNDIS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RNDIS_MSG_PACKET            0x00000001u
#define RNDIS_MSG_INITIALIZE        0x00000002u
#define RNDIS_MSG_HALT              0x00000003u
#define RNDIS_MSG_QUERY             0x00000004u
#define RNDIS_MSG_SET               0x00000005u
#define RNDIS_MSG_RESET             0x00000006u
#define RNDIS_MSG_INDICATE_STATUS   0x00000007u
#define RNDIS_MSG_KEEPALIVE         0x00000008u
#define RNDIS_MSG_COMPLETION        0x80000000u

#define RNDIS_MSG_INITIALIZE_CMPLT  (RNDIS_MSG_COMPLETION | RNDIS_MSG_INITIALIZE)
#define RNDIS_MSG_QUERY_CMPLT       (RNDIS_MSG_COMPLETION | RNDIS_MSG_QUERY)
#define RNDIS_MSG_SET_CMPLT         (RNDIS_MSG_COMPLETION | RNDIS_MSG_SET)
#define RNDIS_MSG_KEEPALIVE_CMPLT   (RNDIS_MSG_COMPLETION | RNDIS_MSG_KEEPALIVE)

#define RNDIS_STATUS_SUCCESS        0x00000000u
#define RNDIS_STATUS_FAILURE        0xC0000001u
#define RNDIS_STATUS_MEDIA_CONNECT  0x4001000Bu
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000Cu

#define RNDIS_OID_GEN_SUPPORTED_LIST          0x00010101u
#define RNDIS_OID_GEN_HARDWARE_STATUS         0x00010102u
#define RNDIS_OID_GEN_MEDIA_SUPPORTED         0x00010103u
#define RNDIS_OID_GEN_MEDIA_IN_USE            0x00010104u
#define RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106u
#define RNDIS_OID_GEN_LINK_SPEED              0x00010107u
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER   0x0001010Eu
#define RNDIS_OID_GEN_MAXIMUM_TOTAL_SIZE      0x00010111u
#define RNDIS_OID_GEN_MEDIA_CONNECT_STATUS    0x00010114u
#define RNDIS_OID_GEN_PHYSICAL_MEDIUM         0x00010202u
#define RNDIS_OID_802_3_PERMANENT_ADDRESS     0x01010101u
#define RNDIS_OID_802_3_CURRENT_ADDRESS       0x01010102u

#define RNDIS_PACKET_TYPE_DIRECTED      0x00000001u
#define RNDIS_PACKET_TYPE_MULTICAST     0x00000002u
#define RNDIS_PACKET_TYPE_ALL_MULTICAST 0x00000004u
#define RNDIS_PACKET_TYPE_BROADCAST     0x00000008u
#define RNDIS_PACKET_TYPE_PROMISCUOUS   0x00000020u

/* Linux rndis_host default: directed + broadcast + all-multicast + promiscuous */
#define RNDIS_DEFAULT_FILTER ( \
    RNDIS_PACKET_TYPE_DIRECTED | \
    RNDIS_PACKET_TYPE_BROADCAST | \
    RNDIS_PACKET_TYPE_ALL_MULTICAST | \
    RNDIS_PACKET_TYPE_PROMISCUOUS)

#define RNDIS_PACKET_HDR_LEN        44
#define RNDIS_INITIALIZE_LEN        24
#define RNDIS_QUERY_SET_HDR_LEN     28
#define RNDIS_OFFSET_BASE           8
#define RNDIS_CONTROL_BUF           1024

#define USB_CDC_SEND_ENCAPSULATED   0x00
#define USB_CDC_GET_ENCAPSULATED    0x01
#define USB_BM_OUT_CLASS_IFACE      0x21
#define USB_BM_IN_CLASS_IFACE       0xA1

typedef struct {
    uint32_t max_transfer_size;
    uint32_t max_packets_per_xfer;
    uint32_t packet_alignment; /* log2 of alignment, as returned by the device */
    uint32_t device_flags;
    uint32_t medium;
} rndis_init_result_t;

typedef void (*rndis_eth_cb)(const uint8_t *frame, size_t len, void *user);

size_t rndis_build_initialize(uint8_t *buf, size_t buf_len, uint32_t request_id,
                              uint32_t max_xfer);
size_t rndis_build_halt(uint8_t *buf, size_t buf_len, uint32_t request_id);
size_t rndis_build_keepalive(uint8_t *buf, size_t buf_len, uint32_t request_id);
size_t rndis_build_query(uint8_t *buf, size_t buf_len, uint32_t request_id,
                         uint32_t oid, const uint8_t *in, uint32_t in_len);
size_t rndis_build_set(uint8_t *buf, size_t buf_len, uint32_t request_id,
                       uint32_t oid, const uint8_t *value, uint32_t value_len);
size_t rndis_build_keepalive_cmplt(uint8_t *buf, size_t buf_len, uint32_t request_id);
size_t rndis_wrap_ethernet(uint8_t *out, size_t out_len,
                           const uint8_t *eth, size_t eth_len);
/* align_shift is RNDIS PacketAlignmentFactor (2 => 4-byte, 4 => 16-byte). */
/* align_shift 는 RNDIS PacketAlignmentFactor (2면 4바이트, 4면 16바이트). */
size_t rndis_wrap_ethernet_aligned(uint8_t *out, size_t out_len,
                                   const uint8_t *eth, size_t eth_len,
                                   uint32_t align_shift);

int rndis_parse_initialize_cmplt(const uint8_t *buf, size_t len,
                                 uint32_t expect_id, rndis_init_result_t *out);
int rndis_parse_set_cmplt(const uint8_t *buf, size_t len, uint32_t expect_id);
int rndis_parse_query_cmplt(const uint8_t *buf, size_t len, uint32_t expect_id,
                            const uint8_t **info, uint32_t *info_len);
int rndis_parse_status(const uint8_t *buf, size_t len, uint32_t *status_out);

/*
 * Walk a bulk-IN buffer and invoke cb for every Ethernet payload.
 * 벌크 IN 버퍼를 순회하며 이더넷 페이로드마다 콜백을 호출한다.
 */
int rndis_unwrap_bulk(const uint8_t *buf, size_t len, rndis_eth_cb cb, void *user);
/* Walk with the device PacketAlignmentFactor (shift: 2=4B, 4=16B). */
/* 기기 PacketAlignmentFactor(시프트: 2=4B, 4=16B)로 순회한다. */
int rndis_unwrap_bulk_aligned(const uint8_t *buf, size_t len, uint32_t align_shift,
                              rndis_eth_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* AT_RNDIS_H */
