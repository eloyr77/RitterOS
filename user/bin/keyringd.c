/*
 * keyringd (70b, item 48): the elf that keeps a user's keyring open, so
 * programs reach their secrets through libkeyring without a password.
 * One per user, running as that user, listening on /run/keyring/UID
 * (keyring_wire.h's messages). login starts it at a login (or finds it
 * running) and opens it with the login password (KW_UNLOCK), then holds a
 * session (KW_SESSION) while the user is logged in; `keyring unlock`
 * starts it when it does not run. It starts locked, and it locks (forgets
 * the keyring's key) when the last login session ends, at `keyring lock`,
 * and after lock_after minutes without use (~/.config/keyring); locked it
 * keeps running, for the next login. Only its user may ask it anything;
 * root (login, passwd) may only unlock, change the password, hold a
 * session and ask its state, each of which needs the password or tells
 * nothing. A keyring without a file is made by the first unlock and
 * written only when something is stored in it. keyringd writes the
 * keyring.db file alone (keyring_db.h).
 *
 * (70c) Programs are told apart by the file each runs (SYS_PEER_PROGRAM,
 * taken when it connects). An entry keeps the programs allowed it, the one
 * that stored it first; another program that asks for it, replaces or
 * removes it waits while keyringd asks the user: a child of keyringd opens
 * an Allow/Deny window through windowd (keyringd serves the others
 * meanwhile, one question at a time). Allow adds the program to the entry
 * for good; Deny is kept until the keyring locks (no second window for
 * it); without a window system, or unanswered in ASK_MS, it is refused
 * (KW_NOT_ALLOWED). `keyring allow PROGRAM SERVICE` (KW_ALLOW) adds a
 * program to a service's entries, given the keyring's password. Entries
 * stored before 70c name no program: every program may use them.
 *
 * (70d) keyringd -s keeps the system keyring, root's programs' (wifid's
 * passphrases...): /system/config/keyring.db, locked by the 32 random
 * bytes of /system/config/keyring.key (root's, 0600) instead of a
 * password, on /run/keyring/system. An elf (login makes its file); it opens
 * the keyring at its start, and both files are written only when the
 * first entry is stored. Only root's programs may use it, every one every
 * entry (root may read the key file anyway: no rule per program, no
 * window); it has no lock, unlock, password or allow.
 */
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include "keyring_db.h"
#include "settings_file.h"
#include "widget.h"
#include "window.h"
#include "../lib/keyring_wire.h"

PROGRAM_HELP("keyringd [-s]", "the elf that keeps your keyring open (login starts it); -s the system's");

#define CLIENTS     64
#define READ_MS     2000                /* a request's bytes, once they started */
#define ASK_MS      50000               /* a window's answer (the client waits KW_WAIT_MS) */
#define DENIED      16                  /* denials kept until the keyring locks */

static struct client {
    int fd;
    int uid;
    bool session;
    uint64_t id;                        /* (70c) this connection, never reused */
    char program[PROGRAM_PATH_MAX];     /* the program at its other end */
} clients[CLIENTS];
static int client_count;
static uint64_t next_id = 1;
static int listener = -1;

static struct keyring k;
static bool is_open, saved_once;        /* saved_once: keyring.db exists */
static int sessions;
static unsigned me;
static long lock_after;                 /* minutes, 0: never */
static uint64_t last_use;
static char path[512], settings[512];
static bool system_mode;                /* (70d) -s */
static uint8_t system_key[32];          /* its key file's bytes */
static bool key_written;                /* /system/config/keyring.key exists */
#define SYSTEM_KEYRING "/system/config/keyring.db"
#define SYSTEM_KEY     "/system/config/keyring.key"
static struct kw_message in;

/* (70c) The question being asked: whose request waits for it. */
static struct {
    bool active;
    pid_t pid;
    int pipe;
    uint64_t client;
    uint64_t started;
    struct kw_message *request;
} ask;

static struct denial {
    char program[PROGRAM_PATH_MAX];
    char service[KEYRING_NAME_MAX + 1], account[KEYRING_NAME_MAX + 1];
} denied[DENIED];
static int denied_count, denied_next;

static void note(const char *text)
{
    char line[600];
    snprintf(line, sizeof(line), "keyringd (%u): %s\n", me, text);
    ritteros_log_write("keyring", line);
}

