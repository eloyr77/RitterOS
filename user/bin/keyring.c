/*
 * keyring add|get|remove SERVICE ACCOUNT | keyring list|lock|unlock|password |
 * keyring allow PROGRAM SERVICE (70a, item 48; 70b through keyringd; 70c
 * allow): your passwords, kept encrypted in
 * ~/.config/keyring.db (docs/keyring.md). The elf keyringd holds the
 * keyring open (login opens it with your password), and these commands
 * ask it through libkeyring, as any program does: add asks the secret and
 * keeps it (a new entry, or the entry's new secret), get prints it, list
 * prints each entry's service and account (never a secret), remove takes
 * it out. lock makes keyringd forget the keyring's key; unlock opens it
 * with its password (starting keyringd when it does not run: over SSH,
 * or with open_at_login = no); password changes the keyring's password.
 * (70c) keyring is a program as any other: an entry another program
 * stored is asked about first (keyringd's window); allow lets a program
 * (a path, or a name in /system's bin or apps) use a service's entries,
 * given the keyring's password. (70d) -s: the system keyring (root's
 * programs', keyringd -s; add, get, remove and list). Asked on a terminal, what is typed is not shown, and a new password or
 * secret is asked twice; from a pipe the lines are read in the same order
 * without prompts, each once.
 */
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <time.h>
#include "keyring.h"
#include "../lib/keyring_wire.h"

PROGRAM_HELP("keyring [-s] add|get|remove SERVICE ACCOUNT | keyring [-s] list | "
             "keyring lock|unlock|password | keyring allow PROGRAM SERVICE",
             "your passwords, kept encrypted (~/.config/keyring.db)");

static bool terminal;                   /* standard input is one: prompts, no echo */
static bool system_keyring;              /* (70d) -s */

/* A line typed without echo, without its line break; false at the end of
 * the input. A byte at a time, so the next line stays for the next
 * question (as passwd). */
static bool ask(const char *prompt, char *out, size_t size)
{
    if (terminal) {
        printf("%s", prompt);
        ritteros_echo(false);
    }
    size_t n = 0;
    bool any = false;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        any = true;
        if (c == '\n')
            break;
        if (n < size - 1)
            out[n++] = c;
    }
    if (terminal) {
        ritteros_echo(true);
        printf("\n");
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return any;
}

/* A new password or secret: twice on a terminal (what is typed is not
 * seen), once from a pipe. */
static bool ask_new(const char *prompt, char *out, size_t size)
{
    if (!ask(prompt, out, size))
        return false;
    if (!terminal)
        return true;
    char again[KEYRING_SECRET_MAX + 2];
    bool same = ask("Again: ", again, sizeof(again)) && !strcmp(out, again);
    explicit_bzero(again, sizeof(again));
    if (!same)
        dprintf(STDERR_FILENO, "keyring: the two differ; nothing changed\n");
    return same;
}

static int fail(const char *why)
{
    dprintf(STDERR_FILENO, "keyring: %s\n", why);
    return 1;
}

/* What a failed call's errno means here. */
static int failed(const char *service, const char *account)
{
    int e = errno;
    if (e == ENOENT)
        dprintf(STDERR_FILENO, "keyring: %s %s: not in the keyring\n", service, account);
    else if (e == EACCES && system_keyring)
        return fail("the system keyring is root's (or it did not open: the log keyring says why)");
    else if (e == EACCES)
        return fail("locked (keyring unlock opens it)");
    else if (e == ECONNREFUSED && system_keyring)
        return fail("the system keyring's keyringd does not run (elf start keyringd)");
    else if (e == ECONNREFUSED)
        return fail("not open: keyringd does not run (keyring unlock starts it)");
    else if (e == EPERM)
        dprintf(STDERR_FILENO, "keyring: %s %s: not allowed to this program (keyring allow PROGRAM "
                               "SERVICE)\n", service, account);
    else if (e == EADDRINUSE)
        return fail("another user holds keyringd's name (/run/keyring/UID)");
    else
        dprintf(STDERR_FILENO, "keyring: %s\n", strerror(e));
    return 1;
}

static void print_entry(const char *service, const char *account, void *arg)
{
    (void)arg;
    printf("%s\t%s\n", service, account);
}

/* A connection to keyringd, started first when it does not run (as this
 * user, in a session of its own: the terminal's signals do not reach it). */
