/*
 * cursor-barrier — Hard cursor confinement for Hyprland
 *
 * Confines the mouse cursor to a specific monitor while a target window
 * is focused. Uses evdev grab + uinput to enforce a hard wall at the device
 * level — no jitter, no escape, zero latency.
 *
 * The mouse is only grabbed in a configurable zone near the boundary, so
 * mouse feel is 100% native during normal use. The grab activates before
 * the cursor can reach the edge even with a fast flick.
 *
 * Buttons held while near the boundary delay the grab until released,
 * preventing games from losing button-release events across device IDs.
 *
 * Usage:
 *   cursor-barrier [OPTIONS] PATTERN [PATTERN...]
 *
 * Options:
 *   -m, --monitor NAME    Confine to this monitor (e.g. DP-1). Auto-detected
 *                         from the matched window's monitor if not specified.
 *   -x, --boundary X      Left boundary X coordinate (overrides --monitor).
 *   -b, --buffer PIXELS   Grab activation zone width in pixels (default: 300).
 *   -h, --help            Show this help.
 *
 * Examples:
 *   cursor-barrier "war thunder"
 *   cursor-barrier "war thunder" "call of duty"
 *   cursor-barrier --monitor DP-1 "war thunder"
 *   cursor-barrier --monitor HDMI-A-1 --buffer 400 "minecraft"
 *   cursor-barrier --boundary 2560 "game.exe"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <getopt.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define MAX_MICE         16
#define MAX_PATTERNS     16
#define DEFAULT_BUFFER   300
#define GRAB_EXIT_EXTRA  200   /* hysteresis: ungrab this many px past grab threshold */

enum state { STATE_IDLE, STATE_WATCHING, STATE_GUARDING };

static volatile int running = 1;
static void handle_signal(int sig) { (void)sig; running = 0; }

/* ── Mouse device management ── */

struct mouse {
    struct libevdev        *dev;
    struct libevdev_uinput *uidev;
    int fd;
    int grabbed;
};

static int find_and_open_mice(struct mouse *mice) {
    int count = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) && count < MAX_MICE) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[128];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        struct libevdev *d = NULL;
        if (libevdev_new_from_fd(fd, &d) < 0) { close(fd); continue; }

        if (!libevdev_has_event_type(d, EV_REL) ||
            !libevdev_has_event_code(d, EV_REL, REL_X)) {
            libevdev_free(d); close(fd); continue;
        }

        struct libevdev_uinput *uid = NULL;
        if (libevdev_uinput_create_from_device(d, LIBEVDEV_UINPUT_OPEN_MANAGED, &uid) < 0) {
            libevdev_free(d); close(fd); continue;
        }

        mice[count].dev    = d;
        mice[count].uidev  = uid;
        mice[count].fd     = fd;
        mice[count].grabbed = 0;
        count++;
    }
    closedir(dir);
    return count;
}

static void grab_mice(struct mouse *mice, int n) {
    for (int i = 0; i < n; i++) {
        if (mice[i].dev && !mice[i].grabbed) {
            libevdev_grab(mice[i].dev, LIBEVDEV_GRAB);
            mice[i].grabbed = 1;
        }
    }
}

static void ungrab_mice(struct mouse *mice, int n) {
    for (int i = 0; i < n; i++) {
        if (mice[i].dev && mice[i].grabbed) {
            libevdev_grab(mice[i].dev, LIBEVDEV_UNGRAB);
            mice[i].grabbed = 0;
        }
    }
}

static void cleanup_mice(struct mouse *mice, int n) {
    for (int i = 0; i < n; i++) {
        if (mice[i].grabbed)  libevdev_grab(mice[i].dev, LIBEVDEV_UNGRAB);
        if (mice[i].uidev)    libevdev_uinput_destroy(mice[i].uidev);
        if (mice[i].dev)      libevdev_free(mice[i].dev);
        if (mice[i].fd >= 0)  close(mice[i].fd);
    }
}

/* ── Hyprland IPC ── */