static void lock_now(const char *why)
{
    if (!is_open)
        return;
    keyring_free(&k);
    is_open = false;
    denied_count = denied_next = 0;
    note(why);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void answer(struct client *c, uint8_t code, const struct kw_field *f, int count)
{
    kw_send(c->fd, code, f, count);
}

static void answer_errno(struct client *c, int e)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", e);
    struct kw_field f = { text, (uint32_t)strlen(text) };
    answer(c, KW_FAILED, &f, 1);
}

/* (70d) The system keyring's key file, written whole and committed
 * before the first save of the keyring it opens. */
static int write_key(void)
{
    int fd = open(SYSTEM_KEY ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    bool ok = write(fd, system_key, 32) == 32 && fsync(fd) == 0;
    int e = errno;
    close(fd);
    if (!ok || rename(SYSTEM_KEY ".new", SYSTEM_KEY) < 0) {
        e = ok ? errno : e;
        unlink(SYSTEM_KEY ".new");
        errno = e ? e : EIO;
        return -1;
    }
    key_written = true;
    note("wrote " SYSTEM_KEY);
    return 0;
}

static int save(void)
{
    if (system_mode && !key_written && write_key() < 0)
        return -1;
    if (keyring_save(path, &k) < 0)
        return -1;
    saved_once = true;
    return 0;
}

/* The entry's names from m's fields 0 and 1; false if either is not one. */
static bool names(const struct kw_message *m, char *service, char *account)
{
    return kw_text(m, 0, service, KEYRING_NAME_MAX + 1) && keyring_name_ok(service) &&
           kw_text(m, 1, account, KEYRING_NAME_MAX + 1) && keyring_name_ok(account);
}

/* Opens the keyring with a password: KW_OK, KW_WRONG or KW_FAILED. */
static int unlock_with(const char *password)
{
    if (is_open)
        return keyring_password_is(&k, password) ? KW_OK : KW_WRONG;
    const char *why = "";
    int r = keyring_load(path, password, &k, &why);
    if (r == 0) {                       /* none yet: a new one, written when used */
        if (!keyring_new(&k, password))
            return KW_FAILED;
        saved_once = false;
    } else if (r < 0) {
        char text[300];
        snprintf(text, sizeof(text), "not opened: %s", why);
        note(text);
        return strstr(why, "wrong password") ? KW_WRONG : KW_FAILED;
    } else {
        saved_once = true;
    }
    is_open = true;
    last_use = now_ns();
    note("opened");
    return KW_OK;
}

static bool was_denied(const char *program, const char *service, const char *account)
{
    for (int i = 0; i < denied_count; i++)
        if (!strcmp(denied[i].program, program) && !strcmp(denied[i].service, service) &&
            !strcmp(denied[i].account, account))
            return true;
    return false;
}

/* --- (70c) The question, in a child of keyringd ---------------------------------- */

static char choice;
static struct widget *ask_root;

static void chose_allow(struct widget *w, void *data)
{
    (void)w, (void)data;
    choice = '1';
    widget_quit(ask_root);
}

static void chose_deny(struct widget *w, void *data)
{
    (void)w, (void)data;
    choice = '0';
    widget_quit(ask_root);
}

/* The window, then one byte down out: '1' allowed, '0' denied (or closed),
 * 'n' no window system. Never returns. */
static _Noreturn void ask_window(int out, const char *program, const char *service,
                                 const char *account)
{
    choice = 'n';
    struct window *win = window_open(460, 200, "Keyring");
    if (win) {
        choice = '0';
        char text[900];
        snprintf(text, sizeof(text), "The program\n%s\nasks for %s %s\nfrom your keyring.", program,
                 service, account);
        ask_root = widget_column();
        struct widget *buttons = widget_row(), *space = widget_column();
        struct widget *allow = widget_button("Allow"), *deny = widget_button("Deny");
        widget_add(ask_root, widget_label(text));
        widget_add(ask_root, space);
        widget_stretch(space, 1);
        widget_add(buttons, allow);
        widget_add(buttons, deny);
        widget_add(ask_root, buttons);
        widget_on(allow, chose_allow, 0);
        widget_on(deny, chose_deny, 0);
        printf("keyringd: asking about %s\n", program);
        widget_run(win, ask_root);
        window_close(win);
    }
    ssize_t w = write(out, &choice, 1);
    (void)w;
    _exit(0);
}

/* Asks the user whether c's program may have (service, account): m, the
 * request, waits in `ask` for the answer. false: it could not be asked. */
static bool start_ask(struct client *c, const struct kw_message *m, const char *service,
                      const char *account)
{
    int fds[2];
    struct kw_message *copy = malloc(sizeof(*copy));
    if (!copy || pipe(fds) < 0) {
        free(copy);
        return false;
    }
    memcpy(copy, m, sizeof(*copy));
    for (int f = 0; f < copy->count; f++)       /* the fields point into its own bytes */
        copy->field[f].data = copy->bytes + ((const uint8_t *)m->field[f].data - m->bytes);
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        close(listener);
        for (int i = 0; i < client_count; i++)
            close(clients[i].fd);
        keyring_free(&k);                       /* (the child needs no secret) */
        ask_window(fds[1], c->program, service, account);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        explicit_bzero(copy, sizeof(*copy));
        free(copy);
        return false;
    }
    ask.active = true;
    ask.pid = pid;
    ask.pipe = fds[0];
    ask.client = c->id;
    ask.started = now_ns();
    ask.request = copy;
    char text[500];
    snprintf(text, sizeof(text), "asking whether %s may have %s %s", c->program, service, account);
    note(text);
    return true;
}

