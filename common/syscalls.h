/*
 * System calls: how programs ask the kernel for something. Shared by the
 * kernel (kernel/process.c) and the programs' library (user/lib).
 *
 *   x86-64:  number in RAX, arguments in RDI, RSI, RDX, R10, R8, R9;
 *            instruction "syscall"; result in RAX (RCX and R11 are lost)
 *   AArch64: number in X8, arguments in X0-X5; instruction "svc #0";
 *            result in X0
 *
 * The meaning follows Unix: a negative result is an error, -ERR_*. The
 * numbers are RitterOS's own, the same on both architectures.
 */
#ifndef RITTEROS_SYSCALLS_H
#define RITTEROS_SYSCALLS_H

#include <stdint.h>

#define SYS_EXIT        0       /* exit(code): end the program; code 0-255 */
#define SYS_WRITE       1       /* write(fd, buffer, size): bytes written */
#define SYS_READ        2       /* read(fd, buffer, size): bytes read, 0 at the end */
#define SYS_OPEN        3       /* open(path, flags, mode): a new file descriptor; mode (with
                                 * OPEN_MODE in flags, 52b) a created file's permissions,
                                 * less the process's umask (52h); without it 0644 */
#define SYS_CLOSE       4       /* close(fd): 0 */
#define SYS_SPAWN       5       /* spawn(path, argv, flags, fds, ids): the new child's ID */
#define SYS_WAIT        6       /* wait(pid or -1, &status, flags): the ended child's ID */
#define SYS_KILL        7       /* kill(pid, signal): 0 */
#define SYS_GETPID      8       /* getpid(): the caller's process ID */
#define SYS_SLEEP       9       /* sleep(milliseconds): 0 */
#define SYS_STAT        10      /* stat(path, struct sys_stat *): 0 */
#define SYS_READDIR     11      /* readdir(fd, struct sys_dirent *): 1, or 0 at the end */
#define SYS_CHDIR       12      /* chdir(path): 0 */
#define SYS_GETCWD      13      /* getcwd(buffer, size): length */
#define SYS_GETPGRP     14      /* getpgrp(): the caller's process group */
#define SYS_FOREGROUND  15      /* foreground(group or 0): the terminal's foreground group */
#define SYS_SIGNAL      16      /* signal(signal, action): 0 */
#define SYS_SYSINFO     17      /* sysinfo(struct sys_info *): 0 */
#define SYS_POWER       18      /* power(POWER_OFF or POWER_REBOOT): returns only if it failed;
                                   power(POWER_TABLE, index, buffer, size) (37b-1),
                                   power(POWER_NAMES, buffer, size) (37b-2),
                                   power(POWER_EVAL, path, buffer, size) (37b-3) */
#define SYS_REPORT      19      /* report(REPORT_*, buffer, size): the report's full length */
#define SYS_SEEK        20      /* seek(fd, offset, SEEK_*): the new position */
#define SYS_THREAD_CREATE 21    /* thread_create(start, fn, arg, stack size or 0): the new
                                 * thread's number (54d: the size, up to USER_STACK_MAX
                                 * bytes, 0 for USER_STACK_SIZE) */
#define SYS_THREAD_EXIT 22      /* thread_exit(value): end the calling thread */
#define SYS_THREAD_JOIN 23      /* thread_join(number, &value): 0 once it has ended */
#define SYS_THREAD_SELF 24      /* thread_self(): the caller's thread number (0: main) */
#define SYS_SNAPSHOT    25      /* snapshot(SNAPSHOT_*, name, buffer, size): see below */
#define SYS_ATTR        26      /* attr(ATTR_*, path, key, buffer, size): see below */
/* Changing the file system: each call is written to the disk at once
 * (one commit), all or nothing. Paths in /.snapshots are read-only. */
#define SYS_MKDIR       27      /* mkdir(path, mode): 0; mode: its rwxrwxrwx bits */
#define SYS_RMDIR       28      /* rmdir(path): 0; the directory must be empty */
#define SYS_UNLINK      29      /* unlink(path): 0; a file, not a directory */
#define SYS_RENAME      30      /* rename(from, to): 0; to must not exist */
#define SYS_COPY        31      /* copy(from, to): 0; an instant copy (shares the data) */
/* Users: every process has a user and a group (IDs; 0 is root, who may do
 * everything). Files have an owner, a group and rwxrwxrwx permissions. */
#define SYS_GETUID      32      /* getuid(): the caller's (real) user ID: who started it */
#define SYS_GETGID      33      /* getgid(): its group ID */
#define SYS_SETUID      34      /* setuid(uid): 0; root only (or its own ID) */
#define SYS_SETGID      35      /* setgid(gid): 0; root only (or its own ID) */
#define SYS_CHMOD       36      /* chmod(path, mode): 0; the owner or root */
#define SYS_CHOWN       37      /* chown(path, uid, gid): 0; root only */
#define SYS_RANDOM      38      /* random(buffer, size): size (at most 256) random bytes */
#define SYS_ECHO        39      /* echo(on): 0; the terminal shows typed text, or not
                                 * (passwords); the foreground job only; on again
                                 * when the program ends */
#define SYS_TERMINAL    40      /* terminal(TERMINAL_*, argument): see below */

/*
 * The terminal for full-screen programs (an editor):
 *   TERMINAL_RAW   argument 1: reads return the bytes as typed, at least
 *                  one, without echo or line editing (keys like the arrows
 *                  arrive as VT100 escape sequences, ESC [ A); 2: the same
 *                  for a program that edits a line itself (the shell): the
 *                  console shows its line again after a message, as it
 *                  does its own; 0: lines again. The foreground job only;
 *                  lines again when the program ends.
 *   TERMINAL_SIZE  the text area: rows << 16 | columns (80x24 without a
 *                  screen)
 * Output understands a few VT100 sequences: ESC [ row ; col H, ESC [ J
 * (0: to the end of the screen, 2: all), ESC [ K (0: to the end of the
 * line, 2: all of it), ESC [ A/B/C/D (cursor moves), ESC [ m (0 normal,
 * 1 bold, 7 reverse, 22, 27, 30-37 and 90-97 colors, 39), ESC [ ? 25 l/h
 * (cursor hidden/shown).
 */
#define TERMINAL_RAW    1
#define TERMINAL_SIZE   2
#define TERMINAL_ISATTY 3       /* terminal(TERMINAL_ISATTY, fd): 1 if fd is a terminal, else 0 */

/*
 * Devices: disks and their partitions, as /dev lists them. /dev/NAME is
 * root's only, read and written as raw bytes (a byte position like a
 * file's). Writing is refused where a mounted file system lies (EBUSY: a
 * mounted partition cannot be opened for writing, its blocks on the disk
 * not written); closing a written device reads the disk's partition table
 * again (new RitterFS partitions are mounted).
 */
#define SYS_DEVICE      41      /* device(index, struct sys_device *): 1, or 0 past the last */
#define SYS_MOUNT       42      /* mount(device, path or 0, buffer, size): 0; the path used
                                 * (0: /volumes/<label>) into buffer; root only */
#define SYS_UNMOUNT     43      /* unmount(path or device): 0; root only; EBUSY while a
                                 * program has a file open there or its directory there */
#define SYS_TIME        44      /* time(): nanoseconds since 1970-01-01 UTC (0: unknown,
                                 * e.g. with noruntime) */
/*
 * The network. Addresses are IPv4, in host byte order (10.0.2.15 is
 * 0x0A00020F).
 */
#define SYS_NETSET      45      /* netset(struct sys_netset *): 0; the card's IPv4
                                 * settings (address 0: none); root only */
#define SYS_PING        46      /* ping(address, id << 16 | seq, size, timeout ms): the
                                 * round trip of an ICMP echo in microseconds */
/* 47 was resolve(name): DNS is the elf dnsd since step 44c */
/* Sockets: file descriptors for network conversations (read and write
 * work on connected ones: TCP's stream, UDP's datagrams to and from the
 * peer; close ends them). Ports and addresses as numbers, no structs. */
#define SYS_SOCKET      48      /* socket(SOCKET_UDP, SOCKET_TCP or SOCKET_LOCAL; | SOCKET_INET6):
                                 * a descriptor */
#define SYS_BIND        49      /* bind(fd, port or 0): 0; the port used. A local socket:
                                 * bind(fd, name, length) names the listener (0 or ERR_*) */
#define SYS_CONNECT     50      /* connect(fd, address, port): 0 (TCP: when connected). A local
                                 * socket: connect(fd, name, length) to a listener's name */
#define SYS_SENDTO      51      /* sendto(fd, buffer, size, address, port): bytes sent */
#define SYS_RECVFROM    52      /* recvfrom(fd, buffer, size, &from, timeout ms or 0):
                                 * bytes; from: address << 16 | port (a uint64_t) */
#define SYS_LISTEN      53      /* listen(fd, backlog): 0 (TCP) */
#define SYS_ACCEPT      54      /* accept(fd, &from): a new descriptor for a connection */

#define SYS_PTY         55      /* a pseudo-terminal: PTY_OPEN (int fds[2]: master, slave),
                                   PTY_SIZE (fd, rows, cols) */
#define PTY_OPEN        1
#define PTY_SIZE        2
#define SYS_BINDCARD    57      /* bindcard(fd, "eth0"): 0; a UDP socket of that card
                                 * only, before bind: it receives what arrives on
                                 * the card, and sends broadcasts out of it (also
                                 * without an address); root only */
#define SYS_NETINFO     58      /* netinfo(struct sys_netinfo *): 0; a card's MAC,
                                 * link and settings */
#define SYS_GUARD       59      /* guard(GUARD_*, struct sys_guard *): the firewall,
                                 * see below */
