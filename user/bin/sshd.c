/*
 * sshd [-p PORT]: the SSH server (root only). It listens on PORT (22) and
 * starts itself again for every connection ("sshd -c ADDRESS", the
 * connection as its standard input and output), like inetd: that process
 * does the key exchange (user/lib/ssh.c), the login and the session.
 *
 * The host key: an Ed25519 seed in /system/config/ssh_host_ed25519_key
 * ("seed = HEX", root only), made at the first start. Its fingerprint is
 * printed, to compare with what the client shows the first time.
 *
 * Logging in (RFC 4252): "publickey" with an Ed25519 key listed in
 * HOME/.ssh/authorized_keys (OpenSSH's format: "ssh-ed25519 BASE64
 * comment"), or "password" (as login checks it). The session (RFC 4254):
 * one channel; "pty-req" and "window-change" set the terminal's size,
 * "shell" starts the user's shell, "exec" runs sh -c COMMAND, both on a
 * pseudo-terminal of their own. What the client sends is typed there;
 * what is shown goes back, within the window the client allows; when the
 * shell ends, its exit status, then the channel is closed. A client that
 * goes away hangs the terminal up (its programs end).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread.h>
#include <unistd.h>
#include "crypto.h"
#include "ritteros.h"
#include "ssh.h"

PROGRAM_HELP("sshd [-p PORT]", "the SSH server: log in from other machines (root)");

#define HOST_KEY        "/system/config/ssh_host_ed25519_key"
#define WINDOW          (1024 * 1024)   /* what the client may send before we ask for more */
#define MAX_PACKET      32768
#define TRIES           10

static struct ssh conn;
static char peer[INET6_ADDRSTRLEN + 8];          /* (64d: an IPv6 one too) */
static char bin_dir[128];

static int fail(const char *what, const char *why)
{
    dprintf(STDERR_FILENO, "sshd: %s: %s\n", what, why);
    return 1;
}

static void hex(const uint8_t *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", in[i]);
}

/* The host key from its file, or a new one (made == true). */
static int host_key(bool *made)
{
    char text[80];
    *made = false;
    int r = config_get(HOST_KEY, "seed", text, sizeof(text));
    if (r > 0 && strlen(text) == 64) {
        for (int i = 0; i < 32; i++) {
            unsigned v = 0;
            for (int j = 0; j < 2; j++) {
                char c = text[2 * i + j];
                v = v * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            conn.host_seed[i] = (uint8_t)v;
        }
    } else if (r < 0 && errno == ENOENT) {
        ritteros_random(conn.host_seed, 32);
        char line[128];
        char seed[65];
        hex(conn.host_seed, 32, seed);
        snprintf(line, sizeof(line), "# sshd's host key (Ed25519): keep it secret\nseed = %s\n", seed);
        int fd = open(HOST_KEY, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || chmod(HOST_KEY, 0600) < 0 || write(fd, line, strlen(line)) < 0 ||
            close(fd) < 0)
            return fail(HOST_KEY, strerror(errno));
        *made = true;
    } else {
        return fail(HOST_KEY, r < 0 ? strerror(errno) : "no seed = ... in it");
    }
    ed25519_public(conn.host_public, conn.host_seed);
    return 0;
}

/* --- logging in --------------------------------------------------------------- */

static struct {
    char name[32], home[128];
    uid_t uid;
    gid_t gid;
} user;

/* Is the key blob listed in HOME/.ssh/authorized_keys? */
static bool authorized(const char *home, const uint8_t *blob, size_t n)
{
    char path[200];
    static char text[16384];
    snprintf(path, sizeof(path), "%s/.ssh/authorized_keys", home);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t length = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (length <= 0)
        return false;
    text[length] = 0;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        while (*line == ' ' || *line == '\t')
            line++;
        if (strncmp(line, "ssh-ed25519 ", 12) == 0) {
            char *key = line + 12;
            while (*key == ' ')
                key++;
            size_t key_length = strcspn(key, " \t\r");
            uint8_t decoded[128];
            long d = base64_decode(key, key_length, decoded, sizeof(decoded));
            if (d == (long)n && memcmp(decoded, blob, n) == 0)
                return true;
        }
        line = end ? end + 1 : 0;
    }
    return false;
}

static int send_failure(void)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, SSH_USERAUTH_FAILURE);
    ssh_put_text(b, "publickey,password");
    ssh_put_bool(b, false);
    return ssh_send(&conn, b);
}

