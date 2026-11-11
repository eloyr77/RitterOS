/*
 * ssh [-p PORT] [USER@]HOST [COMMAND]: log in on another machine (SSH 2,
 * user/lib/ssh.c), or run COMMAND there.
 *
 * The server's host key is checked against HOME/.ssh/known_hosts
 * (OpenSSH's format: "HOST ssh-ed25519 BASE64", "[HOST]:PORT ..." for
 * another port than 22): a host seen the first time is shown with its
 * fingerprint and kept once the user types "yes"; a host whose key
 * changed is refused. Logging in: with HOME/.ssh/id_ed25519 if there is
 * one (OpenSSH's unencrypted format), else with a password (asked, not
 * shown). Then the session runs on a terminal on the other side, with our
 * terminal in raw mode: every key goes there, what comes back is shown.
 * "~." at the start of a line ends the connection. The exit code is the
 * remote command's.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread.h>
#include <unistd.h>
#include "crypto.h"
#include "ritteros.h"
#include "ssh.h"

PROGRAM_HELP("ssh [-p PORT] [USER@]HOST [COMMAND]...",
             "log in on another machine (SSH)");

#define WINDOW      (1024 * 1024)
#define MAX_PACKET  32768

static struct ssh conn;
static char home[128];
static uint32_t channel_theirs;
static volatile uint32_t their_window;
static uint32_t their_max = MAX_PACKET, our_window = WINDOW;
static volatile bool finished;

static int fail(const char *what, const char *why)
{
    dprintf(STDERR_FILENO, "ssh: %s: %s\n", what, why);
    return 255;
}

/* A line typed on the terminal (echo as asked). */
static bool ask(const char *question, char *answer, size_t size, bool echo)
{
    printf("%s", question);
    if (!echo)
        ritteros_echo(false);
    ssize_t n = read(STDIN_FILENO, answer, size - 1);
    if (!echo) {
        ritteros_echo(true);
        printf("\n");
    }
    if (n <= 0)
        return false;
    answer[n] = 0;
    if (answer[n - 1] == '\n')
        answer[n - 1] = 0;
    return true;
}

/* --- the server's host key ------------------------------------------------------- */

static int check_host(const char *host, long port)
{
    char name[300], path[200];
    if (port == 22)
        snprintf(name, sizeof(name), "%s", host);
    else
        snprintf(name, sizeof(name), "[%s]:%ld", host, port);
    snprintf(path, sizeof(path), "%s/.ssh/known_hosts", home);
    uint8_t blob[51];
    ssh_key_blob(conn.server_key, blob);
    char encoded[80], print[64];
    base64_encode(blob, sizeof(blob), encoded, true);
    ssh_fingerprint(conn.server_key, print, sizeof(print));
    static char text[16384];
    int fd = open(path, O_RDONLY);
    ssize_t length = fd >= 0 ? read(fd, text, sizeof(text) - 1) : 0;
    if (fd >= 0)
        close(fd);
    text[length > 0 ? length : 0] = 0;
    size_t name_length = strlen(name);
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        if (strncmp(line, name, name_length) == 0 && line[name_length] == ' ') {
            char *type = line + name_length + 1;
            if (strncmp(type, "ssh-ed25519 ", 12) == 0 &&
                strncmp(type + 12, encoded, strlen(encoded)) == 0)
                return 0;                           /* known, and the same */
            dprintf(STDERR_FILENO,
                    "ssh: WARNING: the host key of %s is not the one in %s (it is %s now).\n"
                    "ssh: Someone may be in between; if the key was changed on purpose, remove "
                    "the line from that file.\n",
                    name, path, print);
            return -1;
        }
        line = end ? end + 1 : 0;
    }
    printf("ssh: %s is not known yet. Its host key: %s\n", name, print);
    char answer[16];
    if (!ask("ssh: Connect, and keep the key in known_hosts? (yes/no) ", answer, sizeof(answer),
             true) ||
        strcmp(answer, "yes") != 0)
        return -1;
    char dir[200];
    snprintf(dir, sizeof(dir), "%s/.ssh", home);
    mkdir(dir, 0700);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    char line[400];
    snprintf(line, sizeof(line), "%s ssh-ed25519 %s\n", name, encoded);
    if (fd < 0 || write(fd, line, strlen(line)) < 0 || close(fd) < 0)
        dprintf(STDERR_FILENO, "ssh: %s: %s (the key is not kept)\n", path, strerror(errno));
    return 0;
}

