/*
 * utun_if.c
 * Open a utun control socket and exchange IP packets with the macOS stack.
 * utun 컨트롤 소켓을 열고 macOS 스택과 IP 패킷을 주고받는다.
 */
#include "utun_if.h"

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

struct at_utun {
    int fd;
    char name[IFNAMSIZ];
};

at_utun_t *at_utun_open(void) {
    at_utun_t *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->fd = -1;
    u->fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (u->fd < 0) {
        at_log(AT_LOG_ERROR, "utun socket failed: %s", strerror(errno));
        at_utun_close(u);
        return NULL;
    }

    struct ctl_info info;
    memset(&info, 0, sizeof(info));
    strlcpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name));
    if (ioctl(u->fd, CTLIOCGINFO, &info) != 0) {
        at_log(AT_LOG_ERROR, "CTLIOCGINFO failed: %s", strerror(errno));
        at_utun_close(u);
        return NULL;
    }

    struct sockaddr_ctl addr;
    memset(&addr, 0, sizeof(addr));
    addr.sc_len = sizeof(addr);
    addr.sc_family = AF_SYSTEM;
    addr.ss_sysaddr = AF_SYS_CONTROL;
    addr.sc_id = info.ctl_id;
    addr.sc_unit = 0; /* kernel assigns / 커널이 번호를 고른다 */
    if (connect(u->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        at_log(AT_LOG_ERROR, "utun connect failed: %s", strerror(errno));
        at_utun_close(u);
        return NULL;
    }

    socklen_t nlen = sizeof(u->name);
    if (getsockopt(u->fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, u->name, &nlen) != 0) {
        at_log(AT_LOG_ERROR, "UTUN_OPT_IFNAME failed: %s", strerror(errno));
        at_utun_close(u);
        return NULL;
    }

    int flags = fcntl(u->fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(u->fd, F_SETFL, flags | O_NONBLOCK);

    /* Larger socket buffers so a TCP burst does not stall on utun. */
    /* TCP 버스트가 utun에서 멈추지 않도록 소켓 버퍼를 키운다. */
    int bufsz = 2 * 1024 * 1024;
    (void)setsockopt(u->fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    (void)setsockopt(u->fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));

    at_log(AT_LOG_INFO, "utun ready: %s", u->name);
    return u;
}

void at_utun_close(at_utun_t *u) {
    if (!u) return;
    if (u->fd >= 0) {
        close(u->fd);
        u->fd = -1;
    }
    free(u);
}

const char *at_utun_name(const at_utun_t *u) { return u ? u->name : ""; }

int at_utun_read_is_drop(ssize_t n, size_t bufsz, size_t cap) {
    if (n < 5) return 0;
    if (bufsz == 0 || n == (ssize_t)bufsz) return 1;
    size_t payload = (size_t)n - 4;
    return payload > cap;
}

int at_utun_read(at_utun_t *u, uint8_t *ip, size_t cap, size_t *len, int *family,
                 int timeout_ms) {
    if (!u || !ip || !len) return -1;
    struct pollfd pfd = { .fd = u->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
    if (pr <= 0) return 1;

    /* Larger than MTU so a full buffer means the kernel datagram was truncated. */
    /* MTU 보다 크게 읽어, 버퍼가 가득 차면 커널 데이터그램이 잘린 것으로 본다. */
    uint8_t buf[4096];
    ssize_t n = read(u->fd, buf, sizeof(buf));
    if (n < 5) return n < 0 ? (errno == EAGAIN ? 1 : -1) : 1;
    if (at_utun_read_is_drop(n, sizeof(buf), cap)) {
        *len = 0;
        return AT_UTUN_READ_DROP;
    }

    uint32_t af;
    memcpy(&af, buf, 4);
    af = ntohl(af);
    if (family) *family = (int)af;
    size_t payload = (size_t)n - 4;
    memcpy(ip, buf + 4, payload);
    *len = payload;
    return 0;
}

int at_utun_write(at_utun_t *u, const uint8_t *ip, size_t len, int family) {
    if (!u || !ip || len == 0) return -1;
    uint8_t buf[2048];
    if (len + 4 > sizeof(buf)) return -1;
    uint32_t af = htonl((uint32_t)family);
    memcpy(buf, &af, 4);
    memcpy(buf + 4, ip, len);
    ssize_t n = write(u->fd, buf, len + 4);
    return (n == (ssize_t)(len + 4)) ? 0 : -1;
}