/* --- Requests --------------------------------------------------------------------- */

/*
 * KW_GET, KW_SET, KW_REMOVE of m from c. A program the entry does not name
 * is asked about (the answer comes later, in answer_asked), unless it was
 * denied already or a question is open; allowed (the user said yes), it is
 * added to the entry first.
 */
static void entry_request(struct client *c, const struct kw_message *m, bool allowed)
{
    char service[KEYRING_NAME_MAX + 1], account[KEYRING_NAME_MAX + 1];
    if (!is_open) {
        answer(c, KW_LOCKED, 0, 0);
        return;
    }
    if (!names(m, service, account)) {
        answer(c, KW_INVALID, 0, 0);
        return;
    }
    last_use = now_ns();
    int i = keyring_find(&k, service, account);
    if (i < 0 && m->code != KW_SET) {
        answer(c, KW_NOT_FOUND, 0, 0);
        return;
    }
    if (m->code == KW_SET && (m->count != 3 || m->field[2].length > KEYRING_SECRET_MAX)) {
        answer(c, KW_INVALID, 0, 0);
        return;
    }
    if (i >= 0 && !allowed && !system_mode && !keyring_program_may(&k.entry[i], c->program)) {
        if (was_denied(c->program, service, account) || ask.active || !start_ask(c, m, service, account))
            answer(c, KW_NOT_ALLOWED, 0, 0);
        return;
    }
    if (i >= 0 && allowed) {
        if (!keyring_program_add(&k.entry[i], c->program)) {
            answer(c, KW_INVALID, 0, 0);
            return;
        }
        if (save() < 0) {
            answer_errno(c, errno);
            return;
        }
        char text[500];
        snprintf(text, sizeof(text), "%s may have %s %s (the user allowed it)", c->program, service,
                 account);
        note(text);
    }
    if (m->code == KW_GET) {
        struct kw_field f = { k.entry[i].secret, (uint32_t)k.entry[i].secret_length };
        answer(c, KW_OK, &f, 1);
        return;
    }
    if (m->code == KW_SET) {
        uint8_t *old = NULL;                /* the secret before, to put back */
        size_t old_n = 0;
        if (i >= 0) {
            old_n = k.entry[i].secret_length;
            if (!(old = malloc(old_n ? old_n : 1))) {
                answer_errno(c, ENOMEM);
                return;
            }
            memcpy(old, k.entry[i].secret, old_n);
        }
        int put = keyring_put(&k, service, account, m->field[2].data, m->field[2].length);
        if (put > 0 && !system_mode && !keyring_program_add(&k.entry[k.count - 1], c->program)) {
            keyring_drop(&k, k.count - 1);          /* (its owner could not be kept) */
            put = -1;
        }
        if (put < 0) {
            answer(c, KW_INVALID, 0, 0);
        } else if (save() < 0) {
            int e = errno;
            if (put)
                keyring_drop(&k, keyring_find(&k, service, account));
            else
                keyring_put(&k, service, account, old, old_n);
            answer_errno(c, e);
        } else {
            answer(c, KW_OK, 0, 0);
        }
        if (old) {
            explicit_bzero(old, old_n);
            free(old);
        }
        return;
    }
    /* KW_REMOVE: the entry kept aside, to put back when the file is not saved */
    struct keyring_entry kept = k.entry[i];
    memmove(&k.entry[i], &k.entry[i + 1], (size_t)(k.count - i - 1) * sizeof(kept));
    k.count--;
    if (save() < 0) {
        int e = errno;
        k.entry[k.count++] = kept;              /* (the array keeps its room) */
        answer_errno(c, e);
    } else {
        kr_entry_free(&kept);
        answer(c, KW_OK, 0, 0);
    }
}

