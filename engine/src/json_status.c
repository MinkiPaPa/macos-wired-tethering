/*
 * json_status.c
 * Line-delimited JSON over a UNIX socket (or stdout).
 * UNIX 소켓(또는 표준출력)으로 줄 단위 JSON을 보낸다.
 */
#include "json_status.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

/* Default to stdout so `list --json` and CLI logs actually appear. */
/* 기본값을 stdout으로 두어 list --json 과 CLI 출력이 실제로 보이게 한다. */
static int g_fd = STDOUT_FILENO;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void set_status_fd(int fd) {
    pthread_mutex_lock(&g_mu);
    g_fd = fd;
    pthread_mutex_unlock(&g_mu);
}

/* Write the whole buffer. Short writes would split a JSON line. */
/* 버퍼 전체를 쓴다. 짧은 write 는 JSON 줄을 자른다. */
static int write_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) return -1;
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

int at_status_open(const char *socket_path) {
    if (!socket_path || socket_path[0] == '\0') {
        set_status_fd(STDOUT_FILENO);
        return 0;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
    /* Admin password prompt can delay process start; wait up to two minutes. */
    /* 관리자 암호 입력으로 프로세스가 늦어질 수 있어 최대 2분 대기한다. */
    for (int i = 0; i < 150; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            int nosig = 1;
            /* Do not die on a closed GUI socket; write() returns EPIPE instead. */
            /* GUI 소켓이 닫혀도 죽지 않는다. write() 가 EPIPE 를 반환한다. */
            (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
            set_status_fd(fd);
            return 0;
        }
        usleep(100000);
    }
    close(fd);
    at_log(AT_LOG_ERROR, "Status socket failed (%s)", socket_path);
    return -1;
}

void at_status_close(void) {
    pthread_mutex_lock(&g_mu);
    if (g_fd >= 0 && g_fd != STDOUT_FILENO) close(g_fd);
    g_fd = -1;
    pthread_mutex_unlock(&g_mu);
}

int at_status_fd(void) {
    pthread_mutex_lock(&g_mu);
    int fd = g_fd;
    pthread_mutex_unlock(&g_mu);
    return fd;
}

int at_status_emit(const char *fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    if (n == 0 || line[n - 1] != '\n') {
        line[n++] = '\n';
        line[n] = '\0';
    }
    pthread_mutex_lock(&g_mu);
    int rc = 0;
    if (g_fd >= 0) {
        rc = write_all(g_fd, line, n);
        if (g_fd == STDOUT_FILENO) fflush(stdout);
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}

void at_status_log(at_log_level_t level, const char *message, void *user) {
    (void)user;
    const char *lv = "info";
    if (level == AT_LOG_DEBUG) lv = "debug";
    else if (level == AT_LOG_WARN) lv = "warn";
    else if (level == AT_LOG_ERROR) lv = "error";
    char esc[1024];
    at_json_escape(message, esc, sizeof(esc));
    (void)at_status_emit("{\"type\":\"log\",\"level\":\"%s\",\"message\":\"%s\"}", lv, esc);
    pthread_mutex_lock(&g_mu);
    int fd = g_fd;
    pthread_mutex_unlock(&g_mu);
    if (fd != STDERR_FILENO) fprintf(stderr, "[%s] %s\n", lv, message);
}

void at_status_devices(const at_usb_device_info_t *devs, int count) {
    at_status_emit("{\"type\":\"devices_begin\",\"count\":%d}", count);
    for (int i = 0; i < count; i++) {
        char name[256], vendor[256], serial[128];
        at_json_escape(devs[i].name, name, sizeof(name));
        at_json_escape(devs[i].vendor, vendor, sizeof(vendor));
        at_json_escape(devs[i].serial, serial, sizeof(serial));
        const char *kind = "unknown";
        if (devs[i].kind == AT_KIND_RNDIS) kind = "rndis";
        else if (devs[i].kind == AT_KIND_ANDROID) kind = "android";
        at_status_emit(
            "{\"type\":\"device\",\"vid\":%u,\"pid\":%u,\"location\":%u,"
            "\"name\":\"%s\",\"vendor\":\"%s\",\"serial\":\"%s\",\"kind\":\"%s\","
            "\"control\":%u,\"data\":%u}",
            devs[i].vid, devs[i].pid, devs[i].location_id,
            name, vendor, serial, kind,
            devs[i].control_ifc, devs[i].data_ifc);
    }
    at_status_emit("{\"type\":\"devices_end\"}");
}

int at_status_engine(const at_engine_t *e) {
    if (!e) return 0;
    at_stats_t st;
    at_net_info_t net;
    uint8_t mac[AT_MAC_LEN];
    at_engine_stats(e, &st);
    at_engine_net(e, &net);
    at_engine_mac(e, mac);
    char macs[24], ip[64], gw[64], dns[160], iface[32], err[512];
    at_mac_format(mac, macs, sizeof(macs));
    at_json_escape(net.ip, ip, sizeof(ip));
    at_json_escape(net.gateway, gw, sizeof(gw));
    at_json_escape(net.dns, dns, sizeof(dns));
    at_json_escape(at_engine_iface(e), iface, sizeof(iface));
    at_json_escape(at_engine_error(e), err, sizeof(err));
    const char *state = "idle";
    switch (at_engine_state(e)) {
        case AT_STATE_CONNECTING: state = "connecting"; break;
        case AT_STATE_CONNECTED: state = "connected"; break;
        case AT_STATE_STOPPING: state = "stopping"; break;
        case AT_STATE_ERROR: state = "error"; break;
        default: break;
    }
    return at_status_emit(
        "{\"type\":\"status\",\"state\":\"%s\",\"iface\":\"%s\",\"ip\":\"%s\","
        "\"gateway\":\"%s\",\"dns\":\"%s\",\"mac\":\"%s\",\"error\":\"%s\","
        "\"rx_bytes\":%llu,\"tx_bytes\":%llu,\"rx_frames\":%llu,\"tx_frames\":%llu,"
        "\"rx_errors\":%llu,\"tx_errors\":%llu,"
        "\"pid\":%d,\"link_mbps\":%u,\"mtu\":%d}",
        state, iface, ip, gw, dns, macs, err,
        (unsigned long long)st.rx_bytes, (unsigned long long)st.tx_bytes,
        (unsigned long long)st.rx_frames, (unsigned long long)st.tx_frames,
        (unsigned long long)st.rx_errors, (unsigned long long)st.tx_errors,
        (int)getpid(), at_engine_link_mbps(e), AT_UTUN_MTU);
}
