/*
 * wsend: keyboard and pointer input for a headless windowd (41h-3), for
 * testing programs without a screen. Each event is a WMSG_INPUT, which
 * windowd handles as one from a mouse or keyboard (focus, menus, the
 * window under the pointer) and answers before the next: when wsend
 * returns, everything it sent has been handled. A windowd on the screen
 * refuses it (its input is the machine's keyboard and mouse).
 *
 *   wsend move X Y              the pointer to (X, Y)
 *   wsend click X Y             there, the left button pressed and let go
 *   wsend right X Y             the same with the right button
 *   wsend drag X1 Y1 X2 Y2      pressed at the first point, moved to the
 *                               second in steps of at most 16 pixels, let go
 *   wsend type TEXT             each byte of TEXT as a key's byte (what
 *                               programs read as typed text)
 *   wsend key NAME              a key as the keyboard sends one: the
 *                               physical key pressed (its USB usage), its
 *                               bytes, the key let go
 *   wsend down NAME             (41h-5) only the press and its bytes: the key
 *   wsend up NAME               held until its `up` (games: walking, firing)
 *   wsend wait [TITLE]          (41h-6) until the windowd answers, or until a
 *                               window of that title is open (WMSG_WINDOWS);
 *                               at most 10 s (exit 1)
 *
 * Key names: a-z, 0-9, space, enter, tab, escape, backspace, up, down,
 * left, right, home, end, pageup, pagedown, insert, delete, ctrl, shift,
 * alt (the left ones: no bytes), f1-f12 (no bytes). A key's bytes are the
 * unmodified key's (a held ctrl does not make `a` a control byte); a
 * program reading physical keys sees ctrl held all the same.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"
#include "windowproto.h"

PROGRAM_HELP("wsend click|right|move X Y | drag X1 Y1 X2 Y2 | type TEXT | key|down|up NAME | wait [TITLE]",
             "send keyboard and pointer input to a headless windowd (for tests)");

static int sock = -1;

static int read_full(int fd, void *buf, size_t n)
{
    for (size_t done = 0; done < n;) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

/* One event, answered: 0, or 1 after saying why it was not taken. */
static int send_event(int type, int buttons, int key, int x, int y)
{
    struct wmsg m = { WMSG_INPUT, (uint32_t)type | (uint32_t)buttons << 8 | (uint32_t)key << 16,
                      (uint32_t)x | (uint32_t)y << 16, 0 };
    if (write(sock, &m, sizeof(m)) != (ssize_t)sizeof(m) || read_full(sock, &m, sizeof(m)) < 0 ||
        m.type != WMSG_INPUT) {
        dprintf(2, "wsend: the window server stopped answering\n");
        return 1;
    }
    if (!m.a) {
        dprintf(2, "wsend: refused: only a headless windowd takes input\n");
        return 1;
    }
    return 0;
}

static int pointer(int buttons, int x, int y)
{
    return send_event(INPUT_POINTER, buttons, 0, x, y);
}

static int keys(const char *bytes)
{
    for (const char *c = bytes; *c; c++)
        if (send_event(INPUT_KEY, 0, (uint8_t)*c, 0, 0))
            return 1;
    return 0;
}

static int number(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0 || v > 65535)
        return 0;
    *out = (int)v;
    return 1;
}

static const struct { const char *name, *bytes; uint8_t usage; } key_names[] = {
    { "enter", "\r", 0x28 }, { "tab", "\t", 0x2B }, { "escape", "\033", 0x29 },
    { "backspace", "\177", 0x2A }, { "up", "\033[A", 0x52 }, { "down", "\033[B", 0x51 },
    { "right", "\033[C", 0x4F }, { "left", "\033[D", 0x50 }, { "space", " ", 0x2C },
    { "home", "\033[H", 0x4A }, { "end", "\033[F", 0x4D }, { "insert", "\033[2~", 0x49 },
    { "delete", "\033[3~", 0x4C }, { "pageup", "\033[5~", 0x4B }, { "pagedown", "\033[6~", 0x4E },
    { "ctrl", "", 0xE0 }, { "shift", "", 0xE1 }, { "alt", "", 0xE2 },     /* (41h-5) */
};

/* (41h-5) A key by its name: its USB usage into *code, its bytes (as the
 * keyboard sends the key alone) into *bytes; false if no key has it. */
static bool find_key(const char *name, int *code, const char **bytes)
{
    static char one[2];
    for (size_t i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++)
        if (!strcmp(name, key_names[i].name)) {
            *code = key_names[i].usage, *bytes = key_names[i].bytes;
            return true;
        }
    if (name[0] >= 'a' && name[0] <= 'z' && !name[1]) {           /* a letter */
        one[0] = name[0];
        *code = 0x04 + (name[0] - 'a'), *bytes = one;
        return true;
    }
    if (name[0] >= '0' && name[0] <= '9' && !name[1]) {           /* a digit: 1-9, then 0 */
        one[0] = name[0];
        *code = name[0] == '0' ? 0x27 : 0x1E + (name[0] - '1'), *bytes = one;
        return true;
    }
    int f;
    if (name[0] == 'f' && number(name + 1, &f) && f >= 1 && f <= 12) {   /* f1-f12 */
        *code = 0x3A + f - 1, *bytes = "";
        return true;
    }
    return false;
}