static int connect_starting(void)
{
    unsigned uid = (unsigned)getuid();
    int fd = kw_connect(uid);
    if (fd >= 0 || errno != ECONNREFUSED)
        return fd;
    char *argv[] = { "keyringd", 0 };
    /* Nothing of ours: over SSH an open output would hold the session. */
    int null = open("/dev/null", O_RDWR);
    if (null < 0)
        return -1;
    int fds[3] = { null, null, null };
    char program[64];
    snprintf(program, sizeof(program), "/system/%s/bin/keyringd",
#if defined(__x86_64__)
             "x86_64"
#else
             "aarch64"
#endif
    );
    int pid = ritteros_spawn(program, argv, SPAWN_NEW_SESSION, fds);
    close(null);
    if (pid < 0)
        return -1;
    for (int i = 0; i < 100; i++) {             /* up to 5 s for it to listen */
        struct timespec t = { 0, 50000000 };
        nanosleep(&t, 0);
        if ((fd = kw_connect(uid)) >= 0 || errno != ECONNREFUSED)
            return fd;
    }
    return -1;
}

/* One request of the lock, unlock and password commands over fd. */
static struct kw_message m;                     /* ask_keyringd's answer */

static int ask_keyringd(int fd, uint8_t code, const struct kw_field *f, int count)
{
    if (!kw_ask(fd, code, f, count, &m)) {
        close(fd);
        return fail(strerror(errno));
    }
    close(fd);
    switch (m.code) {
    case KW_OK:
        return 0;
    case KW_WRONG:
        return fail("wrong password (or the file is damaged)");
    case KW_REFUSED:
        return fail("keyringd refused (not your keyring)");
    case KW_LOCKED:
        return fail("locked (keyring unlock opens it)");
    case KW_NOT_FOUND:
        return fail("no such entries (only entries a program stored name programs)");
    case KW_FAILED: {
        char text[16];
        int e = kw_text(&m, 0, text, sizeof(text)) ? atoi(text) : 0;
        dprintf(STDERR_FILENO, "keyring: not saved: %s\n", e > 0 ? strerror(e) : "keyringd failed");
        return 1;
    }
    default:
        return fail("keyringd refused the request");
    }
}