static struct client *client_by_id(uint64_t id)
{
    for (int i = 0; i < client_count; i++)
        if (clients[i].id == id)
            return &clients[i];
    return 0;
}

/* The question's answer (or its time ran out: timed_out): the request
 * done or refused. */
static void answer_asked(bool timed_out)
{
    char got = 0;
    if (timed_out) {
        kill(ask.pid, SIGTERM);
    } else if (read(ask.pipe, &got, 1) != 1) {
        got = 0;
    }
    int status;
    while (waitpid(ask.pid, &status, 0) < 0 && errno == EINTR)
        ;
    close(ask.pipe);
    ask.active = false;
    struct kw_message *m = ask.request;
    struct client *c = client_by_id(ask.client);
    char service[KEYRING_NAME_MAX + 1] = "", account[KEYRING_NAME_MAX + 1] = "";
    names(m, service, account);
    if (got == '1' && c) {
        entry_request(c, m, true);
    } else {
        if (got == '0' && c) {
            struct denial *d = &denied[denied_next];
            snprintf(d->program, sizeof(d->program), "%s", c->program);
            snprintf(d->service, sizeof(d->service), "%s", service);
            snprintf(d->account, sizeof(d->account), "%s", account);
            denied_next = (denied_next + 1) % DENIED;
            if (denied_count < DENIED)
                denied_count++;
        }
        char text[200];
        snprintf(text, sizeof(text), "not allowed (%s)",
                 got == '0' ? "the user denied it" : got == 'n' ? "no window system to ask in"
                                                                : "no answer");
        note(text);
        if (c)
            answer(c, KW_NOT_ALLOWED, 0, 0);
    }
    explicit_bzero(m, sizeof(*m));
    free(m);
}