/* --- logging in -------------------------------------------------------------------- */

/* HOME/.ssh/id_ed25519 in OpenSSH's format (unencrypted): the seed and
 * public key, or false. */
static bool read_key(uint8_t seed[32], uint8_t public_key[32])
{
    char path[200];
    static char text[8192];
    static uint8_t raw[6144];
    snprintf(path, sizeof(path), "%s/.ssh/id_ed25519", home);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return false;
    text[n] = 0;
    char *start = strstr(text, "-----BEGIN OPENSSH PRIVATE KEY-----");
    char *end = strstr(text, "-----END OPENSSH PRIVATE KEY-----");
    if (!start || !end)
        return false;
    /* The base64 between the two lines, without its line breaks. */
    static char joined[8192];
    size_t j = 0;
    for (char *c = start + 35; c < end && j < sizeof(joined); c++)
        if (*c != '\n' && *c != '\r')
            joined[j++] = *c;
    long length = base64_decode(joined, j, raw, sizeof(raw));
    if (length < 15 || memcmp(raw, "openssh-key-v1\0", 15) != 0)
        return false;
    struct ssh_reader r;
    ssh_reader_init(&r, raw + 15, (size_t)length - 15);
    size_t sn;
    const uint8_t *cipher = ssh_get_string(&r, &sn);
    if (!ssh_string_is(cipher, sn, "none")) {
        dprintf(STDERR_FILENO, "ssh: %s has a passphrase: not supported yet\n", path);
        return false;
    }
    ssh_get_string(&r, &sn);                        /* kdf name, kdf options */
    ssh_get_string(&r, &sn);
    if (ssh_get_u32(&r) != 1)
        return false;
    ssh_get_string(&r, &sn);                        /* the public key's blob */
    const uint8_t *private_part = ssh_get_string(&r, &sn);
    struct ssh_reader p;
    ssh_reader_init(&p, private_part, sn);
    if (ssh_get_u32(&p) != ssh_get_u32(&p))
        return false;                               /* the check numbers differ */
    const uint8_t *type = ssh_get_string(&p, &sn);
    if (!ssh_string_is(type, sn, "ssh-ed25519"))
        return false;
    const uint8_t *pub = ssh_get_string(&p, &sn);
    if (sn != 32)
        return false;
    memcpy(public_key, pub, 32);
    const uint8_t *secret = ssh_get_string(&p, &sn);  /* seed, then the public key again */
    if (sn != 64 || p.bad || r.bad)
        return false;
    memcpy(seed, secret, 32);
    return true;
}

/* USERAUTH_REQUEST's beginning: user, service, method. */
static void request(struct ssh_buf *b, const char *user, const char *method)
{
    ssh_buf_init(b, SSH_USERAUTH_REQUEST);
    ssh_put_text(b, user);
    ssh_put_text(b, "ssh-connection");
    ssh_put_text(b, method);
}

