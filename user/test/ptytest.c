/*
 * Test: pseudo-terminals (step 40b).
 *   ptytest [-hangup] LINE...   start the shell on a new pseudo-terminal
 *                               (30 rows, 100 columns), type each LINE
 *                               after the next prompt ("^C" alone: Ctrl-C
 *                               a moment after the line before; "^D":
 *                               Ctrl-D; ^H in a line: Backspace), then
 *                               print everything the terminal showed and
 *                               how the shell ended. -hangup: close the
 *                               master after the last line instead of
 *                               waiting for the shell to end.
 *   ptytest size                print the terminal's size (run inside)
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <thread.h>
#include <unistd.h>
#include "ritteros.h"

static char shown[65536];
static volatile size_t shown_length;
static volatile bool ended;
static int master;

static int reader(void *arg)
{
    (void)arg;
    for (;;) {
        char buf[512];
        ssize_t n = read(master, buf, sizeof(buf));
        if (n <= 0)
            break;
        size_t at = shown_length;
        if (at + (size_t)n > sizeof(shown))
            n = (ssize_t)(sizeof(shown) - at);
        memcpy(shown + at, buf, (size_t)n);
        __atomic_store_n(&shown_length, at + (size_t)n, __ATOMIC_SEQ_CST);
    }
    ended = true;
    return 0;
}

/* Wait (at most 10 s) until the output ends in a prompt ("$ ") after
 * position from. */
static size_t wait_prompt(size_t from)
{
    for (int i = 0; i < 1000 && !ended; i++) {
        size_t n = shown_length;
        if (n >= from + 2 && shown[n - 2] == '$' && shown[n - 1] == ' ')
            return n;
        usleep(10000);
    }
    return shown_length;
}

static void type(const char *s)
{
    char buf[256];
    size_t n = 0;
    for (; *s && n < sizeof(buf) - 1; s++) {
        if (s[0] == '^' && s[1] == 'H') {
            buf[n++] = 0x7F;
            s++;
        } else {
            buf[n++] = *s;
        }
    }
    write(master, buf, n);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "size") == 0) {
        int rows, cols;
        if (ritteros_terminal_size(&rows, &cols) < 0)
            printf("ptytest: size: %s\n", strerror(errno));
        else
            printf("ptytest: size %dx%d\n", rows, cols);
        return 0;
    }
    int first = 1;
    bool hangup = argc > 1 && strcmp(argv[1], "-hangup") == 0;
    if (hangup)
        first = 2;
    int fds[2];
    if (ritteros_pty_open(fds) < 0) {
        printf("ptytest: %s\n", strerror(errno));
        return 1;
    }
    master = fds[0];
    ritteros_pty_size(master, 30, 100);
    char path[64];
    snprintf(path, sizeof(path), "/system/%s/bin/sh",
#if defined(__x86_64__)
             "x86_64"
#else
             "aarch64"
#endif
    );
    char *shell_argv[] = { "sh", 0 };
    int slave[3] = { fds[1], fds[1], fds[1] };
    int pid = ritteros_spawn(path, shell_argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, slave);
    close(fds[1]);                                  /* the shell has it now */
    if (pid < 0) {
        printf("ptytest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int t = thread_create(reader, 0);
    size_t seen = wait_prompt(0);
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "^C") == 0) {
            usleep(300000);
            type("\003");
            seen = wait_prompt(seen);
            continue;
        }
        if (strcmp(argv[i], "^D") == 0) {
            type("\004");
            continue;
        }
        type(argv[i]);
        type("\r");
        if ((i + 1 < argc && strcmp(argv[i + 1], "^C") == 0) || (hangup && i + 1 == argc))
            continue;                               /* it is still running */
        seen = wait_prompt(seen + 1);
    }
    if (hangup) {
        usleep(300000);
        close(master);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!hangup)
        thread_join(t, 0);
    /* The transcript: each line after "| ", CR LF as a line break, other
     * control bytes named, '$' as '#' (the test counts the console's
     * prompts). */
    size_t bare = 0, crlf = 0;
    printf("| ");
    for (size_t i = 0; i < shown_length; i++) {
        unsigned char c = (unsigned char)shown[i];
        if (c == '\r' && i + 1 < shown_length && shown[i + 1] == '\n') {
            printf("\n| ");
            crlf++;
            i++;
        } else if (c == '\n') {
            printf("<LF>\n| ");
            bare++;
        } else if (c == '$') {
            putchar('#');
        } else if (c == '\b') {
            printf("<BS>");
        } else if (c == 0x1B) {
            printf("<ESC>");
        } else if (c < 0x20) {
            printf("<%02x>", c);
        } else {
            putchar(c);
        }
    }
    printf("\nptytest: %zu line breaks, %zu without CR\n", crlf, bare);
    if (WIFEXITED(status))
        printf("ptytest: the shell ended with exit code %d\n", WEXITSTATUS(status));
    else
        printf("ptytest: the shell ended by signal %d\n", WTERMSIG(status));
    return 0;
}