#define SYS_LOG         60      /* log(LOG_*, ...): the logs, see below; root only */
#define SYS_EFIVAR      61      /* efivar(EFIVAR_GET|EFIVAR_SET, struct sys_efivar *): a
                                 * firmware variable, through UEFI's runtime services
                                 * (boot entries); root only, see below */
#define SYS_FORWARD     56      /* forward(FORWARD_*): forwarding packets between network
                                   cards; ON and OFF root only; the state (1: on) */
#define FORWARD_OFF     0
#define FORWARD_ON      1
#define FORWARD_ASK     2
#define SYS_MASQUERADE  62      /* masquerade(MASQ_*, char *name): mark the private cards for
                                   source NAT (49i); root only (net shows the state) */
#define MASQ_RESET      0       /* clear the private set */
#define MASQ_ADD        1       /* mark the card named *name private */
#define SYS_DEFAULT_ROUTE 102   /* default_route(DEFAULT_ROUTE_*, char *name) (50g): the cards
                                   whose gateway carries the default route, in order (the
                                   first with a link, an address and a gateway; none of them:
                                   the first other card with a gateway); root only (net shows
                                   them). A name may be a card not there now */
#define DEFAULT_ROUTE_RESET 0   /* no preference: the first card with a gateway */
#define DEFAULT_ROUTE_ADD   1   /* the card named *name next in the order */
/* IPv6's socket calls (64c): as their IPv4 forms, with struct sys_inaddr
 * (either family) for the address. A socket made with SOCKET_INET6 talks
 * to IPv6 and IPv4 peers (::ffff:a.b.c.d); the IPv4 calls on it see IPv4
 * peers' addresses and 0 for IPv6 ones. */
#define SYS_CONNECT6    103     /* connect6(fd, struct sys_inaddr *): 0 (TCP: when connected) */
#define SYS_SENDTO6     104     /* sendto6(fd, buffer, size, struct sys_inaddr * or 0 (the
                                   connected peer), 0, flags): bytes sent */
#define SYS_RECVFROM6   105     /* recvfrom6(fd, buffer, size, struct sys_inaddr * (out) or 0,
                                   timeout ms or 0, flags): bytes */
#define SYS_ACCEPT6     106     /* accept6(fd, struct sys_inaddr * (out) or 0): a descriptor */
#define SYS_SOCKNAME6   107     /* sockname6(fd, SOCKNAME_OWN or _PEER, struct sys_inaddr *):
                                   0; an IPv6 socket's own "any" address is :: */
#define SYS_NETSET6     109     /* netset6(struct sys_netset6 *): 0; a card's IPv6 settings
                                   (64b: address6, gateway6, dns6); root only. The same
                                   settings again change nothing (the addresses stay) */
#define NETSET6_AUTO    0       /* addresses from the network's routers (SLAAC); a card's
                                   start */
#define NETSET6_OFF     1       /* no IPv6 on the card */
#define NETSET6_FIXED   2       /* the settings' address (and gateway); routers ignored */
#define NETSET6_DHCP    3       /* (64g) addresses from DHCPv6 (the elf dhclient), none from
                                   the routers' prefixes; their router, routes, MTU kept */
#define SYS_NETLEASE6   111     /* netlease6(struct sys_netlease6 *): 0; (64g) an address and
                                   DNS server DHCPv6 gave a card (dhclient), or with
                                   NETLEASE6_DROP its end; root only. Checked for duplicates
                                   as any address; a card set to OFF or FIXED takes none */
#define NETLEASE6_DROP  1       /* the lease is over: its address and DNS server go */
#define SYS_SWAP        112     /* swap(SWAP_*, struct sys_swap *): 0 (35a) */
#define SWAP_STATUS     0       /* fill the struct: on, its pages, used, written, read */
#define SWAP_ON         1       /* turn swap on with /system/swap (made a fixed file first);
                                   root only; ERR_NOENT without the file, ERR_INVAL if it has
                                   holes, a part block or blocks shared with a snapshot */
#define SWAP_OFF        2       /* turn it off, reading the pages in it back first (35b);
                                   root only; ERR_NOMEM when they do not fit in memory */
#define SYS_TAP         113     /* tap(TAP_*, value) (38a): virtual network cards; root only */
#define TAP_ADD         0       /* a new tapN for user `value` (below 2^24; its /dev/tapN is
                                   that user's and root's): N; ERR_NOSPC when every
                                   tap0..tap63 is taken. (38c) With (N + 1) << 24 in `value`
                                   tapN itself (ERR_EXIST: it is there) */
#define TAP_REMOVE      1       /* remove tap number `value`: 0; ERR_NOENT, or ERR_BUSY while
                                   a program has its /dev/tapN open */
#define SYS_BRIDGE      114     /* bridge(BRIDGE_*, number, char *card) (38b): bridges; root only */
#define BRIDGE_ADD      0       /* the card named *card becomes a port of bridge brN (number
                                   -1: of a new bridge, with the card's MAC; (38c) -2 - N: of
                                   a new bridge brN itself, ERR_EXIST if it is there): N; ERR_NOENT (no
                                   such bridge or card), ERR_BUSY (a port already), ERR_INVAL
                                   (a bridge, lo), ERR_NOTSUP (it cannot take other machines'
                                   frames: Wi-Fi), ERR_NOSPC (br0..br7 or 16 ports taken) */
#define BRIDGE_REMOVE   1       /* the card leaves bridge brN; card 0: the bridge goes, every
                                   port leaving: 0; ERR_NOENT */
#define SYS_SESSION_STAMP 115   /* session_stamp(STAMP_*) (68e): sudo's memory, on the caller's
                                   session leader, for the caller's real user */
#define STAMP_AGE       0       /* its age in seconds; ERR_NOENT when there is none */
#define STAMP_SET       1       /* now (root only: sudo is setuid root): 0 */
#define STAMP_FORGET    2       /* gone (sudo -k): 0 */
#define SYS_GROW        117     /* grow(char *partition) (68j): the RitterFS mounted on the
                                   partition takes the room its partition grew by (disks
                                   grow): its block count of 4 KiB now (unchanged without
                                   new room); root only; ERR_NOENT (no such device),
                                   ERR_INVAL (no RitterFS mounted there), ERR_NOSPC, ERR_ROFS,
                                   ERR_IO */
#define SYS_UNAME       116     /* uname(struct sys_utsname *) (68i): 0; the system's name,
                                   release (VERSION), commit and machine */
#define SYS_NETINFO6    110     /* netinfo6(struct sys_netinfo6 *): 0; a card's IPv6
                                   addresses, router, DNS server and MTU (64d), by name
                                   or by place as netinfo */
#define SYS_PING6       108     /* ping6(struct sys_inaddr *, id << 16 | seq, size, timeout ms)
                                   (64a): as SYS_PING, to an IPv6 address (an IPv4 one as
                                   ::ffff:a.b.c.d too); a link-local or multicast one needs
                                   its card in `scope` (ERR_INVAL without) */
#define SYS_SHARE       63      /* shared memory (window system): share(SHARE_*, ...) */
#define SHARE_CREATE    0       /* share(SHARE_CREATE, size): a new buffer; its descriptor */
#define SHARE_MAP       1       /* share(SHARE_MAP, fd): map it into this program; the address */
#define SHARE_SIZE      2       /* share(SHARE_SIZE, fd): the size it was made with */
#define SHARE_SEND      3       /* share(SHARE_SEND, socket_fd, buffer_fd): pass it over a
                                 * local socket (fd-passing); 0 */
#define SHARE_RECV      4       /* share(SHARE_RECV, socket_fd): take a passed buffer; its
                                 * descriptor, or -ERR_AGAIN if none waits */
#define SHARE_MAX       (16 * 1024 * 1024)      /* a shared buffer's largest size */
#define SYS_DISPLAY     64      /* the screen (window system): display(DISPLAY_*, ...) */
#define DISPLAY_ACQUIRE 0       /* display(DISPLAY_ACQUIRE): park the console; this program owns
                                 * the screen (root, or the user at the console; one at a
                                 * time); 0, ERR_PERM or ERR_BUSY */
#define DISPLAY_RELEASE 1       /* display(DISPLAY_RELEASE): give the screen back to the console */
#define DISPLAY_INFO    2       /* display(DISPLAY_INFO, struct sys_display *): the size and format */
#define DISPLAY_FLUSH   3       /* display(DISPLAY_FLUSH, struct sys_display_flush *): copy a
                                 * rectangle of native pixels to the screen (owner only) */
#define DISPLAY_EVENT   4       /* display(DISPLAY_EVENT, struct sys_input_event *, timeout_ms):
                                 * the next pointer or keyboard event for the display owner
                                 * (1: one, 0: timed out, -ERR_*). While the display is owned,
                                 * the keyboard goes here instead of to the terminal. */
#define DISPLAY_MODES   5       /* (65e-34) display(DISPLAY_MODES, struct sys_display_modes *):
                                 * the screen's mode and refresh, the monitor (its EDID) and the
                                 * modes the firmware can set; anyone may ask */
#define DISPLAY_SET_MODE 6      /* (66b-2) display(DISPLAY_SET_MODE, struct sys_display_set_mode *):
                                 * show an output's monitor in a mode it lists; root or the user
                                 * at the machine's screen; a program owning the screen gets
                                 * INPUT_SCREEN (66b-5). `said` gets what was set or why not */
#define DISPLAY_VBLANK  7       /* (66c-3) display(DISPLAY_VBLANK, uint64_t *frame, timeout_ms):
                                 * wait for the screen's next vertical blank (1, the frame
                                 * number in *frame; 0: timed out; -ERR_NODEV: no driver tells
                                 * it); anyone may */