/* Until a login succeeds: 0, or -1. */
static int log_in(void)
{
    if (ssh_receive(&conn) != SSH_SERVICE_REQUEST)
        return -1;
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, SSH_SERVICE_ACCEPT);
    ssh_put_text(b, "ssh-userauth");
    if (ssh_send(&conn, b) < 0)
        return -1;
    for (int tries = 0; tries < TRIES;) {
        int type = ssh_receive(&conn);
        if (type < 0)
            return -1;
        if (type != SSH_USERAUTH_REQUEST)
            continue;
        struct ssh_reader r;
        ssh_reader_init(&r, conn.packet + 1, conn.packet_length - 1);
        char name[64], service[32], method[32];
        ssh_get_text(&r, name, sizeof(name));
        ssh_get_text(&r, service, sizeof(service));
        ssh_get_text(&r, method, sizeof(method));
        struct passwd *pw = getpwnam(name);
        bool known = pw && strcmp(service, "ssh-connection") == 0;
        if (pw) {
            snprintf(user.name, sizeof(user.name), "%s", pw->pw_name);
            snprintf(user.home, sizeof(user.home), "%s", pw->pw_dir);
            user.uid = pw->pw_uid, user.gid = pw->pw_gid;
        }
        bool ok = false;
        if (strcmp(method, "publickey") == 0) {
            bool signed_request = ssh_get_bool(&r);
            char algorithm[32];
            ssh_get_text(&r, algorithm, sizeof(algorithm));
            size_t blob_length;
            const uint8_t *blob = ssh_get_string(&r, &blob_length);
            bool listed = known && !r.bad && strcmp(algorithm, "ssh-ed25519") == 0 &&
                          blob_length == 51 && authorized(user.home, blob, blob_length);
            if (listed && !signed_request) {        /* "would this key do?" */
                ssh_buf_init(b, SSH_USERAUTH_PK_OK);
                ssh_put_text(b, algorithm);
                ssh_put_string(b, blob, blob_length);
                if (ssh_send(&conn, b) < 0)
                    return -1;
                continue;
            }
            if (listed) {
                size_t n, tn, sn;
                const uint8_t *sig_blob = ssh_get_string(&r, &n);
                struct ssh_reader sr;
                ssh_reader_init(&sr, sig_blob, n);
                const uint8_t *type_name = ssh_get_string(&sr, &tn);
                const uint8_t *sig = ssh_get_string(&sr, &sn);
                /* What the client signed: the session ID and this request. */
                static struct ssh_buf signed_data_buffer;   /* too big for the stack */
                struct ssh_buf *signed_data = &signed_data_buffer;
                ssh_put_string(signed_data, conn.session_id, 32);
                ssh_put_u8(signed_data, SSH_USERAUTH_REQUEST);
                ssh_put_text(signed_data, name);
                ssh_put_text(signed_data, service);
                ssh_put_text(signed_data, "publickey");
                ssh_put_bool(signed_data, true);
                ssh_put_text(signed_data, algorithm);
                ssh_put_string(signed_data, blob, blob_length);
                ok = !sr.bad && ssh_string_is(type_name, tn, "ssh-ed25519") && sn == 64 &&
                     ed25519_verify(sig, blob + 19, signed_data->data, signed_data->length);
            }
            tries++;
        } else if (strcmp(method, "password") == 0) {
            ssh_get_bool(&r);
            char password[128];
            ssh_get_text(&r, password, sizeof(password));
            ok = !r.bad && password_check(pw ? user.name : "nobody", password) == 1 && known;
            memset(password, 0, sizeof(password));
            tries++;
        }
        if (ok) {
            dprintf(STDERR_FILENO, "sshd: %s logged in from %s (%s)\n", user.name, peer, method);
            char line[160];
            snprintf(line, sizeof(line), "%s logged in from %s over SSH (%s)\n", user.name,
                     peer, method);
            ritteros_log_write("login", line);
            ssh_buf_init(b, SSH_USERAUTH_SUCCESS);
            return ssh_send(&conn, b);
        }
        if (strcmp(method, "none") != 0) {
            dprintf(STDERR_FILENO, "sshd: %s from %s: %s refused\n", name, peer, method);
            char line[160];
            snprintf(line, sizeof(line), "%s from %s over SSH: %s refused\n", name, peer, method);
            ritteros_log_write("login", line);
        }
        if (send_failure() < 0)
            return -1;
    }
    ssh_disconnect(&conn, 14, "too many tries");
    return -1;
}

