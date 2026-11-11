/*
 * RitterOS-specific parts of the programs' library. Programs normally use
 * the standard headers (stdio.h, string.h, unistd.h, fcntl.h, stdlib.h,
 * errno.h); system calls themselves are listed in common/syscalls.h.
 */
#ifndef RITTEROS_USER_H
#define RITTEROS_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "../../common/syscalls.h"

/* A raw system call: number and up to six arguments; returns the kernel's
 * result (negative: -ERR_*). user/arch/<arch>/start.S */
int64_t syscall6(int64_t number, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                 int64_t f);

/* Per-thread data (22i): `__thread` variables, a copy for each thread. The
 * system sets it up -- ld.so for a dynamic program, the start code for a
 * static one, thread_create for each new thread -- so programs call none of
 * these. A module (the program or a library) with per-thread data: its
 * initial data (image, filesz bytes; the rest of memsz zeroed), alignment,
 * and its block's offset from the thread pointer. */
struct ritteros_tls_module {
    const void *image;
    size_t filesz, memsz, align;
    long tpoff;
};
int ritteros_set_thread_pointer(void *tp);
int __ritteros_tls_setup(const struct ritteros_tls_module *modules, int count);
/* A module dlopen loaded (22k): into the reserve of every thread's area; 0, or
 * -1 if it does not fit. */
int __ritteros_tls_add(const struct ritteros_tls_module *module);
/* ld.so's dlopen, dlsym, dlclose (22k), handed over at the start: dlfcn.h's
 * functions call them, one at a time. open and symbol put a message into
 * error when they fail (0). */
struct ritteros_dl_loader {
    void *(*open)(const char *file, int flags, char *error, size_t size);
    void *(*symbol)(void *handle, const char *name, char *error, size_t size);
    int (*close)(void *handle);
};
int __ritteros_dl_setup(const struct ritteros_dl_loader *loader);
void __ritteros_start(long *sp);
void *__tls_get_addr(void *index);

/* Uptime and memory (struct sys_info); 0, or -1 with errno set. */
int ritteros_sysinfo(struct sys_info *info);

/* (35a) Swap: SWAP_STATUS fills status; SWAP_ON and SWAP_OFF (root's) turn
 * it on with /system/swap or off. 0, or -1 with errno set. */
int ritteros_swap(int op, struct sys_swap *status);
/* (37b-1) ACPI table number index (the DSDT, the FACS, then the XSDT's
 * list), at most size bytes copied: its full length; -1 with errno ENOENT
 * past the last, EPERM for anyone but root. */
int64_t ritteros_power_table(int index, void *buffer, size_t size);
/* (37b-2) The AML namespace's objects, one a line ("\\_SB_.PCI0 Device"),
 * at most size bytes with a 0: the full length; -1 with errno EPERM for
 * anyone but root. */
int64_t ritteros_power_names(char *buffer, size_t size);
/* (37b-3) The AML object at path ("\\_SB.PCI0._CRS") evaluated, its value as
 * text lines ("\\_SB_.PCI0._CRS: Buffer 238 bytes", ...), at most size bytes
 * with a 0: the full length; -1 with errno ENOENT for no such object, EIO
 * when the evaluation failed, EPERM for anyone but root. */
int64_t ritteros_power_eval(const char *path, char *buffer, size_t size);

/* (37g-4) Sleep (ACPI S3) for seconds (1 to a day), woken by the RTC's
 * alarm; (37g-7) 0: until the power button (or the firmware's own wake,
 * a laptop's lid) wakes it: 0 once awake again; -1 with errno EINVAL for
 * more than a day, EIO
 * when the machine did not sleep (the kernel's log says why), EPERM for
 * anyone but root. */
int ritteros_power_sleep(unsigned seconds);

/* Switch the machine off or restart it (POWER_OFF, POWER_REBOOT); returns
 * only if that did not work. */
int ritteros_power(int how);

/* A kernel report (REPORT_PROCESSES, _THREADS, _PCI, _USB) as text in
 * buffer (zero-terminated, cut short if needed); returns its full length,
 * or -1 with errno set. */
int64_t ritteros_report(int which, char *buffer, size_t size);

/* RitterFS snapshots (SNAPSHOT_CREATE, _DELETE with a name; SNAPSHOT_LIST
 * into buffer); the kernel's result, or -1 with errno set. */
int64_t ritteros_snapshot(int op, const char *name, struct sys_snapshot *buffer, size_t size);

