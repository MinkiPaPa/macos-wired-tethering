/*
 * json_status.h
 * Line-delimited JSON status channel for the GUI.
 * GUI용 줄 단위 JSON 상태 채널.
 */
#ifndef AT_JSON_STATUS_H
#define AT_JSON_STATUS_H

#include "common.h"
#include "usb_rndis.h"
#include "tether_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

int  at_status_open(const char *socket_path);
void at_status_close(void);
int  at_status_fd(void);
int  at_status_emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void at_status_log(at_log_level_t level, const char *message, void *user);
void at_status_devices(const at_usb_device_info_t *devs, int count);
int  at_status_engine(const at_engine_t *e);

#ifdef __cplusplus
}
#endif

#endif /* AT_JSON_STATUS_H */