#define DISPLAY_BUFFERS 8       /* (66d-2) display(DISPLAY_BUFFERS, struct sys_display_buffers *):
                                 * the screen buffers mapped into the screen's owner (write-
                                 * combining: write, never read), how many, their size and
                                 * stride; the same places after a mode change (ask again for the
                                 * size); gone when it releases the screen. -ERR_NODEV where no
                                 * driver has them */
#define DISPLAY_FLIP    9       /* (66d-1) display(DISPLAY_FLIP, buffer, uint64_t *frame): show
                                 * screen buffer N (0: the console's) from the next vertical
                                 * blank; returns once it shows (1; its frame in *frame; 0: it
                                 * did not within 100 ms). The screen's owner only; -ERR_NODEV
                                 * where no driver flips. DISPLAY_BUFFERS (8) maps them (66d-2) */
#define SYS_PIPE        65      /* pipe(int fds[2]): a pipe; fds[0] reads, fds[1] writes. A read
                                 * gives 0 once every write end is closed, a write -ERR_PIPE once
                                 * every read end is closed. Binary-transparent. */
#define SYS_MMAP        66      /* mmap(addr, length, MMAP_* access, MMAP_* flags, fd, offset):
                                 * the mapping's address (see below) */
#define SYS_MUNMAP      67      /* munmap(addr, length): 0; the pages of mappings in the range go */
#define SYS_MPROTECT    68      /* mprotect(addr, length, MMAP_* access): 0; the mappings in the
                                 * range (all of it mapped) get that access (22f) */
#define SYS_MSYNC       69      /* msync(addr, length, MSYNC_* flags): 0; what was written through
                                 * the writable shared mappings in the range goes to the files
                                 * (22j; munmap and the end of the program do it too) */
#define MSYNC_ASYNC      1      /* (all three write back at once) */
#define MSYNC_INVALIDATE 2
#define MSYNC_SYNC       4
#define SYS_THREAD_POINTER 70   /* thread_pointer(address): 0; the calling thread's thread
                                 * pointer (x86 FS base, ARM TPIDR_EL0), where its per-thread
                                 * data is found (22i) */
#define SYS_WLAN        71      /* wlan(WLAN_* op, card name, buffer, length, value): a
                                 * wireless card's radio and link, for the elf wifid (24c;
                                 * root only): the result of the op, or -ERR_* */
#define SYS_PEER_UID    72      /* peer_uid(fd): the user at the other end of a connected
                                 * local socket (ERR_NOTCONN otherwise) */
#define SYS_GETEUID     73      /* geteuid(): the effective user ID, whose rights apply (a
                                 * setuid program's: its file's owner; 50b) */
#define SYS_SPAWN_ENV   74      /* spawn_env(path, argv, envp, flags, fds, ids): SYS_SPAWN
                                 * with an environment (22t: an array of "NAME=value"
                                 * strings ended by a null pointer, on the child's stack
                                 * after its arguments) */
#define SYS_CPUTIME     75      /* cputime(): the CPU time the calling program has run, all
                                 * its threads, in nanoseconds (22o: C's clock) */
#define SYS_SIGACTION   76      /* sigaction(signal, struct sys_sigaction *new or 0, *old or 0,
                                 * restorer): a signal's handler (22r; restorer: where a
                                 * handler returns to, the library's call of SYS_SIGRETURN) */
#define SYS_SIGRETURN   77      /* sigreturn(context): back from a handler to where the
                                 * program was (the registers the kernel kept at context) */
#define SYS_FUTEX       78      /* futex(address, FUTEX_* op, value, timeout_ns): the C
                                 * library's mutexes and condition variables (22y). WAIT:
                                 * if the 32-bit word at address holds value, sleep until
                                 * woken (0), timeout_ns passed (0: no limit; -ERR_TIMEDOUT)
                                 * or a signal (-ERR_INTR); -ERR_AGAIN if it held another.
                                 * WAKE: wake up to value of the program's threads waiting
                                 * there; how many. Words are the program's own (its
                                 * threads share them; never across programs). */

#define SYS_EXECVE      92      /* execve(path, argv, envp): the calling process runs another
                                 * program (55b): same process number, parent, group, session,
                                 * user, folder, umask and descriptors (FD_CLOEXEC ones closed),
                                 * the blocked and ignored signals kept, handled ones back to
                                 * the default, its other threads ended. Returns only on an
                                 * error, the old program untouched; one later (no memory)
                                 * ends the process (SIGKILL) */
#define SYS_FORK        93      /* fork() (56a): a new process, the caller's child, that is a
                                 * copy of it -- its memory (copy-on-write; MAP_SHARED memory
                                 * stays shared), descriptors (the same open files), folder,
                                 * umask, users, group, session, signal handlers and the
                                 * calling thread's blocked set; no pending signals, and only
                                 * the calling thread (same number). The child's ID, and 0 in
                                 * the child; shared buffers (SYS_SHARE) are not passed on */
#define SYS_GETPPID     94      /* getppid() (56b): the parent's process ID (0: the kernel's
                                 * child, or an orphan) */
#define SYS_SETPGID     99      /* setpgid(pid, group) (56b): pid (0: the caller) joins group
                                 * (0: its own ID, a new group) of its session; the caller or
                                 * a child that has not run execve (EACCES), never a session
                                 * leader (EPERM). group -1: the process's group (getpgid) */
#define SYS_SETSID      100     /* setsid() (56b): a new session and group led by the caller,
                                 * no terminal; EPERM if it leads a group. Its ID */
#define SYS_UTIMENS     101     /* utimens(path or 0, fd, struct sys_timespec times[2] or 0,
                                 * flags) (63m): a file's times, [0] the last access (taken,
                                 * not kept: RitterFS has none), [1] the last change; the path
                                 * (UTIMENS_NOFOLLOW: a link itself) or, path 0, the open fd.
                                 * nsec UTIMENS_NOW: now, UTIMENS_OMIT: left as it is; times 0:
                                 * both now. Now needs the owner, write permission or root
                                 * (EACCES); any other time the owner or root (EPERM) */
#define SYS_SYMLINK     95      /* symlink(target, path) (57a): a symbolic link at path (the
                                 * name itself, never followed) naming target, 1-255 bytes,
                                 * kept as given; EPERM where there are none (FAT, exFAT) */
#define SYS_READLINK    96      /* readlink(path, buffer, size): the bytes a symbolic link
                                 * names, not zero-terminated, at most size; how many.
                                 * EINVAL for anything but a link */
#define SYS_LSTAT       97      /* lstat(path, struct sys_stat *): stat of the name itself
                                 * (a link: FILE_TYPE_SYMLINK, its target's length) */
#define SYS_LINK        98      /* link(from, to): a second name for the object at from (a
                                 * link itself, not what it names), on the same volume (EXDEV);
                                 * not a folder (EPERM); the caller owns it, or may read and
                                 * write it, or is root (EPERM) */
#define SYS_THREAD_INTERRUPT 91 /* thread_interrupt(number, THREAD_INTERRUPT_*, entry): 0, ESRCH
                                 * (54c, pthread_cancel): a thread of the calling program.
                                 * WAKE: its waiting call returns ERR_INTR. ENTER: as well, on
                                 * its next way back to the program it runs entry(0) as if a
                                 * signal handler (its own stack, all signals blocked) */
#define THREAD_INTERRUPT_WAKE  0
#define THREAD_INTERRUPT_ENTER 1
#define SYS_FCNTL       90      /* fcntl(fd, FCNTL_*, arg): a descriptor's flags (53b): see below */
#define SYS_SHUTDOWN    87      /* shutdown(fd, SHUTDOWN_*): 0; a connection's sending or
                                 * receiving side ended, the descriptor kept (53a) */
#define SYS_SOCKNAME    88      /* sockname(fd, SOCKNAME_OWN or _PEER, struct sys_sockname *):
                                 * 0; a socket's own or its peer's address (53a) */
#define SYS_SOCKOPT     89      /* sockopt(fd, SOCKOPT_GET or _SET, SOCKOPT_* option,
                                 * uint64_t *value): 0 (53a) */
#define SYS_SIGPENDING  85      /* sigpending(uint64_t *out): the signals waiting that the
                                 * calling thread blocks (52i): 0 */
#define SYS_SIGSUSPEND  86      /* sigsuspend(const uint64_t *mask): block mask instead until a
                                 * signal it lets through comes (52i): -ERR_INTR, after that
                                 * signal's handler ran (then the old set again) */
#define SYS_UMASK       84      /* umask(mask): the old one; mask's bits (0777) are taken off
                                 * the modes of files and folders the process makes (52h);
                                 * passed on to the programs it starts, 022 at the start */
#define SYS_SIGPROCMASK 83      /* sigprocmask(SIGMASK_*, const uint64_t *set or 0, uint64_t *old
                                 * or 0): the calling thread's blocked signals (52f): 0. A
                                 * signal every thread blocks waits until one lets it through */
#define SYS_TERMIOS     82      /* termios(fd, TERMIOS_*, struct sys_termios *): a terminal's
                                 * modes (52e; tcgetattr, tcsetattr, tcflush): 0, -ERR_NOTTY
                                 * (not a terminal), -ERR_PERM (setting: not its foreground) */
#define SYS_POLL        81      /* poll(struct sys_pollfd *, count, timeout_ms): how many
                                 * descriptors have something to report (52d), waiting up to
                                 * timeout_ms (-1: no limit, 0: not at all) for the first;
                                 * -ERR_INTR on a signal, -ERR_INVAL past POLL_MAX */