static void handle(struct client *c)
{
    uint8_t code = in.code;
    bool root_may = code == KW_UNLOCK || code == KW_PASSWORD || code == KW_SESSION ||
                    code == KW_STATUS;
    if (c->uid != (int)me && !(c->uid == 0 && root_may)) {
        answer(c, KW_REFUSED, 0, 0);
        return;
    }
    if (system_mode && code != KW_GET && code != KW_SET && code != KW_REMOVE && code != KW_LIST &&
        code != KW_STATUS) {
        answer(c, KW_INVALID, 0, 0);            /* (70d: no password, no session) */
        return;
    }
    char password[256], fresh[256], program[PROGRAM_PATH_MAX], service[KEYRING_NAME_MAX + 1];
    switch (code) {
    case KW_STATUS: {
        const char *s = is_open ? "open" : "locked";
        struct kw_field f = { s, (uint32_t)strlen(s) };
        answer(c, KW_OK, &f, 1);
        return;
    }
    case KW_SESSION:
        if (!c->session) {
            c->session = true;
            sessions++;
        }
        answer(c, KW_OK, 0, 0);
        return;
    case KW_LOCK:
        lock_now("locked (keyring lock)");
        answer(c, KW_OK, 0, 0);
        return;
    case KW_UNLOCK: {
        if (!kw_text(&in, 0, password, sizeof(password)) || !password[0]) {
            answer(c, KW_INVALID, 0, 0);
            return;
        }
        int r = unlock_with(password);
        explicit_bzero(password, sizeof(password));
        answer(c, (uint8_t)r, 0, 0);
        return;
    }
    case KW_PASSWORD: {
        if (!kw_text(&in, 0, password, sizeof(password)) ||
            !kw_text(&in, 1, fresh, sizeof(fresh)) || !fresh[0]) {
            answer(c, KW_INVALID, 0, 0);
            break;
        }
        bool was_open = is_open;
        int r = unlock_with(password);
        if (r != KW_OK) {
            answer(c, (uint8_t)r, 0, 0);
            break;
        }
        uint8_t header[KEYRING_HEADER];
        memcpy(header, k.header, sizeof(header));
        if (!keyring_rewrap(&k, fresh)) {
            answer(c, KW_FAILED, 0, 0);
        } else if (save() < 0) {
            int e = errno;
            memcpy(k.header, header, sizeof(header));   /* the file keeps the old one */
            answer_errno(c, e);
        } else {
            note("the password changed");
            answer(c, KW_OK, 0, 0);
        }
        if (!was_open)                  /* opened only to change it */
            lock_now("locked again (opened for the password change)");
        break;
    }
    case KW_ALLOW: {                    /* (70c) password, program, service */
        if (!is_open) {
            answer(c, KW_LOCKED, 0, 0);
            return;
        }
        if (!kw_text(&in, 0, password, sizeof(password)) ||
            !kw_text(&in, 1, program, sizeof(program)) || program[0] != '/' ||
            !kw_text(&in, 2, service, sizeof(service)) || !keyring_name_ok(service)) {
            answer(c, KW_INVALID, 0, 0);
            break;
        }
        if (!keyring_password_is(&k, password)) {
            answer(c, KW_WRONG, 0, 0);
            break;
        }
        int n = 0;
        bool ok = true;
        for (int i = 0; i < k.count; i++)
            if (!strcmp(k.entry[i].service, service) && k.entry[i].program_count) {
                ok = ok && keyring_program_add(&k.entry[i], program);
                n++;
            }
        if (n == 0) {
            answer(c, KW_NOT_FOUND, 0, 0);
        } else if (!ok) {
            answer(c, KW_INVALID, 0, 0);
        } else if (save() < 0) {
            answer_errno(c, errno);
        } else {
            char text[16];
            snprintf(text, sizeof(text), "%d", n);
            struct kw_field f = { text, (uint32_t)strlen(text) };
            char line[500];
            snprintf(line, sizeof(line), "%s may have %s (keyring allow)", program, service);
            note(line);
            answer(c, KW_OK, &f, 1);
        }
        break;
    }
    case KW_GET:
    case KW_SET:
    case KW_REMOVE:
        entry_request(c, &in, false);
        return;
    case KW_LIST: {                     /* names only: every program of the user */
        if (!is_open) {
            answer(c, KW_LOCKED, 0, 0);
            return;
        }
        last_use = now_ns();
        uint32_t first = in.count == 1 && in.field[0].length == 4 ? kw_get32(in.field[0].data) : 0;
        uint8_t total[4];
        kw_put32(total, (uint32_t)k.count);
        static struct kw_field f[KW_FIELDS_MAX];
        f[0] = (struct kw_field){ total, 4 };
        int n = 1;
        size_t size = 1 + 8;
        for (uint32_t i = first; i < (uint32_t)k.count && n + 2 <= KW_FIELDS_MAX; i++) {
            struct keyring_entry *e = &k.entry[i];
            size_t more = 8 + strlen(e->service) + strlen(e->account);
            if (size + more > KW_MESSAGE_MAX)
                break;
            f[n++] = (struct kw_field){ e->service, (uint32_t)strlen(e->service) };
            f[n++] = (struct kw_field){ e->account, (uint32_t)strlen(e->account) };
            size += more;
        }
        answer(c, KW_OK, f, n);
        return;
    }
    default:
        answer(c, KW_INVALID, 0, 0);
        return;
    }
    explicit_bzero(password, sizeof(password));
    explicit_bzero(fresh, sizeof(fresh));
}

static void drop_client(int i)
{
    struct client *c = &clients[i];
    close(c->fd);
    bool session = c->session;
    clients[i] = clients[--client_count];
    if (session && --sessions == 0)
        lock_now("locked (the last login session ended)");
}