/* spawn with the child's standard input, output and error taken from the
 * caller's descriptors fds[0..2] (-1: the caller's own 0, 1 or 2); flags
 * as SYS_SPAWN's (SPAWN_NEW_GROUP). The child's ID, or -1 with errno set. */
int ritteros_spawn(const char *path, char *const argv[], int flags, const int fds[3]);
/* (22t) The same with an environment of the caller's choice (envp: "NAME=value"
 * strings ended by a null pointer; ritteros_spawn passes the caller's own). */
int ritteros_spawn_env(const char *path, char *const argv[], char *const envp[], int flags,
                       const int fds[3]);

/* The same with the child running as user uid, group gid (root only:
 * login starts a user's shell this way). */
int ritteros_spawn_as(const char *path, char *const argv[], int flags, const int fds[3],
                      uid_t uid, gid_t gid);

/* An instant copy of a file or a directory tree: the copy shares the data
 * blocks until one side changes; 0, or -1 with errno set. */
int ritteros_copy(const char *from, const char *to);

/* Attributes of path (ATTR_GET, _SET, _REMOVE, _LIST); the kernel's result,
 * or -1 with errno set. */
int64_t ritteros_attr(int op, const char *path, const char *key, void *buffer, size_t size);

/* Hashes (user/lib/crypto.c): SHA-256, HMAC-SHA256, and PBKDF2-HMAC-SHA256
 * for passwords (salt up to PBKDF2_SALT_MAX bytes). */
#define SHA256_SIZE 32
#define PBKDF2_SALT_MAX 64
struct sha256 {
    uint32_t h[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[SHA256_SIZE]);
void sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE]);
void hmac_sha256(const void *key, size_t key_length, const void *data, size_t n,
                 uint8_t out[SHA256_SIZE]);
void pbkdf2_sha256(const void *password, size_t password_length, const uint8_t *salt,
                   size_t salt_length, uint32_t iterations, uint8_t *out, size_t out_length);

/* CRC-32 (IEEE 802.3, as GPT and zlib use it) and CRC-32C (Castagnoli,
 * as RitterFS uses it); common/crc32.c. */
uint32_t crc32(const void *data, size_t size);
uint32_t crc32c(const void *data, size_t size);

/* The time: nanoseconds since 1970-01-01 UTC; 0 if the machine has no
 * clock. */
uint64_t ritteros_time(void);

/* Random bytes from the kernel (at most 256 per call); n, or -1. */
int64_t ritteros_random(void *buffer, size_t n);

/* Whether the terminal shows what is typed (off for passwords; the kernel
 * switches it on again when the program ends); 0, or -1. */
int ritteros_echo(bool on);

/* Raw terminal input for full-screen programs (see SYS_TERMINAL): read()
 * returns bytes as typed, at least one, without echo; the kernel switches
 * back to lines when the program ends. 0, or -1. */
int ritteros_terminal_raw(bool on);
/* Raw input for a program that edits its own line (the shell): the console
 * shows that line again after a message. ritteros_terminal_raw(false) ends it. */
int ritteros_terminal_line_editor(void);

/* Mount a RitterFS partition (by its name in /dev) at path, or with path
 * 0 at /volumes/<label>; the path used goes into buffer. Unmount by the
 * mount's path or the device's name. Root only; 0, or -1 (EBUSY: mounted
 * already, or in use). */
int ritteros_mount(const char *device, const char *path, char *buffer, size_t size);
int ritteros_unmount(const char *path_or_device);

/* Copy from (a file, or a directory with everything in it) to a new path
 * byte by byte, for another file system (where ritteros_copy says EXDEV);
 * remove a file or a directory with everything in it. 0, or -1 with errno
 * (and the path that failed in failed). */
int ritteros_copy_bytes(const char *from, const char *to, char *failed, size_t size);
int ritteros_remove_tree(const char *path);

/* A volume is mounted at path (its file system differs from its parent's;
 * rm and mv refuse to empty or move one). */
bool ritteros_mount_point(const char *path);

/* Entry number index of the device list (disks, each followed by its
 * partitions, as /dev lists them): 1, or 0 past the last. */
int ritteros_device(int index, struct sys_device *out);

/* (68j) The RitterFS mounted on partition (its name in /dev) takes the
 * room its partition grew by (disks grow): its block count of 4 KiB now
 * (unchanged without new room). Root only; -1 with errno (EINVAL: no
 * RitterFS mounted there). */
int64_t ritteros_grow(const char *partition);