static int log_in(const char *user, const char *host)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, SSH_SERVICE_REQUEST);
    ssh_put_text(b, "ssh-userauth");
    if (ssh_send(&conn, b) < 0)
        return -1;
    int type;
    while ((type = ssh_receive(&conn)) == SSH_EXT_INFO)
        ;
    if (type != SSH_SERVICE_ACCEPT)
        return -1;
    uint8_t seed[32], public_key[32];
    if (read_key(seed, public_key)) {
        uint8_t blob[51];
        ssh_key_blob(public_key, blob);
        static struct ssh_buf data_buffer;   /* too big for the stack */
        struct ssh_buf *data = &data_buffer;
        ssh_put_string(data, conn.session_id, 32);
        size_t offset = data->length;
        request(b, user, "publickey");
        ssh_put_bool(b, true);
        ssh_put_text(b, "ssh-ed25519");
        ssh_put_string(b, blob, sizeof(blob));
        memcpy(data->data + offset, b->data, b->length);   /* what is signed */
        data->length = offset + b->length;
        uint8_t sig[64];
        ed25519_sign(sig, seed, public_key, data->data, data->length);
        memset(seed, 0, sizeof(seed));
        static struct ssh_buf sig_blob_buffer;   /* too big for the stack */
        struct ssh_buf *sig_blob = &sig_blob_buffer;
        ssh_put_text(sig_blob, "ssh-ed25519");
        ssh_put_string(sig_blob, sig, 64);
        ssh_put_string(b, sig_blob->data, sig_blob->length);
        if (ssh_send(&conn, b) < 0)
            return -1;
        while ((type = ssh_receive(&conn)) == SSH_USERAUTH_BANNER || type == SSH_EXT_INFO)
            ;
        if (type == SSH_USERAUTH_SUCCESS)
            return 0;
        if (type != SSH_USERAUTH_FAILURE)
            return -1;
    }
    for (int tries = 0; tries < 3; tries++) {
        char prompt[300], password[128];
        snprintf(prompt, sizeof(prompt), "%s@%s's password: ", user, host);
        if (!ask(prompt, password, sizeof(password), false))
            return -1;
        request(b, user, "password");
        ssh_put_bool(b, false);
        ssh_put_text(b, password);
        memset(password, 0, sizeof(password));
        if (ssh_send(&conn, b) < 0)
            return -1;
        while ((type = ssh_receive(&conn)) == SSH_USERAUTH_BANNER || type == SSH_EXT_INFO)
            ;
        if (type == SSH_USERAUTH_SUCCESS)
            return 0;
        if (type != SSH_USERAUTH_FAILURE)
            return -1;
        printf("Permission denied, please try again.\n");
    }
    snprintf(conn.error, sizeof(conn.error), "Permission denied");
    return -1;
}

/* --- the session -------------------------------------------------------------------- */

static void send_data(const uint8_t *data, size_t n)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    while (n && !finished) {
        while (their_window == 0 && !finished)
            usleep(2000);
        size_t chunk = n < their_window ? n : their_window;
        if (chunk > their_max)
            chunk = their_max;
        ssh_buf_init(b, SSH_CHANNEL_DATA);
        ssh_put_u32(b, channel_theirs);
        ssh_put_string(b, data, chunk);
        if (ssh_send(&conn, b) < 0)
            return;
        __atomic_sub_fetch(&their_window, (uint32_t)chunk, __ATOMIC_SEQ_CST);
        data += chunk, n -= chunk;
    }
}

/* Keys typed here go to the other side; "~." at a line's start ends it. */
static int keyboard(void *arg)
{
    (void)arg;
    bool line_start = true, tilde = false;
    for (;;) {
        uint8_t keys[64];
        ssize_t n = read(STDIN_FILENO, keys, sizeof(keys));
        if (n <= 0 || finished)
            break;
        for (ssize_t i = 0; i < n; i++) {
            uint8_t c = keys[i];
            if (tilde) {
                tilde = false;
                if (c == '.') {
                    finished = true;
                    /* The server closes the connection, which ends the
                     * main thread's wait. */
                    ssh_disconnect(&conn, 11, "disconnected by the user");
                    return 0;
                }
                if (c != '~')
                    send_data((const uint8_t *)"~", 1);
            } else if (line_start && c == '~') {
                tilde = true;
                continue;
            }
            send_data(&c, 1);
            line_start = c == '\r' || c == '\n';
        }
    }
    return 0;
}

