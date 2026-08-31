/*
 * main.c
 * CLI entry: list devices or run the tethering engine.
 * CLI 진입점: 장치 나열 또는 테더링 엔진 실행.
 */
#include "common.h"
#include "usb_rndis.h"
#include "tether_engine.h"
#include "json_status.h"

#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

static volatile sig_atomic_t g_stop = 0;
static at_engine_t *g_engine = NULL;
static char g_pidfile[512];

static void unlink_pidfile(void) {
    if (g_pidfile[0]) {
        unlink(g_pidfile);
        g_pidfile[0] = '\0';
    }
}

static void write_pidfile(const char *socket_path) {
    if (!socket_path || socket_path[0] == '\0') return;
    snprintf(g_pidfile, sizeof(g_pidfile), "%s.pid", socket_path);
    FILE *f = fopen(g_pidfile, "w");
    if (!f) {
        at_log(AT_LOG_WARN, "Failed to create PID file (%s)", g_pidfile);
        g_pidfile[0] = '\0';
        return;
    }
    fprintf(f, "%d\n", (int)getpid());
    fflush(f);
    (void)fchmod(fileno(f), 0600);
    fclose(f);
    (void)chmod(g_pidfile, 0600);
    at_log(AT_LOG_INFO, "Engine PID %d (%s)", (int)getpid(), g_pidfile);
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "macOS wired tethering engine %s\n"
        "macOS 사용자 공간 Android USB 테더링 (RNDIS)\n"
        "\n"
        "사용법:\n"
        "  %s list [--json]\n"
        "  %s connect [--location ID] [--vid HEX] [--pid HEX] [--serial S]\n"
        "             [--socket PATH] [--default-route]\n"
        "  %s restore\n"
        "\n"
        "옵션:\n"
        "  --json            장치 목록을 JSON Lines로 출력\n"
        "  --location ID     USB locationID 로 장치를 지정\n"
        "  --vid / --pid     USB 벤더/제품 ID (16진수)\n"
        "  --serial S        USB 시리얼로 장치를 지정\n"
        "  --socket PATH     GUI 상태 채널 (UNIX 소켓)\n"
        "  --default-route   이 연결을 기본 경로로 승격\n"
        "  --version         버전 출력\n",
        AT_VERSION, argv0, argv0, argv0);
}

static int cmd_list(int json_mode) {
    at_usb_device_info_t devs[AT_MAX_DEVICES];
    int n = 0;
    if (at_usb_list_devices(devs, AT_MAX_DEVICES, &n) != 0) {
        at_log(AT_LOG_ERROR, "Failed to list USB devices");
        return 1;
    }
    if (json_mode) {
        at_status_devices(devs, n);
        return 0;
    }
    if (n == 0) {
        printf("감지된 안드로이드/RNDIS 장치가 없습니다.\n");
        printf("USB 케이블을 연결하고, 휴대폰에서 USB 테더링을 켜세요.\n");
        return 0;
    }
    printf("%-8s %-9s %-10s %-10s %s\n", "VID:PID", "Location", "Kind", "Serial", "Name");
    for (int i = 0; i < n; i++) {
        const char *kind = "android";
        if (devs[i].kind == AT_KIND_RNDIS) kind = "rndis";
        printf("%04x:%04x %08x   %-10s %-10s %s\n",
               devs[i].vid, devs[i].pid, devs[i].location_id, kind,
               devs[i].serial[0] ? devs[i].serial : "-",
               devs[i].name);
    }
    return 0;
}

static int pick_device(at_usb_device_info_t *want, int have_filter) {
    at_usb_device_info_t devs[AT_MAX_DEVICES];
    int n = 0;
    if (at_usb_list_devices(devs, AT_MAX_DEVICES, &n) != 0) return -1;
    at_usb_device_info_t *chosen = NULL;
    for (int i = 0; i < n; i++) {
        if (have_filter) {
            int ok = 1;
            if (want->location_id && devs[i].location_id != want->location_id) ok = 0;
            if (want->vid && devs[i].vid != want->vid) ok = 0;
            if (want->pid && devs[i].pid != want->pid) ok = 0;
            if (want->serial[0] && strcmp(want->serial, devs[i].serial) != 0) ok = 0;
            if (!ok) continue;
        }
        if (devs[i].kind == AT_KIND_RNDIS) {
            chosen = &devs[i];
            break;
        }
        if (!chosen) chosen = &devs[i];
    }
    if (!chosen) return -1;
    *want = *chosen;
    return 0;
}

