/*
 * net_config.h
 * Addressing, scutil path publish, and default-route promotion with restore.
 * 주소 설정, scutil 경로 게시, 기본 경로 승격과 해제 시 복구.
 */
#ifndef AT_NET_CONFIG_H
#define AT_NET_CONFIG_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ip[48];
    char netmask[48];
    char gateway[48];
    char dns[128];
    int  ready;
} at_net_info_t;

int at_net_register_service(const char *ifname);
int at_net_remove_service(void);
int at_net_prefer_default_route(const char *ifname, const char *gateway);
/* Drop leftover IPv6 defaults so browsers do not Happy-Eyeballs via Wi-Fi. */
/* 남은 IPv6 기본 경로를 지워 브라우저가 Wi-Fi로 Happy Eyeballs 하지 않게 한다. */
int at_net_drop_ipv6_default(void);
int at_net_disable_wifi_during_tether(void);
/* Turn Wi-Fi back on if this session (or a leftover backup) powered it off. */
/* 이 세션(또는 남은 백업)이 Wi-Fi를 껐으면 다시 켠다. */
int at_net_restore_wifi(void);
int at_net_clear_dhcp(const char *ifname);
int at_net_apply_lease(const char *ifname, const at_net_info_t *lease, int as_primary);
int at_net_publish_path(const char *ifname, const at_net_info_t *lease, int as_primary);
int at_net_unpublish_path(void);

/* Snapshot Global IPv4/DNS + default route; restore leftover from a crash first. */
/* Global IPv4/DNS와 기본 경로를 스냅샷한다. 이전 크래시 잔여 상태는 먼저 복구한다. */
int at_net_begin_session(void);
/* Restore snapshot, drop backup keys, and remove the published tether service. */
/* 스냅샷을 복구하고 백업 키와 게시한 테더 서비스를 지운다. */
int at_net_end_session(void);

#ifdef __cplusplus
}
#endif

#endif /* AT_NET_CONFIG_H */