/* --- the session --------------------------------------------------------------- */

static struct {
    bool open, started, closed_by_us, pty_req;
    uint32_t theirs;                    /* the client's number for the channel */
    volatile uint32_t their_window;     /* how much more we may send */
    uint32_t their_max;
    uint32_t our_window;                /* how much more the client may send */
    uint32_t rows, cols;
    char term[64];                      /* pty-req's TERM (22t: the shell's TERM) */
    int master;                         /* pty: the master side; -1 on the pipe path */
    int in_fd, out_fd, err_fd;          /* the command's stdin/stdout/stderr as we hold them */
    volatile int outputs_live;          /* output threads still running (1 pty, 2 pipe) */
    int pid;
} channel = { .master = -1, .in_fd = -1, .out_fd = -1, .err_fd = -1, .rows = 24, .cols = 80 };

static void reply(bool want, bool ok)
{
    if (!want)
        return;
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    ssh_buf_init(b, ok ? SSH_CHANNEL_SUCCESS : SSH_CHANNEL_FAILURE);
    ssh_put_u32(b, channel.theirs);
    ssh_send(&conn, b);
}

/* Read one output stream (stdout, or stderr as extended data) and send it to
 * the client, honouring the window. */
static void send_stream(int fd, bool extended)
{
    static uint8_t out_data[MAX_PACKET];
    static uint8_t err_data[MAX_PACKET];
    static struct ssh_buf out_b, err_b;             /* too big for the stack; one per stream */
    uint8_t *data = extended ? err_data : out_data;
    struct ssh_buf *b = extended ? &err_b : &out_b;
    for (;;) {
        size_t want = channel.their_max < MAX_PACKET ? channel.their_max : MAX_PACKET;
        ssize_t n = read(fd, data, want);
        if (n <= 0)
            break;
        for (ssize_t done = 0; done < n;) {
            while (channel.their_window == 0)
                usleep(2000);                       /* the client is behind */
            uint32_t chunk = (uint32_t)(n - done);
            if (chunk > channel.their_window)
                chunk = channel.their_window;
            ssh_buf_init(b, extended ? SSH_CHANNEL_EXTENDED_DATA : SSH_CHANNEL_DATA);
            ssh_put_u32(b, channel.theirs);
            if (extended)
                ssh_put_u32(b, 1);                  /* SSH_EXTENDED_DATA_STDERR */
            ssh_put_string(b, data + done, chunk);
            if (ssh_send(&conn, b) < 0)
                return;
            __atomic_sub_fetch(&channel.their_window, chunk, __ATOMIC_SEQ_CST);
            done += chunk;
        }
    }
}

/* Once every output stream is at its end: the command's exit status and the
 * channel's end. */
static void finish_channel(void)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    int status = 0;
    while (waitpid(channel.pid, &status, 0) < 0 && errno == EINTR)
        ;
    ssh_buf_init(b, SSH_CHANNEL_REQUEST);
    ssh_put_u32(b, channel.theirs);
    if (WIFEXITED(status)) {
        ssh_put_text(b, "exit-status");
        ssh_put_bool(b, false);
        ssh_put_u32(b, (uint32_t)WEXITSTATUS(status));
    } else {
        ssh_put_text(b, "exit-signal");
        ssh_put_bool(b, false);
        ssh_put_text(b, WTERMSIG(status) == SIGINT ? "INT" : WTERMSIG(status) == SIGHUP ? "HUP"
                                                                                          : "KILL");
        ssh_put_bool(b, false);
        ssh_put_text(b, "");
        ssh_put_text(b, "");
    }
    ssh_send(&conn, b);
    ssh_buf_init(b, SSH_CHANNEL_EOF);
    ssh_put_u32(b, channel.theirs);
    ssh_send(&conn, b);
    ssh_buf_init(b, SSH_CHANNEL_CLOSE);
    ssh_put_u32(b, channel.theirs);
    ssh_send(&conn, b);
    channel.closed_by_us = true;
}

/* An output thread: pump one stream, and whoever drains the last one ends
 * the channel. */
static int output_out(void *arg)
{
    (void)arg;
    send_stream(channel.out_fd, false);
    if (__atomic_sub_fetch(&channel.outputs_live, 1, __ATOMIC_SEQ_CST) == 0)
        finish_channel();
    return 0;
}