static int connect_hypr_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int hypr_cmd(const char *socket_dir, const char *cmd, char *resp, size_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.socket.sock", socket_dir);
    int fd = connect_hypr_socket(path);
    if (fd < 0) return -1;
    write(fd, cmd, strlen(cmd));
    ssize_t total = 0, n;
    while (total < (ssize_t)len - 1) {
        n = read(fd, resp + total, len - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    resp[total] = '\0';
    close(fd);
    return (int)total;
}

static int get_cursor_x(const char *socket_dir) {
    char resp[256];
    if (hypr_cmd(socket_dir, "cursorpos", resp, sizeof(resp)) <= 0) return -1;
    int x, y;
    return sscanf(resp, "%d, %d", &x, &y) == 2 ? x : -1;
}

/*
 * Query the left boundary X of a named monitor.
 * Returns -1 if the monitor is not found.
 */
static int get_monitor_boundary(const char *socket_dir, const char *monitor_name) {
    char resp[4096];
    if (hypr_cmd(socket_dir, "j/monitors", resp, sizeof(resp)) <= 0) return -1;

    /* Quick JSON scan: find the monitor entry and extract its "x" field.
     * Looks for "name":"MONITOR_NAME" then finds "x": N nearby. */
    char search[128];
    snprintf(search, sizeof(search), "\"name\":\"%s\"", monitor_name);
    char *p = strstr(resp, search);
    if (!p) return -1;

    /* Within the next 512 chars, find "x": */
    char *end = p + 512 < resp + sizeof(resp) ? p + 512 : resp + sizeof(resp);
    char saved = *end; *end = '\0';
    char *xp = strstr(p, "\"x\":");
    *end = saved;
    if (!xp) return -1;

    return atoi(xp + 4);
}

/*
 * Get the left boundary X of the monitor containing the currently active window.
 * Uses j/activewindow (monitor ID) + j/monitors (ID → x).
 * This is reliable regardless of whether the pattern matched via class or title.
 */
static int get_active_window_boundary(const char *socket_dir) {
    char resp[4096];
    if (hypr_cmd(socket_dir, "j/activewindow", resp, sizeof(resp)) <= 0) return -1;

    /* Extract integer monitor ID: "monitor": N */
    char *mp = strstr(resp, "\"monitor\":");
    if (!mp) return -1;
    mp += 10;
    while (*mp == ' ' || *mp == '\t') mp++;
    if (!isdigit((unsigned char)*mp) && *mp != '-') return -1;
    int monitor_id = atoi(mp);

    /* Look up that monitor's x in j/monitors by matching "id": N */
    char mresp[4096];
    if (hypr_cmd(socket_dir, "j/monitors", mresp, sizeof(mresp)) <= 0) return -1;

    char search[64];
    snprintf(search, sizeof(search), "\"id\": %d", monitor_id);
    char *p = strstr(mresp, search);
    if (!p) return -1;

    /* Within the next 512 chars find "x": N */
    char *end = p + 512 < mresp + sizeof(mresp) ? p + 512 : mresp + sizeof(mresp);
    char saved = *end; *end = '\0';
    char *xp = strstr(p, "\"x\":");
    *end = saved;
    if (!xp) return -1;

    int bx = atoi(xp + 4);

    /* Log the detected monitor name for debugging */
    char *np = strstr(p, "\"name\":\"");
    if (np) {
        np += 8;
        char *nend = strchr(np, '"');
        if (nend) {
            char monitor_name[128] = {0};
            size_t nlen = (size_t)(nend - np) < sizeof(monitor_name) - 1
                          ? (size_t)(nend - np) : sizeof(monitor_name) - 1;
            memcpy(monitor_name, np, nlen);
            fprintf(stderr, "cursor-barrier: auto-detected monitor %s (id=%d), boundary x=%d\n",
                    monitor_name, monitor_id, bx);
        }
    }
    return bx;
}

/* ── Helpers ── */

static int contains_lower(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++)
            if (tolower(haystack[i + j]) != needle[j]) break;
        if (j == nlen) return 1;
    }
    return 0;
}

/*
 * Returns the index of the first pattern in pats[0..npatterns) that matches the
 * Hyprland active window class, or -1 if none match. Hyprland events arrive as
 * "class,title"; the command response is a multiline block with a "class:" row.
 */
