/*
 * net_config.c
 * Apply DHCP lease to utun, publish a scutil service, optionally steal the
 * system primary path, and restore the previous route/DNS on disconnect.
 * DHCP 임대를 utun에 적용하고 scutil 서비스를 게시한다.
 * 선택적으로 시스템 기본 경로를 가져오며, 해제 시 이전 경로/DNS를 복구한다.
 */
#include "net_config.h"
#include "net_parse.h"

#include <sys/types.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdio.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <pwd.h>
#include <SystemConfiguration/SystemConfiguration.h>

extern char **environ;

static int run_cmd(char *const argv[]) {
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return -1;
}

static int capture_cmd(char *const argv[], char *out, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return -1;
    }
    size_t n = 0;
    while (n + 1 < out_len) {
        ssize_t r = read(pipefd[0], out + n, out_len - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    /* Trim trailing whitespace / 끝 공백 제거 */
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int capture_cmd_merged(char *const argv[], char *out, size_t out_len) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return -1;
    }
    size_t n = 0;
    while (n + 1 < out_len) {
        ssize_t r = read(pipefd[0], out + n, out_len - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int run_cmd_stdin(char *const argv[], const char *input) {
    int pipefd[2];
    if (!input || pipe(pipefd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    if (pipefd[0] != STDIN_FILENO)
        posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[0]);
    if (rc != 0) {
        close(pipefd[1]);
        return -1;
    }
    size_t total = strlen(input);
    size_t off = 0;
    while (off < total) {
        ssize_t w = write(pipefd[1], input + off, total - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(pipefd[1]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int capture_cmd_stdin(char *const argv[], const char *input, char *out, size_t out_len) {
    int inpipe[2], outpipe[2];
    if (!input || !out || out_len == 0) return -1;
    if (pipe(inpipe) != 0) return -1;
    if (pipe(outpipe) != 0) {
        close(inpipe[0]);
        close(inpipe[1]);
        return -1;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inpipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outpipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, outpipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, inpipe[1]);
    posix_spawn_file_actions_addclose(&fa, outpipe[0]);
    if (inpipe[0] != STDIN_FILENO)
        posix_spawn_file_actions_addclose(&fa, inpipe[0]);
    if (outpipe[1] != STDOUT_FILENO && outpipe[1] != STDERR_FILENO)
        posix_spawn_file_actions_addclose(&fa, outpipe[1]);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(inpipe[0]);
    close(outpipe[1]);
    if (rc != 0) {
        close(inpipe[1]);
        close(outpipe[0]);
        return -1;
    }
    size_t total = strlen(input);
    size_t off = 0;
    while (off < total) {
        ssize_t w = write(inpipe[1], input + off, total - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(inpipe[1]);
    size_t n = 0;
    while (n + 1 < out_len) {
        ssize_t r = read(outpipe[0], out + n, out_len - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(outpipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static const char *kServiceName = "macOS wired tethering";
static const char *kServiceId = "MacOSWiredTetheringUSB";
static const char *kBackupValid = "State:/Network/MacOSWiredTethering/Backup/Valid";
static const char *kBackupIPv4  = "State:/Network/MacOSWiredTethering/Backup/GlobalIPv4";
static const char *kBackupDNS   = "State:/Network/MacOSWiredTethering/Backup/GlobalDNS";
static const char *kBackupRoute = "State:/Network/MacOSWiredTethering/Backup/DefaultRoute";
static const char *kBackupWifi  = "State:/Network/MacOSWiredTethering/Backup/Wifi";
static const char *kGlobalIPv4  = "State:/Network/Global/IPv4";
static const char *kGlobalDNS   = "State:/Network/Global/DNS";

static int g_session_open = 0;
/* Remember that this process turned Wi-Fi off, so restore does not depend on scutil parse. */
/* 이 프로세스가 Wi-Fi를 껐다는 사실을 기억한다. 복구가 scutil 파싱에만 의존하지 않게 한다. */
static int g_wifi_we_turned_off = 0;
static char g_wifi_dev[32];

#define AT_WIFI_MARKER_REL "Library/Application Support/macos-wired-tethering/wifi-restore"
#define AT_WIFI_MARKER_TMP "/tmp/macos-wired-tethering-wifi-restore"

/* Resolve the console user's home even when this process is root. */
/* 이 프로세스가 root 여도 콘솔 사용자 홈을 찾는다. */
static int console_home(char *out, size_t n, uid_t *uid_out) {
    if (!out || n == 0) return -1;
    out[0] = '\0';
    uid_t uid = (uid_t)-1;
    CFStringRef name = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
    if (name) CFRelease(name);
    struct passwd *pw = NULL;
    if (uid != (uid_t)-1 && uid != 0)
        pw = getpwuid(uid);
    if (!pw) {
        const char *login = getlogin();
        if (login && login[0]) pw = getpwnam(login);
    }
    if (!pw || !pw->pw_dir || !pw->pw_dir[0]) return -1;
    strlcpy(out, pw->pw_dir, n);
    if (uid_out) *uid_out = pw->pw_uid;
    return 0;
}

static int wifi_marker_user_path(char *out, size_t n) {
    char home[256];
    if (console_home(home, sizeof(home), NULL) != 0) return -1;
    snprintf(out, n, "%s/%s", home, AT_WIFI_MARKER_REL);
    return 0;
}

static void write_wifi_marker(const char *dev) {
    char home[256], dir[512], path[640];
    uid_t uid = 0;
    if (console_home(home, sizeof(home), &uid) != 0) return;
    snprintf(dir, sizeof(dir), "%s/Library/Application Support", home);
    (void)mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/Library/Application Support/macos-wired-tethering", home);
    snprintf(path, sizeof(path), "%s/wifi-restore", dir);
    (void)mkdir(dir, 0755);
    if (uid > 0) (void)chown(dir, uid, (gid_t)-1);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", (dev && dev[0]) ? dev : "en0");
    fclose(f);
    (void)chmod(path, 0644);
    if (uid > 0) (void)chown(path, uid, (gid_t)-1);
}

static void clear_wifi_marker(void) {
    char path[640];
    if (wifi_marker_user_path(path, sizeof(path)) == 0)
        unlink(path);
    unlink(AT_WIFI_MARKER_TMP);
}

static int wifi_marker_present(void) {
    char path[640];
    if (wifi_marker_user_path(path, sizeof(path)) == 0 && access(path, F_OK) == 0)
        return 1;
    return access(AT_WIFI_MARKER_TMP, F_OK) == 0;
}

static int scutil_run(const char *script) {
    char *argv[] = { "/usr/sbin/scutil", NULL };
    return run_cmd_stdin(argv, script);
}

static int scutil_show(const char *key, char *out, size_t out_len) {
    char script[512];
    snprintf(script, sizeof(script), "show %s\n", key);
    char *argv[] = { "/usr/sbin/scutil", NULL };
    int rc = capture_cmd_stdin(argv, script, out, out_len);
    if (rc != 0) return -1;
    if (strstr(out, "No such key") != NULL) return 1;
    return 0;
}

static int scutil_key_exists(const char *key) {
    char out[2048];
    return scutil_show(key, out, sizeof(out)) == 0;
}

static int scutil_copy_key(const char *from, const char *to) {
    char script[768];
    snprintf(script, sizeof(script),
             "open\n"
             "get %s\n"
             "set %s\n"
             "close\n",
             from, to);
    return scutil_run(script);
}

static void scutil_remove_key(const char *key) {
    char script[512];
    snprintf(script, sizeof(script), "open\nremove %s\nclose\n", key);
    (void)scutil_run(script);
}

/* Find the BSD name of the Wi-Fi hardware port (usually en0). */
/* Wi-Fi 하드웨어 포트의 BSD 이름(보통 en0)을 찾는다. */
static int wifi_find_device(char *dev, size_t n) {
    char out[4096];
    char *argv[] = { "/usr/sbin/networksetup", "-listallhardwareports", NULL };
    if (capture_cmd_merged(argv, out, sizeof(out)) != 0) return -1;
    return at_parse_wifi_device_text(out, dev, n);
}

static int wifi_power_on(const char *dev) {
    if (!dev || !dev[0] || strcmp(dev, "none") == 0) return -1;
    char out[256];
    char *argv[] = { "/usr/sbin/networksetup", "-getairportpower", (char *)dev, NULL };
    if (capture_cmd_merged(argv, out, sizeof(out)) != 0) return -1;
    return at_parse_wifi_power_text(out);
}

static int wifi_set_power(const char *dev, int on) {
    if (!dev || !dev[0] || strcmp(dev, "none") == 0) return -1;
    char *argv[] = {
        "/usr/sbin/networksetup", "-setairportpower", (char *)dev, on ? "on" : "off", NULL
    };
    return run_cmd(argv);
}

/* Apply power, wait, and confirm. Retry because airport power is often asynchronous. */
/* 전원을 적용한 뒤 확인하고, 비동기라 실패하면 재시도한다. */
static int wifi_set_power_verify(const char *dev, int on) {
    if (!dev || !dev[0] || strcmp(dev, "none") == 0) return -1;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (wifi_set_power(dev, on) != 0) {
            at_log(AT_LOG_WARN, "Wi-Fi(%s) power %s command failed (attempt %d)",
                   dev, on ? "on" : "off", attempt + 1);
        }
        usleep(350000);
        if (on) {
            char *upv[] = { "/sbin/ifconfig", (char *)dev, "up", NULL };
            (void)run_cmd(upv);
        }
        int st = wifi_power_on(dev);
        if (st == on) return 0;
        if (st < 0 && attempt == 0) {
            /* getairportpower can lag; treat command success as tentative. */
            /* getairportpower 가 늦을 수 있어 명령 성공은 잠정 성공으로 본다. */
        }
    }
    int st = wifi_power_on(dev);
    return (st == on) ? 0 : -1;
}

static void persist_wifi_backup(const char *dev, int was_on, int turned_off) {
    const char *d = (dev && dev[0]) ? dev : "none";
    char script[1024];
    snprintf(script, sizeof(script),
             "open\n"
             "d.init\n"
             "d.add Device \"%s\"\n"
             "d.add WasOn \"%s\"\n"
             "d.add TurnedOff \"%s\"\n"
             "set %s\n"
             "close\n",
             d, was_on ? "true" : "false", turned_off ? "true" : "false", kBackupWifi);
    if (scutil_run(script) != 0)
        at_log(AT_LOG_WARN, "Failed to store Wi-Fi backup");
}

static void snapshot_wifi(char *dev, size_t dev_n, int *was_on) {
    if (was_on) *was_on = 0;
    if (wifi_find_device(dev, dev_n) != 0) {
        if (dev && dev_n) dev[0] = '\0';
        persist_wifi_backup("none", 0, 0);
        return;
    }
    int on = wifi_power_on(dev);
    if (was_on) *was_on = (on == 1);
    strlcpy(g_wifi_dev, dev, sizeof(g_wifi_dev));
    persist_wifi_backup(dev, on == 1, 0);
}

static int restore_wifi_device(const char *dev) {
    char found[32];
    found[0] = '\0';
    const char *use = (dev && dev[0] && strcmp(dev, "none") != 0) ? dev : NULL;
    if (!use) {
        if (g_wifi_dev[0]) use = g_wifi_dev;
        else if (wifi_find_device(found, sizeof(found)) == 0) use = found;
    }
    if (!use) {
        at_log(AT_LOG_WARN, "No Wi-Fi device found; could not restore power");
        return -1;
    }
    if (wifi_set_power_verify(use, 1) == 0) {
        at_log(AT_LOG_INFO, "Restored Wi-Fi(%s) to the pre-tether state", use);
        g_wifi_we_turned_off = 0;
        clear_wifi_marker();
        return 0;
    }
    /* Stored BSD name may be stale; rediscover and retry. */
    /* 저장한 BSD 이름이 낡았을 수 있어 다시 찾아 재시도한다. */
    if (wifi_find_device(found, sizeof(found)) == 0 && strcmp(found, use) != 0) {
        if (wifi_set_power_verify(found, 1) == 0) {
            at_log(AT_LOG_INFO, "Restored Wi-Fi(%s) to the pre-tether state", found);
            g_wifi_we_turned_off = 0;
            clear_wifi_marker();
            return 0;
        }
        use = found;
    }
    at_log(AT_LOG_WARN, "Failed to turn Wi-Fi(%s) on", use);
    return -1;
}

static void restore_wifi_from_backup(void) {
    char dump[2048];
    char dev[32] = {0}, was[16] = {0}, off[16] = {0};
    if (scutil_show(kBackupWifi, dump, sizeof(dump)) == 0) {
        at_parse_scutil_field(dump, "Device", dev, sizeof(dev));
        at_parse_scutil_field(dump, "WasOn", was, sizeof(was));
        at_parse_scutil_field(dump, "TurnedOff", off, sizeof(off));
    }
    if (!dev[0] && g_wifi_dev[0]) strlcpy(dev, g_wifi_dev, sizeof(dev));
    int should = g_wifi_we_turned_off || at_parse_wifi_truthy(off) || at_parse_wifi_truthy(was);
    if (!should) {
        if (dev[0])
            at_log(AT_LOG_INFO, "Pre-tether Wi-Fi(%s) was off; leaving it off", dev);
        return;
    }
    (void)restore_wifi_device(dev);
}

int at_net_disable_wifi_during_tether(void) {
    char dev[32];
    if (wifi_find_device(dev, sizeof(dev)) != 0) {
        at_log(AT_LOG_INFO, "No Wi-Fi device found; not changing radio power");
        return 0;
    }
    int on = wifi_power_on(dev);
    if (on == 0) {
        at_log(AT_LOG_INFO, "Wi-Fi(%s) is already off", dev);
        persist_wifi_backup(dev, 0, 0);
        return 0;
    }
    /* Unknown (-1): still turn off, but record that we did so restore will turn it back on. */
    /* 상태를 모르면(-1) 끄되, 우리가 껐다고 기록해 해제 시 다시 켠다. */
    if (wifi_set_power_verify(dev, 0) != 0) {
        at_log(AT_LOG_WARN, "Failed to turn Wi-Fi(%s) off", dev);
        return -1;
    }
    strlcpy(g_wifi_dev, dev, sizeof(g_wifi_dev));
    g_wifi_we_turned_off = 1;
    persist_wifi_backup(dev, 1, 1);
    write_wifi_marker(dev);
    at_log(AT_LOG_INFO, "Wi-Fi(%s) off while tethered", dev);
    return 0;
}

static int snapshot_default_route(char *kind, size_t kind_n, char *gw, size_t gw_n,
                                  char *iface, size_t iface_n) {
    strlcpy(kind, "none", kind_n);
    gw[0] = '\0';
    iface[0] = '\0';
    char out[2048];
    char *argv[] = { "/sbin/route", "-n", "get", "default", NULL };
    if (capture_cmd(argv, out, sizeof(out)) != 0) return 0;
    at_parse_scutil_field(out, "gateway:", gw, gw_n);
    at_parse_scutil_field(out, "interface:", iface, iface_n);
    if (gw[0] && strncmp(gw, "link#", 5) == 0) {
        strlcpy(kind, "iface", kind_n);
        gw[0] = '\0';
    } else if (gw[0] && strchr(gw, '.') != NULL) {
        strlcpy(kind, "ipv4", kind_n);
    } else if (iface[0]) {
        strlcpy(kind, "iface", kind_n);
    }
    return 0;
}

static int snapshot_default_route6(char *kind, size_t kind_n, char *gw, size_t gw_n,
                                   char *iface, size_t iface_n) {
    strlcpy(kind, "none", kind_n);
    gw[0] = '\0';
    iface[0] = '\0';
    char out[2048];
    char *argv[] = { "/sbin/route", "-n", "get", "-inet6", "default", NULL };
    if (capture_cmd(argv, out, sizeof(out)) != 0) return 0;
    at_parse_scutil_field(out, "gateway:", gw, gw_n);
    at_parse_scutil_field(out, "interface:", iface, iface_n);
    if (gw[0] && strncmp(gw, "link#", 5) == 0) {
        strlcpy(kind, "iface", kind_n);
        gw[0] = '\0';
    } else if (gw[0] && strchr(gw, ':') != NULL) {
        strlcpy(kind, "ipv6", kind_n);
    } else if (iface[0]) {
        strlcpy(kind, "iface", kind_n);
    }
    return 0;
}

static int restore_default_route(const char *kind, const char *gw, const char *iface) {
    char *delv[] = { "/sbin/route", "-n", "delete", "default", NULL };
    (void)run_cmd(delv);
    if (!kind || strcmp(kind, "none") == 0) {
        at_log(AT_LOG_INFO, "No prior default route to restore");
        return 0;
    }
    if (strcmp(kind, "ipv4") == 0 && gw && gw[0]) {
        char *addv[] = { "/sbin/route", "-n", "add", "default", (char *)gw, NULL };
        if (run_cmd(addv) != 0) {
            at_log(AT_LOG_WARN, "Failed to restore default route (%s)", gw);
            return -1;
        }
        at_log(AT_LOG_INFO, "Restored default route via %s", gw);
        return 0;
    }
    if (iface && iface[0]) {
        char *add2[] = {
            "/sbin/route", "-n", "add", "default", "-interface", (char *)iface, NULL
        };
        if (run_cmd(add2) != 0) {
            at_log(AT_LOG_WARN, "Failed to restore default route (iface %s)", iface);
            return -1;
        }
        at_log(AT_LOG_INFO, "Restored default route via interface %s",
               iface);
        return 0;
    }
    return 0;
}

static int restore_default_route6(const char *kind, const char *gw, const char *iface) {
    for (int i = 0; i < 4; i++) {
        char *delv[] = { "/sbin/route", "-n", "delete", "-inet6", "default", NULL };
        if (run_cmd(delv) != 0) break;
    }
    if (!kind || strcmp(kind, "none") == 0) return 0;
    if (strcmp(kind, "ipv6") == 0 && gw && gw[0]) {
        char *addv[] = { "/sbin/route", "-n", "add", "-inet6", "default", (char *)gw, NULL };
        if (run_cmd(addv) != 0) {
            at_log(AT_LOG_WARN, "Failed to restore IPv6 default route (%s)", gw);
            return -1;
        }
        at_log(AT_LOG_INFO, "Restored IPv6 default route via %s", gw);
        return 0;
    }
    if (iface && iface[0] && strcmp(iface, "none") != 0) {
        char *add2[] = {
            "/sbin/route", "-n", "add", "-inet6", "default",
            "-interface", (char *)iface, NULL
        };
        if (run_cmd(add2) != 0) {
            at_log(AT_LOG_WARN, "Failed to restore IPv6 default route (iface %s)", iface);
            return -1;
        }
        at_log(AT_LOG_INFO, "Restored IPv6 default route via interface %s", iface);
        return 0;
    }
    return 0;
}

static void remove_backup_keys(void) {
    scutil_remove_key(kBackupIPv4);
    scutil_remove_key(kBackupDNS);
    scutil_remove_key(kBackupRoute);
    scutil_remove_key(kBackupWifi);
    scutil_remove_key(kBackupValid);
    scutil_remove_key("State:/Network/MacOSWiredTethering/Backup");
    scutil_remove_key("State:/Network/MacOSWiredTethering");
}

/* Restore Global IPv4/DNS and default route from scutil backup (crash or disconnect). */
/* scutil 백업에서 Global IPv4/DNS와 기본 경로를 복구한다. 크래시·해제 공통. */
static int restore_from_backup(int remove_backup) {
    /* Wi-Fi power is independent of the IPv4 snapshot. Always restore it first. */
    /* Wi-Fi 전원은 IPv4 스냅샷과 별개이므로 항상 먼저 복구한다. */
    restore_wifi_from_backup();
    if (g_wifi_we_turned_off)
        (void)restore_wifi_device(g_wifi_dev[0] ? g_wifi_dev : NULL);

    char valid[2048], route[2048];
    int have_valid = scutil_show(kBackupValid, valid, sizeof(valid)) == 0;
    if (!have_valid) {
        if (remove_backup) remove_backup_keys();
        return 0;
    }

    char has_v4[32] = {0}, has_dns[32] = {0};
    at_parse_scutil_field(valid, "HasIPv4", has_v4, sizeof(has_v4));
    at_parse_scutil_field(valid, "HasDNS", has_dns, sizeof(has_dns));

    if (strcmp(has_v4, "true") == 0) {
        if (scutil_copy_key(kBackupIPv4, kGlobalIPv4) != 0)
            at_log(AT_LOG_WARN, "Failed to restore Global IPv4");
        else
            at_log(AT_LOG_INFO, "Restored State:/Network/Global/IPv4");
    } else {
        scutil_remove_key(kGlobalIPv4);
        at_log(AT_LOG_INFO, "Removed the Global IPv4 key created by tethering");
    }

    if (strcmp(has_dns, "true") == 0) {
        if (scutil_copy_key(kBackupDNS, kGlobalDNS) != 0)
            at_log(AT_LOG_WARN, "Failed to restore Global DNS");
        else
            at_log(AT_LOG_INFO, "Restored State:/Network/Global/DNS");
    } else {
        scutil_remove_key(kGlobalDNS);
        at_log(AT_LOG_INFO, "Removed the Global DNS key created by tethering");
    }

    if (scutil_show(kBackupRoute, route, sizeof(route)) == 0) {
        char kind[32] = {0}, gw[64] = {0}, iface[32] = {0};
        at_parse_scutil_field(route, "Kind", kind, sizeof(kind));
        at_parse_scutil_field(route, "Gateway", gw, sizeof(gw));
        at_parse_scutil_field(route, "Interface", iface, sizeof(iface));
        (void)restore_default_route(kind[0] ? kind : "none", gw, iface);
        char kind6[32] = {0}, gw6[128] = {0}, if6[32] = {0};
        at_parse_scutil_field(route, "Kind6", kind6, sizeof(kind6));
        at_parse_scutil_field(route, "Gateway6", gw6, sizeof(gw6));
        at_parse_scutil_field(route, "Interface6", if6, sizeof(if6));
        (void)restore_default_route6(kind6[0] ? kind6 : "none", gw6, if6);
    } else {
        (void)restore_default_route("none", "", "");
    }

    if (remove_backup) remove_backup_keys();
    return 0;
}

static int take_snapshot_to_backup(void) {
    int has_v4 = scutil_key_exists(kGlobalIPv4);
    int has_dns = scutil_key_exists(kGlobalDNS);
    if (has_v4) {
        if (scutil_copy_key(kGlobalIPv4, kBackupIPv4) != 0)
            at_log(AT_LOG_WARN, "Failed to backup Global IPv4");
    }
    if (has_dns) {
        if (scutil_copy_key(kGlobalDNS, kBackupDNS) != 0)
            at_log(AT_LOG_WARN, "Failed to backup Global DNS");
    }

    char kind[32], gw[64], iface[32];
    char kind6[32], gw6[128], if6[32];
    char wdev[32];
    int wifi_on = 0;
    snapshot_default_route(kind, sizeof(kind), gw, sizeof(gw), iface, sizeof(iface));
    snapshot_default_route6(kind6, sizeof(kind6), gw6, sizeof(gw6), if6, sizeof(if6));
    snapshot_wifi(wdev, sizeof(wdev), &wifi_on);
    char script[2048];
    snprintf(script, sizeof(script),
             "open\n"
             "d.init\n"
             "d.add Kind %s\n"
             "d.add Gateway \"%s\"\n"
             "d.add Interface %s\n"
             "d.add Kind6 %s\n"
             "d.add Gateway6 \"%s\"\n"
             "d.add Interface6 %s\n"
             "d.add WifiDev \"%s\"\n"
             "d.add WifiWasOn \"%s\"\n"
             "set %s\n"
             "d.init\n"
             "d.add HasIPv4 %s\n"
             "d.add HasDNS %s\n"
             "d.add Ready true\n"
             "set %s\n"
             "close\n",
             kind, gw, iface[0] ? iface : "none",
             kind6, gw6, if6[0] ? if6 : "none",
             wdev[0] ? wdev : "none",
             wifi_on ? "true" : "false",
             kBackupRoute,
             has_v4 ? "true" : "false",
             has_dns ? "true" : "false",
             kBackupValid);
    if (scutil_run(script) != 0) {
        at_log(AT_LOG_WARN, "Failed to store network snapshot");
        return -1;
    }
    at_log(AT_LOG_INFO,
           "Network snapshot: default kind=%s gw=%s if=%s v6=%s gw6=%s wifi=%s/%s global_v4=%d dns=%d",
           kind, gw[0] ? gw : "-", iface[0] ? iface : "-",
           kind6, gw6[0] ? gw6 : "-",
           wdev[0] ? wdev : "-", wifi_on ? "on" : "off",
           has_v4, has_dns);
    return 0;
}

static int iface_is_virtual_tether(const char *iface) {
    return iface && (strncmp(iface, "utun", 4) == 0 || strncmp(iface, "feth", 4) == 0);
}

static void destroy_stale_feth(void) {
    char out[8192];
    char *argv[] = { "/sbin/ifconfig", "-l", NULL };
    if (capture_cmd(argv, out, sizeof(out)) != 0) return;
    char copy[8192];
    strlcpy(copy, out, sizeof(copy));
    char *sp = NULL;
    for (char *tok = strtok_r(copy, " \t\n", &sp); tok; tok = strtok_r(NULL, " \t\n", &sp)) {
        if (strncmp(tok, "feth", 4) != 0) continue;
        at_log(AT_LOG_WARN, "Destroying leftover virtual Ethernet %s", tok);
        char *d[] = { "/sbin/ifconfig", tok, "destroy", NULL };
        (void)run_cmd(d);
    }
}

int at_net_begin_session(void) {
    if (g_session_open) return 0;

    /* Old feth pairs steal 10.11.226.11 and look like a live default. */
    /* 예전 feth 쌍이 10.11.226.11 을 가져가 살아있는 기본 경로처럼 보인다. */
    destroy_stale_feth();

    char live_kind[32], live_gw[64], live_if[32];
    snapshot_default_route(live_kind, sizeof(live_kind), live_gw, sizeof(live_gw),
                           live_if, sizeof(live_if));
    int live_ok = strcmp(live_kind, "none") != 0 && !iface_is_virtual_tether(live_if);

    /* Do not restore a stale backup over a working Wi-Fi/Ethernet default. */
    /* 살아있는 Wi-Fi/유선 기본 경로 위에 낡은 백업을 덮어쓰지 않는다. */
    if (scutil_key_exists(kBackupValid)) {
        if (live_ok) {
            at_log(AT_LOG_INFO,
                   "Live default route %s (%s) is present; discarding the previous backup",
                   live_gw[0] ? live_gw : live_if, live_if);
            remove_backup_keys();
        } else {
            at_log(AT_LOG_WARN,
                   "Restoring leftover network backup from a previous tether session");
            (void)restore_from_backup(1);
            (void)at_net_unpublish_path();
            (void)at_net_remove_service();
        }
    }
    if (take_snapshot_to_backup() != 0) return -1;
    g_session_open = 1;
    return 0;
}

int at_net_end_session(void) {
    int had = g_session_open || g_wifi_we_turned_off ||
              scutil_key_exists(kBackupValid) || scutil_key_exists(kBackupWifi);
    if (!had) {
        /* Marker from a previous killed engine: still try to power Wi-Fi on. */
        /* 이전에 강제 종료된 엔진의 마커가 있으면 Wi-Fi를 켠다. */
        if (wifi_marker_present())
            (void)restore_wifi_device(NULL);
        /* Drop leftover scutil/networksetup service even without a snapshot. */
        /* 스냅샷이 없어도 남은 scutil·networksetup 서비스는 지운다. */
        (void)at_net_unpublish_path();
        (void)at_net_remove_service();
        return 0;
    }
    (void)restore_from_backup(1);
    /* Belt-and-suspenders: in-process flag survives a missing/unreadable scutil key. */
    /* 이중 안전장치: scutil 키가 없거나 읽히지 않아도 프로세스 플래그로 복구한다. */
    if (g_wifi_we_turned_off)
        (void)restore_wifi_device(g_wifi_dev[0] ? g_wifi_dev : NULL);
    (void)at_net_unpublish_path();
    (void)at_net_remove_service();
    g_session_open = 0;
    return 0;
}

int at_net_restore_wifi(void) {
    restore_wifi_from_backup();
    if (g_wifi_we_turned_off || wifi_marker_present())
        return restore_wifi_device(g_wifi_dev[0] ? g_wifi_dev : NULL);
    return 0;
}

static int register_via_scutil(const char *ifname) {
    /* Register a named service so configd / NWPathMonitor can rank the tunnel. */
    /* configd / NWPathMonitor 가 터널을 인식하도록 이름 있는 서비스를 등록한다. */
    char script[2048];
    snprintf(script, sizeof(script),
             "open\n"
             "d.init\n"
             "d.add UserDefinedName \"%s\"\n"
             "set Setup:/Network/Service/%s\n"
             "d.init\n"
             "d.add Type IPSec\n"
             "d.add Hardware Hardware\n"
             "d.add DeviceName %s\n"
             "d.add UserDefinedName \"%s\"\n"
             "set Setup:/Network/Service/%s/Interface\n"
             "d.init\n"
             "d.add ConfigMethod Manual\n"
             "set Setup:/Network/Service/%s/IPv4\n"
             "close\n",
             kServiceName, kServiceId, ifname, kServiceName, kServiceId, kServiceId);
    char *argv[] = { "/usr/sbin/scutil", NULL };
    if (run_cmd_stdin(argv, script) != 0) {
        at_log(AT_LOG_WARN, "scutil service register failed");
        return -1;
    }
    at_log(AT_LOG_INFO, "Registered network service '%s' (%s) via scutil",
           kServiceName, ifname);
    return 0;
}

int at_net_register_service(const char *ifname) {
    if (!ifname) return -1;
    char *detect[] = { "/usr/sbin/networksetup", "-detectnewhardware", NULL };
    (void)run_cmd(detect);
    /* Named service helps configd rank the utun tunnel. System Settings may still hide it. */
    /* 이름 있는 서비스가 있으면 configd가 utun 터널을 순위 매긴다. 시스템 설정에는 안 보일 수 있다. */
    char out[512];
    char *create[] = {
        "/usr/sbin/networksetup", "-createnetworkservice",
        (char *)kServiceName, (char *)ifname, NULL
    };
    if (capture_cmd_merged(create, out, sizeof(out)) != 0) {
        at_log(AT_LOG_INFO, "networksetup service create failed (%s); retrying with scutil",
               out[0] ? out : "unknown");
        (void)register_via_scutil(ifname);
    } else {
        at_log(AT_LOG_INFO, "Created network service '%s' (%s)", kServiceName, ifname);
    }
    char *setdhcp[] = {
        "/usr/sbin/networksetup", "-setmanual", (char *)kServiceName,
        "0.0.0.0", "255.255.255.255", "0.0.0.0", NULL
    };
    (void)run_cmd(setdhcp);
    return 0;
}

int at_net_remove_service(void) {
    char *rm[] = {
        "/usr/sbin/networksetup", "-removenetworkservice",
        (char *)kServiceName, NULL
    };
    (void)run_cmd(rm);
    char script[512];
    snprintf(script, sizeof(script),
             "open\n"
             "remove Setup:/Network/Service/%s/IPv4\n"
             "remove Setup:/Network/Service/%s/Interface\n"
             "remove Setup:/Network/Service/%s\n"
             "close\n",
             kServiceId, kServiceId, kServiceId);
    char *sc[] = { "/usr/sbin/scutil", NULL };
    (void)run_cmd_stdin(sc, script);
    return 0;
}

int at_net_apply_lease(const char *ifname, const at_net_info_t *lease, int as_primary) {
    if (!ifname || !lease || lease->ip[0] == '\0') return -1;
    if (!at_ifname_ok(ifname) || !at_lease_addrs_ok(lease)) {
        at_log(AT_LOG_ERROR, "Lease address is not dotted IPv4");
        return -1;
    }
    const char *mask = lease->netmask[0] ? lease->netmask : "255.255.255.0";
    const char *peer = lease->gateway[0] ? lease->gateway : lease->ip;

    /* utun is point-to-point: ifconfig NAME inet LOCAL PEER. */
    /* utun은 점대점이다. ifconfig NAME inet 로컬 피어. */
    char *p2p[] = {
        "/sbin/ifconfig", (char *)ifname, "inet", (char *)lease->ip,
        (char *)peer, "netmask", "255.255.255.255", "up", NULL
    };
    if (run_cmd(p2p) != 0) {
        char *plain[] = {
            "/sbin/ifconfig", (char *)ifname, "inet", (char *)lease->ip,
            "netmask", (char *)mask, "up", NULL
        };
        if (run_cmd(plain) != 0) {
            at_log(AT_LOG_ERROR, "ifconfig %s inet %s failed", ifname, lease->ip);
            return -1;
        }
    }
    at_log(AT_LOG_INFO, "Applied %s address %s gateway %s", ifname, lease->ip, peer);
    /* utun is L3: 1500 matches typical RNDIS Ethernet payload (Linux rndis_host). */
    /* utun은 L3이다. 1500은 일반 RNDIS 이더넷 페이로드와 같다(Linux rndis_host). */
    char mtu_s[16];
    snprintf(mtu_s, sizeof(mtu_s), "%d", AT_UTUN_MTU);
    char *mtu[] = { "/sbin/ifconfig", (char *)ifname, "mtu", mtu_s, NULL };
    if (run_cmd(mtu) != 0)
        at_log(AT_LOG_WARN, "Failed to set %s MTU %d", ifname, AT_UTUN_MTU);
    else
        at_log(AT_LOG_INFO, "%s MTU %d", ifname, AT_UTUN_MTU);
    (void)at_net_publish_path(ifname, lease, as_primary);
    return 0;
}

int at_net_publish_path(const char *ifname, const at_net_info_t *lease, int as_primary) {
    if (!ifname || !lease) return -1;
    if (!at_ifname_ok(ifname) || !at_lease_addrs_ok(lease)) {
        at_log(AT_LOG_ERROR, "Publish addresses are not dotted IPv4");
        return -1;
    }
    if (as_primary && !g_session_open) {
        at_log(AT_LOG_WARN, "No snapshot; not stealing the system primary route");
        as_primary = 0;
    }
    const char *gw = lease->gateway[0] ? lease->gateway : lease->ip;
    char dns1[64] = {0}, dns2[64] = {0};
    if (lease->dns[0]) sscanf(lease->dns, "%63s %63s", dns1, dns2);
    if (!dns1[0]) strlcpy(dns1, gw, sizeof(dns1));

    char dns_line[160];
    if (dns2[0])
        snprintf(dns_line, sizeof(dns_line), "%s %s", dns1, dns2);
    else
        snprintf(dns_line, sizeof(dns_line), "%s", dns1);

    char script[4096];
    if (as_primary) {
        /* Steal Global IPv4/DNS so Chrome/NWPathMonitor treat the tunnel as primary. */
        /* Chrome/NWPathMonitor 가 터널을 기본 경로로 보도록 Global IPv4/DNS 를 가져온다. */
        snprintf(script, sizeof(script),
                 "open\n"
                 "d.init\n"
                 "d.add Addresses * %s\n"
                 "d.add Destinations * %s\n"
                 "d.add InterfaceName %s\n"
                 "d.add Router %s\n"
                 "set State:/Network/Service/%s/IPv4\n"
                 "d.init\n"
                 "d.add DeviceName %s\n"
                 "d.add Type IPSec\n"
                 "set State:/Network/Service/%s/Interface\n"
                 "d.init\n"
                 "d.add ServerAddresses * %s\n"
                 "set State:/Network/Service/%s/DNS\n"
                 "d.init\n"
                 "d.add PrimaryInterface %s\n"
                 "d.add PrimaryService %s\n"
                 "d.add Router %s\n"
                 "set State:/Network/Global/IPv4\n"
                 "d.init\n"
                 "d.add ServerAddresses * %s\n"
                 "set State:/Network/Global/DNS\n"
                 "close\n",
                 lease->ip, gw, ifname, gw, kServiceId,
                 ifname, kServiceId,
                 dns_line, kServiceId,
                 ifname, kServiceId, gw,
                 dns_line);
    } else {
        snprintf(script, sizeof(script),
                 "open\n"
                 "d.init\n"
                 "d.add Addresses * %s\n"
                 "d.add Destinations * %s\n"
                 "d.add InterfaceName %s\n"
                 "d.add Router %s\n"
                 "set State:/Network/Service/%s/IPv4\n"
                 "d.init\n"
                 "d.add DeviceName %s\n"
                 "d.add Type IPSec\n"
                 "set State:/Network/Service/%s/Interface\n"
                 "d.init\n"
                 "d.add ServerAddresses * %s\n"
                 "set State:/Network/Service/%s/DNS\n"
                 "close\n",
                 lease->ip, gw, ifname, gw, kServiceId,
                 ifname, kServiceId,
                 dns_line, kServiceId);
    }
    if (scutil_run(script) != 0) {
        at_log(AT_LOG_WARN, "Failed to publish network path via scutil");
        return -1;
    }
    at_log(AT_LOG_INFO, "Published system path: if=%s ip=%s gw=%s dns=%s primary=%d",
           ifname, lease->ip, gw, dns_line, as_primary);
    return 0;
}

int at_net_unpublish_path(void) {
    char script[1024];
    snprintf(script, sizeof(script),
             "open\n"
             "remove State:/Network/Service/%s/IPv4\n"
             "remove State:/Network/Service/%s/DNS\n"
             "remove State:/Network/Service/%s/Interface\n"
             "remove State:/Network/Service/%s\n"
             "remove Setup:/Network/Service/%s/IPv4\n"
             "remove Setup:/Network/Service/%s/Interface\n"
             "remove Setup:/Network/Service/%s\n"
             "close\n",
             kServiceId, kServiceId, kServiceId, kServiceId,
             kServiceId, kServiceId, kServiceId);
    (void)scutil_run(script);
    return 0;
}

int at_net_clear_dhcp(const char *ifname) {
    /* Restore Wi-Fi/wired default and Global DNS before tearing the tunnel down. */
    /* 터널을 내리기 전에 Wi-Fi/유선 기본 경로와 Global DNS를 복구한다. */
    (void)at_net_end_session();
    if (ifname) {
        char *argv[] = { "/sbin/ifconfig", (char *)ifname, "inet", "-alias", NULL };
        (void)run_cmd(argv);
        char *down[] = { "/sbin/ifconfig", (char *)ifname, "down", NULL };
        (void)run_cmd(down);
    }
    (void)at_net_unpublish_path();
    (void)at_net_remove_service();
    return 0;
}

int at_net_prefer_default_route(const char *ifname, const char *gateway) {
    if (!ifname || !gateway || gateway[0] == '\0') return -1;
    if (!at_ifname_ok(ifname) || !at_ipv4_dotted_ok(gateway)) return -1;
    if (!g_session_open) {
        at_log(AT_LOG_WARN, "No snapshot; skipping default route change");
        return -1;
    }
    /* utun is point-to-point: send the default into the tunnel interface. */
    /* utun은 점대점이므로 기본 경로를 터널 인터페이스로 넣는다. */
    char *delv[] = { "/sbin/route", "-n", "delete", "default", NULL };
    (void)run_cmd(delv);
    char *add_if[] = {
        "/sbin/route", "-n", "add", "default", "-interface", (char *)ifname, NULL
    };
    if (run_cmd(add_if) != 0) {
        char *addv[] = { "/sbin/route", "-n", "add", "default", (char *)gateway, NULL };
        if (run_cmd(addv) != 0) {
            at_log(AT_LOG_WARN, "Failed to set default route");
            return -1;
        }
    }
    /* Chrome Happy Eyeballs would otherwise use leftover Wi-Fi IPv6 and time out. */
    /* Chrome Happy Eyeballs가 남은 Wi-Fi IPv6를 쓰면 타임아웃 난다. */
    (void)at_net_drop_ipv6_default();
    at_log(AT_LOG_INFO, "Default route set via %s (%s)", gateway, ifname);
    return 0;
}

int at_net_drop_ipv6_default(void) {
    /* One quiet attempt. macOS has no IPv6 default most of the time, and
     * repeating `route delete` every 2s flooded the log and forked extra work. */
    /* 한 번만, 출력 없이 시도한다. IPv6 기본 경로가 없는 경우가 많고
     * 2초마다 route delete 를 반복하면 로그가 차고 프로세스만 늘어난다. */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *del6[] = { "/sbin/route", "-n", "delete", "-inet6", "default", NULL };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, del6[0], &fa, NULL, del6, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return 1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
}