static int output_err(void *arg)
{
    (void)arg;
    send_stream(channel.err_fd, true);
    if (__atomic_sub_fetch(&channel.outputs_live, 1, __ATOMIC_SEQ_CST) == 0)
        finish_channel();
    return 0;
}

/* Interactive (a pty was asked for): the shell on a new pseudo-terminal, as a
 * new session with the pty as its controlling terminal. */
static bool start_pty(const char *command)
{
    int fds[2];
    if (ritteros_pty_open(fds) < 0)
        return false;
    ritteros_pty_size(fds[0], (int)channel.rows, (int)channel.cols);
    char shell[160];
    snprintf(shell, sizeof(shell), "%s/sh", bin_dir);
    char *argv[] = { "sh", command ? "-c" : 0, (char *)command, 0 };
    int slave[3] = { fds[1], fds[1], fds[1] };
    if (chdir(user.home) < 0)
        chdir("/");
    channel.pid = ritteros_spawn_as(shell, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, slave,
                                    user.uid, user.gid);
    chdir("/");
    close(fds[1]);                                  /* the shell's now */
    if (channel.pid < 0) {
        close(fds[0]);
        return false;
    }
    channel.master = channel.in_fd = channel.out_fd = fds[0];
    channel.outputs_live = 1;
    channel.started = true;
    return thread_create(output_out, 0) >= 0;
}

/* Non-interactive (no pty asked for): the command on raw pipes, with separate
 * stdout and stderr, so binary data and file transfers are clean. */
static bool start_pipe(const char *command)
{
    int in[2], out[2], err[2];
    if (ritteros_pipe(in) < 0)
        return false;
    if (ritteros_pipe(out) < 0) {
        close(in[0]), close(in[1]);
        return false;
    }
    if (ritteros_pipe(err) < 0) {
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        return false;
    }
    char shell[160];
    snprintf(shell, sizeof(shell), "%s/sh", bin_dir);
    char *argv[] = { "sh", command ? "-c" : 0, (char *)command, 0 };
    int child[3] = { in[0], out[1], err[1] };       /* stdin read, stdout write, stderr write */
    if (chdir(user.home) < 0)
        chdir("/");
    /* (41h-8) A session of its own, as with a terminal (and Unix's sshd):
     * in sshd's, every connection's commands shared one (sudo's memory,
     * 68e, carried from one ssh call to the next). */
    channel.pid = ritteros_spawn_as(shell, argv, SPAWN_NEW_GROUP | SPAWN_NEW_SESSION, child, user.uid,
                                    user.gid);
    chdir("/");
    close(in[0]), close(out[1]), close(err[1]);     /* the command's ends now */
    if (channel.pid < 0) {
        close(in[1]), close(out[0]), close(err[0]);
        return false;
    }
    channel.in_fd = in[1];                          /* we write the client's input here */
    channel.out_fd = out[0];
    channel.err_fd = err[0];
    channel.outputs_live = 2;
    channel.started = true;
    if (thread_create(output_out, 0) < 0 || thread_create(output_err, 0) < 0)
        return false;
    return true;
}

/* The session's environment (22t): the user's HOME, USER, LOGNAME and
 * SHELL, and TERM as the client asked with its terminal (none without). */
static void session_environment(void)
{
    char shell[160];
    snprintf(shell, sizeof(shell), "%s/sh", bin_dir);
    setenv("HOME", user.home, 1);
    setenv("USER", user.name, 1);
    setenv("LOGNAME", user.name, 1);
    setenv("SHELL", shell, 1);
    if (channel.pty_req && channel.term[0])
        setenv("TERM", channel.term, 1);
    else
        unsetenv("TERM");
}

static bool start(const char *command)
{
    session_environment();
    return channel.pty_req ? start_pty(command) : start_pipe(command);
}

