/*
 * dhcp_client.h
 * Userspace DHCPv4 client that talks Ethernet frames (USB RNDIS path).
 * USB RNDIS 경로로 이더넷 프레임을 주고받는 사용자 공간 DHCPv4 클라이언트.
 */
#ifndef AT_DHCP_CLIENT_H
#define AT_DHCP_CLIENT_H

#include "net_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct at_dhcp at_dhcp_t;

at_dhcp_t *at_dhcp_create(const uint8_t mac[AT_MAC_LEN]);
void at_dhcp_destroy(at_dhcp_t *d);

/* Build a Discover/Request Ethernet frame if a transmit is due. 0 = none. */
/* 송신이 필요하면 Discover/Request 이더넷 프레임을 만든다. 0 이면 없음. */
size_t at_dhcp_want_tx(at_dhcp_t *d, uint8_t *eth, size_t cap);

/*
 * Feed a frame from the phone.
 * 1 = newly bound or lease address changed; -1 = NAK / lease lost; 0 = no change.
 * 폰에서 온 프레임을 넣는다.
 * 1 = 신규 바인드 또는 주소 변경, -1 = NAK/임대 상실, 0 = 변화 없음.
 */
int at_dhcp_on_frame(at_dhcp_t *d, const uint8_t *eth, size_t len);

/* True while BOUND / RENEWING / REBINDING (address is still usable). */
/* BOUND·RENEWING·REBINDING 이면 참 (주소는 아직 쓸 수 있다). */
int at_dhcp_bound(const at_dhcp_t *d);
void at_dhcp_lease(const at_dhcp_t *d, at_net_info_t *out);

#ifdef AT_DHCP_TEST
/* Pretend `age_ms` has passed since the last ACK (unit tests). */
/* 마지막 ACK 이후 age_ms 가 지난 것처럼 만든다 (단위 테스트). */
void at_dhcp_debug_age_ms(at_dhcp_t *d, uint64_t age_ms);
uint32_t at_dhcp_debug_lease_secs(const at_dhcp_t *d);
uint32_t at_dhcp_debug_t1_secs(const at_dhcp_t *d);
uint32_t at_dhcp_debug_t2_secs(const at_dhcp_t *d);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AT_DHCP_CLIENT_H */