static int usage(void)
{
    dprintf(2, "usage: wsend click|right|move X Y | drag X1 Y1 X2 Y2 | type TEXT | key|down|up NAME | "
               "wait [TITLE]\n");
    return 2;
}

/* A connection to the windowd at `at`, its user's or root's; -1 (errno). */
static int reach(const char *at)
{
    int fd = ritteros_local_connect(at);
    if (fd >= 0) {                                  /* (15k) its user's or root's */
        int uid = ritteros_peer_uid(fd);
        if (uid != (int)getuid() && uid != 0) {
            close(fd);
            errno = EACCES;
            return -1;
        }
    }
    return fd;
}

/* (41h-6) Whether a window titled `title` is open there (WMSG_WINDOWS). */
static bool has_window(int fd, const char *title)
{
    struct wmsg m = { WMSG_WINDOWS, 0, 0, 0 };
    static char text[8192];
    if (write(fd, &m, sizeof(m)) != (ssize_t)sizeof(m) || read_full(fd, &m, sizeof(m)) < 0 ||
        m.type != WMSG_WINDOWS || m.c >= sizeof(text) || (m.c && read_full(fd, text, m.c) < 0))
        return false;
    text[m.c] = 0;
    size_t n = strlen(title);
    for (const char *line = text; *line;) {         /* the titles, one per line */
        const char *end = strchr(line, '\n');
        if (!end)
            break;
        if ((size_t)(end - line) == n && !strncmp(line, title, n))
            return true;
        line = end + 1;
    }
    return false;
}

/* (41h-6) Until the windowd answers (and with a title, until that window
 * is open), every 100 ms for 10 s. */
static int wait_for(const char *at, const char *title)
{
    for (int i = 0; i < 100; i++) {
        int fd = reach(at);
        if (fd >= 0) {
            bool ok = !title || has_window(fd, title);
            close(fd);
            if (ok)
                return 0;
        }
        usleep(100000);
    }
    if (title)
        dprintf(2, "wsend: no window \"%s\" at %s (waited 10 s)\n", title, at);
    else
        dprintf(2, "wsend: no window server at %s (waited 10 s)\n", at);
    return 1;
}

int main(int argc, char **argv)
{
    int p[4];
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "wait") && argc <= 3) {        /* (41h-6) */
        const char *at = getenv("WINDOW_SOCKET");
        return wait_for(at && *at ? at : WINDOW_SOCKET, argc == 3 ? argv[2] : 0);
    }
    const char *bytes = 0;
    int code = -1;                                  /* a named key's: pressed around its bytes */
    bool pointing = !strcmp(cmd, "click") || !strcmp(cmd, "right") || !strcmp(cmd, "move");
    if (pointing ? argc != 4 || !number(argv[2], &p[0]) || !number(argv[3], &p[1])
        : !strcmp(cmd, "drag") ? argc != 6 || !number(argv[2], &p[0]) || !number(argv[3], &p[1]) ||
                                     !number(argv[4], &p[2]) || !number(argv[5], &p[3])
        : !strcmp(cmd, "type") ? argc != 3
        : !strcmp(cmd, "key") || !strcmp(cmd, "down") || !strcmp(cmd, "up") ? argc != 3
        : 1)
        return usage();
    if (!strcmp(cmd, "type"))
        bytes = argv[2];
    else if (!pointing && strcmp(cmd, "drag") && !find_key(argv[2], &code, &bytes)) {   /* key, down, up */
        dprintf(2, "wsend: no key %s (a-z, 0-9, space, enter, tab, escape, backspace, up, down, "
                   "left, right, home, end, pageup, pagedown, insert, delete, ctrl, shift, alt, "
                   "f1-f12)\n", argv[2]);
        return 2;
    }
    const char *at = getenv("WINDOW_SOCKET");
    if (!at || !*at)
        at = WINDOW_SOCKET;
    sock = reach(at);
    if (sock < 0) {
        dprintf(2, "wsend: no window server at %s: %s\n", at, strerror(errno));
        return 1;
    }
    int r;
    if (code >= 0 && !strcmp(cmd, "down"))         /* (41h-5) held until its up */
        r = send_event(INPUT_KEYCODE, 1, code, 0, 0) || keys(bytes);
    else if (code >= 0 && !strcmp(cmd, "up"))
        r = send_event(INPUT_KEYCODE, 0, code, 0, 0);
    else if (code >= 0)
        r = send_event(INPUT_KEYCODE, 1, code, 0, 0) || keys(bytes) ||
            send_event(INPUT_KEYCODE, 0, code, 0, 0);
    else if (bytes)
        r = keys(bytes);
    else if (!strcmp(cmd, "move"))
        r = pointer(0, p[0], p[1]);
    else if (pointing) {                            /* click, right: there, down, up */
        int b = cmd[0] == 'c' ? MOUSE_LEFT : MOUSE_RIGHT;
        r = pointer(0, p[0], p[1]) || pointer(b, p[0], p[1]) || pointer(0, p[0], p[1]);
    } else {                                        /* drag */
        r = pointer(0, p[0], p[1]) || pointer(MOUSE_LEFT, p[0], p[1]);
        int dx = p[2] - p[0], dy = p[3] - p[1];
        int steps = (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) / 16 + 1;
        for (int i = 1; i <= steps && !r; i++)
            r = pointer(MOUSE_LEFT, p[0] + dx * i / steps, p[1] + dy * i / steps);
        r = r || pointer(0, p[2], p[3]);
    }
    close(sock);
    return r;
}