/* The terminal's text area in characters (80x24 without a screen); 0. */
int ritteros_terminal_size(int *rows, int *cols);

/* A pseudo-terminal (sshd): fds[0] its master side (read: what is shown,
 * write: typing), fds[1] its slave side, to give to a new session's
 * programs (spawn with SPAWN_NEW_SESSION: it becomes their terminal);
 * its size in characters. 0, or -1. */
int ritteros_pty_open(int fds[2]);
int ritteros_pty_size(int fd, int rows, int cols);

/* A pipe: fds[0] the read end, fds[1] the write end. A read gives 0 once
 * every write end is closed; a write fails (errno EPIPE) once every read
 * end is closed. Binary-transparent. 0, or -1. */
int ritteros_pipe(int fds[2]);

/* The network (IPv4, host byte order). ip_parse reads "10.0.2.15" or
 * "10.0.2.15/24" (prefix may be 0: then no "/N" is allowed) and returns
 * whether it was one; ip_format writes one. ritteros_netset sets a card's
 * settings (root only); ritteros_ping sends one ICMP echo and returns the
 * round trip in microseconds, or -1 (errno: ETIMEDOUT, ENETUNREACH,
 * EHOSTUNREACH, EINTR). (user/lib/net.c) */
bool ip_parse(const char *text, uint32_t *ip, uint32_t *prefix);
void ip_format(uint32_t ip, char *out, size_t size);
int ritteros_netset(const struct sys_netset *settings);
/* A card's IPv6 settings (64b; root only): NETSET6_AUTO (from the
 * network's routers), NETSET6_OFF, NETSET6_FIXED (address and prefix, a
 * gateway); a DNS server instead of the router's. */
int ritteros_netset6(const struct sys_netset6 *settings);
/* A card's MAC address, link and settings (name filled in); 0 or -1. */
int ritteros_netinfo(struct sys_netinfo *info);
/* (64d) A card's IPv6 side: its addresses, router, DNS server and MTU, by
 * name or (an empty name) by its place in the list, as ritteros_netinfo. */
int ritteros_netinfo6(struct sys_netinfo6 *info);
/* (64g) A DHCPv6 lease for a card (root; the elf dhclient): its address and
 * DNS server, or NETLEASE6_DROP to end it. */
int ritteros_netlease6(const struct sys_netlease6 *lease);
/* The same for the card at place index in the kernel's list (0 first, the
 * name filled in): loop until -1 with errno ENODEV to see every card,
 * whatever its kind or name. 0 or -1. */
int ritteros_netcard(int index, struct sys_netinfo *info);
/* A wireless card's radio and link, for the elf wifid (root only; 24c):
 * op is a WLAN_* constant, card its name ("wlan0"), buffer/length and value
 * as that op wants (common/syscalls.h). The op's result, or -1 with errno. */
int64_t ritteros_wlan(int op, const char *card, void *buffer, size_t length, uint64_t value);
/* A UDP socket of one card only, before ritteros_bind (root only): for a
 * DHCP client, which sends broadcasts out of the card before it has an
 * address. 0 or -1. */
int ritteros_bindcard(int fd, const char *card);
/* Forwarding between cards: FORWARD_ASK, FORWARD_ON or FORWARD_OFF (root
 * only); 1 (on), 0 (off) or -1. */
int ritteros_forward(int mode);
/* Masquerade (source NAT, 49i): MASQ_RESET clears the private cards,
 * MASQ_ADD marks the card named `card` private (root). */
int ritteros_masquerade(int op, const char *card);
/* Virtual network cards (38a, root only): TAP_ADD makes a tapN whose
 * /dev/tapN is user `value`'s (and root's), giving N; TAP_REMOVE removes
 * tap number `value` (EBUSY while a program has it open). -1 on errors. */
int ritteros_tap(int op, unsigned value);
/* Bridges (38b, root only): BRIDGE_ADD makes the card named `card` a port
 * of bridge brN (bridge -1: of a new one), giving N; BRIDGE_REMOVE takes
 * it out (card 0: the whole bridge goes). -1 on errors (ENOTSUP: a card
 * that cannot take other machines' frames, a Wi-Fi card). */
int ritteros_bridge(int op, int bridge, const char *card);
/* The default route's cards (50g): DEFAULT_ROUTE_RESET clears the order,
 * DEFAULT_ROUTE_ADD puts the card named `card` (ethN, wlanN, tapN or brN,
 * there or not) next (root). */
