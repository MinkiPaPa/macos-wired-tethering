/*
 * test_net_parse.c
 * scutil / networksetup / route text parsers (no spawn).
 * scutil·networksetup·route 텍스트 파서. 프로세스를 띄우지 않는다.
 */
#include "net_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_str(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) == 0) return;
    fprintf(stderr, "FAIL %s: got \"%s\" want \"%s\"\n", name, got, want);
    exit(1);
}

int main(void) {
    assert(at_parse_wifi_truthy("true"));
    assert(at_parse_wifi_truthy("YES"));
    assert(at_parse_wifi_truthy("1"));
    assert(at_parse_wifi_truthy("on"));
    assert(!at_parse_wifi_truthy("false"));
    assert(!at_parse_wifi_truthy(""));
    assert(!at_parse_wifi_truthy(NULL));

    assert(at_parse_wifi_power_text("Wi-Fi Power (en0): Off") == 0);
    assert(at_parse_wifi_power_text("Wi-Fi Power (en0): On") == 1);
    assert(at_parse_wifi_power_text("en0의 전원: 꺼짐") == 0);
    assert(at_parse_wifi_power_text("en0의 전원: 켜짐") == 1);
    assert(at_parse_wifi_power_text("") == -1);

    const char *ports =
        "Hardware Port: Ethernet\nDevice: en1\nEthernet Address: aa:bb\n\n"
        "Hardware Port: Wi-Fi\nDevice: en0\nEthernet Address: cc:dd\n";
    char dev[32];
    assert(at_parse_wifi_device_text(ports, dev, sizeof(dev)) == 0);
    expect_str("wifi en", dev, "en0");

    const char *ports_ko =
        "하드웨어 포트: 이더넷\n장치: en2\n\n"
        "하드웨어 포트: 와이파이\n장치: en0\n";
    assert(at_parse_wifi_device_text(ports_ko, dev, sizeof(dev)) == 0);
    expect_str("wifi ko", dev, "en0");

    const char *scutil =
        "  Kind : ipv4\n"
        "  Kind6 : ipv6\n"
        "  Gateway : 192.168.0.1\n"
        "  Interface : en0\n";
    char kind[32], kind6[32], gw[64], iface[32];
    at_parse_scutil_field(scutil, "Kind", kind, sizeof(kind));
    at_parse_scutil_field(scutil, "Kind6", kind6, sizeof(kind6));
    at_parse_scutil_field(scutil, "Gateway", gw, sizeof(gw));
    at_parse_scutil_field(scutil, "Interface", iface, sizeof(iface));
    expect_str("kind", kind, "ipv4");
    expect_str("kind6", kind6, "ipv6");
    expect_str("gw", gw, "192.168.0.1");
    expect_str("if", iface, "en0");

    const char *route =
        "   route to: default\n"
        "destination: default\n"
        "       gateway: 10.0.0.1\n"
        "  interface: en0\n";
    at_parse_scutil_field(route, "gateway:", gw, sizeof(gw));
    at_parse_scutil_field(route, "interface:", iface, sizeof(iface));
    expect_str("route gw", gw, "10.0.0.1");
    expect_str("route if", iface, "en0");

    assert(at_ipv4_dotted_ok("10.11.226.2"));
    assert(at_ipv4_dotted_ok("192.168.0.1"));
    assert(!at_ipv4_dotted_ok(""));
    assert(!at_ipv4_dotted_ok(NULL));
    assert(!at_ipv4_dotted_ok("10.0.0.1\n"));
    assert(!at_ipv4_dotted_ok("10.0.0.1;id"));
    assert(!at_ipv4_dotted_ok("10.0.0"));
    assert(!at_ipv4_dotted_ok("999.1.1.1"));
    assert(at_ifname_ok("utun7"));
    assert(at_ifname_ok("en0"));
    assert(!at_ifname_ok(""));
    assert(!at_ifname_ok("utun/7"));
    assert(!at_ifname_ok("utun 7"));
    assert(!at_ifname_ok("../utun0"));

    at_net_info_t lease;
    memset(&lease, 0, sizeof(lease));
    strlcpy(lease.ip, "10.1.2.3", sizeof(lease.ip));
    strlcpy(lease.gateway, "10.1.2.1", sizeof(lease.gateway));
    strlcpy(lease.dns, "10.1.2.1 8.8.8.8", sizeof(lease.dns));
    assert(at_lease_addrs_ok(&lease));
    strlcpy(lease.ip, "10.1.2.3\nset foo", sizeof(lease.ip));
    assert(!at_lease_addrs_ok(&lease));
    strlcpy(lease.ip, "10.1.2.3", sizeof(lease.ip));
    strlcpy(lease.dns, "10.1.2.1; rm -rf /", sizeof(lease.dns));
    assert(!at_lease_addrs_ok(&lease));

    printf("test_net_parse: all checks passed\n");
    return 0;
}