static void session(void)
{
    static struct ssh_buf b_buffer;   /* too big for the stack */
    struct ssh_buf *b = &b_buffer;
    for (;;) {
        int type = ssh_receive(&conn);
        if (type < 0)
            return;
        struct ssh_reader r;
        ssh_reader_init(&r, conn.packet + 1, conn.packet_length - 1);
        switch (type) {
        case SSH_GLOBAL_REQUEST: {
            char name[64];
            ssh_get_text(&r, name, sizeof(name));
            if (ssh_get_bool(&r)) {
                ssh_buf_init(b, SSH_REQUEST_FAILURE);
                ssh_send(&conn, b);
            }
            break;
        }
        case SSH_CHANNEL_OPEN: {
            char kind[32];
            ssh_get_text(&r, kind, sizeof(kind));
            uint32_t theirs = ssh_get_u32(&r), window = ssh_get_u32(&r), max = ssh_get_u32(&r);
            if (channel.open || strcmp(kind, "session") != 0 || r.bad) {
                ssh_buf_init(b, SSH_CHANNEL_OPEN_FAILURE);
                ssh_put_u32(b, theirs);
                ssh_put_u32(b, 1);                  /* administratively prohibited */
                ssh_put_text(b, "only one session channel");
                ssh_put_text(b, "");
                ssh_send(&conn, b);
                break;
            }
            channel.open = true;
            channel.theirs = theirs;
            channel.their_window = window;
            channel.their_max = max < MAX_PACKET ? max : MAX_PACKET;
            channel.our_window = WINDOW;
            ssh_buf_init(b, SSH_CHANNEL_OPEN_CONFIRMATION);
            ssh_put_u32(b, theirs);
            ssh_put_u32(b, 0);                      /* our number for it */
            ssh_put_u32(b, WINDOW);
            ssh_put_u32(b, MAX_PACKET);
            ssh_send(&conn, b);
            break;
        }
        case SSH_CHANNEL_REQUEST: {
            ssh_get_u32(&r);
            char kind[64];
            ssh_get_text(&r, kind, sizeof(kind));
            bool want = ssh_get_bool(&r);
            if (strcmp(kind, "pty-req") == 0 || strcmp(kind, "window-change") == 0) {
                size_t n;
                if (kind[0] == 'p') {
                    channel.pty_req = true;         /* interactive: run on a pty */
                    const char *term = (const char *)ssh_get_string(&r, &n);   /* TERM */
                    if (term && n < sizeof(channel.term)) {
                        memcpy(channel.term, term, n);
                        channel.term[n] = 0;
                        for (size_t k = 0; k < n; k++)      /* (a name: printable only) */
                            if (channel.term[k] < '!' || channel.term[k] > '~')
                                channel.term[0] = 0;
                    }
                }
                uint32_t cols = ssh_get_u32(&r), rows = ssh_get_u32(&r);
                if (!r.bad && rows && cols && rows < 1000 && cols < 1000) {
                    channel.rows = rows, channel.cols = cols;
                    if (channel.master >= 0)
                        ritteros_pty_size(channel.master, (int)rows, (int)cols);
                }
                reply(want, !r.bad);
            } else if ((strcmp(kind, "shell") == 0 || strcmp(kind, "exec") == 0) &&
                       !channel.started) {
                char command[1024];
                bool exec = kind[0] == 'e';
                if (exec && !ssh_get_text(&r, command, sizeof(command))) {
                    reply(want, false);
                    break;
                }
                reply(want, start(exec ? command : 0));
            } else {
                reply(want, false);                 /* env, signal, x11-req, ... */
            }
            break;
        }
        case SSH_CHANNEL_DATA: {
            ssh_get_u32(&r);
            size_t n;
            const uint8_t *data = ssh_get_string(&r, &n);
            if (channel.in_fd >= 0 && n)
                write(channel.in_fd, data, n);      /* the command's input */
            channel.our_window = n > channel.our_window ? 0 : channel.our_window - (uint32_t)n;
            if (channel.our_window < WINDOW / 2) {
                ssh_buf_init(b, SSH_CHANNEL_WINDOW_ADJUST);
                ssh_put_u32(b, channel.theirs);
                ssh_put_u32(b, WINDOW - channel.our_window);
                ssh_send(&conn, b);
                channel.our_window = WINDOW;
            }
            break;
        }
        case SSH_CHANNEL_WINDOW_ADJUST:
            ssh_get_u32(&r);
            __atomic_add_fetch(&channel.their_window, ssh_get_u32(&r), __ATOMIC_SEQ_CST);
            break;
        case SSH_CHANNEL_EOF:
            if (channel.master >= 0) {
                write(channel.master, "\004", 1);   /* pty: Ctrl-D ends the input */
            } else if (channel.in_fd >= 0) {
                close(channel.in_fd);               /* pipe: a real stdin EOF */
                channel.in_fd = -1;
            }
            break;
        case SSH_CHANNEL_CLOSE:
            if (!channel.closed_by_us) {
                ssh_buf_init(b, SSH_CHANNEL_CLOSE);
                ssh_put_u32(b, channel.theirs);
                ssh_send(&conn, b);
            }
            return;
        case SSH_KEXINIT:
            ssh_disconnect(&conn, 2, "a new key exchange is not supported yet");
            return;
        default: {
            ssh_buf_init(b, SSH_UNIMPLEMENTED);
            ssh_put_u32(b, conn.seq_in - 1);
            ssh_send(&conn, b);
        }
        }
    }
}

