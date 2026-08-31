/*
 * net_parse.h
 * Pure parsers for scutil / networksetup / route text (no process spawn).
 * scutil·networksetup·route 출력의 순수 파서. 프로세스를 띄우지 않는다.
 */
#ifndef AT_NET_PARSE_H
#define AT_NET_PARSE_H

#include "common.h"
#include "net_config.h"

#ifdef __cplusplus
extern "C" {
#endif

int at_str_contains_ci(const char *s, const char *needle);
int at_str_line_has_prefix(const char *line, const char *prefix);
int at_parse_wifi_truthy(const char *s);

/* Copy a scutil / route-get field. Distinguishes Kind vs Kind6. */
/* scutil·route-get 필드를 복사한다. Kind 와 Kind6 를 구분한다. */
void at_parse_scutil_field(const char *text, const char *label, char *out, size_t n);

/* 1=on, 0=off, -1=unknown. English and Korean networksetup wording. */
/* 1=켜짐, 0=꺼짐, -1=모름. 영문·한글 networksetup 문구. */
int at_parse_wifi_power_text(const char *out);

/* BSD name from `networksetup -listallhardwareports` (Wi-Fi / 와이파이). */
/* listallhardwareports 출력에서 Wi-Fi BSD 이름을 찾는다. */
int at_parse_wifi_device_text(const char *text, char *dev, size_t n);

/* 1 if s is a dotted IPv4 with no extra characters (safe for scutil/ifconfig). */
/* 점분 IPv4 이고 다른 문자가 없으면 1. scutil·ifconfig 에 넣어도 안전하다. */
int at_ipv4_dotted_ok(const char *s);
/* 1 if name is a short BSD ifname: letters then alnum, no slash or space. */
/* 짧은 BSD 인터페이스 이름이면 1. 알파벳으로 시작하고 슬래시·공백이 없다. */
int at_ifname_ok(const char *name);
/* 1 if lease IP/mask/gw/DNS are empty or dotted IPv4 only. */
/* 임대의 IP·마스크·게이트웨이·DNS 가 비었거나 점분 IPv4 뿐이면 1. */
int at_lease_addrs_ok(const at_net_info_t *lease);

#ifdef __cplusplus
}
#endif

#endif /* AT_NET_PARSE_H */