static int match_pattern_idx(const char *haystack,
                              const char pats[][256], int npatterns) {
    char class_name[512] = {0};
    const char *match_text = haystack;

    const char *class_line = strstr(haystack, "class:");
    if (class_line) {
        class_line += 6;
        while (*class_line == ' ' || *class_line == '\t') class_line++;
        size_t len = strcspn(class_line, "\r\n");
        if (len >= sizeof(class_name)) len = sizeof(class_name) - 1;
        memcpy(class_name, class_line, len);
        match_text = class_name;
    } else {
        const char *comma = strchr(haystack, ',');
        if (comma) {
            size_t len = (size_t)(comma - haystack);
            if (len >= sizeof(class_name)) len = sizeof(class_name) - 1;
            memcpy(class_name, haystack, len);
            match_text = class_name;
        }
    }

    for (int i = 0; i < npatterns; i++) {
        if (contains_lower(match_text, pats[i]))
            return i;
    }
    return -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] PATTERN [PATTERN...]\n"
        "\n"
        "Confine the cursor to a monitor while a window matching any PATTERN is focused.\n"
        "PATTERNs are matched case-insensitively against the window class name.\n"
        "\n"
        "Options:\n"
        "  -m, --monitor NAME    Monitor to confine to (e.g. DP-1, HDMI-A-1).\n"
        "                        Auto-detected from the matched window if not given.\n"
        "  -x, --boundary X      Left boundary X coordinate (overrides --monitor).\n"
        "  -b, --buffer PIXELS   Grab activation zone in pixels (default: %d).\n"
        "  -h, --help            Show this help.\n"
        "\n"
        "Requires: Hyprland compositor, libevdev, /dev/input access (input group).\n"
        "\n"
        "Examples:\n"
        "  %s \"war thunder\"\n"
        "  %s \"war thunder\" \"call of duty\"\n"
        "  %s --monitor DP-1 \"war thunder\"\n"
        "  %s --monitor HDMI-A-1 --buffer 400 \"minecraft\"\n"
        "  %s --boundary 2560 \"game.exe\"\n",
        prog, DEFAULT_BUFFER, prog, prog, prog, prog, prog);
}

/* ── Main ── */