static void channel_request(struct ssh_buf *b, const char *kind, bool want)
{
    ssh_buf_init(b, SSH_CHANNEL_REQUEST);
    ssh_put_u32(b, channel_theirs);
    ssh_put_text(b, kind);
    ssh_put_bool(b, want);
}

static int session(const char *command)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, SSH_CHANNEL_OPEN);
    ssh_put_text(b, "session");
    ssh_put_u32(b, 0);
    ssh_put_u32(b, WINDOW);
    ssh_put_u32(b, MAX_PACKET);
    if (ssh_send(&conn, b) < 0)
        return 255;
    int type;
    while ((type = ssh_receive(&conn)) == SSH_GLOBAL_REQUEST || type == SSH_REQUEST_FAILURE)
        ;
    if (type != SSH_CHANNEL_OPEN_CONFIRMATION)
        return fail("session", "the server refused it");
    struct ssh_reader r;
    ssh_reader_init(&r, conn.packet + 1, conn.packet_length - 1);
    ssh_get_u32(&r);
    channel_theirs = ssh_get_u32(&r);
    their_window = ssh_get_u32(&r);
    uint32_t max = ssh_get_u32(&r);
    their_max = max < MAX_PACKET ? max : MAX_PACKET;
    int rows, cols;
    ritteros_terminal_size(&rows, &cols);
    channel_request(b, "pty-req", false);
    ssh_put_text(b, "vt100");
    ssh_put_u32(b, (uint32_t)cols);
    ssh_put_u32(b, (uint32_t)rows);
    ssh_put_u32(b, 0);
    ssh_put_u32(b, 0);
    ssh_put_string(b, "", 1);                       /* the modes: TTY_OP_END only */
    ssh_send(&conn, b);
    channel_request(b, command ? "exec" : "shell", true);
    if (command)
        ssh_put_text(b, command);
    ssh_send(&conn, b);
    ritteros_terminal_raw(true);
    thread_create(keyboard, 0);
    int status = 255;
    while (!finished && (type = ssh_receive(&conn)) >= 0) {
        ssh_reader_init(&r, conn.packet + 1, conn.packet_length - 1);
        if (type == SSH_CHANNEL_DATA || type == SSH_CHANNEL_EXTENDED_DATA) {
            ssh_get_u32(&r);
            if (type == SSH_CHANNEL_EXTENDED_DATA)
                ssh_get_u32(&r);
            size_t n;
            const uint8_t *data = ssh_get_string(&r, &n);
            write(type == SSH_CHANNEL_DATA ? STDOUT_FILENO : STDERR_FILENO, data, n);
            our_window = n > our_window ? 0 : our_window - (uint32_t)n;
            if (our_window < WINDOW / 2) {
                ssh_buf_init(b, SSH_CHANNEL_WINDOW_ADJUST);
                ssh_put_u32(b, channel_theirs);
                ssh_put_u32(b, WINDOW - our_window);
                ssh_send(&conn, b);
                our_window = WINDOW;
            }
        } else if (type == SSH_CHANNEL_WINDOW_ADJUST) {
            ssh_get_u32(&r);
            __atomic_add_fetch(&their_window, ssh_get_u32(&r), __ATOMIC_SEQ_CST);
        } else if (type == SSH_CHANNEL_REQUEST) {
            ssh_get_u32(&r);
            char kind[32];
            ssh_get_text(&r, kind, sizeof(kind));
            bool want = ssh_get_bool(&r);
            if (strcmp(kind, "exit-status") == 0)
                status = (int)ssh_get_u32(&r);
            else if (strcmp(kind, "exit-signal") == 0)
                status = 128 + 1;
            if (want) {
                ssh_buf_init(b, SSH_CHANNEL_FAILURE);
                ssh_put_u32(b, channel_theirs);
                ssh_send(&conn, b);
            }
        } else if (type == SSH_CHANNEL_FAILURE) {
            ritteros_terminal_raw(false);
            fail("session", command ? "the server did not run the command"
                                    : "the server did not start a shell");
            break;
        } else if (type == SSH_CHANNEL_CLOSE) {
            ssh_buf_init(b, SSH_CHANNEL_CLOSE);
            ssh_put_u32(b, channel_theirs);
            ssh_send(&conn, b);
            break;
        } else if (type == SSH_GLOBAL_REQUEST) {
            char name[64];
            ssh_get_text(&r, name, sizeof(name));
            if (ssh_get_bool(&r)) {
                ssh_buf_init(b, SSH_REQUEST_FAILURE);
                ssh_send(&conn, b);
            }
        }
    }
    finished = true;
    ritteros_terminal_raw(false);
    return status;
}

