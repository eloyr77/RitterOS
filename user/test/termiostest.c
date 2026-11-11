/*
 * Test: termios (52e, SYS_TERMIOS). Lines the suite looks for:
 * "termiostest: NAME ok" or "termiostest: FAIL ...".
 *
 *   termiostest          the pty half, then the console half
 *   termiostest show     the console's modes now (after the test: as before)
 *
 * The pty half runs "termiostest child" in a session of its own on a
 * pseudo-terminal's slave and drives it through the master: the defaults,
 * VMIN 0 (no wait, and VTIME's), VMIN 3 (waits for three bytes typed in
 * two goes), ISIG off (Ctrl-C a byte), ICRNL (Enter read as \n), lines
 * again, and the modes back once it ended. The console half asks the test
 * to type: two keys a moment apart (VMIN 2), then Ctrl-C with ISIG off.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include "ritteros.h"

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif

static int failed;

static void result(const char *name, int ok)
{
    if (ok)
        printf("termiostest: %s ok\n", name);
    else {
        printf("termiostest: FAIL %s (errno %d)\n", name, errno);
        failed = 1;
    }
}

static long ms_since(uint64_t start)
{
    return (long)((ritteros_time() - start) / 1000000);
}

static int is_default(const struct termios *t)
{
    return (t->c_lflag & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG) &&
           t->c_cc[VMIN] == 1 && t->c_cc[VINTR] == 3 && t->c_cc[VEOF] == 4;
}

/* --- the child, on the pty slave: its lines go back through the master --- */

static void say(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
}

static int child(void)
{
    struct termios t, saved;
    char buf[16];
    say(tcgetattr(0, &saved) == 0 && is_default(&saved) ? "C defaults ok" : "C defaults bad");
    t = saved;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    uint64_t start = ritteros_time();
    ssize_t n = read(0, buf, sizeof(buf));
    say(n == 0 && ms_since(start) < 200 ? "C vmin0 ok" : "C vmin0 bad");
    t.c_cc[VTIME] = 3;
    tcsetattr(0, TCSANOW, &t);
    start = ritteros_time();
    n = read(0, buf, sizeof(buf));
    long took = ms_since(start);
    say(n == 0 && took >= 250 && took < 2000 ? "C vtime ok" : "C vtime bad");
    t.c_cc[VMIN] = 3;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    say("C ready3");
    n = read(0, buf, sizeof(buf));
    say(n == 3 && !memcmp(buf, "xyz", 3) ? "C vmin3 ok" : "C vmin3 bad");
    t.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &t);
    say("C readyc");
    n = read(0, buf, sizeof(buf));
    say(n == 1 && buf[0] == 3 ? "C isig ok" : "C isig bad");
    t.c_iflag |= ICRNL;
    tcsetattr(0, TCSANOW, &t);
    say("C readycr");
    n = read(0, buf, sizeof(buf));
    say(n == 1 && buf[0] == '\n' ? "C icrnl ok" : "C icrnl bad");
    tcsetattr(0, TCSANOW, &saved);
    say("C readyline");
    n = read(0, buf, sizeof(buf));
    say(n == 6 && !memcmp(buf, "hello\n", 6) ? "C lines ok" : "C lines bad");
    cfmakeraw(&t);
    tcsetattr(0, TCSANOW, &t);                  /* (left raw: the end puts it back) */
    say("C done");
    return 0;
}

/* --- the parent, on the master --- */

static char seen[4096];
static size_t seen_length;

/* Read the master until text has come (or 5 s); true if it came. */
static int wait_for(int master, const char *text)
{
    uint64_t start = ritteros_time();
    while (!strstr(seen, text) && ms_since(start) < 5000) {
        char b[256];
        ssize_t n = read(master, b, sizeof(b));
        if (n <= 0)
            break;
        for (ssize_t i = 0; i < n && seen_length + 1 < sizeof(seen); i++)
            if (b[i] != '\r')
                seen[seen_length++] = b[i];
        seen[seen_length] = 0;
    }
    return strstr(seen, text) != 0;
}

static void pty_half(void)
{
    int fds[2];
    if (ritteros_pty_open(fds) < 0) {
        result("a pty", 0);
        return;
    }
    char *argv[] = { "termiostest", "child", 0 };
    int slave[3] = { fds[1], fds[1], fds[1] };
    int pid = ritteros_spawn("/system/" ARCH "/test/termiostest", argv,
                             SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, slave);
    close(fds[1]);
    int m = fds[0];
    wait_for(m, "C vtime");
    wait_for(m, "C ready3");
    write(m, "x", 1);
    usleep(200000);
    write(m, "yz", 2);
    wait_for(m, "C readyc");
    write(m, "\003", 1);
    wait_for(m, "C readycr");
    write(m, "\r", 1);
    wait_for(m, "C readyline");
    write(m, "hello\r", 6);
    wait_for(m, "C done");
    int status = -1;
    waitpid(pid, &status, 0);
    static const char *const names[] = { "defaults", "vmin0", "vtime", "vmin3", "isig", "icrnl",
                                         "lines" };
    for (int i = 0; i < 7; i++) {
        char want[32], name[32];
        snprintf(want, sizeof(want), "C %s ok", names[i]);
        snprintf(name, sizeof(name), "pty %s", names[i]);
        result(name, strstr(seen, want) != 0);
    }
    struct termios t;
    result("pty restored at the end", tcgetattr(m, &t) == 0 && is_default(&t));
    close(m);
}

/* --- the console half --- */

static void console_half(void)
{
    struct termios saved, t;
    int ok = tcgetattr(0, &saved) == 0 && is_default(&saved);
    int file = open("/readme.txt", O_RDONLY);
    errno = 0;
    ok &= tcgetattr(file, &t) == -1 && errno == ENOTTY;
    close(file);
    ok &= tcgetattr(9, &t) == -1 && errno == EBADF;
    result("console defaults and errors", ok);
    t = saved;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 2;
    tcsetattr(0, TCSAFLUSH, &t);
    printf("termiostest: type two keys\n");
    char buf[8];
    uint64_t start = ritteros_time();
    ssize_t n = read(0, buf, sizeof(buf));
    long took = ms_since(start);
    result("console vmin2", n == 2 && buf[0] == 'a' && buf[1] == 'b' && took >= 300);
    t.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &t);
    printf("termiostest: type Ctrl-C\n");
    n = read(0, buf, sizeof(buf));
    result("console isig off", n >= 1 && buf[0] == 3);
    /* (left raw on purpose: its end puts the console back) */
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "child"))
        return child();
    if (argc == 2 && !strcmp(argv[1], "show")) {
        struct termios t;
        tcgetattr(0, &t);
        printf("termiostest: the console has %s, %s, %s\n", t.c_lflag & ICANON ? "lines" : "raw input",
               t.c_lflag & ECHO ? "echo" : "no echo", t.c_lflag & ISIG ? "Ctrl-C" : "no Ctrl-C");
        return 0;
    }
    pty_half();
    console_half();
    printf(failed ? "termiostest: FAIL (see above)\n" : "termiostest: all ok\n");
    return failed;
}