int main(int argc, char **argv) {
    const char *monitor_name = NULL;
    int boundary_x           = -1;   /* -1 = not set, auto-detect */
    int boundary_explicit    = 0;    /* set when --monitor or --boundary given */
    int buffer               = DEFAULT_BUFFER;

    static struct option opts[] = {
        { "monitor",  required_argument, NULL, 'm' },
        { "boundary", required_argument, NULL, 'x' },
        { "buffer",   required_argument, NULL, 'b' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "m:x:b:h", opts, NULL)) != -1) {
        switch (c) {
            case 'm': monitor_name = optarg;        boundary_explicit = 1; break;
            case 'x': boundary_x   = atoi(optarg); boundary_explicit = 1; break;
            case 'b': buffer       = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "cursor-barrier: at least one PATTERN is required\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Collect all remaining arguments as patterns */
    int npatterns = argc - optind;
    if (npatterns > MAX_PATTERNS) {
        fprintf(stderr, "cursor-barrier: too many patterns (max %d)\n", MAX_PATTERNS);
        return 1;
    }
    const char *patterns[MAX_PATTERNS];
    char pats[MAX_PATTERNS][256];  /* lowercase versions */
    for (int i = 0; i < npatterns; i++) {
        patterns[i] = argv[optind + i];
        memset(pats[i], 0, sizeof(pats[i]));
        for (size_t k = 0; k < strlen(patterns[i]) && k < sizeof(pats[i]) - 1; k++)
            pats[i][k] = tolower(patterns[i][k]);
    }

    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *xrd = getenv("XDG_RUNTIME_DIR");
    if (!his || !xrd) {
        fprintf(stderr, "cursor-barrier: HYPRLAND_INSTANCE_SIGNATURE and XDG_RUNTIME_DIR "
                        "must be set (are you running under Hyprland?)\n");
        return 1;
    }

    char socket_dir[512], event_path[512];
    snprintf(socket_dir, sizeof(socket_dir), "%s/hypr/%s", xrd, his);
    snprintf(event_path, sizeof(event_path), "%s/.socket2.sock", socket_dir);

    /* Resolve boundary from --monitor if --boundary not given */
    if (boundary_x < 0 && monitor_name) {
        boundary_x = get_monitor_boundary(socket_dir, monitor_name);
        if (boundary_x < 0) {
            fprintf(stderr, "cursor-barrier: monitor '%s' not found\n", monitor_name);
            return 1;
        }
        fprintf(stderr, "cursor-barrier: monitor %s boundary x=%d\n", monitor_name, boundary_x);
    }

    struct mouse mice[MAX_MICE];
    memset(mice, 0, sizeof(mice));
    int nmice = find_and_open_mice(mice);
    if (nmice == 0) {
        fprintf(stderr, "cursor-barrier: no mouse devices found — "
                        "are you in the 'input' group?\n");
        return 1;
    }
    fprintf(stderr, "cursor-barrier: found %d mouse device(s)\n", nmice);

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    int evfd = connect_hypr_socket(event_path);
    if (evfd < 0) {
        fprintf(stderr, "cursor-barrier: cannot connect to Hyprland event socket\n");
        cleanup_mice(mice, nmice);
        return 1;
    }

    struct pollfd pfds[1 + MAX_MICE];
    int pfd_mouse[MAX_MICE];
    pfds[0] = (struct pollfd){ .fd = evfd, .events = POLLIN };
    int npfds = 1;
    for (int i = 0; i < nmice; i++) {
        if (!mice[i].dev) continue;
        pfd_mouse[npfds] = i;
        pfds[npfds++] = (struct pollfd){ .fd = mice[i].fd, .events = POLLIN };
    }

    enum state state    = STATE_IDLE;
    int matched_idx     = -1;   /* index into pats[] of currently focused pattern, or -1 */
    int cursor_x        = boundary_x >= 0 ? boundary_x + 500 : 9999;
    int buttons_held    = 0;
    char resp[4096], evbuf[4096];

    /* Print startup banner */
    if (boundary_explicit && boundary_x >= 0) {
        if (npatterns == 1)
            fprintf(stderr, "cursor-barrier: watching for '%s', boundary x=%d, buffer=%dpx\n",
                    patterns[0], boundary_x, buffer);
        else
            fprintf(stderr, "cursor-barrier: watching for %d patterns, boundary x=%d, buffer=%dpx\n",
                    npatterns, boundary_x, buffer);
    } else {
        if (npatterns == 1)
            fprintf(stderr, "cursor-barrier: watching for '%s' (boundary will be auto-detected on first focus)\n",
                    patterns[0]);
        else
            fprintf(stderr, "cursor-barrier: watching for %d patterns (boundary will be auto-detected on first focus)\n",
                    npatterns);
    }

    /* Check initial state */
    if (hypr_cmd(socket_dir, "activewindow", resp, sizeof(resp)) > 0) {
        matched_idx = match_pattern_idx(resp, pats, npatterns);
        if (matched_idx >= 0) {
            if (!boundary_explicit) {
                boundary_x = get_active_window_boundary(socket_dir);
            }
            cursor_x = get_cursor_x(socket_dir);
            if (cursor_x < 0) cursor_x = boundary_x >= 0 ? boundary_x + 500 : 9999;
            state = STATE_WATCHING;
        }
    }

    while (running) {
        int timeout;
        switch (state) {
            case STATE_IDLE:     timeout = -1; break;
            case STATE_WATCHING: timeout = 500; break;
            case STATE_GUARDING: timeout = 100; break;
            default:             timeout = 30; break;
        }

        int ret = poll(pfds, npfds, timeout);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        /* ── Hyprland events ── */
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(evfd, evbuf, sizeof(evbuf) - 1);
            if (n <= 0) {
                close(evfd);
                usleep(500000);
                evfd = connect_hypr_socket(event_path);
                if (evfd < 0) break;
                pfds[0].fd = evfd;
                continue;
            }
            evbuf[n] = '\0';
            char *line = evbuf;
            while (line && *line) {
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                if (strncmp(line, "activewindow>>", 14) == 0) {
                    int prev_idx = matched_idx;
                    matched_idx = match_pattern_idx(line + 14, pats, npatterns);
                    /* Re-detect boundary when the focused pattern changes and no
                     * explicit boundary was given — different patterns may be on
                     * different monitors. */
                    if (!boundary_explicit && matched_idx >= 0
                            && matched_idx != prev_idx) {
                        boundary_x = get_active_window_boundary(socket_dir);
                    }
                }
                if (nl) line = nl + 1; else break;
            }
        }

        /* ── State transitions ── */
        if (matched_idx < 0 && state != STATE_IDLE) {
            if (state == STATE_GUARDING) ungrab_mice(mice, nmice);
            state = STATE_IDLE;
        }
        else if (matched_idx >= 0 && state == STATE_IDLE) {
            /* Auto-detect boundary on focus if not explicitly set */
            if (!boundary_explicit) {
                boundary_x = get_active_window_boundary(socket_dir);
            }
            cursor_x = get_cursor_x(socket_dir);
            if (cursor_x < 0) cursor_x = boundary_x >= 0 ? boundary_x + 500 : 9999;
            state = STATE_WATCHING;
        }

        /* ── IDLE: drain mouse events so poll doesn't spin ── */
        if (state == STATE_IDLE) {
            for (int p = 1; p < npfds; p++) {
                if (!(pfds[p].revents & POLLIN)) continue;
                int mi = pfd_mouse[p];
                struct input_event ev;
                while (libevdev_next_event(mice[mi].dev,
                        LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS)
                    ; /* discard */
            }
        }

        /* ── WATCHING ── */
        if (state == STATE_WATCHING) {
            /* Read mouse events without grabbing — track button state and
             * check cursor position only when the mouse actually moved. */
            int mouse_moved = 0;
            for (int p = 1; p < npfds; p++) {
                if (!(pfds[p].revents & POLLIN)) continue;
                int mi = pfd_mouse[p];
                struct input_event ev;
                while (libevdev_next_event(mice[mi].dev,
                        LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
                    if (ev.type == EV_KEY && ev.code >= BTN_LEFT && ev.code <= BTN_TASK) {
                        if (ev.value == 1) buttons_held++;
                        else if (ev.value == 0 && buttons_held > 0) buttons_held--;
                    }
                    if (ev.type == EV_REL)
                        mouse_moved = 1;
                }
            }

            if (mouse_moved && boundary_x >= 0) {
                cursor_x = get_cursor_x(socket_dir);
                /* Grab only when near boundary AND no buttons held */
                if (cursor_x >= 0 && cursor_x < boundary_x + buffer && buttons_held == 0) {
                    grab_mice(mice, nmice);
                    state = STATE_GUARDING;
                }
            }
        }

        /* ── GUARDING ── */
        if (state == STATE_GUARDING) {
            for (int p = 1; p < npfds; p++) {
                int mi = pfd_mouse[p];
                if (!mice[mi].grabbed) continue;

                struct input_event ev;
                while (libevdev_next_event(mice[mi].dev,
                        LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {

                    /* Track button state */
                    if (ev.type == EV_KEY && ev.code >= BTN_LEFT && ev.code <= BTN_TASK) {
                        if (ev.value == 1) buttons_held++;
                        else if (ev.value == 0 && buttons_held > 0) buttons_held--;
                    }

                    /* Enforce hard wall on leftward movement */
                    if (ev.type == EV_REL && ev.code == REL_X && ev.value < 0) {
                        if (cursor_x <= boundary_x) {
                            continue; /* at wall, drop event */
                        }
                        int space = cursor_x - boundary_x;
                        if (-ev.value > space) ev.value = -space;
                        cursor_x += ev.value;
                    } else if (ev.type == EV_REL && ev.code == REL_X) {
                        cursor_x += ev.value;
                    }

                    libevdev_uinput_write_event(mice[mi].uidev, ev.type, ev.code, ev.value);
                }
            }

            /* Sync cursor_x from Hyprland periodically to correct drift.
             * With the 100ms poll timeout this runs ~10 times/sec. */
            {
                int real_x = get_cursor_x(socket_dir);
                if (real_x >= 0) cursor_x = real_x;
            }

            /* Return to WATCHING once cursor is safely away from the edge.
             * Only ungrab when no buttons are held — ungrabbing while a button
             * is pressed causes the game to see the press on the virtual device
             * but the release on the real device, which sticks buttons down. */
            if (cursor_x > boundary_x + buffer + GRAB_EXIT_EXTRA && buttons_held == 0) {
                ungrab_mice(mice, nmice);
                state = STATE_WATCHING;
            }
        }
    }

    ungrab_mice(mice, nmice);
    cleanup_mice(mice, nmice);
    close(evfd);
    return 0;
}