/* (64d) A TCP connection to host's port: each of getaddrinfo's addresses
 * in its order (IPv6 first where the machine has it) until one answers; a
 * descriptor, or -1 with errno (ENONAME and the like for the name, the
 * last connect's otherwise). */
static int connect_name(const char *host, const char *service)
{
    char name[256];
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']' && n - 2 < sizeof(name)) {
        memcpy(name, host + 1, n - 2);
        name[n - 2] = 0;
        host = name;
    }
    struct addrinfo hints, *list;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    errno = 0;
    int r = getaddrinfo(host, service, &hints, &list);
    if (r) {
        if (!errno)
            errno = r == EAI_NONAME ? ENONAME : EINVAL;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *a = list; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
            int e = errno;
            close(fd);
            fd = -1;
            errno = e;
        }
    }
    freeaddrinfo(list);
    return fd;
}

int main(int argc, char **argv)
{
    long port = 22;
    int arg = 1;
    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        port = atol(argv[2]);
        arg = 3;
    }
    if (arg >= argc || port < 1 || port > 65535) {
        dprintf(STDERR_FILENO, "usage: ssh [-p PORT] [USER@]HOST [COMMAND]\n");
        return 2;
    }
    struct passwd *me = getpwuid(getuid());
    snprintf(home, sizeof(home), "%s", me ? me->pw_dir : "/");
    char user[64], host[256];
    char *at = strchr(argv[arg], '@');
    if (at) {
        snprintf(user, sizeof(user), "%.*s", (int)(at - argv[arg]), argv[arg]);
        snprintf(host, sizeof(host), "%s", at + 1);
    } else {
        snprintf(user, sizeof(user), "%s", me ? me->pw_name : "root");
        snprintf(host, sizeof(host), "%s", argv[arg]);
    }
    /* The command: the words after the host, joined. */
    static char command[1024];
    for (int i = arg + 1; i < argc; i++) {
        size_t used = strlen(command);
        snprintf(command + used, sizeof(command) - used, "%s%s", used ? " " : "", argv[i]);
    }
    size_t n = strlen(host);                        /* (64d) user@[2001:db8::5] */
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']') {
        memmove(host, host + 1, n - 2);
        host[n - 2] = 0;
    }
    char service[8];
    snprintf(service, sizeof(service), "%ld", (long)port);
    int fd = connect_name(host, service);
    if (fd < 0)
        return fail(host, strerror(errno));
    if (ssh_start(&conn, fd, false) < 0)
        return fail(host, conn.error);
    if (check_host(host, port) < 0) {
        ssh_disconnect(&conn, 11, "host key not accepted");
        return 255;
    }
    if (log_in(user, host) < 0)
        return fail(host, conn.error[0] ? conn.error : "the login failed");
    int status = session(command[0] ? command : 0);
    if (conn.error[0] && !finished)
        fail(host, conn.error);
    printf("ssh: the connection to %s is closed\n", host);
    exit(status);
}
