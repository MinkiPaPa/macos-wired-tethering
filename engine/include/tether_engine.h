/*
 * tether_engine.h
 * Orchestrates USB RNDIS + utun + DHCP packet pump.
 * USB RNDIS, utun, DHCP 패킷 펌프를 조율한다.
 */
#ifndef AT_TETHER_ENGINE_H
#define AT_TETHER_ENGINE_H

#include "common.h"
#include "usb_rndis.h"
#include "net_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AT_STATE_IDLE = 0,
    AT_STATE_CONNECTING,
    AT_STATE_CONNECTED,
    AT_STATE_STOPPING,
    AT_STATE_ERROR
} at_state_t;

typedef struct {
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_errors;
    uint64_t tx_errors;
} at_stats_t;

typedef struct at_engine at_engine_t;

typedef struct {
    at_usb_device_info_t device;
    int prefer_default_route;
} at_engine_opts_t;

at_engine_t *at_engine_start(const at_engine_opts_t *opts);
/* Reason for the last NULL from at_engine_start. Empty after a successful start. */
/* at_engine_start 가 NULL 을 준 이유. 성공 후에는 빈 문자열. */
const char *at_engine_last_error(void);
void at_engine_stop(at_engine_t *e);

at_state_t at_engine_state(const at_engine_t *e);
void at_engine_stats(const at_engine_t *e, at_stats_t *out);
void at_engine_net(const at_engine_t *e, at_net_info_t *out);
const char *at_engine_iface(const at_engine_t *e);
const char *at_engine_error(const at_engine_t *e);
void at_engine_mac(const at_engine_t *e, uint8_t mac[AT_MAC_LEN]);
/* USB bus ceiling in Mbps (High Speed = 480). / USB 버스 상한(Mbps). High Speed = 480. */
uint32_t at_engine_link_mbps(const at_engine_t *e);

#ifdef __cplusplus
}
#endif

#endif /* AT_TETHER_ENGINE_H */