#define SYS_FSTAT       80      /* fstat(fd, struct sys_stat *): 0; stat of an open descriptor
                                 * (52b): a file's as stat gives it, now; a terminal, pipe or
                                 * socket by its type (FILE_TYPE_CHAR, _FIFO, _SOCKET) */
#define SYS_DUP2        79      /* dup2(old, new): descriptor new refers to old's open file
                                 * too (one position, as POSIX's dup2; 52a), new closed first if
                                 * it was open; new -1: the lowest free descriptor (dup). The
                                 * new descriptor, -ERR_BADF (old not open, new out of range)
                                 * or -ERR_MFILE (none free). old == new: new, nothing done. */

/* termios (52e, termios.h). The kernel keeps what a program set and acts
 * on: ICANON (off: bytes as typed, no line editing, no echo; VMIN and
 * VTIME then say how a read waits), ECHO (lines only), ISIG (Ctrl-C a
 * signal, else a byte), ICRNL (without ICANON: Enter read as \n). The
 * other flags are kept but change nothing. Numbers are Linux's, speeds
 * plain numbers (BSD's). A process that changed ICANON, ECHO or ISIG gets
 * the old ones back when it ends. */
#define TERMIOS_GET     0
#define TERMIOS_SET     1       /* at once (TCSANOW; TCSADRAIN alike: output never waits) */
#define TERMIOS_SET_FLUSH 2     /* TCSAFLUSH: typed input not yet read is dropped first */
#define TERMIOS_FLUSH   3       /* tcflush: drop typed input not yet read */
#define TERMIOS_ISIG    0000001 /* c_lflag */
#define TERMIOS_ICANON  0000002
#define TERMIOS_ECHO    0000010
#define TERMIOS_ECHOE   0000020
#define TERMIOS_ECHOK   0000040
#define TERMIOS_ECHONL  0000100
#define TERMIOS_NOFLSH  0000200
#define TERMIOS_TOSTOP  0000400
#define TERMIOS_IEXTEN  0100000
#define TERMIOS_BRKINT  0000002 /* c_iflag */
#define TERMIOS_INPCK   0000020
#define TERMIOS_ISTRIP  0000040
#define TERMIOS_INLCR   0000100
#define TERMIOS_IGNCR   0000200
#define TERMIOS_ICRNL   0000400
#define TERMIOS_IXON    0002000
#define TERMIOS_OPOST   0000001 /* c_oflag */
#define TERMIOS_ONLCR   0000004
#define TERMIOS_CSIZE   0000060 /* c_cflag */
#define TERMIOS_CS8     0000060
#define TERMIOS_CREAD   0000200
#define TERMIOS_PARENB  0000400
#define TERMIOS_VINTR   0       /* c_cc */
#define TERMIOS_VQUIT   1
#define TERMIOS_VERASE  2
#define TERMIOS_VKILL   3
#define TERMIOS_VEOF    4
#define TERMIOS_VTIME   5       /* tenths of a second */
#define TERMIOS_VMIN    6
#define TERMIOS_VSTART  8
#define TERMIOS_VSTOP   9
#define TERMIOS_VSUSP   10
#define TERMIOS_NCCS    32

struct sys_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[TERMIOS_NCCS];
    uint32_t ispeed, ospeed;
};

/* poll (52d, poll.h): events asked for and found per descriptor, Linux's
 * numbers. POLLERR, POLLHUP and POLLNVAL (not an open descriptor) are
 * reported whether asked for or not; a negative fd is skipped. */
#define POLLIN          0x001   /* reading would not wait (data, or the end) */
#define POLLPRI         0x002   /* (never reported) */
#define POLLOUT         0x004   /* writing would not wait */
#define POLLERR         0x008   /* an error: writing to a pipe nobody reads */
#define POLLHUP         0x010   /* the other side is gone */
#define POLLNVAL        0x020
#define POLLRDNORM      0x040   /* as POLLIN */
#define POLLWRNORM      0x100   /* as POLLOUT */
#define POLL_MAX        256     /* descriptors in one call (52k: as many as a program has) */

struct sys_pollfd {
    int32_t fd;
    int16_t events, revents;
};

#define FUTEX_WAIT      0
#define FUTEX_WAKE      1

/* mmap (22a, sys/mman.h): access is MMAP_READ, _WRITE, _EXEC (never write
 * and execute at once; 0: a reservation, nothing mapped); flags are
 * MMAP_SHARED or MMAP_PRIVATE, with MMAP_ANONYMOUS (zeroed memory, no file)
 * and MMAP_FIXED (exactly at addr, replacing mappings there; addr then must
 * be in the mmap window). A read-only mapping of a RitterFS file shares one
 * copy of its pages with every other mapping of the same version of the
 * file; a private writable one shares them until a page's first write
 * copies it (22e). Anonymous pages are made when first touched; a file's
 * pages are read when mapped; touching a page past the file's end is
 * SIGBUS. Writable shared file mappings are not there yet (-ERR_NOTSUP). */
#define MMAP_READ       1
#define MMAP_WRITE      2
#define MMAP_EXEC       4
#define MMAP_SHARED     0x01
#define MMAP_PRIVATE    0x02
#define MMAP_FIXED      0x10
#define MMAP_ANONYMOUS  0x20
#define MMAP_WINDOW_BASE 0x0000300000000000ULL  /* mappings go here, above the shared buffers */
#define MMAP_WINDOW_TOP  0x0000700000000000ULL  /* and below the stacks */

/* An input event delivered to the display owner (DISPLAY_EVENT). */
#define INPUT_POINTER   1       /* the pointer moved or a button changed */
#define INPUT_KEY       2       /* a key: `key` is the byte the terminal would have received */
#define INPUT_WHEEL     3       /* the mouse wheel turned: `key` holds the steps as a signed byte,
                                 * negative up (away from the user); x, y the pointer (15o) */
#define INPUT_KEYCODE   4       /* a physical key pressed (buttons 1) or released (buttons 0):
                                 * `key` is its USB HID usage ID, modifiers too (15p) */
#define INPUT_SCREEN    5       /* (66b-5) the screen has a new mode (DISPLAY_SET_MODE): x, y
                                 * its width and height; DISPLAY_INFO gives the rest */
#define INPUT_BLANK     6       /* (37e) `key` 1: the screen goes off after a while without
                                 * input (draw nothing until 0); 0: it is on again */
struct sys_input_event {
    uint8_t type;               /* INPUT_POINTER or INPUT_KEY */
    uint8_t buttons;            /* INPUT_POINTER: buttons held (MOUSE_* bits) */
    uint8_t key;                /* INPUT_KEY: the byte */
    uint8_t modifiers;          /* INPUT_POINTER: keys held (INPUT_CTRL...) on any keyboard */
    uint16_t x, y;              /* INPUT_POINTER: the pointer, in pixels */
};
#define MOUSE_LEFT      0x01    /* buttons bits, as a USB/PS2 mouse reports them */
#define MOUSE_RIGHT     0x02
#define MOUSE_MIDDLE    0x04
#define INPUT_CTRL      0x01    /* modifiers bits: left or right Ctrl, Shift, Alt */
#define INPUT_SHIFT     0x02
#define INPUT_ALT       0x04

/* The screen's size and pixel format (DISPLAY_INFO). Pixels are 32-bit; a
 * color is (r << red_shift) | (g << green_shift) | (b << blue_shift). */
struct sys_display {
    uint32_t width, height;     /* visible pixels */
    uint32_t stride;            /* pixels from one row to the next */
    uint8_t bytes_per_pixel;    /* always 4 */
    uint8_t red_shift, green_shift, blue_shift;
};

/* A rectangle to copy to the screen (DISPLAY_FLUSH): `w` x `h` native pixels
 * at (x, y) on the screen, read from the program's buffer at `pixels`, whose
 * rows are `stride` bytes apart. */
struct sys_display_flush {
    uint64_t pixels;
    uint32_t stride;
    uint32_t x, y, w, h;
};

/* (65e-34) The screen as the firmware and the monitor describe it
 * (DISPLAY_MODES). Refresh rates in millihertz (59950 is 59.95 Hz), 0
 * where the monitor's EDID does not give one; without an EDID only `now`'s
 * size and the firmware's modes are known. */
#define DISPLAY_MODES_MAX 32
#define DISPLAY_EDID_MAX  512
struct sys_display_mode {
    uint32_t width, height, refresh_mhz;
};
struct sys_display_modes {
    struct sys_display_mode now;        /* the screen's mode, its refresh as the EDID lists it */
    struct sys_display_mode native;     /* the monitor's preferred mode (its first timing) */
    char name[14];                      /* the monitor's name, "" if unknown */
    uint16_t edid_size;                 /* bytes in edid; 0: the firmware gave none */
    uint32_t mode_count;                /* the modes the firmware can set */
    struct sys_display_mode modes[DISPLAY_MODES_MAX];   /* refresh as the EDID lists the size */
    uint8_t edid[DISPLAY_EDID_MAX];     /* as the firmware gave it (base and extension blocks) */
};

/* (66d-2) DISPLAY_BUFFERS' answer: buffer k at address[k], width x height
 * pixels in the screen's format (DISPLAY_INFO), rows `stride` bytes apart. */
#define DISPLAY_BUFFERS_MAX 2
struct sys_display_buffers {
    uint32_t count;
    uint32_t width, height, stride;
    uint64_t address[DISPLAY_BUFFERS_MAX];
};

/* (66b-2) DISPLAY_SET_MODE's request and answer. */
struct sys_display_set_mode {
    char output[8];                     /* "hdmi0" (displays lists them) */
    uint32_t width, height;
    uint32_t refresh_mhz;               /* 0: the monitor's usual rate for that size */
    char said[128];                     /* (out) "hdmi0 now 1920x1080 at 60.000 Hz ...", or why not */
};