static int cmd_connect(at_usb_device_info_t *want, int have_filter,
                       int default_route, const char *socket_path) {
    if (at_status_open(socket_path) != 0) {
        at_log(AT_LOG_ERROR, "Could not open the status channel");
        return 1;
    }
    write_pidfile(socket_path);
    at_set_logger(at_status_log, NULL);

    if (pick_device(want, have_filter) != 0) {
        at_log(AT_LOG_ERROR, "No RNDIS device to connect. Turn on USB tethering.");
        at_status_emit("{\"type\":\"status\",\"state\":\"error\",\"error\":\"no rndis device\"}");
        at_status_close();
        unlink_pidfile();
        return 1;
    }
    if (want->kind != AT_KIND_RNDIS) {
        at_log(AT_LOG_ERROR, "The device is not in RNDIS mode. Enable USB tethering on the phone.");
        at_status_emit("{\"type\":\"status\",\"state\":\"error\",\"error\":\"tethering not enabled\"}");
        at_status_close();
        unlink_pidfile();
        return 1;
    }

    at_engine_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.device = *want;
    opts.prefer_default_route = default_route;

    at_log(AT_LOG_INFO, "Connecting to %s (%04x:%04x)",
           want->name, want->vid, want->pid);
    g_engine = at_engine_start(&opts);
    if (!g_engine) {
        char esc[512];
        at_json_escape(at_engine_last_error(), esc, sizeof(esc));
        at_status_emit("{\"type\":\"status\",\"state\":\"error\",\"error\":\"%s\"}", esc);
        at_status_close();
        unlink_pidfile();
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* osascript/admin shell exit delivers SIGHUP; default is to die without restore. */
    /* osascript/관리자 셸이 끝나면 SIGHUP 이 온다. 기본 동작은 복구 없이 종료다. */
    signal(SIGHUP, on_signal);
    /* Closed GUI socket must not SIGPIPE-kill us before at_engine_stop. */
    /* GUI 소켓이 닫혀도 SIGPIPE 로 죽지 않고 at_engine_stop 을 타게 한다. */
    signal(SIGPIPE, SIG_IGN);

    while (!g_stop) {
        if (at_status_engine(g_engine) != 0) {
            at_log(AT_LOG_INFO, "GUI disconnected; stopping the engine");
            break;
        }
        if (at_engine_state(g_engine) == AT_STATE_ERROR) {
            g_stop = 1;
            break;
        }
        int sfd = at_status_fd();
        fd_set rfds;
        FD_ZERO(&rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int maxfd = 0;
        if (sfd >= 0 && sfd != STDOUT_FILENO) {
            FD_SET(sfd, &rfds);
            maxfd = sfd;
        }
        int sr = select(maxfd + 1, (maxfd > 0) ? &rfds : NULL, NULL, NULL, &tv);
        if (sr > 0 && sfd >= 0 && FD_ISSET(sfd, &rfds)) {
            char tmp[64];
            ssize_t n = recv(sfd, tmp, sizeof(tmp), 0);
            if (n == 0) {
                at_log(AT_LOG_INFO, "GUI disconnected; stopping the engine");
                break;
            }
            if (n > 0 && strncmp(tmp, "stop", 4) == 0) break;
        }
    }

    int failed = (at_engine_state(g_engine) == AT_STATE_ERROR);
    char err[256];
    strlcpy(err, at_engine_error(g_engine), sizeof(err));
    at_log(AT_LOG_INFO, "Stopping tether");
    at_engine_stop(g_engine);
    g_engine = NULL;
    /* Keep the error on the status channel so the GUI does not flash idle. */
    /* 상태 채널에 오류를 남겨 GUI 가 idle 로 깜빡이지 않게 한다. */
    if (failed) {
        char esc[512];
        at_json_escape(err, esc, sizeof(esc));
        at_status_emit("{\"type\":\"status\",\"state\":\"error\",\"error\":\"%s\"}", esc);
    } else {
        at_status_emit("{\"type\":\"status\",\"state\":\"idle\"}");
    }
    at_status_close();
    unlink_pidfile();
    return failed ? 1 : 0;
}

/* Restore scutil backup + Wi-Fi after a killed engine skipped at_engine_stop. */
/* 강제 종료로 at_engine_stop 을 건너뛴 뒤 scutil 백업과 Wi-Fi를 복구한다. */
static int cmd_restore(void) {
    at_log(AT_LOG_INFO, "Restoring leftover tether session");
    (void)at_net_end_session();
    (void)at_net_restore_wifi();
    return 0;
}

static int take_arg(int argc, char **argv, int *i, char *out, size_t out_len) {
    if (*i + 1 >= argc) return -1;
    (*i)++;
    strlcpy(out, argv[*i], out_len);
    return 0;
}

int main(int argc, char **argv) {
    int json_mode = 0, default_route = 0, have_filter = 0;
    char socket_path[512] = {0};
    at_usb_device_info_t want;
    memset(&want, 0, sizeof(want));
    const char *cmd = NULL;

    /* macOS getopt stops at the first non-option, so parse argv by hand. */
    /* macOS getopt는 첫 비옵션에서 멈추므로 argv를 직접 파싱한다. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0 || strcmp(a, "-j") == 0) {
            json_mode = 1;
        } else if (strcmp(a, "--location") == 0 || strcmp(a, "-L") == 0) {
            char tmp[32] = {0};
            if (take_arg(argc, argv, &i, tmp, sizeof(tmp)) != 0) { usage(argv[0]); return 2; }
            want.location_id = (uint32_t)strtoul(tmp, NULL, 0);
            have_filter = 1;
        } else if (strcmp(a, "--vid") == 0 || strcmp(a, "-v") == 0) {
            char tmp[16] = {0};
            if (take_arg(argc, argv, &i, tmp, sizeof(tmp)) != 0) { usage(argv[0]); return 2; }
            want.vid = (uint16_t)strtoul(tmp, NULL, 16);
            have_filter = 1;
        } else if (strcmp(a, "--pid") == 0 || strcmp(a, "-p") == 0) {
            char tmp[16] = {0};
            if (take_arg(argc, argv, &i, tmp, sizeof(tmp)) != 0) { usage(argv[0]); return 2; }
            want.pid = (uint16_t)strtoul(tmp, NULL, 16);
            have_filter = 1;
        } else if (strcmp(a, "--serial") == 0 || strcmp(a, "-s") == 0) {
            if (take_arg(argc, argv, &i, want.serial, sizeof(want.serial)) != 0) { usage(argv[0]); return 2; }
            have_filter = 1;
        } else if (strcmp(a, "--socket") == 0 || strcmp(a, "-S") == 0) {
            if (take_arg(argc, argv, &i, socket_path, sizeof(socket_path)) != 0) { usage(argv[0]); return 2; }
        } else if (strcmp(a, "--default-route") == 0 || strcmp(a, "-R") == 0) {
            default_route = 1;
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("macOS wired tethering %s\n", AT_VERSION);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (a[0] != '-' && !cmd) {
            cmd = a;
        } else {
            fprintf(stderr, "알 수 없는 옵션: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }
    if (!cmd) cmd = "list";
    if (strcmp(cmd, "list") == 0) {
        if (socket_path[0]) at_status_open(socket_path);
        else if (json_mode) at_status_open(NULL);
        return cmd_list(json_mode || socket_path[0]);
    }
    if (strcmp(cmd, "connect") == 0)
        return cmd_connect(&want, have_filter, default_route, socket_path[0] ? socket_path : NULL);
    if (strcmp(cmd, "restore") == 0)
        return cmd_restore();
    if (strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    fprintf(stderr, "알 수 없는 명령: %s\n", cmd);
    usage(argv[0]);
    return 2;
}
