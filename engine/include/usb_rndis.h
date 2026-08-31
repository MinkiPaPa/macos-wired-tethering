/*
 * usb_rndis.h
 * IOKit USB transport for RNDIS control and bulk data.
 * RNDIS 제어/데이터용 IOKit USB 전송 계층.
 */
#ifndef AT_USB_RNDIS_H
#define AT_USB_RNDIS_H

#include "common.h"
#include "rndis.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AT_KIND_UNKNOWN = 0,
    AT_KIND_RNDIS   = 1, /* USB tethering is already on / 테더링 이미 켜짐 */
    AT_KIND_ANDROID = 2  /* Phone seen, but not in RNDIS mode / 폰은 보이나 RNDIS 아님 */
} at_device_kind_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint32_t location_id;
    char     name[AT_MAX_NAME];
    char     vendor[AT_MAX_NAME];
    char     serial[AT_MAX_SERIAL];
    at_device_kind_t kind;
    uint8_t  control_ifc;
    uint8_t  data_ifc;
} at_usb_device_info_t;

typedef struct at_usb_rndis at_usb_rndis_t;

int at_usb_list_devices(at_usb_device_info_t *out, int max_out, int *count);

at_usb_rndis_t *at_usb_open(const at_usb_device_info_t *want);
void at_usb_close(at_usb_rndis_t *u);

int at_usb_rndis_init(at_usb_rndis_t *u, uint8_t mac_out[AT_MAC_LEN],
                      rndis_init_result_t *init_out);
int at_usb_rndis_halt(at_usb_rndis_t *u);

int at_usb_bulk_write(at_usb_rndis_t *u, const uint8_t *data, size_t len);
int at_usb_bulk_read(at_usb_rndis_t *u, uint8_t *data, size_t cap, size_t *got,
                     uint32_t timeout_ms);

int at_usb_poll_interrupt(at_usb_rndis_t *u, uint32_t timeout_ms);
int at_usb_handle_control_events(at_usb_rndis_t *u);
int at_usb_keepalive(at_usb_rndis_t *u);

uint32_t at_usb_max_transfer(const at_usb_rndis_t *u);
uint32_t at_usb_max_packets(const at_usb_rndis_t *u);
uint32_t at_usb_packet_align(const at_usb_rndis_t *u);
uint32_t at_usb_bus_mbps(const at_usb_rndis_t *u);
uint32_t at_usb_rndis_link_mbps(const at_usb_rndis_t *u);

/* Abort a blocking bulk IN so TX can run. Safe from another thread. */
/* 블로킹 bulk IN을 중단해 TX가 진행되게 한다. 다른 스레드에서 호출해도 된다. */
int at_usb_abort_in(at_usb_rndis_t *u);

/* rc: 0=data, 1=timeout/abort, -1=error. buf is the completed IN buffer. */
/* rc: 0=데이터, 1=타임아웃/중단, -1=오류. buf 는 완료된 IN 버퍼. */
typedef void (*at_usb_in_cb)(void *user, int rc, size_t got, uint8_t *buf);
typedef void (*at_usb_out_cb)(void *user, int rc);

#define AT_USB_IN_DEPTH 2

/* Call from the USB worker thread. Attaches a CFRunLoop source for async I/O. */
/* USB 작업 스레드에서 호출한다. 비동기 I/O용 CFRunLoop 소스를 붙인다. */
int at_usb_async_attach(at_usb_rndis_t *u);
void at_usb_async_detach(at_usb_rndis_t *u);
int at_usb_async_submit_in(at_usb_rndis_t *u, uint8_t *buf, size_t cap,
                           at_usb_in_cb cb, void *user);
/* 0=queued, 1=OUT already in flight, -1=error. */
/* 0=큐에 넣음, 1=OUT이 이미 진행 중, -1=오류. */
int at_usb_async_submit_out(at_usb_rndis_t *u, uint8_t *buf, size_t len,
                            at_usb_out_cb cb, void *user);
int at_usb_async_out_busy(const at_usb_rndis_t *u);
int at_usb_async_run(at_usb_rndis_t *u, double seconds);
void at_usb_async_wake(at_usb_rndis_t *u);
int at_usb_abort_out(at_usb_rndis_t *u);

/* Clear a bulk pipe stall after a failed URB. is_in=1 for bulk IN. */
/* 실패한 URB 뒤의 벌크 파이프 스톨을 해제한다. is_in=1 이면 bulk IN. */
int at_usb_clear_stall(at_usb_rndis_t *u, int is_in);

/* 1 if IOKit reported the USB device gone (unplug). */
/* IOKit 가 USB 장치 제거를 알렸으면 1. */
int at_usb_is_gone(const at_usb_rndis_t *u);

#ifdef __cplusplus
}
#endif

#endif /* AT_USB_RNDIS_H */