/* (53a) shutdown's sides, sockname's which, socket options (SOCKOPT_*:
 * the value a uint64_t; times in milliseconds, 0: no limit). A receive
 * or send that runs past its time is ERR_AGAIN. REUSEADDR, KEEPALIVE,
 * NODELAY and BROADCAST are kept and change nothing (a port is only
 * taken by a listener; segments go out at once; broadcasts are allowed);
 * ERROR reads 0, TYPE the SOCKET_* kind, the buffers 65536. */
/* (53b) fcntl: FCNTL_DUPFD (the lowest free descriptor from arg on;
 * _CLOEXEC marks it), GETFD/SETFD (FD_CLOEXEC: kept, nothing to do it
 * yet: spawn passes only 0-2 on), GETFL (OPEN_READ, _WRITE or _READ_WRITE,
 * OPEN_APPEND, OPEN_NONBLOCK), SETFL (OPEN_APPEND and OPEN_NONBLOCK; the
 * open file's, shared by its descriptors). Linux's numbers. sendto's and
 * recvfrom's last argument may hold SOCKET_DONTWAIT (MSG_DONTWAIT). */
#define FCNTL_DUPFD     0
#define FCNTL_GETFD     1
#define FCNTL_SETFD     2
#define FCNTL_GETFL     3
#define FCNTL_SETFL     4
#define FCNTL_DUPFD_CLOEXEC 1030
#define FCNTL_CLOEXEC   1
#define SOCKET_DONTWAIT 1
#define SHUTDOWN_READ   0
#define SHUTDOWN_WRITE  1
#define SHUTDOWN_BOTH   2
#define SOCKNAME_OWN    0
#define SOCKNAME_PEER   1
#define SOCKOPT_GET     0
#define SOCKOPT_SET     1
#define SOCKOPT_RCVTIMEO 1
#define SOCKOPT_SNDTIMEO 2
#define SOCKOPT_REUSEADDR 3
#define SOCKOPT_KEEPALIVE 4
#define SOCKOPT_NODELAY 5
#define SOCKOPT_BROADCAST 6
#define SOCKOPT_ERROR   7
#define SOCKOPT_TYPE    8
#define SOCKOPT_RCVBUF  9
#define SOCKOPT_SNDBUF  10
#define SOCKOPT_V6ONLY  11      /* (64d) an IPv6 socket that takes IPv6 peers only */
#define SOCKET_UDP      1
#define SOCKET_TCP      2
#define SOCKET_INET6    0x100   /* (64c) with SOCKET_UDP or SOCKET_TCP: an IPv6 socket */
#define SOCKET_LOCAL    3       /* a local (Unix-domain) stream socket, named by a path-like
                                 * string in a kernel registry (not a file on the disk) */
#define LOCAL_PATH_MAX  108     /* a local socket's name, with its NUL (like Unix sun_path) */

struct sys_sockname {           /* (53a) sockname's answer */
    uint32_t type;              /* SOCKET_* */
    uint32_t address;           /* IPv4, host order (0: any, or none yet) */
    uint16_t port;
    uint16_t reserved;
    char name[LOCAL_PATH_MAX];  /* a local socket's: its listener's name ("": none) */
};

#define NETSET_LEASED   1       /* netset flags: the address came from DHCP (lease) */

struct sys_netset {
    char name[8];               /* "eth0" */
    uint32_t address, prefix;   /* 10.0.2.15 and 24 */
    uint32_t gateway, dns;      /* 0: none */
    uint32_t dns_port;          /* 0: 53 (another for tests) */
    uint32_t flags;             /* NETSET_* */
    uint32_t lease;             /* NETSET_LEASED: the lease's seconds (shown by net) */
};

/*
 * The firewall (guard; roadmap item 12). The kernel checks every packet
 * that arrives for the machine on a card or leaves it on one (lo never;
 * forwarded packets not yet) against a table of rules: the first rule
 * that matches decides, else the default of its direction. Packets of a
 * connection a rule allowed (both ways, also its replies) pass without
 * the rules. The program guardd loads the table from /system/config/guard;
 * until then (GUARD_STATE_BOOT) what comes in is dropped.
 *   GUARD_LOAD    the table in *g (state, in_default, out_default, count,
 *                 rules); root only; its counters start at 0
 *   GUARD_OFF     no checks at all (everything passes); root only
 *   GUARD_STATUS  the table as loaded, with its counters and the last
 *                 packets dropped (newest first), into *g
 *   GUARD_SET6    (64e) IPv6's own table (struct sys_guard6: count, rules);
 *                 root only; the defaults are GUARD_LOAD's. Until it is
 *                 set, IPv6 packets are judged by the IPv4 table's rules
 *                 that name no address (64c's interim)
 *   GUARD_GET6    IPv6's table as set, its counters and drops, into *g6
 * The rules file holds both families' rules: one without addresses goes
 * into both tables, one with an IPv4 or IPv6 address into its family's.
 * Neighbour discovery, router advertisements, MLD and ICMPv6's errors
 * always pass; ICMPv6 counts as icmp.
 */
#define GUARD_LOAD      1
#define GUARD_OFF       2
#define GUARD_STATUS    3
#define GUARD_SET6      4
#define GUARD_GET6      5

#define GUARD_ALLOW     1       /* actions */
#define GUARD_DROP      2
#define GUARD_LIMIT     3       /* allow, at most limit new connections a minute */
#define GUARD_IN        1       /* directions */
#define GUARD_OUT       2
#define GUARD_FORWARD   3       /* a packet passing between two cards (49h) */
#define GUARD_ANY_CARD  0xFF
#define GUARD_WLAN_CARD 0x80    /* (50k) a card byte with this bit: wlanN (N in the low bits), else ethN */
/* (38b) What kind of card a card byte names (a rule's kinds[], a drop's
 * card_kind): */
#define GUARD_KIND_ETH  0       /* ethN or wlanN, as the card byte says (as before 38b) */
#define GUARD_KIND_TAP  1       /* tapN, N the card byte */
#define GUARD_KIND_BRIDGE 2     /* brN, N the card byte */
#define GUARD_RULES     64
#define GUARD_DROPS     20

#define GUARD_STATE_OFF     0   /* no checks */
#define GUARD_STATE_ON      1   /* the loaded table */
#define GUARD_STATE_BOOT    2   /* none loaded yet: what comes in is dropped */

struct sys_guard_rule {
    uint8_t action, direction;
    uint8_t protocol;           /* 0: any; 1 ICMP, 6 TCP, 17 UDP */
    uint8_t card;               /* N of ethN, GUARD_WLAN_CARD | N of wlanN, or GUARD_ANY_CARD
                                 * (forward: the incoming card) */
    uint16_t port_low, port_high;   /* the local port (in) or the remote one (out); 0, 0: any */
    uint32_t from, from_prefix; /* the sender's address and prefix length (0: any) */
    uint32_t to, to_prefix;     /* the receiver's */
    uint32_t limit;             /* GUARD_LIMIT: new connections a minute */
    uint8_t out_card;           /* forward: the outgoing card, or GUARD_ANY_CARD */
    uint8_t kinds[3];           /* (38b) GUARD_KIND_* of card ([0]) and out_card ([1]); [2] 0 */
    uint64_t packets;           /* status: packets it decided */
};

struct sys_guard_drop {
    uint64_t time;              /* nanoseconds since 1970 (0: the clock is unknown) */
    uint32_t from, to;
    uint16_t port;              /* the local port (in) or the remote one (out) */
    uint8_t direction, protocol, card;
    uint8_t rule;               /* 1-based, or 0: the default */
    uint16_t card_kind;         /* (38b) GUARD_KIND_* of card */
};

/* (64e) IPv6's rules: as sys_guard_rule's, with 16-byte addresses. */
struct sys_guard6_rule {
    uint8_t action, direction;
    uint8_t protocol;           /* 0: any; 1 ICMP (ICMPv6), 6 TCP, 17 UDP */
    uint8_t card;               /* as sys_guard_rule's */
    uint16_t port_low, port_high;
    uint8_t from[16], to[16];   /* network order */
    uint8_t from_prefix, to_prefix;     /* 0-128; 0: any */
    uint8_t out_card;
    uint8_t number;             /* its place among the file's rules, 1 the first */
    uint8_t both;               /* 1: it names no address, so the IPv4 table has it too */
    uint8_t kinds[3];           /* (38b) as sys_guard_rule's */
    uint32_t limit;
    uint64_t packets;           /* status: IPv6 packets it decided */
};

struct sys_guard6_drop {
    uint64_t time;
    uint8_t from[16], to[16];
    uint16_t port;
    uint8_t direction, protocol, card;
    uint8_t rule;               /* the rule's number in the file, or 0: the default */
    uint16_t card_kind;         /* (38b) GUARD_KIND_* of card */
};

struct sys_guard6 {
    uint16_t count;             /* rules */
    uint16_t reserved;
    uint32_t drop_count;        /* status: entries in drops */
    uint64_t in_allowed, in_dropped, out_allowed, out_dropped;  /* status: by the defaults */
    struct sys_guard6_rule rules[GUARD_RULES];
    struct sys_guard6_drop drops[GUARD_DROPS];
};

struct sys_guard {
    uint32_t state;             /* GUARD_STATE_* (status) */
    uint8_t in_default, out_default;    /* GUARD_ALLOW or GUARD_DROP */
    uint16_t count;             /* rules */
    uint64_t in_allowed, in_dropped, out_allowed, out_dropped;  /* status: by the defaults */
    uint64_t connection_packets;    /* status: passed as part of an allowed connection */
    uint64_t fwd_allowed, fwd_dropped;  /* status: forwarded packets by the default (49h) */
    uint32_t drop_count;        /* status: entries in drops */
    uint8_t forward_default;    /* GUARD_ALLOW or GUARD_DROP, for forwarded packets (49h) */
    uint8_t reserved[3];
    struct sys_guard_rule rules[GUARD_RULES];
    struct sys_guard_drop drops[GUARD_DROPS];
};