int main(int argc, char **argv)
{
    system_mode = argc == 2 && !strcmp(argv[1], "-s");
    if (argc != 1 && !system_mode) {
        dprintf(STDERR_FILENO, "usage: keyringd [-s]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    me = (unsigned)getuid();
    if (system_mode && me != 0) {
        dprintf(STDERR_FILENO, "keyringd: -s: the system keyring is root's\n");
        return 1;
    }
    struct passwd *pw = getpwuid(me);
    const char *home = pw ? pw->pw_dir : getenv("HOME");
    if (!home) {
        dprintf(STDERR_FILENO, "keyringd: no home folder\n");
        return 1;
    }
    char name[40];
    if (system_mode) {
        snprintf(path, sizeof(path), "%s", SYSTEM_KEYRING);
        snprintf(name, sizeof(name), "%s", KW_SYSTEM_SOCKET);
    } else {
        snprintf(path, sizeof(path), "%s/.config/keyring.db", home);
        snprintf(settings, sizeof(settings), "%s/" SETTINGS_USER_KEYRING, home);
        settings_make(settings, SETTINGS_USER_KEYRING_TEXT);
        char value[32];
        if (config_get(settings, "lock_after", value, sizeof(value)) == 1)
            lock_after = strtol(value, NULL, 10);
        if (lock_after < 0)
            lock_after = 0;
        snprintf(name, sizeof(name), KW_SOCKET, me);
    }
    listener = ritteros_local_listen(name, 16);
    if (listener < 0) {
        char text[120];
        snprintf(text, sizeof(text), "%s: %s", name, strerror(errno));
        note(text);
        dprintf(STDERR_FILENO, "keyringd: %s\n", text);
        return 1;
    }
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    int null = open("/dev/null", O_RDWR);           /* (started by login: no terminal) */
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        if (null > 2)
            close(null);
    }
    note("listening");
    if (system_mode) {                          /* (70d) opened at once, by its key file */
        struct stat st;
        int fd = open(SYSTEM_KEY, O_RDONLY);
        bool have_key = fd >= 0 && read(fd, system_key, 32) == 32;
        if (fd >= 0)
            close(fd);
        const char *why = "";
        if (stat(path, &st) < 0) {
            if (ritteros_random(system_key, 32) < 0 || !keyring_new_key(&k, system_key))
                note("no randomness for a new system keyring");
            else
                is_open = true, key_written = false;
        } else if (!have_key) {
            note("not opened: " SYSTEM_KEY " is missing or short");
        } else if (keyring_load_key(path, system_key, &k, &why) != 1) {
            char text[300];
            snprintf(text, sizeof(text), "not opened: %s", why);
            note(text);
        } else {
            is_open = saved_once = key_written = true;
            note("opened");
        }
    }
    for (;;) {
        struct pollfd pf[CLIENTS + 2];
        pf[0] = (struct pollfd){ listener, POLLIN, 0 };
        pf[1] = (struct pollfd){ ask.active ? ask.pipe : -1, POLLIN, 0 };
        for (int i = 0; i < client_count; i++)
            pf[i + 2] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
        int wait = -1;
        uint64_t t = now_ns();
        if (is_open && lock_after > 0) {
            uint64_t due = last_use + (uint64_t)lock_after * 60 * 1000000000u;
            uint64_t ms = due <= t ? 0 : (due - t) / 1000000 + 1;
            wait = ms > 3600000 ? 3600000 : (int)ms;
        }
        if (ask.active) {
            uint64_t due = ask.started + (uint64_t)ASK_MS * 1000000u;
            int ms = due <= t ? 0 : (int)((due - t) / 1000000 + 1);
            if (wait < 0 || ms < wait)
                wait = ms;
        }
        int r = poll(pf, (nfds_t)client_count + 2, wait);
        if (r < 0 && errno != EINTR)
            return 1;
        if (is_open && lock_after > 0 &&
            now_ns() >= last_use + (uint64_t)lock_after * 60 * 1000000000u)
            lock_now("locked (unused for lock_after minutes)");
        if (ask.active && (r > 0 && pf[1].revents))
            answer_asked(false);
        else if (ask.active && now_ns() >= ask.started + (uint64_t)ASK_MS * 1000000u)
            answer_asked(true);
        if (r <= 0)
            continue;
        for (int i = client_count - 1; i >= 0; i--) {
            if (!pf[i + 2].revents)
                continue;
            if (!kw_receive(clients[i].fd, &in, READ_MS)) {
                drop_client(i);
                continue;
            }
            handle(&clients[i]);
            explicit_bzero(in.bytes, sizeof(in.bytes));
        }
        if (pf[0].revents & POLLIN) {
            int fd = ritteros_accept(listener, 0, 0);
            if (fd >= 0) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                struct client c = { fd, ritteros_peer_uid(fd), false, next_id++, "" };
                if (client_count == CLIENTS || c.uid < 0 ||
                    ritteros_peer_program(fd, c.program, sizeof(c.program)) < 0)
                    close(fd);
                else
                    clients[client_count++] = c;
            }
        }
    }
}