int ritteros_default_route(int op, const char *card);
int64_t ritteros_ping(uint32_t ip, uint16_t id, uint16_t seq, size_t size, uint32_t timeout_ms);
/* The same to any address (64a): IPv6 (a link-local or multicast one with
 * its card in `scope`: EINVAL without), or IPv4 as ::ffff:a.b.c.d. */
int64_t ritteros_ping6(const struct sys_inaddr *to, uint16_t id, uint16_t seq, size_t size,
                       uint32_t timeout_ms);

/* A name's IPv4 address: an address written out is taken as it is,
 * localhost is 127.0.0.1, any other name is asked of the elf dnsd (DNS,
 * on this machine's port 53). 0, or -1 (errno: ENONAME, ETIMEDOUT,
 * ENETUNREACH: no DNS server known). */
int ritteros_resolve(const char *name, uint32_t *ip);
/* (64d) A name's IPv6 address (16 bytes): one written out is taken as it is
 * (no %card), localhost is ::1, any other name is asked of dnsd for its
 * AAAA record. 0, or -1 with errno as ritteros_resolve's. */
int ritteros_resolve6(const char *name, uint8_t address[16]);

/* DNS messages (RFC 1035; user/lib/dns.c), for dnsd and the resolver.
 * dns_query: a question for name's A record (its length, 0 if the name
 * cannot be one); dns_answer: the first A record of an answer, 0 or an
 * errno (ENONAME, ENETUNREACH: refused, ETIMEDOUT: server failure,
 * EINVAL); dns_question: the name a simple question (one, type A) asks
 * for; dns_reply: the answer to a question, one A record; dns_failure:
 * an answer without records, with rcode (2: server failure, 5: refused). */
#define DNS_NAME_MAX 253
size_t dns_query(const char *name, uint16_t id, uint8_t *out, size_t size);
int dns_answer(const uint8_t *m, size_t length, uint32_t *address, uint32_t *ttl);
bool dns_question(const uint8_t *m, size_t length, char *name, size_t size);
size_t dns_reply(const uint8_t *query, size_t length, uint32_t address, uint32_t ttl,
                 uint8_t *out, size_t size);
size_t dns_failure(const uint8_t *query, size_t length, int rcode, uint8_t *out, size_t size);
/* (64d) Any record type: dns_query_type asks for `type` (1 A, 28 AAAA);
 * dns_answer_aaaa takes the first AAAA record (errors as dns_answer);
 * dns_question_type gives a simple question's name and type; dns_reply_aaaa
 * answers with one AAAA record. */
size_t dns_query_type(const char *name, uint16_t id, uint16_t type, uint8_t *out, size_t size);
int dns_answer_aaaa(const uint8_t *m, size_t length, uint8_t address[16], uint32_t *ttl);
bool dns_question_type(const uint8_t *m, size_t length, char *name, size_t size, uint16_t *type);
size_t dns_reply_aaaa(const uint8_t *query, size_t length, const uint8_t address[16], uint32_t ttl,
                      uint8_t *out, size_t size);

/* Sockets (SOCKET_UDP, SOCKET_TCP): descriptors that read() and write()
 * work on once connected. bind returns the port used; recvfrom stores the
 * sender's address and port (either may be 0) and waits at most
 * timeout_ms (0: as long as it takes); accept the peer's. -1 and errno on
 * errors. */
int ritteros_socket(int type);
int ritteros_bind(int fd, uint16_t port);
int ritteros_connect(int fd, uint32_t ip, uint16_t port);
ssize_t ritteros_sendto(int fd, const void *data, size_t size, uint32_t ip, uint16_t port);
ssize_t ritteros_recvfrom(int fd, void *data, size_t size, uint32_t *ip, uint16_t *port,
                          uint32_t timeout_ms);
int ritteros_listen(int fd, int backlog);
int ritteros_accept(int fd, uint32_t *ip, uint16_t *port);
/* Local (Unix-domain) stream sockets: two programs on one machine talk over
 * a named connection (SOCKET_LOCAL). The name is a path-like string.
 * bind_local/connect_local act on a socket made with ritteros_socket;
 * local_listen and local_connect make the socket too (a listening or a
 * connected descriptor). accept on a local listener returns a new
 * descriptor (its address is 0). read/write/close as usual. */
int ritteros_bind_local(int fd, const char *path);
int ritteros_connect_local(int fd, const char *path);
int ritteros_local_listen(const char *path, int backlog);
int ritteros_local_connect(const char *path);
/* The user at the other end of a connected local socket: the client's user
 * for a server, the user who bound the name for a client (a name says
 * nothing about whose it is). The user ID, or -1 with errno (ENOTCONN). */