/*
 * Logs: the kernel keeps lines in memory, one ring per log, each line
 * with its time in front, until the elf logd takes them and writes them
 * to /system/logs/NAME.log. "kernel" holds the kernel's own messages.
 *   LOG_AS     log(LOG_AS, name): what the caller (and the children it
 *              starts from now on) print on the console also goes to the
 *              log name ("": none). elvesd names its elves' logs so.
 *   LOG_WRITE  log(LOG_WRITE, name, text): add text to the log name
 *   LOG_TAKE   log(LOG_TAKE, buffer, size): what the logs hold, taken, as
 *              struct sys_log_record + its text, one after another; the
 *              bytes used
 */
#define LOG_AS          1
#define LOG_WRITE       2
#define LOG_TAKE        3
#define LOG_NAME_SIZE   16

struct sys_log_record {
    char name[LOG_NAME_SIZE];
    uint32_t length;            /* bytes of text that follow */
    uint32_t lost;              /* bytes dropped before them (the log was full) */
};

/*
 * Firmware variables (SYS_EFIVAR, 48c): GET fills attributes, size and data
 * (ERR_NOENT: none of that name; ERR_2BIG: more than EFIVAR_DATA_MAX bytes,
 * size says how many); SET writes them (size 0 deletes the variable).
 * ERR_NOSYS without runtime services (noruntime, or the firmware refused
 * SetVirtualAddressMap), ERR_IO for the firmware's other errors.
 */
#define EFIVAR_GET          1
#define EFIVAR_SET          2
#define EFIVAR_DATA_MAX     4096
#define EFIVAR_NON_VOLATILE 0x1     /* attributes: kept across restarts */
#define EFIVAR_BOOT_ACCESS  0x2     /* seen by the firmware's boot manager */
#define EFIVAR_RUNTIME      0x4     /* seen after ExitBootServices */

struct sys_efivar {
    char name[64];              /* ASCII: "BootOrder", "Boot0003" */
    uint8_t guid[16];           /* the vendor's GUID, as the firmware keeps it */
    uint32_t attributes;
    uint32_t size;              /* bytes of data */
    uint8_t data[EFIVAR_DATA_MAX];
};

/* An Internet address and port (64a; roadmap item 29, IPv6): the system
 * calls that came with IPv6 take this for both families, an IPv4 address
 * as ::ffff:a.b.c.d (family 2 or 10, as AF_INET and AF_INET6). */
struct sys_inaddr {
    uint16_t family;            /* 2 IPv4, 10 IPv6 */
    uint16_t port;              /* host byte order; 0 where there is none (ping) */
    uint32_t scope;             /* the card for a link-local or multicast address: its
                                 * index in the kernel's list + 1 (netinfo's index + 1,
                                 * as BSD's interface index); 0: none */
    uint8_t address[16];        /* network order, as on the wire */
};

struct sys_netset6 {
    char name[8];               /* "eth0" */
    uint32_t mode;              /* NETSET6_* */
    uint32_t prefix;            /* NETSET6_FIXED: the address's prefix length (1-128) */
    uint8_t address[16];        /* NETSET6_FIXED's address */
    uint8_t gateway[16];        /* NETSET6_FIXED's router (0: none) */
    uint8_t dns[16];            /* a DNS server instead of the router's (0: the router's) */
    uint32_t dns_port;          /* 0: 53 */
};

/* SYS_UNAME's answer (68i): POSIX's struct utsname, the same layout
 * (sys/utsname.h). Each field is a string. */
#define SYS_UTSNAME_LENGTH 65
struct sys_utsname {
    char sysname[SYS_UTSNAME_LENGTH];   /* "RitterOS" */
    char nodename[SYS_UTSNAME_LENGTH];  /* "ritteros" (no host names yet) */
    char release[SYS_UTSNAME_LENGTH];   /* the VERSION file's: "2026.11" */
    char version[SYS_UTSNAME_LENGTH];   /* the commit built from ("513e2a4", + changed), or "unknown" */
    char machine[SYS_UTSNAME_LENGTH];   /* "x86_64" or "aarch64" */
};

struct sys_netinfo6 {
    char name[8];               /* "eth0" (in); empty: the card at `index` (filled in) */
    uint16_t index;             /* (in) as sys_netinfo's; ERR_NODEV past the last */
    uint16_t mode;              /* NETSET6_* */
    uint32_t count;             /* addresses in use or being checked below */
    struct {
        uint8_t address[16];
        uint8_t prefix;
        uint8_t state;          /* 1 being checked for duplicates, 2 in use, 3 a duplicate */
        uint8_t origin;         /* 1 link-local, 2 from the router, 3 fixed */
        uint8_t reserved;
        uint32_t valid, preferred;  /* seconds left; 0xFFFFFFFF: for ever */
    } addresses[4];
    uint8_t router[16];         /* the default router (a router's or the gateway; 0: none) */
    uint8_t dns[16];            /* the DNS server: the settings', else the router's (0: none) */
    uint32_t dns_port;          /* 0: 53 */
    uint32_t mtu;               /* the router's (0: the card's) */
};

/* (64g) sys_netinfo6's mode also holds the router's flags (its last
 * advertisement): NETINFO6_MANAGED, addresses from DHCPv6; NETINFO6_OTHER,
 * other settings (the DNS server) from DHCPv6. */
#define NETINFO6_MANAGED    0x100
#define NETINFO6_OTHER      0x200

struct sys_netlease6 {
    char name[8];               /* "eth0" */
    uint32_t flags;             /* NETLEASE6_DROP */
    uint32_t valid, preferred;  /* the address's lifetimes in seconds (0xFFFFFFFF: for ever) */
    uint8_t address[16];        /* the leased address (0: only the DNS server: stateless) */
    uint8_t dns[16];            /* the DNS server DHCPv6 named (0: none) */
};

struct sys_netinfo {
    char name[8];               /* "eth0" (in); empty: the card at `index` (filled in) */
    uint8_t mac[6];
    uint16_t index;             /* with an empty name: the card's place in the kernel's
                                 * list, 0 first (in); ERR_NODEV past the last */
    uint32_t link_speed;        /* Mbit/s; 0: no link */
    uint32_t address, prefix, gateway, dns, dns_port;
    uint32_t lease;             /* seconds; 0: a fixed address (or none) */
};

/* Wireless cards (24c, kernel/wlan.c): the elf wifid scans, joins and
 * runs WPA's handshake through SYS_WLAN; the kernel carries the data (802.11
 * data frames to and from Ethernet frames, CCMP with the keys wifid sets)
 * and gives wifid the management frames and the EAPOL frames it receives. */
#define WLAN_INFO       1       /* buffer: struct sys_wlan_info (out) */
#define WLAN_CHANNEL    2       /* value: the channel to listen and send on */
#define WLAN_SEND       3       /* buffer: an 802.11 management frame (no FCS); the kernel
                                 * gives it its sequence number */
#define WLAN_RECEIVE    4       /* buffer: a struct sys_wlan_frame, then the bytes (out);
                                 * value: how long to wait (ms); its length, or -ERR_TIMEDOUT */
#define WLAN_JOIN       5       /* buffer: struct sys_wlan_join: the access point's frames are
                                 * taken (the link is up, or with WLAN_JOIN_SECURED once the
                                 * pairwise key is set); aid 0 before associating, then again
                                 * with the AID once associated (the card's firmware is told) */
#define WLAN_LEAVE      6       /* the link is down, the keys are gone */
#define WLAN_KEY        7       /* buffer: struct sys_wlan_key */
#define WLAN_EAPOL      8       /* buffer: the destination (6 bytes), then an EAPOL frame,
                                 * sent as a data frame (protected once a pairwise key is set) */
#define WLAN_COUNTRY    10      /* buffer: a two-letter country code (the firmware then
                                 * picks the channels and power it may use); -ERR_NOTSUP
                                 * when the card has no such setting */
#define WLAN_SCAN       9       /* start a full scan on a card whose driver scans in its
                                 * firmware (the MT7921): the beacons it hears arrive through
                                 * WLAN_RECEIVE like any management frame. 0, or -ERR_NOTSUP
                                 * when the driver has no scan of its own (wifid then tunes the
                                 * channels and listens itself, as for the test card).
                                 * buffer (optional): a network's name (up to 32 bytes) to ask
                                 * for by name (an active scan: hidden networks answer it) */
#define WLAN_SIGNAL     11      /* the joined access point's signal, averaged over the frames
                                 * it sent us, as -dBm (55: -55 dBm), 0 if not known (wifid
                                 * looks for a better access point when it is weak) */

#define WLAN_FRAME_MAX      2400    /* an 802.11 frame's bytes, at most */
#define WLAN_CHANNELS_MAX   64

struct sys_wlan_info {
    uint8_t mac[6];
    uint8_t joined;             /* 1: the link is up */
    uint8_t channel;            /* the channel now */
    uint8_t bssid[6];           /* the access point joined */
    uint16_t aid;
    uint8_t channels;           /* how many the card has, in channel_list */
    uint8_t reserved[3];
    uint8_t channel_list[WLAN_CHANNELS_MAX];
    uint32_t rx_management, rx_eapol, rx_data, rx_replayed, rx_bad_mic, rx_dropped;
    uint32_t tx_data, tx_management;
};