static int usage(void)
{
    dprintf(STDERR_FILENO, "usage: keyring [-s] add|get|remove SERVICE ACCOUNT | keyring [-s] list | "
                           "keyring lock|unlock|password | keyring allow PROGRAM SERVICE\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "-s")) {
        system_keyring = true;
        argc--, argv++;
    }
    if (argc < 2)
        return usage();
    const char *command = argv[1];
    bool named = !strcmp(command, "add") || !strcmp(command, "get") || !strcmp(command, "remove") ||
                 !strcmp(command, "allow");
    bool alone = !strcmp(command, "list") || !strcmp(command, "lock") ||
                 !strcmp(command, "unlock") || !strcmp(command, "password");
    if (named ? argc != 4 : !alone || argc != 2)
        return usage();
    const char *service = named ? argv[2] : "", *account = named ? argv[3] : "";
    terminal = isatty(STDIN_FILENO);
    if (system_keyring) {
        if (strcmp(command, "add") && strcmp(command, "get") && strcmp(command, "remove") &&
            strcmp(command, "list"))
            return fail("the system keyring has no password: -s takes add, get, remove and list");
        keyring_system();
    }

    if (!strcmp(command, "allow")) {            /* (70c) keyring allow PROGRAM SERVICE */
        const char *program = argv[2];
        service = argv[3];
        char found[PATH_MAX], real[PATH_MAX];
        struct stat st;
        const char *arch =
#if defined(__x86_64__)
            "x86_64";
#else
            "aarch64";
#endif
        if (strchr(program, '/'))
            snprintf(found, sizeof(found), "%s", program);
        else if (snprintf(found, sizeof(found), "/system/%s/bin/%s", arch, program) > 0 &&
                 stat(found, &st) < 0)
            snprintf(found, sizeof(found), "/system/%s/apps/%s.program/%s", arch, program, program);
        if (stat(found, &st) < 0 || !S_ISREG(st.st_mode) || !realpath(found, real) ||
            strlen(real) >= PROGRAM_PATH_MAX) {
            dprintf(STDERR_FILENO, "keyring: %s: no such program\n", program);
            return 1;
        }
        int fd = kw_connect((unsigned)getuid());
        if (fd < 0)
            return failed(service, "");
        char password[256];
        if (!ask("Keyring password: ", password, sizeof(password))) {
            close(fd);
            return 1;
        }
        struct kw_field f[3] = { { password, (uint32_t)strlen(password) },
                                 { real, (uint32_t)strlen(real) },
                                 { service, (uint32_t)strlen(service) } };
        int status = ask_keyringd(fd, KW_ALLOW, f, 3);
        explicit_bzero(password, sizeof(password));
        if (!status) {
            char n[16] = "?";
            kw_text(&m, 0, n, sizeof(n));
            printf("keyring: %s may use %s (%s %s)\n", real, service, n,
                   !strcmp(n, "1") ? "entry" : "entries");
        }
        return status;
    }

    if (!strcmp(command, "add")) {
        char secret[KEYRING_SECRET_MAX + 2], prompt[600];
        snprintf(prompt, sizeof(prompt), "Secret for %s %s: ", service, account);
        if (!ask_new(prompt, secret, sizeof(secret)))
            return 1;
        int status = 0;
        if (strlen(secret) > KEYRING_SECRET_MAX) {
            status = fail("a secret is at most 4096 bytes");
        } else if (keyring_set(service, account, secret, strlen(secret)) < 0) {
            status = errno == EINVAL
                         ? fail("a service or an account is 1-255 bytes without control characters")
                         : failed(service, account);
        } else {
            printf("keyring: %s %s kept\n", service, account);
        }
        explicit_bzero(secret, sizeof(secret));
        return status;
    }
    if (!strcmp(command, "get")) {
        char secret[KEYRING_SECRET_MAX];
        ssize_t n = keyring_get(service, account, secret, sizeof(secret));
        if (n < 0)
            return errno == EINVAL
                       ? fail("a service or an account is 1-255 bytes without control characters")
                       : failed(service, account);
        size_t shown = (size_t)n < sizeof(secret) ? (size_t)n : sizeof(secret);
        bool ok = write(STDOUT_FILENO, secret, shown) == (ssize_t)shown &&
                  write(STDOUT_FILENO, "\n", 1) == 1;
        explicit_bzero(secret, sizeof(secret));
        return ok ? 0 : 1;
    }
    if (!strcmp(command, "remove")) {
        if (keyring_remove(service, account) < 0)
            return errno == EINVAL
                       ? fail("a service or an account is 1-255 bytes without control characters")
                       : failed(service, account);
        printf("keyring: %s %s removed\n", service, account);
        return 0;
    }
    if (!strcmp(command, "list")) {
        int n = keyring_list(print_entry, 0);
        if (n < 0)
            return failed("", "");
        if (n == 0)
            printf("keyring: no entries\n");
        return 0;
    }
    if (!strcmp(command, "lock")) {
        int fd = kw_connect((unsigned)getuid());
        if (fd < 0) {
            if (errno != ECONNREFUSED)
                return failed("", "");
            printf("keyring: locked (keyringd does not run)\n");
            return 0;
        }
        if (ask_keyringd(fd, KW_LOCK, 0, 0))
            return 1;
        printf("keyring: locked\n");
        return 0;
    }
    int fd = !strcmp(command, "unlock") ? connect_starting() : kw_connect((unsigned)getuid());
    if (fd < 0)
        return failed("", "");
    char password[256], fresh[256];
    if (!ask("Keyring password: ", password, sizeof(password))) {
        close(fd);
        return 1;
    }
    int status;
    if (!strcmp(command, "unlock")) {
        struct kw_field f = { password, (uint32_t)strlen(password) };
        status = ask_keyringd(fd, KW_UNLOCK, &f, 1);
        if (!status)
            printf("keyring: open\n");
    } else if (!ask_new("New keyring password: ", fresh, sizeof(fresh))) {
        close(fd);
        status = 1;
    } else if (!fresh[0]) {
        close(fd);
        status = fail("an empty password is not allowed");
    } else {
        struct kw_field f[2] = { { password, (uint32_t)strlen(password) },
                                 { fresh, (uint32_t)strlen(fresh) } };
        status = ask_keyringd(fd, KW_PASSWORD, f, 2);
        if (!status)
            printf("keyring: the password changed\n");
    }
    explicit_bzero(password, sizeof(password));
    explicit_bzero(fresh, sizeof(fresh));
    return status;
}
