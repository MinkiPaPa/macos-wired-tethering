/*
 * net_parse.c
 * String parsers used by net_config. Kept free of posix_spawn so tests can run.
 * net_config 가 쓰는 문자열 파서. posix_spawn 없이 테스트할 수 있게 둔다.
 */
#include "net_parse.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <strings.h>

int at_str_contains_ci(const char *s, const char *needle) {
    if (!s || !needle || !needle[0]) return 0;
    size_t n = strlen(needle);
    for (; *s; s++) {
        if (strncasecmp(s, needle, n) == 0) return 1;
    }
    return 0;
}

int at_str_line_has_prefix(const char *line, const char *prefix) {
    return line && prefix && strncmp(line, prefix, strlen(prefix)) == 0;
}

int at_parse_wifi_truthy(const char *s) {
    if (!s || !s[0]) return 0;
    return strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
           strcmp(s, "1") == 0 || strcasecmp(s, "on") == 0;
}

static int scutil_key_at(const char *p, const char *label) {
    size_t n = strlen(label);
    if (strncasecmp(p, label, n) != 0) return 0;
    unsigned char c = (unsigned char)p[n];
    return c == '\0' || c == ' ' || c == '\t' || c == ':' || c == '\n' || c == '\r';
}

void at_parse_scutil_field(const char *text, const char *label, char *out, size_t n) {
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!text || !label) return;
    for (const char *p = text; *p; p++) {
        int at_start = (p == text || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t' ||
                        p[-1] == '{' || p[-1] == '\r');
        if (!at_start || !scutil_key_at(p, label)) continue;
        p += strlen(label);
        while (*p == ' ' || *p == '\t' || *p == ':') p++;
        if (*p == '"') p++;
        size_t i = 0;
        while (p[i] && p[i] != '\n' && p[i] != '\r' && p[i] != '"' && i + 1 < n) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
        while (i > 0 && (out[i - 1] == ' ' || out[i - 1] == '\t' || out[i - 1] == '}'))
            out[--i] = '\0';
        return;
    }
}

int at_parse_wifi_power_text(const char *out) {
    if (!out || !out[0]) return -1;
    if (at_str_contains_ci(out, "Off") || strstr(out, "꺼짐") || strstr(out, "끄기") ||
        strstr(out, "꺼져"))
        return 0;
    if (strstr(out, ": On") || strstr(out, ": on") || strstr(out, ":On") ||
        strstr(out, "켜짐") || strstr(out, "켜기") || at_str_contains_ci(out, "enabled"))
        return 1;
    if (at_str_contains_ci(out, "On") || strstr(out, "켜")) return 1;
    return -1;
}

int at_parse_wifi_device_text(const char *text, char *dev, size_t n) {
    if (!dev || n == 0) return -1;
    dev[0] = '\0';
    if (!text) return -1;
    char copy[4096];
    strlcpy(copy, text, sizeof(copy));
    int want = 0;
    char *sp = NULL;
    for (char *line = strtok_r(copy, "\n", &sp); line; line = strtok_r(NULL, "\n", &sp)) {
        if (at_str_line_has_prefix(line, "Hardware Port:") ||
            at_str_line_has_prefix(line, "하드웨어 포트:")) {
            want = (at_str_contains_ci(line, "Wi-Fi") || at_str_contains_ci(line, "WiFi") ||
                    at_str_contains_ci(line, "AirPort") ||
                    at_str_contains_ci(line, "와이파이")) ? 1 : 0;
            continue;
        }
        int is_dev = at_str_line_has_prefix(line, "Device:") ||
                     at_str_line_has_prefix(line, "장치:");
        if (!want || !is_dev) continue;
        const char *p = strchr(line, ':');
        if (!p) continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        strlcpy(dev, p, n);
        size_t i = strlen(dev);
        while (i > 0 && (dev[i - 1] == ' ' || dev[i - 1] == '\t' || dev[i - 1] == '\r'))
            dev[--i] = '\0';
        return dev[0] ? 0 : -1;
    }
    return -1;
}

int at_ipv4_dotted_ok(const char *s) {
    if (!s || !s[0]) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p != '.' && !isdigit(*p)) return 0;
    }
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

int at_ifname_ok(const char *name) {
    if (!name || !name[0] || !isalpha((unsigned char)name[0])) return 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p, ++n) {
        if (n >= AT_IFNAMSIZ - 1) return 0;
        if (!isalnum(*p)) return 0;
    }
    return n > 0;
}

int at_lease_addrs_ok(const at_net_info_t *lease) {
    if (!lease || !at_ipv4_dotted_ok(lease->ip)) return 0;
    if (lease->netmask[0] && !at_ipv4_dotted_ok(lease->netmask)) return 0;
    if (lease->gateway[0] && !at_ipv4_dotted_ok(lease->gateway)) return 0;
    if (!lease->dns[0]) return 1;
    char dns1[64] = {0}, dns2[64] = {0};
    if (sscanf(lease->dns, "%63s %63s", dns1, dns2) < 1) return 0;
    if (!at_ipv4_dotted_ok(dns1)) return 0;
    if (dns2[0] && !at_ipv4_dotted_ok(dns2)) return 0;
    return 1;
}