int ritteros_peer_uid(int fd);
/* Shared buffers (SYS_SHARE): create one (a descriptor), map it into this
 * program (its address, or 0 on error), ask its size, and pass it to / take
 * it from another program over a local socket. The buffer lives until every
 * descriptor that holds it is closed. */
int ritteros_share_create(size_t size);
void *ritteros_share_map(int fd);
long ritteros_share_size(int fd);
int ritteros_share_send(int socket_fd, int buffer_fd);
int ritteros_share_recv(int socket_fd);
/* The screen (SYS_DISPLAY), for the window server windowd: take over the
 * screen (parks the text console, root only), give it back, ask its size and
 * format, and copy a rectangle of native pixels to it. */
int ritteros_display_acquire(void);
int ritteros_display_release(void);
int ritteros_display_info(struct sys_display *info);
int ritteros_display_flush(const void *pixels, uint32_t stride, uint32_t x, uint32_t y,
                           uint32_t w, uint32_t h);
int ritteros_display_event(struct sys_input_event *event, uint32_t timeout_ms);
/* (65e-34) The screen's mode and refresh, the monitor (its EDID) and the
 * modes the firmware can set; anyone may ask. */
int ritteros_display_modes(struct sys_display_modes *modes);
/* (66b-2) Show output's monitor at width x height (refresh in millihertz,
 * 0: the monitor's usual rate for that size), a mode it lists; root or the
 * user at the machine's screen, not while a program owns the screen
 * (EBUSY). said (said_size bytes, may be 0) gets what was set or why not. */
int ritteros_display_set_mode(const char *output, uint32_t width, uint32_t height,
                              uint32_t refresh_mhz, char *said, size_t said_size);
/* (66c-3) Wait (at most timeout_ms; 0: a second) for the screen's next
 * vertical blank: 1 with its frame number in *frame (may be 0), 0 if none
 * came, -1 (ENODEV) where no graphics driver tells it. */
int ritteros_display_vblank(uint64_t *frame, uint32_t timeout_ms);
/* (66d-1) Show screen buffer `buffer` (0: the console's) from the next
 * vertical blank: 1 once it shows (its frame in *frame, may be 0), 0 if it
 * did not in time, -1 with errno (ENODEV: no driver flips; EPERM: not the
 * screen's owner; EINVAL: no such buffer). */
int ritteros_display_flip(int buffer, uint64_t *frame);
/* (66d-2) The screen buffers mapped into the screen's owner (write only:
 * reading them is very slow), their count, size and stride; the same
 * places after a mode change (ask again for the size). -1 with errno
 * (ENODEV: no driver has them; EPERM: not the screen's owner). */
int ritteros_display_buffers(struct sys_display_buffers *buffers);
#define NETWORK_CONFIG "/system/config/network"
/* The firewall (user/lib/guard.c): its rules as text, one per line of
 * GUARD_FILE (read top to bottom, "#" starts a comment):
 *   in = drop | allow        what comes in when no rule matches
 *   out = drop | allow       what goes out when none does
 *   forward = drop | allow   what is forwarded between cards when none does
 *   ACTION DIRECTION [PROTOCOL] [PORT | PORT-PORT] [from ADDRESS[/N]] [to ADDRESS[/N]] [on ethN]
 *   limit DIRECTION ... N/minute
 * ACTION allow or drop (limit: allow at most N new connections a minute,
 * its rate the last word); DIRECTION in, out or forward; PROTOCOL tcp,
 * udp or icmp (a port needs tcp or udp: coming in the local one, going out
 * or forwarded the remote one). A forward rule names its cards with
 * "in ethN" (incoming) and "out ethN" (outgoing) instead of "on ethN".
 * guard_parse adds one line to g (rules, defaults): 0, or
 * -1 with the reason in why; guard_read a whole file (missing defaults:
 * in = drop, out = allow, forward = drop): 0, or -1 with "line N: reason" in why (errno:
 * the file could not be read); guard_format writes a rule back in words.
 * ritteros_guard: the system call SYS_GUARD (GUARD_LOAD, GUARD_OFF,
 * GUARD_STATUS). */