#define WLAN_FRAME_MANAGEMENT 1 /* an 802.11 management frame as received */
#define WLAN_FRAME_EAPOL      2 /* an EAPOL frame's bytes (from source) */
#define WLAN_FRAME_SCAN_DONE  3 /* a WLAN_SCAN has ended (no bytes) */
struct sys_wlan_frame {
    uint16_t kind;
    uint16_t length;            /* the bytes after this header */
    uint8_t channel;            /* where it was received */
    int8_t signal;              /* dBm, if the card says (else 0) */
    uint8_t source[6];          /* (EAPOL: the sender) */
    uint8_t protected_frame;    /* (management: it came protected and was opened) */
    uint8_t reserved[3];
};

#define WLAN_JOIN_MFP     1     /* management frames are protected (WPA3, 802.11w) */
#define WLAN_JOIN_SECURED 2     /* WPA: the link is up only once the pairwise key is set */
#define WLAN_JOIN_POWERSAVE 4   /* the card may doze between beacons (powersave = on in
                                 * /system/config/wifi/radio) */
struct sys_wlan_join {
    uint8_t bssid[6];
    uint16_t aid;
    uint8_t channel;
    uint8_t flags;              /* WLAN_JOIN_* */
    uint8_t reserved[2];
};

#define WLAN_KEY_PAIRWISE 1     /* CCMP-128, for frames to and from the access point */
#define WLAN_KEY_GROUP    2     /* CCMP-128, for group frames from it (index 1-3) */
#define WLAN_KEY_IGTK     3     /* BIP-CMAC-128, group management frames (index 4-5) */
struct sys_wlan_key {
    uint8_t kind;               /* WLAN_KEY_* */
    uint8_t index;
    uint8_t length;             /* 16 */
    uint8_t reserved;
    uint8_t rsc[8];             /* the receive counter to start from (PN, little-endian) */
    uint8_t key[32];
};

#define DEVICE_PARTITION 1      /* flags: a partition (else a disk) */
#define DEVICE_SYSTEM    2      /* the disk with the system partition, or that partition */
#define DEVICE_EMPTY     4      /* its first 12 KiB are all zero */
#define DEVICE_READ_ONLY 8      /* mounted, but read only (its check found problems) */

#define DEVICE_TABLE_NONE 0     /* a disk's partition table */
#define DEVICE_TABLE_GPT  1
#define DEVICE_TABLE_MBR  2

struct sys_device {
    char name[32];              /* "nvme0n1", "usbdisk0p2", ... (/dev/NAME) */
    char disk[32];              /* a partition's disk; "" for a disk */
    char kind[16];              /* "NVMe", "SATA", "USB", "partition" */
    char model[48];             /* a disk's own name for itself */
    uint64_t block_size, block_count;
    uint32_t flags;             /* DEVICE_* */
    uint32_t number;            /* a partition's entry in the table (1: the first) */
    uint32_t table;             /* a disk's DEVICE_TABLE_* */
    uint32_t partitions;        /* a disk's partitions */
    uint64_t first_usable;      /* a disk's blocks that partitions may use (GPT: */
    uint64_t last_usable;       /* from its header; MBR: 1 to the end) */
    uint64_t first_block;       /* a partition's first block on its disk */
    uint8_t type_guid[16];      /* a GPT partition's type */
    uint32_t mbr_type;          /* an MBR partition's type byte */
    char type_name[32];         /* the type's name if known, else "" */
    char part_name[40];         /* a GPT partition's name */
    char filesystem[16];        /* "RitterFS", "FAT32", "FAT16", "FAT12", "exFAT", "NTFS",
                                 * "ext", "APFS", or "" (none found) */
    char label[65];             /* the file system's label */
    uint64_t generation;        /* RitterFS: the newest valid superblock's */
    char mounted_at[64];        /* "/" for the system partition; "" if not mounted */
};

/*
 * Threads of a program: up to PROCESS_THREADS share its memory and files;
 * number 0 is the one that ran main. thread_create starts a thread at the
 * library's `start` routine with the stack pointer at a 16-byte frame
 * holding fn and arg (fn first); it calls fn(arg) and thread_exit with the
 * result. exit() from any thread, a fault or a signal ends all of them;
 * the last thread's thread_exit ends the program with exit code 0.
 */
#define PROCESS_THREADS 256

/* Reports: the kernel's text, as its rescue console prints it. */
#define REPORT_PROCESSES 1
#define REPORT_THREADS   2
#define REPORT_PCI       3
#define REPORT_USB       4
#define REPORT_DISKS     5      /* what the program disks prints */
#define REPORT_NET       6      /* what the program net prints */
#define REPORT_MEMORY    7      /* the page cache (22g): mem's second line */
#define REPORT_CPUS      8      /* (65b) the processor's cores: what the program cpus prints */
#define REPORT_DISPLAYS  9      /* (66a) the screen, graphics devices, outputs, monitors: displays */
#define REPORT_DRIVERS   10     /* (67a) the kernel's modules, loaded or not and why: drivers */
#define REPORT_POWER     11     /* (37a) what the firmware offers for power management: power */
#define REPORT_DISPLAY_SLEEP 12 /* (37g-5c-2) what the graphics drivers kept over the last sleep:
                                 * displays sleep */

/*
 * RitterFS snapshots of the file system the program runs from. Each one is
 * readable (read-only) at /.snapshots/<name>/...
 *   SNAPSHOT_CREATE: keep the current state under name; returns the
 *                    generation (commit) it shows
 *   SNAPSHOT_DELETE: delete it; returns the blocks that became free
 *   SNAPSHOT_LIST:   fill buffer with struct sys_snapshot entries (as many
 *                    as fit in size bytes); returns how many there are
 * Errors: ERR_EXIST, ERR_NOENT, ERR_INVAL (a bad name), ERR_ROFS, ERR_IO.
 */
#define SNAPSHOT_CREATE 1
#define SNAPSHOT_DELETE 2
#define SNAPSHOT_LIST   3

struct sys_snapshot {
    char name[64];              /* zero-terminated */
    uint64_t generation;
    uint64_t created;           /* nanoseconds since 1970 */
};

/*
 * Attributes of a file or directory: key=value pairs (keys 1-63 bytes
 * without '=', values up to 1024 bytes of any content).
 *   ATTR_GET:    the value into buffer (at most size bytes); returns its
 *                full length
 *   ATTR_SET:    set key to buffer[0..size); 0. Written to the disk at once
 *   ATTR_REMOVE: remove key; 0. Written to the disk at once
 *   ATTR_LIST:   the keys into buffer, each followed by a NUL (as many as
 *                fit); returns the bytes all of them need (key unused)
 * Errors: ERR_NOENT (no such file), ERR_NODATA (no such attribute),
 * ERR_INVAL (a bad key or a too long value), ERR_ROFS (in a snapshot).
 */
#define ATTR_GET        1
#define ATTR_SET        2
#define ATTR_REMOVE     3
#define ATTR_LIST       4
#define ATTR_KEY_MAX    63
#define ATTR_VALUE_MAX  1024

#define SEEK_SET        0       /* seek: from the start */
#define SEEK_CUR        1       /* from the current position */
#define SEEK_END        2       /* from the end */

#define SPAWN_NEW_GROUP 1       /* spawn flag: the child leads a new process group */
#define SPAWN_FOREGROUND 2      /* with SPAWN_NEW_GROUP: its group gets the terminal
                                 * before it starts (as foreground() would allow) */
#define SPAWN_NEW_SESSION 4     /* with SPAWN_NEW_GROUP: the child leads a new session
                                 * (it may hand the terminal to its jobs) */
#define SPAWN_AS        8       /* root only: the child runs as the user and group in
                                 * spawn's 5th argument (uid | gid << 32) */
#define SPAWN_JOIN_GROUP 16     /* (51a) the child joins the process group in the flags'
                                 * upper 32 bits (flags | (uint64_t)group << 32): one
                                 * of the caller's session, so a pipeline is one job
                                 * (Ctrl-C reaches all its commands); not with
                                 * SPAWN_NEW_GROUP */
/* spawn's fds: 0, or the caller's descriptors that become the child's
 * standard input, output and error (int32 each; -1: the caller's own fd
 * 0, 1 or 2). Without them the child gets the caller's 0, 1 and 2. */
#define WAIT_NO_HANG    1       /* wait flag: return 0 at once if no child has ended */

/* signal() actions (SYS_SIGNAL, the call before 22r's SYS_SIGACTION; kept
 * for older programs). SIGNAL_IGNORE (SIGINT and SIGTERM only there): the
 * signal does not end the process; it only interrupts a waiting system
 * call (-ERR_INTR). */
#define SIGNAL_DEFAULT  0
#define SIGNAL_IGNORE   1

#define POWER_OFF       0
#define POWER_REBOOT    1
#define POWER_TABLE     2       /* (37b-1) ACPI table number index (the DSDT, the FACS, then
                                   the XSDT's list) copied whole or in part to buffer: its full
                                   length, ERR_NOENT past the last; root only */
#define POWER_NAMES     3       /* (37b-2) the AML namespace's objects as text lines (power
                                   names) into buffer, at most size bytes with a 0: the full
                                   length; root only */
#define POWER_EVAL      4       /* (37b-3) the AML object at the path (a string) evaluated, its
                                   value as text lines (power eval) into buffer: the full
                                   length; ERR_NOENT for no such object, ERR_IO when it failed;
                                   root only */
#define POWER_SLEEP     5       /* (37g-4) sleep (ACPI S3) for args[1] seconds (1 to a day),
                                   woken by the RTC's alarm; (37g-7) 0: until woken (the power
                                   button): 0 once awake again, ERR_INVAL for more than a day, ERR_IO when it did not sleep (the kernel's
                                   log says why: a driver that cannot); root only */

