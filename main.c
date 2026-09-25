/*
 * wifid — wireless interface daemon for CactOS.
 *
 * Watches for wireless devices appearing in devfs (name contains "wlan",
 * "wifi" or "wl") and keeps the AF_UNIX service /run/wifid.sock for clients.
 * The kernel wireless stack (wlan/802.11) is not implemented yet — wlan* nodes
 * are not registered, so the daemon silently waits for such a node to appear.
 *
 * Protocol (one line per connection):
 *   status -> "wifi=none\n" or "wifi=present path=/dev/<node>\n"
 *
 * Started by the cgoct supervisor as /sbin/wifid.
 *
 * /etc/wifid.conf (all keys optional; created on first start):
 *   file=/var/log/wifid.log
 *   console=0
 *   interval=5
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include <socket.h>
#include <poll.h>
#include <dirent.h>

#define CONFIG_PATH "/etc/wifid.conf"
#define SOCK_PATH   "/run/wifid.sock"
#define LOG_DEFAULT "/var/log/wifid.log"
#define MAX_NODE    64

static char log_path[128] = LOG_DEFAULT;
static int  console_on    = 0;
static int  interval_sec  = 5;
static int  out_fd        = -1;

static char wifi_node[MAX_NODE] = ""; /* "" = no wireless devices */

/* Default config: written on first start if the file does not exist yet. */
static const char default_config[] =
    "# wifid config - auto-generated on first start.\n"
    "#\n"
    "# file     - event log\n"
    "# console  - duplicate to /dev/console (0|1)\n"
    "# interval - /dev poll period (sec)\n"
    "\n"
    "file=/var/log/wifid.log\n"
    "console=0\n"
    "interval=5\n";

static void ensure_dir(const char *path) {
    (void)mkdir(path, 0755);
}

static void config_write_default(void) {
    int fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return;
    write(fd, default_config, sizeof(default_config) - 1);
    close(fd);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        config_write_default();
        f = fopen(CONFIG_PATH, "r");
        if (!f) return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        } else if (strcmp(key, "interval") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 3600) interval_sec = v;
        }
    }
    fclose(f);
}

static void log_event(const char *msg) {
    if (out_fd >= 0) {
        write(out_fd, msg, strlen(msg));
    }
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, msg, strlen(msg));
            close(cfd);
        }
    }
}

static int is_wifi_name(const char *nm) {
    return (strstr(nm, "wlan") != 0 || strstr(nm, "wifi") != 0 ||
            strstr(nm, "wl") != 0);
}

/* Update wifi_node; return 1 if the state changed. */
static int scan_wifi(void) {
    char found[MAX_NODE] = "";
    int fd = open("/dev", O_RDONLY);
    if (fd >= 0) {
        struct dirent buf[24];
        int n;
        while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
            int count = n / (int)sizeof(struct dirent);
            int i;
            for (i = 0; i < count; i++) {
                if (is_wifi_name(buf[i].d_name) && found[0] == '\0') {
                    snprintf(found, sizeof(found), "%s", buf[i].d_name);
                }
            }
        }
        close(fd);
    }

    if (strcmp(found, wifi_node) == 0) return 0;

    char line[128];
    if (found[0] == '\0') {
        snprintf(line, sizeof(line), "wifid: wireless device removed\n");
    } else {
        snprintf(line, sizeof(line), "wifid: wireless device present (%s)\n", found);
    }
    log_event(line);
    printf("%s", line);
    strncpy(wifi_node, found, sizeof(wifi_node) - 1);
    wifi_node[sizeof(wifi_node) - 1] = '\0';
    return 1;
}

static void handle_client(int cl) {
    char req[64];
    char b;
    int  got = 0;
    int  n;

    while (got < (int)sizeof(req) - 1) {
        n = (int)recv(cl, &b, 1, 0);
        if (n <= 0) break;
        if (b == '\n' || b == '\r') break;
        req[got++] = b;
    }
    req[got] = '\0';

    if (strcmp(req, "status") == 0) {
        char resp[160];
        if (wifi_node[0] == '\0') {
            snprintf(resp, sizeof(resp), "wifi=none\n");
        } else {
            snprintf(resp, sizeof(resp), "wifi=present path=/dev/%s\n", wifi_node);
        }
        send(cl, resp, (uint32_t)strlen(resp), 0);
        log_event("wifid: status requested\n");
        return;
    }

    const char *err = "ERR unknown command\n";
    send(cl, err, (uint32_t)strlen(err), 0);
}

static int bind_listener(void) {
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(srv);
        return -1;
    }
    if (listen(srv, 4) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

static long mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("wifid: starting\n");
    config_load();
    ensure_dir("/var/log");
    ensure_dir("/run");

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("wifid: cannot open %s\n", log_path);
    }
    log_event("wifid: starting\n");

    scan_wifi();

    int srv = -1;
    long next_scan = mono_ms() + interval_sec * 1000L;

    for (;;) {
        if (srv < 0) {
            srv = bind_listener();
            if (srv < 0) {
                sleep(3);
                continue;
            }
            printf("wifid: listening on %s\n", SOCK_PATH);
            log_event("wifid: listening\n");
        }

        long now = mono_ms();
        int timeout = (int)(next_scan - now);
        if (timeout < 0) timeout = 0;
        if (timeout > 1000) timeout = 1000;

        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;

        if (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN)) {
            int cl = accept(srv, 0, 0);
            if (cl >= 0) {
                handle_client(cl);
                close(cl);
            }
        }

        if (mono_ms() >= next_scan) {
            scan_wifi();
            next_scan = mono_ms() + interval_sec * 1000L;
        }
    }
    return 0;
}