#define GUARD_FILE "/system/config/guard"
int guard_parse(const char *line, struct sys_guard *g, char *why, size_t size);
int guard_read(const char *path, struct sys_guard *g, char *why, size_t size);
void guard_format(const struct sys_guard_rule *rule, char *out, size_t size);
int ritteros_guard(int op, struct sys_guard *g);
/* (64e) Both families: guard_read6 reads the file into g (IPv4's table:
 * the rules without an address and those with an IPv4 one) and g6
 * (IPv6's: without one or with an IPv6 one, each with its number in the
 * file); guard_format6 writes an IPv6 rule back in words; ritteros_guard6
 * is SYS_GUARD on IPv6's table (GUARD_SET6, GUARD_GET6). guard_parse
 * checks an IPv6 rule but adds it nowhere. */
int guard_read6(const char *path, struct sys_guard *g, struct sys_guard6 *g6, char *why, size_t size);
void guard_format6(const struct sys_guard6_rule *rule, char *out, size_t size);
int ritteros_guard6(int op, struct sys_guard6 *g);

/* Logs (SYS_LOG, root only; user/lib/syscalls.c): the log this process's
 * console output (and its children's) also goes to ("": none); a text
 * added to a log; what the logs hold, taken (struct sys_log_record + text,
 * one after another): the bytes, or -1. */
#define LOGS_DIR "/system/logs"
int ritteros_log_as(const char *name);
/* A firmware variable (SYS_EFIVAR: EFIVAR_GET, EFIVAR_SET; root only). */
int ritteros_efivar(int op, struct sys_efivar *v);
int ritteros_log_write(const char *name, const char *text);
int ritteros_log_take(void *buffer, size_t size);

/* The elves: programs elvesd keeps running, one file each (user/bin/elvesd.c). */
#define ELVES_DIR "/system/config/elves"
#define WIFI_DIR "/system/config/wifi"               /* the wireless link's settings (24e) */
#define WIFI_NETWORKS "/system/config/wifi/networks" /* one file per saved network (root) */
#define WIFI_RADIO "/system/config/wifi/radio"       /* the radio itself: country (24f) */
/* elvesd's process ID, or 0 if it does not run; elvesd_start starts it
 * from bin_dir (/system/<arch>/bin) if it does not run: its process ID, or
 * -1 (user/lib/elves.c). */
int elvesd_pid(void);
int elvesd_start(const char *bin_dir);
/* The process ID of the first running process with that program name, or
 * 0; logs_flush asks logd to write what it keeps and waits (up to 3 s)
 * until it has (poweroff, reboot). */
int process_named(const char *name);
void logs_flush(void);

/* The system's settings: one file per topic in CONFIG_DIR, written as
 * "key = value" lines (spaces around both ignored; "#" starts a comment
 * line). config_get puts key's value from the file at path into value:
 * 1, 0 if the file has no such key, -1 (errno set) if it cannot be read
 * (user/lib/config.c). */
#define CONFIG_DIR "/system/config"
int config_get(const char *path, const char *key, char *value, size_t size);

/* Users (user/lib/users.c, with <pwd.h>). Names: 1-31 of a-z 0-9 _ -,
 * not starting with a digit or '-'. Passwords are stored in
 * PASSWORDS_FILE (root only) as PBKDF2-HMAC-SHA256 with a random salt. */
#define USER_NAME_MAX 31
#define PASSWORDS_FILE "/system/config/passwords"
#define PASSWORD_SALT 16
#define PASSWORD_ITERATIONS 100000
bool user_name_valid(const char *name);
/* Replace (line 0: remove) user's line in one of the two files, which then
 * gets the given mode; 0, or -1 with errno set. */
int user_file_set(const char *path, const char *user, const char *line, unsigned mode);
int password_set(const char *user, const char *password);
/* 1: the password is right, 0: wrong (or none set), -1: cannot read. It
 * takes as long for a user without a password. */
int password_check(const char *user, const char *password);
bool password_exists(const char *user);
/* (68e) sudo's memory (SYS_SESSION_STAMP) on this session's leader, for
 * the caller's real user: STAMP_AGE its age in seconds, STAMP_SET now
 * (root only), STAMP_FORGET gone; -1 with errno (ENOENT: none, EPERM). */
int ritteros_session_stamp(int op);

/*
 * The program's entry in the shell's help: PROGRAM_HELP("ls [PATH]...",
 * "list directories") once in its source. It goes into the ELF section
 * .ritteros.help (a tab between the two texts), which is kept in the file
 * but not loaded (user/user.ld); sh reads it from there.
 */
#define PROGRAM_HELP(usage, description)                                        \
    __attribute__((section(".ritteros.help"), used))                           \
    static const char ritteros_program_help[] = usage "\t" description

#endif