/*
 * foreground(group): make group (of the caller's session) the terminal's
 * foreground job, the one that reads input and gets Ctrl-C. Allowed for the
 * session's leader (the shell) and for the foreground job itself.
 * foreground(0) only returns the current foreground group.
 */

#define FILE_TYPE_FILE  1
#define FILE_TYPE_DIR   2
#define FILE_TYPE_BLOCK 3       /* a disk or partition in /dev */
#define FILE_TYPE_CHAR  4       /* (52b, fstat) a terminal, a pseudo-terminal, a sound device */
#define FILE_TYPE_FIFO  5       /* (52b) a pipe's end */
#define FILE_TYPE_SOCKET 6      /* (52b) a socket */
#define FILE_TYPE_SYMLINK 7     /* (57a) a symbolic link (lstat, readdir) */

struct sys_stat {
    uint64_t id;                /* RitterFS object ID */
    uint64_t size;              /* bytes */
    uint32_t type;              /* FILE_TYPE_* */
    uint32_t links;
    uint64_t created, modified; /* nanoseconds since 1970 */
    uint32_t uid, gid;          /* owner */
    uint32_t mode;              /* permissions: rwxrwxrwx bits */
    uint32_t device;            /* which file system: 1 the system's, then each volume
                                 * (a mount point differs from its parent); 0 for /dev */
};

/* (63m) SYS_UTIMENS's times. */
struct sys_timespec {
    int64_t sec;                /* since 1970 */
    int64_t nsec;               /* 0-999999999, or: */
};
#define UTIMENS_NOW     ((1L << 30) - 1)
#define UTIMENS_OMIT    ((1L << 30) - 2)
#define UTIMENS_NOFOLLOW 1      /* flags: a symbolic link itself */

struct sys_dirent {
    uint64_t id;
    uint32_t type;              /* FILE_TYPE_* */
    uint32_t name_length;
    char name[256];             /* UTF-8, zero-terminated */
};

/* (35a) Swap: SYS_SWAP's SWAP_STATUS. Pages since the boot. */
struct sys_swap {
    uint64_t on;                /* 1: on */
    uint64_t pages;             /* slots in the swap file (pages of 4 KiB) */
    uint64_t used;              /* slots holding a page */
    uint64_t written, read;     /* pages written to it and read from it */
};

struct sys_info {
    uint64_t ticks;             /* timer ticks since the start */
    uint64_t ticks_per_second;
    uint64_t free_pages, usable_pages, page_size;
};

/*
 * How a process ended (wait's status), as on Unix: exit(code) gives
 * code << 8; a process stopped by a signal has the signal's number in the
 * low 7 bits. Faults are signals too: page faults and protection errors
 * SIGSEGV, undefined or privileged instructions SIGILL, division SIGFPE.
 */
#define SIGHUP          1       /* the terminal is gone (a pseudo-terminal's master closed) */
#define SIGINT          2       /* Ctrl-C */
#define SIGQUIT         3       /* (22r: these and the others below, Linux's numbers) */
#define SIGILL          4
#define SIGTRAP         5
#define SIGABRT         6       /* abort */
#define SIGBUS          7       /* touching a mapped file past its end (22e) */
#define SIGFPE          8
#define SIGKILL         9       /* cannot be handled or ignored */
#define SIGUSR1         10
#define SIGSEGV         11
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGTERM         15
#define SIGCHLD         17
#define SIGNALS         32      /* signals are 1 to 31 */
#define SIGMASK_BLOCK   0       /* sigprocmask (52f): add set, take it away, or set it */
#define SIGMASK_UNBLOCK 1
#define SIGMASK_SET     2

/* A signal's action for SYS_SIGACTION (22r): handler SIGNAL_DEFAULT (the
 * program ends), SIGNAL_IGNORE, or a function's address; mask: the signals
 * (bit n: signal n) blocked while it runs, besides the signal itself unless
 * SIGACTION_NODEFER; SIGACTION_RESETHAND: back to the default once used. */
struct sys_sigaction {
    uint64_t handler;
    uint64_t mask;
    uint32_t flags;
    uint32_t reserved;
};
#define SIGACTION_RESETHAND 1
#define SIGACTION_NODEFER   2
#define STATUS_EXITED(code)     (((code) & 0xFF) << 8)
#define STATUS_SIGNALED(sig)    ((sig) & 0x7F)

/* open() flags: one of the three access modes, and any of the others. A
 * file that was written is committed when it is closed (also at a
 * program's exit), and in between every 8 MiB. */
#define OPEN_READ       0
#define OPEN_WRITE      1
#define OPEN_READ_WRITE 2
#define OPEN_ACCESS     3       /* mask of the three above */
#define OPEN_CREATE     4       /* create the file if it does not exist */
#define OPEN_TRUNCATE   8       /* cut it to 0 bytes (when opened for writing) */
#define OPEN_APPEND     16      /* every write goes to the end */
#define OPEN_NONBLOCK   128     /* (53b) reads, writes, accept and connect never wait: EAGAIN
                                 * (connect: EINPROGRESS, then poll for POLLOUT) */
#define OPEN_EXCLUSIVE  64      /* (52j) with OPEN_CREATE: ERR_EXIST if the name is taken (O_EXCL) */
#define OPEN_MODE       32      /* (52b) the third argument is a created file's mode (open's
                                 * own: O_CREAT's mode argument); programs built before it
                                 * passed 0 there and get 0644 */

/* File descriptors every program starts with: the terminal. Reading it
 * gives a line at a time (edited and echoed by the kernel), including its
 * "\n"; Ctrl-D on an empty line is the end (read returns 0), Ctrl-C stops
 * the program. */
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_MAX          256     /* file descriptors per program (16 until 52k) */

/* Error numbers (as in Unix). */
#define ERR_NOENT       2       /* no such file or directory */
#define ERR_SRCH        3       /* no such process */
#define ERR_INTR        4       /* interrupted: the process is being stopped */
#define ERR_IO          5       /* input/output error (e.g. a damaged block) */
#define ERR_NXIO        6       /* (52c) no such device: /dev/tty without a terminal */
#define ERR_NOTTY       25      /* (52e) not a terminal (termios on a file) */
#define ERR_DEADLK      35      /* (54a, the library's) a thread would wait for itself */
#define ERR_LOOP        40      /* (57a) more than 40 symbolic links in one path */
#define ERR_NOTSOCK     88      /* (53a) a socket call on another kind of descriptor */
#define ERR_DESTADDRREQ 89      /* (53a, the library's) a datagram without an address */
#define ERR_MSGSIZE     90      /* (53a) a datagram too long */
#define ERR_NOPROTOOPT  92      /* (53a) no such socket option */
#define ERR_PROTONOSUPPORT 93   /* (53a) socket(): no such protocol for the family */
#define ERR_SOCKTNOSUPPORT 94   /* (53a) socket(): no such type */
#define ERR_AFNOSUPPORT 97      /* (53a) an address family RitterOS has not (AF_INET6) */
#define ERR_ADDRNOTAVAIL 99     /* (53a) an address that is not the machine's */
#define ERR_CONNABORTED 103     /* (53a) */
#define ERR_ISCONN      106     /* (53a) sendto with an address on a stream */
#define ERR_ALREADY     114     /* (53b) a connect still under way */
#define ERR_INPROGRESS  115     /* (53b) a connect without waiting: started */
#define ERR_2BIG        7       /* argument list too long */
#define ERR_NOEXEC      8       /* not a program for this CPU */
#define ERR_BADF        9       /* not an open file descriptor */
#define ERR_CHILD       10      /* no child to wait for */
#define ERR_AGAIN       11      /* try again: e.g. no free thread number */
#define ERR_NOMEM       12      /* out of memory */
#define ERR_ACCES       13      /* permission denied (by a file's permissions) */
#define ERR_FAULT       14      /* a pointer outside the program's memory */
#define ERR_EXIST       17      /* the name is already taken */
#define ERR_NODEV       19      /* no such device (e.g. no card at that place in the list) */
#define ERR_NOTDIR      20      /* a path component is not a directory */
#define ERR_ISDIR       21      /* reading a directory as a file */
#define ERR_PERM        1       /* not allowed */
#define ERR_INVAL       22      /* an invalid argument */
#define ERR_MFILE       24      /* too many open files */
#define ERR_ROFS        30      /* the file system is read-only (for programs, so far) */
#define ERR_NAMETOOLONG 36      /* a path that is too long */
#define ERR_NOSYS       38      /* no such system call */
#define ERR_NOTEMPTY    39      /* removing a directory that is not empty */
#define ERR_NOSPC       28      /* no space left on the disk */
#define ERR_NODATA      61      /* no such attribute */
#define ERR_BUSY        16      /* in use: a mounted device, a volume someone is in */
#define ERR_XDEV        18      /* on another file system (rename, instant copy) */
#define ERR_NOTSUP      95      /* the file system cannot do that (attributes on FAT) */
#define ERR_PIPE        32      /* writing to a connection the other side closed */
#define ERR_ADDRINUSE   98      /* the port is taken */
#define ERR_NETUNREACH  101     /* no route: no address, no link or no gateway */
#define ERR_CONNRESET   104     /* the other side reset the connection */
#define ERR_NOTCONN     107     /* not connected */
#define ERR_CONNREFUSED 111     /* nobody listens on that port */
#define ERR_NONAME      150     /* DNS: no such name (RitterOS's own number) */
#define ERR_TIMEDOUT    110     /* no answer in time */
#define ERR_HOSTUNREACH 113     /* no answer to ARP: the neighbour is not there */

#endif