/* One connection, on standard input and output. */
static int serve(void)
{
    bool made;
    if (host_key(&made))
        return 1;
    if (ssh_start(&conn, STDIN_FILENO, true) < 0)
        return fail(peer, conn.error);
    if (log_in() < 0) {
        if (conn.error[0])
            fail(peer, conn.error);
        return 1;
    }
    session();
    if (conn.error[0] && !channel.closed_by_us)
        fail(peer, conn.error);
    dprintf(STDERR_FILENO, "sshd: %s from %s: the session ended\n", user.name, peer);
    char line[160];
    snprintf(line, sizeof(line), "%s logged out (SSH from %s)\n", user.name, peer);
    ritteros_log_write("login", line);
    return 0;                                       /* the terminal hangs up as we end */
}

int main(int argc, char **argv)
{
    size_t n = 0;
    for (size_t i = 0; argv[0][i]; i++)
        if (argv[0][i] == '/')
            n = i;
    if (n == 0)                                     /* started by its name: the usual place */
        snprintf(bin_dir, sizeof(bin_dir), "/system/%s/bin",
#if defined(__x86_64__)
                 "x86_64"
#else
                 "aarch64"
#endif
        );
    else
        snprintf(bin_dir, sizeof(bin_dir), "%.*s", (int)n, argv[0]);
    if (argc == 3 && strcmp(argv[1], "-c") == 0) {
        snprintf(peer, sizeof(peer), "%s", argv[2]);
        return serve();
    }
    long port = 22;
    if (argc == 3 && strcmp(argv[1], "-p") == 0)
        port = atol(argv[2]);
    else if (argc != 1)
        port = 0;
    if (port < 1 || port > 65535) {
        dprintf(STDERR_FILENO, "usage: sshd [-p PORT]\n");
        return 2;
    }
    if (getuid() != 0)
        return fail("sshd", "only root may start it");
    bool made;
    if (host_key(&made))
        return 1;
    char print[64];
    ssh_fingerprint(conn.host_public, print, sizeof(print));
    printf("sshd: %s host key %s\n", made ? "made a new" : "the", print);
    /* (64d) An IPv6 socket: IPv6 clients and IPv4 ones (::ffff:a.b.c.d). */
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 here;
    memset(&here, 0, sizeof(here));
    here.sin6_family = AF_INET6;
    here.sin6_port = htons((uint16_t)port);
    if (fd < 0 || bind(fd, (struct sockaddr *)&here, sizeof(here)) < 0 || listen(fd, 8) < 0)
        return fail("listen", strerror(errno));
    printf("sshd: listening on port %ld\n", port);
    for (;;) {
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0)
            ;                                       /* connections that ended */
        struct sockaddr_in6 from;
        socklen_t from_length = sizeof(from);
        int c = accept(fd, (struct sockaddr *)&from, &from_length);
        if (c < 0) {
            if (errno == EINTR)
                return 0;                           /* Ctrl-C */
            continue;
        }
        /* The client as people write it: 10.0.2.2 (not ::ffff:10.0.2.2),
         * 2001:db8::5, fe80::1%eth0. */
        char address[INET6_ADDRSTRLEN + 8];
        if (IN6_IS_ADDR_V4MAPPED(&from.sin6_addr))
            inet_ntop(AF_INET, from.sin6_addr.s6_addr + 12, address, sizeof(address));
        else if (getnameinfo((struct sockaddr *)&from, from_length, address, sizeof(address), 0, 0,
                             NI_NUMERICHOST) != 0)
            snprintf(address, sizeof(address), "?");
        char path[160];
        snprintf(path, sizeof(path), "%s/sshd", bin_dir);
        char *child_argv[] = { "sshd", "-c", address, 0 };
        int fds[3] = { c, c, -1 };
        int pid = ritteros_spawn(path, child_argv, SPAWN_NEW_GROUP, fds);
        if (pid < 0)
            fail(address, strerror(errno));
        else
            printf("sshd: a connection from %s (process %d)\n", address, pid);
        close(c);
    }
}
