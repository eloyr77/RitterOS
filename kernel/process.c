#include <stddef.h>
#include "release.h"
#include "gpu.h"
#include "process.h"
#include "sleep.h"
#include "acpi.h"
#include "cpus.h"
#include "panic.h"
#include "percpu.h"
#include "arch.h"
#include "console.h"
#include "display.h"
#include "devices.h"
#include "mounts.h"
#include "elf.h"
#include "socket.h"
#include "shbuf.h"
#include "ipv4.h"
#include "ipv6.h"
#include "net.h"
#include "wlan.h"
#include "futex.h"
#include "gpt.h"
#include "heap.h"
#include "guard.h"
#include "log.h"
#include "kprintf.h"
#include "memory.h"
#include "mmap.h"
#include "paging.h"
#include "pmm.h"
#include "sound.h"
#include "pipe.h"
#include "usbserial.h"
#include "tap.h"
#include "bridge.h"
#include "pty.h"
#include "report.h"
#include "ritterfs.h"
#include "vfs.h"
#include "sched.h"
#include "swap.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "terminal.h"
#include "random.h"
#include "timer.h"
#include "uefi.h"
#include "vmlayout.h"
#include "winput.h"

#define MAX_SEGMENTS        16          /* program headers we accept */
#define WRITE_CHUNK         256
#define PATH_MAX            256         /* bytes, with the terminating zero */
#define ARGS_MAX            131072      /* bytes of argument strings (64j; 4096 until
                                         * then: RitterOS's own make could not start
                                         * the kernel's link once IPv6 lengthened it) */
#define ARGC_MAX            4096        /* words (37g-5a-2; 128 until then: the x86
                                         * kernel's link grew to 131) */
#define ENV_MAX             16384       /* bytes of environment strings (22t) */
#define ENVC_MAX            256         /* environment variables */
#define LINE_MAX            160         /* a line typed for a program */

/* FILE_SNAPSHOTS: a directory .snapshots (its position: the entry);
 * FILE_DEVICES: the directory /dev (the same); FILE_DEVICE: a disk or
 * partition there (its position: a byte on it); FILE_VOLUMES: the
 * directory /volumes (the entry). */
enum file_kind { FILE_CLOSED, FILE_TERMINAL, FILE_RITTERFS, FILE_SNAPSHOTS, FILE_DEVICES,
                 FILE_DEVICE, FILE_VOLUMES, FILE_SOCKET, FILE_PTY, FILE_PIPE, FILE_PCM,
                 FILE_NULL, FILE_ZERO,          /* /dev/null, /dev/zero (52c) */
                 FILE_USBSERIAL,                /* /dev/usbserialN (50e) */
                 FILE_TAP,                      /* /dev/tapN: a virtual card's wire (38a) */
                 FILE_SHARE, FILE_RESERVED };    /* a slot claimed, not yet installed (49b) */

/* An open file (51a: POSIX's "open file description"): what open() made,
 * shared by every descriptor that refers to it -- a program's and its
 * children's (spawn passes them on), so they share one position, and a log
 * written by several programs in turn comes out whole. refs counts the
 * descriptors: the last one going closes it at once (commits what was
 * written, drops the pipe end, wakes calls waiting on it, as close always
 * did); holds counts system calls using it meanwhile, which keep only its
 * memory, and one for all the descriptors (65e-42): freed when it is 0. */
struct open_file {
    int refs, holds;
    enum file_kind kind;
    struct vfs *fs;            /* the live file system or a snapshot's view */
    uint64_t object_id;             /* RitterFS: the object, its inode, the position */
    struct rfs_inode inode;
    uint64_t offset;
    bool readable, writable, append;
    bool nonblock;                  /* (53b) O_NONBLOCK: calls never wait (ERR_AGAIN) */
    bool written;                   /* changed since the last commit: commit at close */
    uint64_t unsaved;               /* bytes written since the last commit */
    struct block_device *device;    /* FILE_DEVICE */
    struct socket *socket;          /* FILE_SOCKET */
    struct pty *pty;                /* FILE_PTY: a pseudo-terminal's master or slave side */
    struct pipe *pipe;              /* FILE_PIPE: one end of a pipe */
    int pcm;                        /* FILE_PCM: the sound device's number (/dev/pcmN) */
    struct usbserial *serial;       /* FILE_USBSERIAL: the port (kept after an unplug) */
    struct tap *tap;                /* FILE_TAP: the card (its holder until the close) */
    struct shbuf *shbuf;            /* FILE_SHARE: a shared buffer */
    uint64_t shbuf_va;              /* where it is mapped in this program, or 0 */
    bool master;
    uint8_t made_up;                /* the system's root: names listed after its own */
};

/* The console's descriptors of a new process (before spawn gives it its
 * parent's): one open file for them all, never closed. */
static struct open_file console_terminal = { .refs = 1, .kind = FILE_TERMINAL };

/* A written file is committed at close, and also after this much data
 * (the uncommitted blocks wait in memory). */
#define WRITE_COMMIT_BYTES (8 * 1024 * 1024)

enum process_state { PROCESS_NEW, PROCESS_ALIVE, PROCESS_ZOMBIE };

/* A thread number's state: free, a thread running, or ended and not yet
 * joined (its result waits). */
enum { SLOT_FREE, SLOT_RUNNING, SLOT_ENDED };

/* Thread n's stack ends here: below the main stack and its guard page,
 * each with a guard page of its own (unmapped) below it. The main stack's
 * top is the process's stack_top: USER_STACK_TOP less a random shift (22l). */
#define STACK_SHIFT_MAX     (1ULL << 30)    /* stacks up to 1 GiB lower */
#define THREAD_STACK_TOP_AT(top, n) ((top) - (uint64_t)(n) * USER_STACK_SLOT)
#define THREAD_STACK_TOP(n) THREAD_STACK_TOP_AT(USER_STACK_TOP - STACK_SHIFT_MAX, n)
#define SHARE_SHIFT_MAX     (1ULL << 40)    /* shared buffers from up to 1 TiB higher */

/* Shared buffers (SYS_SHARE) are mapped from here up: a window far above the
 * program's code and heap (a few MiB) and far below its stacks (near
 * USER_STACK_TOP, ~128 TiB). Programs have no other dynamic mappings. */
#define SHARE_BASE          0x0000200000000000ULL

struct process {
    int pid, group;
    uint32_t uid, gid;              /* its user and group (0: root): whose rights apply */
    uint32_t ruid;                  /* the real user: who started it (differs only in a
                                     * setuid program, 50b) */
    int session;                    /* the ID of its session's leader (the shell) */
    uint32_t umask;                 /* (52h) bits taken off new files' and folders' modes */
    uint64_t start_blocked;         /* (52i) its first thread's blocked signals: its spawner's */
    char cwd[PATH_MAX];             /* current directory: absolute, normalized */
    uint32_t ignored;               /* bit n: signal n is ignored */
    /* Signal handlers (22r): an address per signal (0: none), what each
     * blocks while it runs and its SIGACTION_* flags; the signals sent and
     * not yet handled; where handlers return to (the library's sigreturn). */
    uint64_t handlers[SIGNALS], handler_masks[SIGNALS];
    uint8_t handler_flags[SIGNALS];
    uint64_t pending;
    uint64_t restorer;
    char log[LOG_NAME_SIZE];        /* the log its console output also goes to ("": none) */
    char name[32];                  /* the file name, for messages */
    enum process_state state;
    struct process *parent;         /* 0: the kernel, or none (orphan) */
    bool orphan;                    /* nobody will wait: freed when it ends */
    int status;                     /* how it ended (a zombie) */
    volatile int kill_signal;       /* a signal asked it to end (0: none) */
    /* Its threads, under the table lock: number 0 ran main. */
    struct thread *threads[PROCESS_THREADS];
    uint8_t slots[PROCESS_THREADS];             /* SLOT_* */
    int64_t results[PROCESS_THREADS];           /* thread_exit values, until joined */
    volatile int live;                          /* threads not yet ended */
    uint64_t cpu_done;                          /* CPU time of its ended threads (22o) */
    bool exiting;                               /* exit: the other threads leave */
    struct thread *exiter;                      /* the thread that ends the process */
    struct wait_queue thread_events;            /* join and exit wait here */
    struct wait_queue children;     /* it waits here for its children to end */
    struct process *next;           /* in the list of all processes */
    uint64_t space;                 /* its address space (paging_user_*) */
    uint64_t share_next;            /* next free address for a shared-buffer mapping */
    uint64_t screen_va, screen_pages;   /* (66d-2) the screen buffers mapped here, or 0 */
    uint64_t stack_top;             /* the main stack's top: randomized (22l) */
    uint64_t bias, image_lo, image_hi;  /* where a position-independent program went (22l) */
    struct mmap_state mm;           /* its mmap mappings (22a) */
    uint64_t entry, user_sp;        /* where its program starts */
    struct vfs *fs;
    struct open_file *files[FD_MAX];   /* 0: closed (51a: shared open files) */
    uint8_t cloexec[FD_MAX / 8];    /* (53b) FD_CLOEXEC per descriptor (kept: no exec yet) */
    struct spinlock fds;            /* guards claiming, installing and closing slots (49b) */
    int fds_changing;               /* (65e-42) slot changes under way: no looks without fds */
    char line[LINE_MAX + 1];        /* typed line not yet read by the program */
    size_t line_length, line_read;
    char carry[4];                  /* output: an incomplete UTF-8 character */
    size_t carry_length;
    struct pty *tty;                /* its terminal: a pseudo-terminal, or 0: the console */
    bool no_terminal;               /* a session started without one: neither (49k) */
    bool execed;                    /* (56b) it ran execve: its parent may not move its group */
    bool execing;                   /* (35b) in execve, its memory made new: not pageout's */
    /* (68e) sudo's memory, on a session's leader: when (arch_counter, which
     * counts a sleep too) and for which real user sudo last took a password;
     * it ends with the leader, so a later session never has it. */
    bool stamped;
    uint32_t stamp_uid;
    uint64_t stamp_at;
};

/* All processes; the lock also guards their state, parent, group, thread
 * and kill_signal (taken in interrupt handlers too: Ctrl-C). */
static struct process *all_processes;
static struct spinlock table = SPINLOCK_INIT("processes");
static struct wait_queue kernel_children;   /* the kernel waits here (terminal jobs) */
static struct wait_queue sleepers;          /* processes in sleep() */
static int next_pid = 1;

static struct process *current_process(void)
{
    return thread_current()->process;
}

/* (35b) The process pageout is writing pages of, and the one swap off is
 * reading pages back into (set under the table lock): an ended one is freed
 * only once neither is at it (free_process), and exec waits too. */
static struct process *paging_out, *paging_in;
static struct wait_queue paging_done;
static int pageout_last;                    /* the ID pageout went to last */

static bool swap_at(const struct process *p)
{
    return __atomic_load_n(&paging_out, __ATOMIC_ACQUIRE) == p ||
           __atomic_load_n(&paging_in, __ATOMIC_ACQUIRE) == p;
}

static void free_process(struct process *p)
{
    wait_event(&paging_done, !swap_at(p));
    kfree(p);
}

int64_t process_swap_in_all(void)
{
    int64_t r = 0;
    for (int last = 0; !r;) {
        struct process *p = 0;
        uint64_t state = spin_lock(&table);
        for (struct process *q = all_processes; q; q = q->next)
            if (q->state == PROCESS_ALIVE && !q->exiting && !q->execing && q->space &&
                q->pid > last && (!p || q->pid < p->pid))
                p = q;
        if (p) {
            last = p->pid;
            __atomic_store_n(&paging_in, p, __ATOMIC_RELEASE);
        }
        spin_unlock(&table, state);
        if (!p)
            break;
        r = mmap_swap_in_all(&p->mm, p->space);
        __atomic_store_n(&paging_in, (struct process *)0, __ATOMIC_RELEASE);
        wait_queue_wake_all(&paging_done);
    }
    return r ? r : mmap_anon_swap_in_all();     /* (35c-2) shared memory's pages too */
}

uint64_t process_pageout(uint64_t want, uint64_t *looked)
{
    struct process *p = 0;
    *looked = 0;
    uint64_t state = spin_lock(&table);
    for (int pass = 0; pass < 2 && !p; pass++) {
        int best = 0;
        for (struct process *q = all_processes; q; q = q->next)
            if (q->state == PROCESS_ALIVE && !q->exiting && !q->execing && q->space &&
                q->pid > pageout_last &&
                (!best || q->pid < best)) {
                best = q->pid;
                p = q;
            }
        if (!p)
            pageout_last = 0;                       /* past the last one: from the first */
    }
    if (p) {
        pageout_last = p->pid;
        __atomic_store_n(&paging_out, p, __ATOMIC_RELEASE);
    }
    spin_unlock(&table, state);
    if (!p)
        return 0;
    /* Its memory stays while it is listed and alive: it ends on its own
     * thread, which takes the mappings' lock (mmap_release_all) before its
     * tables go, and mmap_pageout finds nothing once that has run. */
    uint64_t n = mmap_pageout(&p->mm, p->space, want, looked);
    __atomic_store_n(&paging_out, (struct process *)0, __ATOMIC_RELEASE);
    wait_queue_wake_all(&paging_done);
    return n + mmap_pageout_shared();           /* (35c-2) out of every process */
}

void process_each_space(void (*fn)(struct mmap_state *m, uint64_t space, void *ctx), void *ctx)
{
    for (int last = 0;;) {
        struct process *p = 0;
        uint64_t state = spin_lock(&table);
        /* (Ending ones too: their threads may still run until they stop.) */
        for (struct process *q = all_processes; q; q = q->next)
            if (q->state == PROCESS_ALIVE && !q->execing && q->space && q->pid > last &&
                (!p || q->pid < p->pid))
                p = q;
        if (p) {
            last = p->pid;
            __atomic_store_n(&paging_out, p, __ATOMIC_RELEASE);
        }
        spin_unlock(&table, state);
        if (!p)
            return;
        fn(&p->mm, p->space, ctx);
        __atomic_store_n(&paging_out, (struct process *)0, __ATOMIC_RELEASE);
        wait_queue_wake_all(&paging_done);
    }
}

/* --- the program's memory ----------------------------------------------------- */

/* Copy between the kernel and the program's memory at addr; false unless all
 * of it is mapped with the access needed. The kernel goes through the direct
 * map, never through the program's own addresses. (65e) Each page from its
 * look to the end of its copy unchanged: until 65e another thread's fork
 * could make the page read-only again between the fault and the second look
 * (EFAULT for a good buffer, seen under smpstress), or between the look and
 * the copy (written into the page the child now shares). A page in place is
 * copied with interrupts off while no change runs (mmap_copy_begin: the
 * changes wait for it); else faulted in, then copied with the mappings'
 * lock held to read (65e-38), looked at again first. */
static bool copy_page_now(struct process *p, uint64_t addr, uint8_t *k, uint64_t n, bool to_user,
                          unsigned need)
{
    uint64_t phys;
    unsigned access;
    bool done = false;
    uint64_t irq = arch_irq_save();
    if (mmap_copy_begin(&p->mm) && paging_user_query(p->space, addr, &phys, &access) &&
        (access & need)) {
        if (to_user)
            memcpy(phys_to_virt(phys), k, n);
        else
            memcpy(k, phys_to_virt(phys), n);
        done = true;
    }
    mmap_copy_end(&p->mm);
    arch_irq_restore(irq);
    return done;
}

static bool copy_user(struct process *p, uint64_t addr, void *kernel, uint64_t size, bool to_user)
{
    uint8_t *k = kernel;
    while (size > 0) {
        if (addr >= USER_TOP)
            return false;
        unsigned need = to_user ? USER_WRITE : USER_READ;
        uint64_t n = PAGE_SIZE - addr % PAGE_SIZE;
        if (n > size)
            n = size;
        if (copy_page_now(p, addr, k, n, to_user, need)) {
            k += n;
            addr += n;
            size -= n;
            continue;
        }
        /* A page of a mapping not made yet, or a copy-on-write page's first
         * write (22e): made as the program's own touch would make it, then
         * copied with the mappings held to read (65e-38: a fault may need
         * them to write, so it runs before). Another thread's fork may make
         * it read-only again between: then again (an unmap ends it there).
         * (65e-48) The copy itself as a page in place is (counted, interrupts
         * off): munmap clears its pages holding the lock only to read, with
         * such copies stopped, so a page it takes out is never copied. */
        bool copied = false;
        while (!copied) {
            if (mmap_fault(&p->mm, p->space, addr, to_user, false) != 0)
                return false;
            mmap_read_begin(&p->mm);
            copied = copy_page_now(p, addr, k, n, to_user, need);
            mmap_read_end(&p->mm);
        }
        k += n;
        addr += n;
        size -= n;
    }
    return true;
}

/* A zero-terminated string from the program, at most size bytes with the
 * zero. Returns 0 or -ERR_FAULT / -ERR_NAMETOOLONG. */
static int64_t copy_string_from_user(struct process *p, uint64_t addr, char *out, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if (!copy_user(p, addr + i, &out[i], 1, false))
            return -ERR_FAULT;
        if (!out[i])
            return 0;
    }
    return -ERR_NAMETOOLONG;
}

/* --- paths ---------------------------------------------------------------------- */

/*
 * Snapshots appear under /.snapshots, which is not a directory on the disk:
 * it lists the snapshots, and /.snapshots/<name>/... is a path in that
 * snapshot's read-only view (rfs_snapshot.c keeps the views, so a file
 * opened in one stays readable after the snapshot is deleted, as long as
 * its blocks have not been reused).
 */
#define SNAPSHOT_DIR "/.snapshots"

static bool in_snapshots(const char *path)
{
    size_t n = sizeof(SNAPSHOT_DIR) - 1;
    return strncmp(path, SNAPSHOT_DIR, n) == 0 && (path[n] == 0 || path[n] == '/');
}

/* /volumes: the mounts there (kernel/mounts.c). */
static bool in_volumes(const char *path)
{
    size_t n = sizeof(VOLUMES_DIR) - 1;
    return strncmp(path, VOLUMES_DIR, n) == 0 && (path[n] == 0 || path[n] == '/');
}

/* An absolute, normalized path: the file system that holds it (the
 * system's, a mounted volume's, or a snapshot's view of either) and the
 * path there. *snapshots is set for the directories that are no object:
 * a .snapshots directory itself, and /volumes. Returns 0 or ERR_NOENT (no
 * such snapshot or volume). */
static int locate(struct vfs *fs, const char *path, struct vfs **in,
                  const char **inner, bool *snapshots)
{
    size_t n = sizeof(SNAPSHOT_DIR) - 1;
    *snapshots = false;
    *in = fs;
    *inner = path;
    if (strcmp(path, VOLUMES_DIR) == 0) {
        *snapshots = true;
        return 0;
    }
    const char *rest;
    struct vfs *mounted;
    if (mounts_find(path, &mounted, &rest)) {
        *in = mounted;
        *inner = path = rest;
    } else if (in_volumes(path)) {
        return ERR_NOENT;
    }
    /* Snapshots: RitterFS's only (on FAT, .snapshots is an ordinary name). */
    if (vfs_ritterfs(*in) && in_snapshots(path)) {
        if (path[n] == 0) {
            *snapshots = true;
            return 0;
        }
        const char *name = path + n + 1, *rest = name;
        while (*rest && *rest != '/')
            rest++;
        char wanted[RFS_SNAPSHOT_NAME_MAX + 1];
        if ((size_t)(rest - name) >= sizeof(wanted))
            return ERR_NOENT;
        memcpy(wanted, name, (size_t)(rest - name));
        wanted[rest - name] = 0;
        struct ritterfs *view = ritterfs_snapshot_view(vfs_ritterfs(*in), wanted);
        if (!view)
            return ERR_NOENT;
        *in = &view->vfs;
        *inner = *rest ? rest : "/";
    }
    return 0;
}

/* The same, and the object and inode there; /.snapshots itself is a
 * directory with no object (ID 0). */
static int lookup(struct vfs *fs, const char *path, struct vfs **in, uint64_t *id,
                  struct rfs_inode *inode, bool *snapshots)
{
    const char *inner;
    if (locate(fs, path, in, &inner, snapshots))
        return ERR_NOENT;
    if (*snapshots) {
        memset(inode, 0, sizeof(*inode));
        inode->type = RFS_TYPE_DIR;
        inode->link_count = 1;
        *id = 0;
        return 0;
    }
    return vfs_lookup(*in, inner, id, inode) ? 0 : ERR_NOENT;
}

/*
 * /dev is not on the disk either: it lists the devices (kernel/devices.c),
 * and /dev/NAME is a disk or partition, readable by root as raw bytes.
 */
#define DEVICE_DIR "/dev"

static bool in_devices(const char *path)
{
    size_t n = sizeof(DEVICE_DIR) - 1;
    return strncmp(path, DEVICE_DIR, n) == 0 && (path[n] == 0 || path[n] == '/');
}

/* The device a path in /dev names, or 0 (also for /dev itself). */
static struct block_device *device_at(const char *path)
{
    const char *name = path + sizeof(DEVICE_DIR);       /* after "/dev/" */
    if (path[sizeof(DEVICE_DIR) - 1] != '/' || !*name)
        return 0;
    for (const char *c = name; *c; c++)
        if (*c == '/')
            return 0;
    return devices_by_name(name);
}

static int64_t close_file(struct open_file *f);
static int64_t file_put(struct open_file *f);
static void display_process_ended(int pid);
static void screen_unmap(struct process *p);
struct socket;
static int64_t socket_read(struct process *p, struct socket *s, uint64_t buffer, uint64_t size,
                           uint64_t from_addr, uint32_t timeout_ms, bool six);

/* --- permissions ---------------------------------------------------------------- */

#define MAY_READ    4
#define MAY_WRITE   2
#define MAY_EXEC    1               /* for a directory: look up names in it */

/* Whether p may do `want` with the object: root may do everything, others
 * what the owner's, the group's or everyone's bits allow. */
static bool may(const struct process *p, const struct rfs_inode *inode, unsigned want)
{
    if (p->uid == 0)
        return true;
    unsigned mode = rfs_mode(inode->mode);
    unsigned bits = p->uid == inode->uid ? mode >> 6 : p->gid == inode->gid ? mode >> 3 : mode;
    return (bits & want) == want;
}

/* (52g) May p start this file as a program? x for it, and for root at
 * least one x bit (POSIX's rule: a 0644 text file is nobody's program). */
static bool may_run(const struct process *p, const struct rfs_inode *inode)
{
    return may(p, inode, MAY_EXEC) && (p->uid != 0 || (rfs_mode(inode->mode) & 0111));
}

/* Every directory on the way to path (in fs, inside it) must allow looking
 * up names: x. Returns 0 or -ERR_ACCES (-ERR_NOENT if one is missing). */
static int64_t search_allowed(const struct process *p, struct vfs *fs, const char *path)
{
    if (p->uid == 0)
        return 0;
    char prefix[PATH_MAX];
    size_t n = strlen(path);
    for (size_t i = 0; i < n; i++) {
        if (path[i] != '/' || (i > 0 && path[i - 1] == '/'))
            continue;
        if (i + 1 >= n)
            break;                                  /* the last component */
        size_t len = i == 0 ? 1 : i;
        memcpy(prefix, path, len);
        prefix[len] = 0;
        uint64_t id;
        struct rfs_inode dir;
        if (!vfs_lookup(fs, prefix, &id, &dir))
            return -ERR_NOENT;
        if (dir.type == RFS_TYPE_DIR && !may(p, &dir, MAY_EXEC))
            return -ERR_ACCES;
    }
    return 0;
}

/* lookup() for a program: also the directories' x on the way. Returns 0,
 * -ERR_NOENT or -ERR_ACCES. */
static int64_t find(struct process *p, const char *path, struct vfs **in, uint64_t *id,
                    struct rfs_inode *inode, bool *snapshots)
{
    const char *inner;
    if (locate(p->fs, path, in, &inner, snapshots))
        return -ERR_NOENT;
    if (*snapshots)
        return lookup(p->fs, path, in, id, inode, snapshots) ? -ERR_NOENT : 0;
    int64_t error = search_allowed(p, *in, inner);
    if (error)
        return error;
    return vfs_lookup(*in, inner, id, inode) ? 0 : -ERR_NOENT;
}

/* The directory a path would be created in (or removed from) must allow
 * writing and looking up names. */
static int64_t parent_allows(struct process *p, const char *path)
{
    char parent[PATH_MAX];
    size_t n = strlen(path);                        /* absolute: starts with '/' */
    while (n > 1 && path[n - 1] == '/')
        n--;
    while (n > 1 && path[n - 1] != '/')
        n--;                                        /* now up to the parent's '/' */
    if (n > 1)
        n--;                                        /* without it, unless the parent is / */
    memcpy(parent, path, n);
    parent[n] = 0;
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode dir;
    bool snapshots;
    int64_t error = find(p, parent, &fs, &id, &dir, &snapshots);
    if (error)
        return error;
    return may(p, &dir, MAY_WRITE | MAY_EXEC) ? 0 : -ERR_ACCES;
}

/* (57b) In a sticky folder (/tmp) only the entry's owner, the folder's
 * owner or root removes or renames the entry. */
static int64_t sticky_allows(struct process *p, const char *path)
{
    if (p->uid == 0)
        return 0;
    char parent[PATH_MAX];
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] != '/')
        n--;
    if (n > 1)
        n--;
    memcpy(parent, path, n);
    parent[n] = 0;
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode dir, entry;
    bool snapshots;
    if (find(p, parent, &fs, &id, &dir, &snapshots) || snapshots ||
        !(rfs_mode(dir.mode) & RFS_MODE_STICKY) || p->uid == dir.uid)
        return 0;
    if (find(p, path, &fs, &id, &entry, &snapshots))
        return 0;                                   /* (missing: the change says so) */
    return p->uid == entry.uid ? 0 : -ERR_PERM;
}

/* A new object belongs to the program that made it, with these rights. */
static int set_creator(const struct process *p, struct vfs *fs, uint64_t id, unsigned mode)
{
    int r = vfs_chown(fs, id, p->uid, p->gid);
    return r == RFS_OK ? vfs_chmod(fs, id, mode) : r;
}

/* Where a change to path goes: the file system holding it and the path
 * there. Snapshots, /volumes and /.snapshots themselves: read only. */
static int64_t target(struct process *p, const char *path, struct vfs **fs,
                      const char **inner)
{
    bool virtual;
    if (locate(p->fs, path, fs, inner, &virtual))
        return in_volumes(path) ? -ERR_ROFS : -ERR_NOENT;  /* /volumes holds only mounts */
    return virtual || vfs_read_only(*fs) ? -ERR_ROFS : 0;
}

/* (57a) The absolute path of path (relative to p's folder) with every
 * symbolic link on the way replaced by what it names -- a relative one from
 * the link's folder, an absolute one from / -- and "." and ".." taken out
 * as the components come (so ".." after a link leaves its target's
 * folder); the last component's link too when follow is set (not for
 * lstat, readlink, unlink, rename, link, symlink or mkdir, which mean the
 * name itself). Every folder passed through must allow p to look up names
 * (x), so a link never reaches past a folder p may not search. 0 or ERR_*
 * (ERR_LOOP after LINKS_MAX links). */
#define LINKS_MAX 40
static int resolve_path(struct process *p, const char *path, char *out, bool follow)
{
    char pending[PATH_MAX], next[PATH_MAX];
    size_t length = strlen(path), n = 0;
    if (length >= PATH_MAX)
        return ERR_NAMETOOLONG;
    memcpy(pending, path, length + 1);
    if (pending[0] != '/') {
        n = strlen(p->cwd);
        memcpy(out, p->cwd, n + 1);
        if (n == 1)
            n = 0;                                  /* (the folder is /) */
    }
    out[n] = 0;
    int links = 0;
    bool plain = false;                             /* in /dev: nothing to follow there */
    const char *s = pending;
    while (*s) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        size_t len = 0;
        while (s[len] && s[len] != '/')
            len++;
        const char *after = s + len;
        while (*after == '/')
            after++;
        bool last = !*after;
        if (len == 1 && s[0] == '.') {
            s += len;
            continue;
        }
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;                                /* drop the last component */
            if (n > 0)
                n--;
            out[n] = 0;
            s += len;
            continue;
        }
        if (n + 1 + len >= PATH_MAX)
            return ERR_NAMETOOLONG;
        size_t before = n;
        out[n++] = '/';
        memcpy(out + n, s, len);
        n += len;
        out[n] = 0;
        s += len;
        if (plain || (last && !follow))
            continue;
        if (in_devices(out)) {
            plain = true;
            continue;
        }
        struct vfs *fs;
        const char *inner;
        bool virtual;
        uint64_t id;
        struct rfs_inode ino;
        if (locate(p->fs, out, &fs, &inner, &virtual) ||
            (!virtual && !vfs_lookup(fs, inner, &id, &ino))) {
            if (!last)
                return ERR_NOENT;
            continue;                               /* (a name to be made, or an error later) */
        }
        if (virtual)
            continue;
        if (ino.type == RFS_TYPE_DIR) {
            if (!last && !may(p, &ino, MAY_EXEC))
                return ERR_ACCES;
            continue;
        }
        if (ino.type != RFS_TYPE_SYMLINK) {
            if (!last)
                return ERR_NOTDIR;
            continue;
        }
        if (++links > LINKS_MAX)
            return ERR_LOOP;
        char target[256];
        if (ino.size == 0 || ino.size > 255 ||
            vfs_read(fs, id, &ino, 0, target, ino.size) != (int64_t)ino.size)
            return ERR_IO;
        size_t tlen = (size_t)ino.size, rest = strlen(s);
        if (tlen + rest >= PATH_MAX)
            return ERR_NAMETOOLONG;
        memcpy(next, target, tlen);                 /* what it names, then the rest */
        memcpy(next + tlen, s, rest + 1);
        memcpy(pending, next, tlen + rest + 1);
        s = pending;
        n = target[0] == '/' ? 0 : before;          /* from / or from the link's folder */
        out[n] = 0;
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    return 0;
}

/* --- loading ------------------------------------------------------------------ */

/* What a program's first stack gets besides its arguments (22b): the
 * auxiliary vector, telling its loader where the program's headers and entry
 * point are. A static program gets the same since 22i (its start code finds
 * its per-thread data in the headers), but no AT_BASE: that marks a loader. */
struct start_info {
    bool dynamic;
    uint16_t phnum;
    uint64_t entry, base;           /* the program's entry, the loader's lowest address */
    uint64_t phdr;                  /* its headers in memory (PT_PHDR; 0: the copy below) */
    Elf64_Phdr ph[MAX_SEGMENTS];    /* the program's headers (copied onto its stack) */
};

/* An ELF program for this CPU that p may run: its file system, object,
 * inode and headers. 0 or ERR_*. */
static int read_program(struct process *p, const char *path, struct vfs **fs, uint64_t *id,
                        struct rfs_inode *inode, Elf64_Ehdr *eh, Elf64_Phdr ph[MAX_SEGMENTS])
{
    bool snapshots;
    int64_t error = find(p, path, fs, id, inode, &snapshots);
    if (error)
        return (int)-error;
    if (inode->type != RFS_TYPE_FILE)
        return ERR_NOEXEC;
    if (!may_run(p, inode))
        return ERR_ACCES;
    if (vfs_read(*fs, *id, inode, 0, eh, sizeof(*eh)) != (int64_t)sizeof(*eh) ||
        eh->e_magic != ELF_MAGIC || eh->e_class != ELFCLASS64 || eh->e_data != ELFDATA2LSB ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) || eh->e_machine != ELF_MACHINE_NATIVE)
        return ERR_NOEXEC;                      /* not an ELF program for this CPU */
    uint64_t ph_size = (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr);
    if (eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > MAX_SEGMENTS ||
        vfs_read(*fs, *id, inode, eh->e_phoff, ph, ph_size) != (int64_t)ph_size)
        return ERR_NOEXEC;
    return 0;
}

#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/* Fill [va, va + n) of p's mapping made for a segment from the file at
 * offset, a page at a time (each made by its first write; 22g). 0 or ERR_*. */
static int fill_segment(struct process *p, struct vfs *fs, uint64_t id,
                        const struct rfs_inode *inode, uint64_t va, uint64_t n, uint64_t offset)
{
    while (n) {
        uint64_t chunk = PAGE_SIZE - va % PAGE_SIZE, phys;
        unsigned access;
        if (chunk > n)
            chunk = n;
        /* (The page's own address: the query adds the offset of the one asked.) */
        if (mmap_fault(&p->mm, p->space, va, true, false) ||
            !paging_user_query(p->space, PAGE_DOWN(va), &phys, &access))
            return ERR_NOMEM;
        if (vfs_read(fs, id, inode, offset, (uint8_t *)phys_to_virt(phys) + va % PAGE_SIZE,
                     chunk) != (int64_t)chunk)
            return ERR_IO;
        va += chunk;
        offset += chunk;
        n -= chunk;
    }
    return 0;
}

/*
 * Map a program's loadable segments into p's address space, with the
 * permissions the ELF file asks for (never writable and executable at
 * once); its entry must be in its code. Since 22g a segment is a mapping of
 * the page cache's pages of the file, shared by every program running the
 * same version of it (a writable one copy-on-write), and its zeroed rest
 * (bss) anonymous memory. What cannot be shared that way -- a read-only
 * segment whose zeroed rest begins inside a page of the file, a segment not
 * placed in its page like the file -- is a copy. Returns 0 or ERR_*.
 */
/* A position-independent program or loader (ET_DYN, 22l) goes at a random
 * place: the offset added to its addresses, its span clear of [avoid, avoid +
 * avoid_size) (the program, for its loader). Between 4 GiB and 8 TiB higher. */
#define PIE_LOW     (4ULL << 30)
#define PIE_RANGE   (8ULL << 40)

static uint64_t choose_bias(const Elf64_Ehdr *eh, const Elf64_Phdr *ph, uint64_t avoid,
                            uint64_t avoid_size)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            if (PAGE_DOWN(ph[i].p_vaddr) < lo)
                lo = PAGE_DOWN(ph[i].p_vaddr);
            if (PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi)
                hi = PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz);
        }
    if (lo >= hi || hi - lo > (1ULL << 32))
        return 0;                               /* (refused when its segments are checked) */
    for (int tries = 0; tries < 64; tries++) {  /* (bounded: never a kernel that spins) */
        uint64_t r;
        random_bytes(&r, sizeof(r));
        uint64_t at = PIE_LOW + (r % (PIE_RANGE / PAGE_SIZE)) * PAGE_SIZE;
        if (at + (hi - lo) <= avoid || at >= avoid + avoid_size)
            return at - lo;
    }
    return 0;
}

static int load_segments(struct process *p, struct vfs *fs, uint64_t id,
                         const struct rfs_inode *inode, const Elf64_Ehdr *eh, const Elf64_Phdr *ph,
                         uint64_t bias)
{
    const uint64_t limit = THREAD_STACK_TOP(PROCESS_THREADS);  /* below all thread stacks */
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *s = &ph[i];
        if (s->p_type != PT_LOAD || s->p_memsz == 0)
            continue;
        uint64_t vaddr = s->p_vaddr + bias, end = vaddr + s->p_memsz;
        if (s->p_filesz > s->p_memsz || vaddr < USER_LOAD_MIN || end < vaddr ||
            end > limit || ((s->p_flags & PF_W) && (s->p_flags & PF_X)) ||
            s->p_offset > inode->size || s->p_filesz > inode->size - s->p_offset)
            return ERR_NOEXEC;                  /* outside the program area or the file, W and X */
        /* Segments may not share a page (a mapping would replace the other). */
        for (int j = 0; j < i; j++)
            if (ph[j].p_type == PT_LOAD && ph[j].p_memsz &&
                PAGE_DOWN(vaddr) < PAGE_UP(ph[j].p_vaddr + bias + ph[j].p_memsz) &&
                PAGE_DOWN(ph[j].p_vaddr + bias) < PAGE_UP(end))
                return ERR_NOEXEC;
    }
    struct mmap_file file = { fs, id, true, false };
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *s = &ph[i];
        if (s->p_type != PT_LOAD || s->p_memsz == 0)
            continue;
        unsigned access = MMAP_READ | (s->p_flags & PF_W ? MMAP_WRITE : 0) |
                          (s->p_flags & PF_X ? MMAP_EXEC : 0);
        uint64_t vaddr = s->p_vaddr + bias;
        uint64_t base = PAGE_DOWN(vaddr), file_end = vaddr + s->p_filesz;
        uint64_t top = PAGE_UP(vaddr + s->p_memsz), bss = PAGE_UP(file_end);
        bool tail = s->p_memsz > s->p_filesz && file_end % PAGE_SIZE;
        int64_t r;
        if (s->p_filesz && s->p_offset % PAGE_SIZE == vaddr % PAGE_SIZE &&
            (!tail || (access & MMAP_WRITE) || !vfs_ritterfs(fs))) {
            r = mmap_map_segment(&p->mm, p->space, base, bss - base, access, &file,
                                 PAGE_DOWN(s->p_offset));
            if (r < 0)
                return (int)-r;
            if (tail) {                         /* the page's bytes past the segment's: zeros */
                uint64_t phys;
                unsigned have;
                /* (A copy-on-write page gets its own copy first; other file
                 * systems' pages are the program's copies already.) */
                if ((vfs_ritterfs(fs) && mmap_fault(&p->mm, p->space, file_end, true, false)) ||
                    !paging_user_query(p->space, PAGE_DOWN(file_end), &phys, &have))
                    return ERR_NOMEM;
                memset((uint8_t *)phys_to_virt(phys) + file_end % PAGE_SIZE, 0,
                       PAGE_SIZE - file_end % PAGE_SIZE);
            }
            if (bss < top && (r = mmap_map_segment(&p->mm, p->space, bss, top - bss, access, 0, 0)) < 0)
                return (int)-r;
            continue;
        }
        /* A copy: anonymous memory, filled while writable, then given its access. */
        int error;
        r = mmap_map_segment(&p->mm, p->space, base, top - base, MMAP_READ | MMAP_WRITE, 0, 0);
        if (r < 0)
            return (int)-r;
        if ((error = fill_segment(p, fs, id, inode, vaddr, s->p_filesz, s->p_offset)))
            return error;
        if (access != (MMAP_READ | MMAP_WRITE) &&
            (r = mmap_protect(&p->mm, p->space, base, top - base, access)) < 0)
            return (int)-r;
    }
    if (mmap_fault(&p->mm, p->space, eh->e_entry + bias, false, true))
        return ERR_NOEXEC;                      /* the entry is not in its code */
    return 0;
}

/*
 * Load the program at path and give it a stack. A program that uses shared
 * libraries names its loader (PT_INTERP: /system/<arch>/lib/ld.so, 22b),
 * which is loaded too, at its own addresses, and started first; start gets
 * what it needs to find the program. Returns 0 or ERR_*.
 */
static int load(struct process *p, const char *path, struct start_info *start)
{
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    Elf64_Ehdr eh;
    Elf64_Phdr ph[MAX_SEGMENTS];
    int error = read_program(p, path, &fs, &id, &inode, &eh, ph);
    if (error)
        return error;
    /* A setuid program (50b) runs as its file's owner, its caller staying
     * the real user. Only on the system's own file system: a stick from
     * elsewhere (or a snapshot of an older version) could hold a root-owned
     * one (as Unix's nosuid for such mounts). */
    if ((rfs_mode(inode.mode) & RFS_MODE_SETUID) && fs == mounts_root())
        p->uid = inode.uid;
    char loader[PATH_MAX];
    start->dynamic = false;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_INTERP)
            continue;
        if (ph[i].p_filesz < 2 || ph[i].p_filesz > sizeof(loader) ||
            vfs_read(fs, id, &inode, ph[i].p_offset, loader, ph[i].p_filesz) !=
                (int64_t)ph[i].p_filesz || loader[ph[i].p_filesz - 1] != '\0')
            return ERR_NOEXEC;
        start->dynamic = true;
    }
    /* Position-independent (22l): a random place. Its loader relocates it;
     * without one it must relocate itself (ld.so, started by itself). */
    uint64_t bias = 0, lo = UINT64_MAX, hi = 0;
    if (eh.e_type == ET_DYN && !(bias = choose_bias(&eh, ph, 0, 0)))
        return ERR_NOEXEC;
    if ((error = load_segments(p, fs, id, &inode, &eh, ph, bias)))
        return error;
    p->entry = eh.e_entry + bias;
    start->entry = eh.e_entry + bias;
    start->phnum = eh.e_phnum;
    start->phdr = 0;
    memcpy(start->ph, ph, (size_t)eh.e_phnum * sizeof(Elf64_Phdr));
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_PHDR)
            start->phdr = ph[i].p_vaddr + bias;     /* its headers, mapped with it */
        if (ph[i].p_type == PT_LOAD && ph[i].p_memsz) {
            if (ph[i].p_vaddr + bias < lo)
                lo = ph[i].p_vaddr + bias;
            if (ph[i].p_vaddr + bias + ph[i].p_memsz > hi)
                hi = ph[i].p_vaddr + bias + ph[i].p_memsz;
        }
    }
    p->bias = bias;                             /* (for fault reports: the address as linked) */
    p->image_lo = lo;
    p->image_hi = hi;
    if (start->dynamic) {
        Elf64_Ehdr leh;
        Elf64_Phdr lph[MAX_SEGMENTS];
        if ((error = read_program(p, loader, &fs, &id, &inode, &leh, lph)))
            return error;
        uint64_t lbias = 0;                     /* (a position-independent loader: anywhere) */
        if (leh.e_type == ET_DYN && !(lbias = choose_bias(&leh, lph, PAGE_DOWN(lo), PAGE_UP(hi) - PAGE_DOWN(lo))))
            return ERR_NOEXEC;
        start->base = UINT64_MAX;
        for (int i = 0; i < leh.e_phnum; i++) {
            if (lph[i].p_type == PT_INTERP)
                return ERR_NOEXEC;              /* a loader is static itself */
            if (lph[i].p_type == PT_LOAD && lph[i].p_vaddr + lbias < start->base)
                start->base = lph[i].p_vaddr + lbias;
        }
        if ((error = load_segments(p, fs, id, &inode, &leh, lph, lbias)))
            return error;
        p->entry = leh.e_entry + lbias;         /* the loader runs first */
    }
    /* (54d) The main stack: its pages come when first touched. */
    if (mmap_map_segment(&p->mm, p->space, p->stack_top - USER_STACK_SIZE, USER_STACK_SIZE,
                         MMAP_READ | MMAP_WRITE, 0, 0) < 0)
        return ERR_NOMEM;
    return 0;
}

/*
 * The arguments, as the System V ABI places them at the top of the stack:
 * the stack pointer points to argc, followed by the argv pointers, a null
 * pointer, an empty environment (another null pointer) and the auxiliary
 * vector (type and value pairs up to AT_NULL; for a dynamic program where
 * its program headers -- copied onto the stack -- and its entry are, for
 * its loader); the strings lie above. Returns the initial stack pointer, or
 * 0 if they do not fit.
 */
static uint64_t push_arguments(struct process *p, int argc, const char *const argv[], int envc,
                               const char *const envp[], const struct start_info *start)
{
    uint64_t strings = 0, env_strings = 0;
    for (int i = 0; i < argc; i++)
        strings += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++)
        env_strings += strlen(envp[i]) + 1;
    if (strings > ARGS_MAX || env_strings > ENV_MAX)
        return 0;
    strings += env_strings;
    uint64_t text = (p->stack_top - strings) & ~15ULL;
    uint64_t headers = text, aux[14];
    int naux = 0;
    {
        uint64_t size = (uint64_t)start->phnum * sizeof(Elf64_Phdr);
        headers = (text - size) & ~15ULL;
        if (!copy_user(p, headers, (void *)start->ph, size, true))
            return 0;
        aux[naux++] = AT_PHDR;   aux[naux++] = start->phdr ? start->phdr : headers;
        aux[naux++] = AT_PHENT;  aux[naux++] = sizeof(Elf64_Phdr);
        aux[naux++] = AT_PHNUM;  aux[naux++] = start->phnum;
        aux[naux++] = AT_PAGESZ; aux[naux++] = PAGE_SIZE;
        if (start->dynamic) {
            aux[naux++] = AT_BASE;   aux[naux++] = start->base;
        }
        aux[naux++] = AT_ENTRY;  aux[naux++] = start->entry;
    }
    aux[naux++] = AT_NULL;
    aux[naux++] = 0;
    /* argc, argv[], 0, envp[], 0, aux (22t: the environment, as on Unix) */
    uint64_t words = (uint64_t)argc + 3 + (uint64_t)envc + (uint64_t)naux;
    uint64_t sp = (headers - words * 8) & ~15ULL;
    uint64_t at = text, slot = sp;
    uint64_t value = (uint64_t)argc;
    if (!copy_user(p, slot, &value, 8, true))
        return 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        slot += 8;
        if (!copy_user(p, at, (void *)argv[i], n, true) || !copy_user(p, slot, &at, 8, true))
            return 0;
        at += n;
    }
    value = 0;
    slot += 8;
    if (!copy_user(p, slot, &value, 8, true))           /* argv's end */
        return 0;
    for (int i = 0; i < envc; i++) {
        size_t n = strlen(envp[i]) + 1;
        slot += 8;
        if (!copy_user(p, at, (void *)envp[i], n, true) || !copy_user(p, slot, &at, 8, true))
            return 0;
        at += n;
    }
    return copy_user(p, slot + 8, &value, 8, true) &&
           copy_user(p, slot + 16, aux, (uint64_t)naux * 8, true) ? sp : 0;
}

/* --- creating, running, ending ----------------------------------------------- */

struct process *process_create(struct vfs *fs, const char *path, int argc,
                               const char *const argv[], int envc, const char *const envp[],
                               struct process *parent, int group, int *error)
{
    struct process *p = kzalloc(sizeof(*p));
    if (!p) {
        *error = ERR_NOMEM;
        return 0;
    }
    const char *name = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' && c[1])
            name = c + 1;
    size_t n = strlen(name) < sizeof(p->name) - 1 ? strlen(name) : sizeof(p->name) - 1;
    memcpy(p->name, name, n);
    p->fs = fs;
    p->fds.name = "fds";                            /* zeroed by kzalloc; name it for panics */
    p->uid = parent ? parent->uid : 0;              /* the kernel starts programs as root */
    p->ruid = parent ? parent->ruid : 0;            /* who started it (a setuid program's
                                                     * caller, 50b) */
    p->gid = parent ? parent->gid : 0;
    for (int fd = FD_STDIN; fd <= FD_STDERR; fd++)
        p->files[fd] = &console_terminal;
    memcpy(p->cwd, parent ? parent->cwd : "/", parent ? sizeof(p->cwd) : 2);
    char full[PATH_MAX];
    struct start_info start;
    *error = resolve_path(p, path, full, true);
    uint64_t r[2];                                  /* address-space randomization (22l) */
    random_bytes(r, sizeof(r));
    p->share_next = SHARE_BASE + (r[0] % (SHARE_SHIFT_MAX / PAGE_SIZE)) * PAGE_SIZE;
    p->stack_top = USER_STACK_TOP - (r[1] % (STACK_SHIFT_MAX / PAGE_SIZE)) * PAGE_SIZE;
    mmap_init(&p->mm);
    p->space = *error ? 0 : paging_user_create();
    if (!*error)
        *error = !p->space ? ERR_NOMEM : load(p, full, &start);
    if (!*error && !(p->user_sp = push_arguments(p, argc, argv, envc, envp, &start)))
        *error = ERR_2BIG;
    p->mm.pageable = !*error;                       /* (35b) loaded: pageout may take pages */
    if (*error) {
        if (p->space) {
            mmap_release_all(&p->mm, p->space);     /* (its segments, 22g) */
            paging_user_destroy(p->space);
        }
        kfree(p);
        return 0;
    }
    p->execed = true;                               /* (56b: it runs a program from its start) */
    p->tty = parent ? parent->tty : 0;
    p->umask = parent ? parent->umask : 022;        /* (52h: passed on, Unix's usual start) */
    p->no_terminal = parent && parent->no_terminal;
    if (parent)
        memcpy(p->log, parent->log, sizeof(p->log));
    if (p->tty)
        pty_hold_terminal(p->tty);
    uint64_t state = spin_lock(&table);
    p->pid = next_pid++;
    p->group = group ? group : p->pid;
    p->session = parent ? parent->session : p->pid;
    p->parent = parent;
    p->state = PROCESS_NEW;
    p->next = all_processes;
    all_processes = p;
    spin_unlock(&table, state);
    return p;
}

int process_id(const struct process *p)
{
    return p->pid;
}

int process_group(const struct process *p)
{
    return p->group;
}

static void unlink_process(struct process *p)
{
    for (struct process **link = &all_processes; *link; link = &(*link)->next) {
        if (*link == p) {
            *link = p->next;
            return;
        }
    }
}

/* The process's thread: enter user mode at the program's start. */
static void user_thread(void *arg)
{
    struct process *p = arg;
    thread_current()->sig_blocked = p->start_blocked;
    arch_enter_user(p->entry, p->user_sp, thread_stack_top(thread_current()));
}

bool process_start(struct process *p)
{
    struct thread *self = thread_current();         /* (52i: the spawner's set, as posix_spawn) */
    p->start_blocked = self->process ? self->sig_blocked : 0;
    uint64_t state = spin_lock(&table);
    p->state = PROCESS_ALIVE;
    spin_unlock(&table, state);
    struct thread *t = thread_create_for(p->name, user_thread, p, p, p->space);
    state = spin_lock(&table);
    p->threads[0] = t;
    p->slots[0] = t ? SLOT_RUNNING : SLOT_FREE;
    p->live = t ? 1 : 0;
    if (!t)
        unlink_process(p);
    spin_unlock(&table, state);
    if (!t) {
        mmap_release_all(&p->mm, p->space);
        paging_user_destroy(p->space);
        if (p->tty)
            pty_release_terminal(p->tty);
        for (int fd = 0; fd < FD_MAX; fd++)         /* (51a: the open files it was given) */
            if (p->files[fd])
                file_put(p->files[fd]);
        free_process(p);
    }
    return t != 0;
}

/* The CPU time p has run (22o: SYS_CPUTIME, C's clock), in nanoseconds:
 * its ended threads' and its running ones'. */
static uint64_t process_cpu_ns(struct process *p)
{
    uint64_t state = spin_lock(&table);
    uint64_t counts = p->cpu_done;
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (p->threads[i])
            counts += sched_cpu_counts(p->threads[i]);
    spin_unlock(&table, state);
    uint64_t hz = arch_counter_hz();
    if (!hz)
        return 0;
    /* (in two parts: counts times 10^9 would overflow) */
    return counts / hz * 1000000000 + counts % hz * 1000000000 / hz;
}

/* The calling thread's number in p (the table lock is held), or -1. */
static int thread_slot(struct process *p)
{
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (p->threads[i] == thread_current())
            return i;
    return -1;
}

static __attribute__((noreturn)) void process_exit(int status);

/* End the calling thread, not the process: keep its result for a join
 * (unless the process is ending), and leave. The last thread to go ends the
 * process instead, with exit code 0. */
static __attribute__((noreturn)) void leave_thread(struct process *p, int64_t value)
{
    uint64_t state = spin_lock(&table);
    if (p->live == 1 && !p->exiting) {
        spin_unlock(&table, state);
        process_exit(STATUS_EXITED(0));
    }
    int slot = thread_slot(p);
    p->cpu_done += sched_cpu_counts(thread_current());  /* (its CPU time stays the process's) */
    if (slot >= 0) {
        p->threads[slot] = 0;
        p->slots[slot] = p->exiting ? SLOT_FREE : SLOT_ENDED;
        p->results[slot] = value;
    }
    p->live--;
    thread_current()->process = 0;                  /* a kernel thread for its last steps */
    spin_unlock(&table, state);
    sched_set_space(0);
    wait_queue_wake_all(&p->thread_events);
    thread_exit();
}

/* End the running process with status: the other threads leave first (they
 * still use its memory), then free what it owns, leave a zombie for its
 * parent (or nothing, for an orphan), end the thread. If another thread is
 * ending the process already, this one only leaves. */
static __attribute__((noreturn)) void process_exit(int status)
{
    struct process *p = current_process();
    uint64_t state = spin_lock(&table);
    if (p->exiting && p->exiter != thread_current()) {
        spin_unlock(&table, state);
        leave_thread(p, 0);
    }
    p->exiting = true;
    p->exiter = thread_current();
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (p->threads[i] && p->threads[i] != thread_current())
            sched_request_stop(p->threads[i]);
    spin_unlock(&table, state);
    wait_event(&p->thread_events, p->live == 1);

    if (p->carry_length) {                          /* an unfinished character */
        p->carry[p->carry_length] = 0;
        kprintf("%s", p->carry);
    }
    /* (65e-42) Its other threads have ended: no look at a slot meanwhile. */
    for (int fd = 0; fd < FD_MAX; fd++) {           /* what it wrote is committed */
        uint64_t s = spin_lock(&p->fds);
        struct open_file *f = p->files[fd];
        p->files[fd] = 0;
        spin_unlock(&p->fds, s);
        if (f)
            file_put(f);
    }
    terminal_process_ended(p->pid);                 /* echo back on if it switched it off */
    display_process_ended(p->pid);                  /* give the screen back if it owned it */
    if (p->tty) {
        struct pty *tty = p->tty;
        pty_process_ended(tty, p->pid);
        p->tty = 0;
        pty_release_terminal(tty);
    }
    mmap_release_all(&p->mm, p->space);             /* shared pages are not the space's to free */
    sched_set_space(0);                             /* off the program's page tables */
    cpus_wait_space_unused(p->space);               /* (65c-2) and every core off them */
    paging_user_destroy(p->space);
    p->space = 0;

    struct process *free_list = 0;
    state = spin_lock(&table);
    p->state = PROCESS_ZOMBIE;
    p->status = status;
    for (int i = 0; i < PROCESS_THREADS; i++)
        p->threads[i] = 0;
    p->live = 0;
    thread_current()->process = 0;                  /* a kernel thread for its last steps */
    /* Its children are orphans now; those that already ended go at once. */
    for (struct process **link = &all_processes; *link;) {
        struct process *c = *link;
        if (c->parent == p) {
            c->parent = 0;
            c->orphan = true;
            if (c->state == PROCESS_ZOMBIE) {
                *link = c->next;
                c->next = free_list;
                free_list = c;
                continue;
            }
        }
        link = &c->next;
    }
    bool orphan = p->orphan;
    if (orphan)
        unlink_process(p);
    struct wait_queue *notify = p->parent ? &p->parent->children : &kernel_children;
    spin_unlock(&table, state);

    if (!orphan)
        wait_queue_wake_all(notify);
    else
        free_process(p);
    while (free_list) {
        struct process *c = free_list;
        free_list = c->next;
        free_process(c);
    }
    thread_exit();
}

/* A child of parent with this ID (-1: any) that has ended, taken out of the
 * table; *any: whether parent has such a child at all. */
static struct process *take_zombie(struct process *parent, int pid, bool *any)
{
    struct process *found = 0;
    *any = false;
    uint64_t state = spin_lock(&table);
    for (struct process **link = &all_processes; *link; link = &(*link)->next) {
        struct process *c = *link;
        if (c->parent != parent || c->orphan || (pid != -1 && c->pid != pid))
            continue;
        *any = true;
        if (c->state == PROCESS_ZOMBIE) {
            *link = c->next;
            found = c;
            break;
        }
    }
    spin_unlock(&table, state);
    return found;
}

int process_wait(struct process *parent, int pid, int *status, bool no_hang)
{
    struct wait_queue *q = parent ? &parent->children : &kernel_children;
    struct process *found = 0;
    bool any = true;
    if (no_hang) {
        found = take_zombie(parent, pid, &any);
        if (!found && any)
            return 0;                               /* children, none ended yet */
    } else if (!wait_event_interruptible(q, (found = take_zombie(parent, pid, &any)) || !any)) {
        return -ERR_INTR;
    }
    if (!found)
        return -ERR_CHILD;
    int id = found->pid;
    *status = found->status;
    free_process(found);
    return id;
}

/* Ask p to end with signal (the table lock is held). */
static void signal_locked(struct process *p, int signal)
{
    if (p->state == PROCESS_ZOMBIE)
        return;
    bool ignored = signal < 32 && (p->ignored & (1u << signal));
    bool handled = signal > 0 && signal < SIGNALS && signal != SIGKILL && p->handlers[signal];
    /* (52f) Blocked by every thread: it waits in pending, and ends the
     * program (or runs its handler) once a thread unblocks it. */
    uint64_t bit = signal > 0 && signal < SIGNALS && signal != SIGKILL ? 1ULL << signal : 0;
    bool blocked = false;
    for (int i = 0; i < PROCESS_THREADS; i++)       /* (no thread listed: not blocked) */
        if (p->threads[i] && !(blocked = bit && (p->threads[i]->sig_blocked & bit)))
            break;
    if (handled || (blocked && !ignored))
        p->pending |= 1ULL << signal;               /* its handler runs at the next return */
    else if (!ignored && !p->kill_signal)
        p->kill_signal = signal;
    /* Every thread: a waiting call returns (ignored, handled), or it ends;
     * one that blocks the signal waits on (52f), unless the program ends. */
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (p->threads[i] && (p->kill_signal || !(p->threads[i]->sig_blocked & bit)))
            sched_request_stop(p->threads[i]);
}

/* thread_interrupt (54c): wake thread `number`'s waiting call, and with
 * ENTER have it run entry first thing on its way back to the program. */
static int64_t sys_thread_interrupt(struct process *p, uint64_t number, uint64_t how, uint64_t entry)
{
    if (number >= PROCESS_THREADS || how > THREAD_INTERRUPT_ENTER ||
        (how == THREAD_INTERRUPT_ENTER && (entry < USER_LOAD_MIN || entry >= USER_STACK_TOP)))
        return number >= PROCESS_THREADS ? -ERR_SRCH : -ERR_INVAL;
    uint64_t state = spin_lock(&table);
    struct thread *t = p->slots[number] == SLOT_RUNNING ? p->threads[number] : 0;
    if (t) {
        if (how == THREAD_INTERRUPT_ENTER)
            t->enter_at = entry;
        sched_request_stop(t);
    }
    spin_unlock(&table, state);
    return t ? 0 : -ERR_SRCH;
}

/* sigpending(out) (52i): the signals waiting that this thread blocks. */
static int64_t sys_sigpending(struct process *p, uint64_t out)
{
    uint64_t state = spin_lock(&table);
    uint64_t set = p->pending & thread_current()->sig_blocked;
    spin_unlock(&table, state);
    return copy_user(p, out, &set, sizeof(set), true) ? 0 : -ERR_FAULT;
}

/* sigsuspend(mask) (52i): the thread blocks mask instead until a signal
 * it lets through comes: -ERR_INTR, its handler run on the way back with
 * that mask, the old one back when it returns (run_handler); or the
 * program ends. Ignored signals do not end the wait. */
static int64_t sys_sigsuspend(struct process *p, uint64_t addr)
{
    uint64_t mask;
    if (!copy_user(p, addr, &mask, sizeof(mask), false))
        return -ERR_FAULT;
    struct thread *t = thread_current();
    static struct wait_queue never;                 /* (only a stop request wakes it) */
    uint64_t state = spin_lock(&table);
    t->suspend_mask = t->sig_blocked;
    t->sig_blocked = mask & ~(1ULL << SIGKILL) & ~1ULL;
    t->suspended = true;
    spin_unlock(&table, state);
    for (;;) {
        state = spin_lock(&table);
        bool come = (p->pending & ~t->sig_blocked) || p->kill_signal || p->exiting;
        spin_unlock(&table, state);
        if (come)
            break;
        if (!wait_event_interruptible(&never, false))
            t->stop_requested = false;              /* (an ignored signal: wait on) */
    }
    return -ERR_INTR;
}

/* sigprocmask(how, set, old) (52f): the calling thread's blocked signals;
 * SIGKILL is never blocked. A signal waiting that is now let through is
 * delivered on the way back (process_before_return). */
static int64_t sys_sigprocmask(struct process *p, uint64_t how, uint64_t set_addr, uint64_t old_addr)
{
    struct thread *t = thread_current();
    uint64_t old = t->sig_blocked, set = 0;
    if (how > SIGMASK_SET)
        return -ERR_INVAL;
    if (old_addr && !copy_user(p, old_addr, &old, sizeof(old), true))
        return -ERR_FAULT;
    if (!set_addr)
        return 0;
    if (!copy_user(p, set_addr, &set, sizeof(set), false))
        return -ERR_FAULT;
    uint64_t state = spin_lock(&table);
    uint64_t blocked = how == SIGMASK_BLOCK ? old | set : how == SIGMASK_UNBLOCK ? old & ~set : set;
    t->sig_blocked = blocked & ~(1ULL << SIGKILL) & ~1ULL;
    spin_unlock(&table, state);
    return 0;
}

void process_hang_up(struct pty *t)
{
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q; q = q->next)
        if (q->tty == t && q->state != PROCESS_ZOMBIE)
            signal_locked(q, SIGHUP);
    spin_unlock(&table, state);
}

int process_signal(int pid, int signal, uint32_t uid)
{
    int result = -ERR_SRCH;
    uint64_t state = spin_lock(&table);
    for (struct process *p = all_processes; p; p = p->next) {
        if (p->pid == pid && p->state != PROCESS_ZOMBIE) {
            if (uid != 0 && p->uid != uid) {
                result = -ERR_PERM;
                continue;
            }
            signal_locked(p, signal);
            result = 0;
        }
    }
    spin_unlock(&table, state);
    return result;
}

bool process_signal_group(int group, int signal)
{
    bool takes = false;
    uint64_t state = spin_lock(&table);
    for (struct process *p = all_processes; p; p = p->next) {
        if (p->group == group && p->state != PROCESS_ZOMBIE &&
            !(signal < 32 && (p->ignored & (1u << signal))))
            takes = true;
    }
    for (struct process *p = all_processes; p && takes; p = p->next) {
        if (p->group == group)
            signal_locked(p, signal);
    }
    spin_unlock(&table, state);
    return takes;
}

/* Call handler for signal on the way back to the program: its blocked set
 * then also holds the handler's mask and (unless NODEFER) the signal. */
static bool run_handler(struct process *p, void *frame, int signal, uint64_t handler,
                        uint64_t mask, uint8_t flags)
{
    struct thread *t = thread_current();
    uint64_t old = t->suspended ? t->suspend_mask : t->sig_blocked;   /* (52i) */
    if (!arch_signal_enter(frame, handler, signal, p->restorer, old))
        return false;
    t->suspended = false;
    t->sig_blocked = old | mask | (flags & SIGACTION_NODEFER ? 0 : 1ULL << signal);
    t->sig_blocked &= ~(1ULL << SIGKILL);
    return true;
}

void process_before_return(void *frame)
{
    struct process *p = current_process();
    if (p && p->kill_signal)
        process_exit(STATUS_SIGNALED(p->kill_signal));
    if (p && p->exiting && p->exiter != thread_current())
        leave_thread(p, 0);                         /* another thread ends the process */
    if (!p)
        return;
    thread_current()->stop_requested = false;       /* an ignored or handled signal: done */
    if (thread_current()->enter_at) {               /* (54c) an asynchronous cancel */
        uint64_t entry = thread_current()->enter_at;
        thread_current()->enter_at = 0;
        if (!run_handler(p, frame, 0, entry, ~0ULL, SIGACTION_NODEFER)) {
            kprintf("%s: no room on its stack for its cancellation: program stopped\n", p->name);
            process_exit(STATUS_SIGNALED(SIGSEGV));
        }
        return;
    }
    /* A handled signal waiting (22r): the lowest this thread does not block.
     * (65e-43) Looked at first without the process table's lock, which
     * every return to a program took: on the laptop 8 pairs of threads
     * passing tokens through pipes spun on it 45 s in 10 s. A signal is
     * posted under that lock before its thread is woken or interrupted
     * (the wake-up's locks, the interrupt's barrier), so it is seen here;
     * one posted after this look is taken at the next return. */
    struct thread *t = thread_current();
    if (!(__atomic_load_n(&p->pending, __ATOMIC_ACQUIRE) & ~t->sig_blocked)) {
        if (t->suspended) {
            t->sig_blocked = t->suspend_mask;       /* (52i: no handler ran: the set again) */
            t->suspended = false;
        }
        return;
    }
    uint64_t state = spin_lock(&table);
    uint64_t ready = p->pending & ~t->sig_blocked;
    int signal = ready ? __builtin_ctzll(ready) : 0;
    uint64_t handler = 0, mask = 0;
    uint8_t flags = 0;
    bool ignored = false;
    if (signal) {
        p->pending &= ~(1ULL << signal);
        handler = p->handlers[signal];
        mask = p->handler_masks[signal];
        flags = p->handler_flags[signal];
        ignored = p->ignored & (1u << signal);
        if (flags & SIGACTION_RESETHAND)
            p->handlers[signal] = 0;
    }
    spin_unlock(&table, state);
    if (t->suspended && (!signal || (!handler && ignored))) {
        t->sig_blocked = t->suspend_mask;           /* (52i: no handler ran: the set again) */
        t->suspended = false;
    }
    if (!signal || (!handler && ignored))
        return;                                     /* (nothing, or ignored since it came) */
    if (!handler)
        process_exit(STATUS_SIGNALED(signal));      /* back to the default since it came */
    if (!run_handler(p, frame, signal, handler, mask, flags)) {
        kprintf("%s: no room on its stack for the handler of signal %d: program stopped\n",
                p->name, signal);
        process_exit(STATUS_SIGNALED(SIGSEGV));
    }
}

bool process_fault_signal(void *frame, int signal)
{
    struct process *p = current_process();
    if (!p || signal <= 0 || signal >= SIGNALS)
        return false;
    uint64_t state = spin_lock(&table);
    uint64_t handler = p->handlers[signal], mask = p->handler_masks[signal];
    uint8_t flags = p->handler_flags[signal];
    bool blocked = thread_current()->sig_blocked & (1ULL << signal);
    if (handler && !blocked && (flags & SIGACTION_RESETHAND))
        p->handlers[signal] = 0;
    spin_unlock(&table, state);
    /* (a fault while its own handler runs ends the program, as on Unix) */
    return handler && !blocked && run_handler(p, frame, signal, handler, mask, flags);
}

void process_sigreturn(void *frame)
{
    uint64_t blocked;
    if (!arch_signal_return(frame, &blocked)) {
        struct process *p = current_process();
        kprintf("%s: a signal handler's return with a damaged context: program stopped\n",
                p ? p->name : "?");
        process_exit(STATUS_SIGNALED(SIGSEGV));
    }
    thread_current()->sig_blocked = blocked & ~(1ULL << SIGKILL);
}

bool process_copy_to_user(uint64_t addr, const void *kernel, uint64_t size)
{
    struct process *p = current_process();
    return p && copy_user(p, addr, (void *)kernel, size, true);
}

bool process_copy_from_user(void *kernel, uint64_t addr, uint64_t size)
{
    struct process *p = current_process();
    return p && copy_user(p, addr, kernel, size, false);
}

int process_page_fault(uint64_t address, bool write, bool exec)
{
    struct process *p = current_process();
    return p && p->space ? mmap_fault(&p->mm, p->space, address, write, exec) : SIGSEGV;
}

void process_fault(const char *what, uint64_t pc, uint64_t address, int signal)
{
    struct process *p = current_process();
    bool locked = kprintf_lock();
    console_set_color(COLOR_ERROR);
    kprintf("%s: %s at pc %p", p ? p->name : "?", what, (void *)pc);
    if (p && p->bias && pc >= p->image_lo && pc < p->image_hi)
        kprintf(" (%p as linked)", (void *)(pc - p->bias));   /* (22l: for a disassembler) */
    if (address != ~0ULL)
        kprintf(", address %p", (void *)address);
    kprintf(": program stopped\n");
    console_set_color(COLOR_TEXT);
    kprintf_unlock(locked);
    process_exit(STATUS_SIGNALED(signal));
}

const char *process_error_text(int error)
{
    switch (error) {
    case ERR_NOENT:  return "No such file or directory";
    case ERR_LOOP:   return "Too many levels of symbolic links";
    case ERR_NOEXEC: return "Exec format error";
    case ERR_NOMEM:  return "Out of memory";
    case ERR_2BIG:   return "Argument list too long";
    case ERR_IO:     return "Input/output error";
    default:         return "Error";
    }
}

void process_print_all(void)
{
    static const char *const states[] = { "new", "running", "ended" };
    struct { int pid, ppid, group; enum process_state state; char name[32]; } copy[32];
    int n = 0;
    uint64_t lock_state = spin_lock(&table);
    for (struct process *p = all_processes; p && n < 32; p = p->next, n++) {
        copy[n].pid = p->pid;
        copy[n].ppid = p->parent ? p->parent->pid : 0;
        copy[n].group = p->group;
        copy[n].state = p->state;
        memcpy(copy[n].name, p->name, sizeof(copy[n].name));
    }
    spin_unlock(&table, lock_state);
    kprintf("  pid  parent  group  state    name\n");
    for (int i = n - 1; i >= 0; i--)
        kprintf("  %3d  %6d  %5d  %-7s  %s\n", copy[i].pid, copy[i].ppid, copy[i].group,
                states[copy[i].state], copy[i].name);
    if (!n)
        kprintf("  (no processes)\n");
}

/* --- files and the terminal ----------------------------------------------------- */

/* A descriptor's open file, or 0. The caller holds p->fds (every close
 * takes it too, so no other thread can close it meanwhile) or uses
 * file_get. */
static struct open_file *file(struct process *p, uint64_t fd)
{
    struct open_file *f = fd < FD_MAX ? p->files[fd] : 0;
    return f && f->kind != FILE_CLOSED && f->kind != FILE_RESERVED ? f : 0;
}

/* A descriptor's open file held, for a system call that uses it without
 * p->fds (it may sleep): another thread's close still closes it, but its
 * memory stays until file_release. */
static struct open_file *file_get(struct process *p, uint64_t fd)
{
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    if (f)
        __atomic_add_fetch(&f->holds, 1, __ATOMIC_SEQ_CST);
    spin_unlock(&p->fds, s);
    return f;
}

/* (65e-42) The descriptors together keep one hold of their own (from the
 * claim), given back when the last reference goes: so the one that takes
 * holds to 0 frees it, and nobody else. Until 65e-42 a call's release freed
 * when holds reached 0 and refs was 0, two looks: a close between them
 * (holds back up, refs to 0) made both free it (seen as "heap: double
 * free" under smpstress's descriptors part, the old kernel too). */
static void file_release(struct open_file *f)
{
    if (f != &console_terminal && __atomic_sub_fetch(&f->holds, 1, __ATOMIC_SEQ_CST) == 0)
        kfree(f);
}

/* Drop a descriptor's reference: the last one closes the file
 * (close_file's result; the descriptors' hold keeps it while it does). */
static int64_t file_put(struct open_file *f)
{
    if (f == &console_terminal)
        return 0;
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_SEQ_CST) != 0)
        return 0;
    int64_t r = close_file(f);
    file_release(f);                                /* (the descriptors' hold) */
    return r;
}

/* (65e-42) Around every change of a slot that a look may hold (p->fds
 * held): see fd_use_peek, below. */
static void fds_change_begin(struct process *p)
{
    __atomic_add_fetch(&p->fds_changing, 1, __ATOMIC_SEQ_CST);
    cpus_wait_fd_peeks();
}

static void fds_change_end(struct process *p)
{
    __atomic_sub_fetch(&p->fds_changing, 1, __ATOMIC_RELEASE);
}

/* The descriptor table (49b): threads of one program share it, so claiming,
 * installing and closing a slot run under p->fds (interrupts off: a short
 * critical section the timer tick cannot preempt). A claimed slot is
 * FILE_RESERVED until installed, so a second thread's scan skips it and the
 * slow part of open() (the file system work) happens without the lock. */
static int fd_claim(struct process *p)
{
    struct open_file *f = kzalloc(sizeof(*f));      /* (51a: made here, so installing cannot fail) */
    if (!f)
        return -ERR_NOMEM;
    f->refs = 1;
    f->holds = 1;                                   /* (65e-42: the descriptors' hold) */
    f->kind = FILE_RESERVED;
    uint64_t s = spin_lock(&p->fds);
    int fd = 0;
    while (fd < FD_MAX && p->files[fd])
        fd++;
    if (fd < FD_MAX)                                /* (65e-42: a look sees it made) */
        __atomic_store_n(&p->files[fd], f, __ATOMIC_RELEASE);
    spin_unlock(&p->fds, s);
    if (fd == FD_MAX)
        kfree(f);
    return fd < FD_MAX ? fd : -ERR_MFILE;
}

static void fd_install(struct process *p, int fd, const struct open_file *f)
{
    uint64_t s = spin_lock(&p->fds);
    fds_change_begin(p);                            /* (65e-42: no look sees it half made) */
    struct open_file *slot = p->files[fd];
    *slot = *f;                                     /* f->kind is the real kind */
    slot->refs = 1;
    slot->holds = 1;                                /* (65e-42: the descriptors' hold) */
    fds_change_end(p);
    spin_unlock(&p->fds, s);
}

static void fd_release(struct process *p, int fd)   /* a claim that was not installed */
{
    uint64_t s = spin_lock(&p->fds);
    fds_change_begin(p);
    struct open_file *f = p->files[fd];
    __atomic_store_n(&p->files[fd], 0, __ATOMIC_RELEASE);
    fds_change_end(p);
    spin_unlock(&p->fds, s);
    kfree(f);
}

/* The socket of a descriptor with a reference held, so a blocking call
 * (recv, connect, accept, a TCP send) keeps it alive even if another thread
 * closes the descriptor meanwhile (49b); release with socket_close. 0 if
 * the descriptor is not an open socket. */
static struct socket *socket_grab(struct process *p, uint64_t fd)
{
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    struct socket *sock = f && f->kind == FILE_SOCKET ? f->socket : 0;
    if (sock)
        socket_hold(sock);
    spin_unlock(&p->fds, s);
    return sock;
}

/* Copy a local socket's name (bind/connect) from the program into out
 * (char[LOCAL_PATH_MAX], NUL-terminated). 0 or ERR_*. */
static int copy_local_name(struct process *p, uint64_t user, uint64_t len, char *out)
{
    if (len == 0 || len >= LOCAL_PATH_MAX)
        return ERR_NAMETOOLONG;
    if (!copy_user(p, user, out, len, false))
        return ERR_FAULT;
    out[len] = 0;
    return 0;
}

/* Bytes of buf[0..n) that end on a complete UTF-8 character. */
static size_t whole_characters(const char *buf, size_t n)
{
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = (uint8_t)buf[n - back];
        if ((c & 0xC0) == 0x80)
            continue;                               /* a continuation byte */
        unsigned length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return length > back ? n - back : n;
    }
    return n;
}

/* Output to the terminal. A UTF-8 character split between two writes is
 * kept until its last byte arrives, so the console never sees half of it. */
static int64_t write_terminal(struct process *p, uint64_t fd, uint64_t buffer, uint64_t size)
{
    char chunk[sizeof(p->carry) + WRITE_CHUNK + 1];
    uint64_t done = 0;
    bool locked = kprintf_lock();
    if (fd == FD_STDERR)
        console_set_color(COLOR_ERROR);
    while (done < size) {
        uint64_t n = size - done < WRITE_CHUNK ? size - done : WRITE_CHUNK;
        memcpy(chunk, p->carry, p->carry_length);
        if (!copy_user(p, buffer + done, chunk + p->carry_length, n, false))
            break;
        size_t total = p->carry_length + n, whole = whole_characters(chunk, total);
        if (total - whole >= sizeof(p->carry))
            whole = total;                          /* not UTF-8 after all: print it */
        p->carry_length = total - whole;
        memcpy(p->carry, chunk + whole, p->carry_length);
        chunk[whole] = 0;
        thread_current()->log_quiet++;              /* a program's output, not the kernel's */
        kprintf("%s", chunk);
        thread_current()->log_quiet--;
        terminal_wrote(chunk, whole);
        if (p->log[0])
            log_append(p->log, chunk, whole);
        done += n;
    }
    if (fd == FD_STDERR)
        console_set_color(COLOR_TEXT);
    kprintf_unlock(locked);
    return done == 0 && size > 0 ? -ERR_FAULT : (int64_t)done;
}

static bool console_user(struct process *p);

/* The foreground group of p's terminal (a pseudo-terminal's, or the
 * console's). */
static int foreground_of(struct process *p)
{
    if (p->no_terminal)
        return -1;                                  /* (no group is: never the foreground) */
    return p->tty ? pty_foreground(p->tty) : terminal_foreground();
}

static void set_foreground_of(struct process *p, int group)
{
    if (p->no_terminal)
        return;
    if (p->tty)
        pty_set_foreground(p->tty, group);
    else
        terminal_set_foreground(group, p->session);
}

/* (65e) Whether p's terminal belongs to p's session now: a pseudo-terminal
 * to the session it is the terminal of, the console (shared: the console
 * shell's session, elves', sshd's commands on pipes) to the session that
 * gave its foreground out last. */
static bool terminal_is_sessions(struct process *p)
{
    int session = p->tty ? pty_session(p->tty) : terminal_foreground_session();
    return session && session == p->session;
}

/* A pseudo-terminal's side: the slave as the terminal's input and output
 * (only the foreground job reads; in the background the input is at its
 * end), the master as what sshd reads and types. A reference is held
 * while the call waits, so that a close by another thread cannot free it. */
/* (65e-40) What a read or a write needs of its descriptor, taken in one
 * hold of p->fds: the open file held (holds), a socket's socket, a pipe's or
 * pseudo-terminal's end held (pipe_hold, pty_hold_terminal: under the lock,
 * before anything that may sleep, 49l: else another thread's close could
 * free it first), and the flags. Until 65e-40 a read of a pipe took the
 * lock four times (O_NONBLOCK, socket or not, the file, its end): on the
 * laptop 8 pairs of threads passing a token through pipes spun on it 146 s
 * in 10 s. */
struct fd_use {
    struct open_file *f;            /* held, unless a socket */
    struct socket *sock;            /* a socket's, held */
    void *end;                      /* a pipe's or pseudo-terminal's, held */
    enum file_kind kind;            /* as it was then */
    bool nonblock, master, writable;
};

/* u's holds of open file f, looked at under p->fds or by fd_use_peek. */
static void use_take(struct fd_use *u, struct open_file *f)
{
    u->kind = f->kind;
    u->nonblock = f->nonblock;
    u->master = f->master;
    u->writable = f->writable;
    if (f->kind == FILE_SOCKET) {
        u->sock = f->socket;
        socket_hold(u->sock);
    } else {
        u->f = f;
        __atomic_add_fetch(&f->holds, 1, __ATOMIC_SEQ_CST);
        if (f->kind == FILE_PIPE) {
            u->end = f->pipe;
            pipe_hold(f->pipe);
        } else if (f->kind == FILE_PTY) {
            u->end = f->pty;
            pty_hold_terminal(f->pty);
        }
    }
}

/* (65e-42) The slot looked at without p->fds: only the program's own
 * threads look into its table, and everything that changes a slot (an
 * install, a close, dup2, exec's close-on-exec) first marks the table
 * changing and waits until no core is in such a look (fds_change_begin):
 * a look either sees the mark and takes the lock instead, or ends before
 * the slot changes, its holds taken (an open file is freed only after
 * its slot was cleared and its holds given back). The look is counted in
 * this core's own block, so cores never share it. Until 65e-42 every read
 * and write took p->fds once: on the laptop 8 pairs of threads passing
 * tokens through pipes (3.3 million calls a second) spun on it 111 s in
 * 10 s. 1: got it, 0: no such descriptor, -1: take the lock. */
static int fd_use_peek(struct process *p, uint64_t fd, struct fd_use *u)
{
    uint64_t irq = arch_irq_save();
    struct cpu *c = arch_this_cpu();
    __atomic_store_n(&c->fd_peeks, c->fd_peeks + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);       /* (the changer's fence matches) */
    int r = -1;
    /* (Acquire: the slot is read after the mark. Relaxed, ARM read the slot
     * first, then the mark of a change already over: the old file, closed
     * meanwhile -- pipe_read(0) under smpstress's descriptors part.) */
    if (!__atomic_load_n(&p->fds_changing, __ATOMIC_ACQUIRE)) {
        struct open_file *f = fd < FD_MAX ? __atomic_load_n(&p->files[fd], __ATOMIC_ACQUIRE) : 0;
        r = f && f->kind != FILE_CLOSED && f->kind != FILE_RESERVED;
        if (r)
            use_take(u, f);
    }
    __atomic_store_n(&c->fd_peeks, c->fd_peeks - 1, __ATOMIC_RELEASE);
    arch_irq_restore(irq);
    return r;
}

static bool fd_use_get(struct process *p, uint64_t fd, struct fd_use *u)
{
    memset(u, 0, sizeof(*u));
    int peeked = fd_use_peek(p, fd, u);
    if (peeked >= 0)
        return peeked;
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    if (f)
        use_take(u, f);
    spin_unlock(&p->fds, s);
    return f != 0;
}

static void fd_use_put(struct fd_use *u)
{
    if (u->sock)
        socket_close(u->sock);
    if (u->end && u->kind == FILE_PIPE)
        pipe_unhold(u->end);
    else if (u->end)
        pty_release_terminal(u->end);
    if (u->f)
        file_release(u->f);
}

/* (65e-40) The end comes held with the descriptor (fd_use_get): the call
 * gives it back. (65e-41) The four calls below copy through a buffer on
 * the kernel stack (32 KiB): until 65e-41 each took 4 KiB from kmalloc,
 * a run of two pages with the block's header, searched under the page
 * allocator's lock (on the laptop 8 pairs of threads passing tokens
 * through pipes spun on it 180 s in 10 s). */
#define END_CHUNK 4096

static int64_t read_pty(struct process *p, struct pty *t, bool master, uint64_t buffer,
                        uint64_t size)
{
    if (!master && p->tty == t && p->group != pty_foreground(t))
        return 0;
    uint8_t chunk[END_CHUNK];
    size_t n = size < END_CHUNK ? size : END_CHUNK;
    int64_t r = master ? pty_read_master(t, chunk, n) : pty_read_slave(t, chunk, n);
    if (r > 0 && !copy_user(p, buffer, chunk, (uint64_t)r, true))
        r = -ERR_FAULT;
    return r;
}

static int64_t write_pty(struct process *p, struct pty *t, bool master, uint64_t buffer,
                         uint64_t size)
{
    uint8_t chunk[END_CHUNK];
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t n = size - done < END_CHUNK ? size - done : END_CHUNK;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        int64_t r = master ? pty_write_master(t, chunk, n) : pty_write_slave(t, chunk, n);
        if (r <= 0) {
            error = r;
            break;
        }
        done += (uint64_t)r;
    }
    return done > 0 ? (int64_t)done : error;
}

/* A pipe end. A reference is held across the call (pipe_hold, taken with
 * the descriptor), so a close by another thread of the process cannot free
 * the pipe while it waits. */
static int64_t read_pipe(struct process *p, struct pipe *pp, uint64_t buffer, uint64_t size)
{
    uint8_t chunk[END_CHUNK];
    int64_t r = pipe_read(pp, chunk, size < END_CHUNK ? size : END_CHUNK);
    if (r > 0 && !copy_user(p, buffer, chunk, (uint64_t)r, true))
        r = -ERR_FAULT;
    return r;
}

static int64_t write_pipe(struct process *p, struct pipe *pp, uint64_t buffer, uint64_t size)
{
    uint8_t chunk[END_CHUNK];
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t n = size - done < END_CHUNK ? size - done : END_CHUNK;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        int64_t r = pipe_write(pp, chunk, n);
        if (r <= 0) {
            error = r;
            break;
        }
        done += (uint64_t)r;
    }
    return done > 0 ? (int64_t)done : error;
}

/* (52c) /dev/null (reads nothing, takes every write), /dev/zero (reads
 * zeros) and /dev/tty (the caller's terminal, whatever its descriptors
 * are): not listed by readdir, as /dev/pcmN are not. */
enum { SPECIAL_NONE, SPECIAL_NULL, SPECIAL_ZERO, SPECIAL_TTY };

static int special_device(const char *path)
{
    return !strcmp(path, DEVICE_DIR "/null") ? SPECIAL_NULL
         : !strcmp(path, DEVICE_DIR "/zero") ? SPECIAL_ZERO
         : !strcmp(path, DEVICE_DIR "/tty") ? SPECIAL_TTY : SPECIAL_NONE;
}

/* /dev/tty: the process's pseudo-terminal (a slave end of its own), or
 * the console when that is its terminal; ENXIO without one (49k). */
static int64_t open_tty(struct process *p, uint64_t access, struct open_file *f)
{
    if (p->tty) {
        f->kind = FILE_PTY;
        f->pty = p->tty;
        f->master = false;
        pty_hold(p->tty, false);
    } else if (!p->no_terminal) {
        f->kind = FILE_TERMINAL;
    } else {
        return -ERR_NXIO;
    }
    f->readable = access != OPEN_WRITE;
    f->writable = access != OPEN_READ;
    return 0;
}

/* /dev/zero's reads: zeros, a page at a time. */
static int64_t read_zero(struct process *p, uint64_t buffer, uint64_t size)
{
    static uint8_t zeros[512];              /* (never written: copy_user takes either way) */
    uint64_t done = 0;
    while (done < size) {
        uint64_t n = size - done < sizeof(zeros) ? size - done : sizeof(zeros);
        if (!copy_user(p, buffer + done, zeros, n, true))
            return done ? (int64_t)done : -ERR_FAULT;
        done += n;
    }
    return (int64_t)done;
}

/* The number N of a path DEVICE_DIR/<name>N (one or two digits), or -1. */
static int device_number(const char *path, const char *name)
{
    size_t d = strlen(DEVICE_DIR), n = d + 1 + strlen(name);
    if (strncmp(path, DEVICE_DIR, d) || path[d] != '/' || strncmp(path + d + 1, name, n - d - 1))
        return -1;
    if (!path[n] || strlen(path + n) > 2)
        return -1;
    int v = 0;
    for (const char *c = path + n; *c; c++) {
        if (*c < '0' || *c > '9')
            return -1;
        v = v * 10 + (*c - '0');
    }
    return v;
}

/* The number N of a path /dev/usbserialN, or -1. */
static int serial_number(const char *path)
{
    return device_number(path, "usbserial");
}

/* The number N of a path /dev/tapN, or -1. */
static int tap_number(const char *path)
{
    return device_number(path, "tap");
}

/* The number N of a path /dev/pcmN, or -1. */
static int pcm_number(const char *path)
{
    return device_number(path, "pcm");
}

/* A sound device /dev/pcmN: copy the program's PCM in and play it (blocks
 * while the ring is full). */
static int64_t write_pcm(struct process *p, int dev, uint64_t buffer, uint64_t size)
{
    uint8_t *chunk = kmalloc(4096);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t n = size - done < 4096 ? size - done : 4096;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        long r = sound_write(dev, chunk, n);
        if (r <= 0) {
            error = r < 0 ? -ERR_IO : (int64_t)done;
            break;
        }
        done += (uint64_t)r;
    }
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

/* Input from the terminal: one edited line at a time, as a Unix terminal
 * in its usual ("canonical") mode does. Only the foreground job reads it;
 * for a process in the background it is at its end. */
static struct mutex terminal_input = MUTEX_INIT("terminal input");

static int64_t read_terminal(struct process *p, uint64_t buffer, uint64_t size)
{
    if (p->group != terminal_foreground())
        return 0;
    if (thread_current()->dont_wait && p->line_read == p->line_length && !terminal_input_waiting())
        return -ERR_AGAIN;                          /* (53b; a line read waits for its Enter) */
    if (terminal_is_raw() && p->line_read == p->line_length) {
        uint8_t bytes[64];                          /* what has been typed so far */
        mutex_lock(&terminal_input);
        int n = terminal_read_raw(bytes, size < sizeof(bytes) ? size : sizeof(bytes));
        mutex_unlock(&terminal_input);
        if (n == TERMINAL_INTERRUPTED)
            return -ERR_INTR;
        return copy_user(p, buffer, bytes, (size_t)n, true) ? n : -ERR_FAULT;
    }
    if (p->line_read == p->line_length) {
        bool newline;
        mutex_lock(&terminal_input);
        int n = terminal_read_line(p->line, LINE_MAX, &newline);
        mutex_unlock(&terminal_input);
        if (n == TERMINAL_INTERRUPTED)
            return -ERR_INTR;
        if (n == TERMINAL_EOF)
            return 0;
        if (newline)
            p->line[n++] = '\n';
        p->line_length = (size_t)n;
        p->line_read = 0;
    }
    uint64_t n = p->line_length - p->line_read;
    if (n > size)
        n = size;
    if (!copy_user(p, buffer, p->line + p->line_read, n, true))
        return -ERR_FAULT;
    p->line_read += n;
    return (int64_t)n;
}

/* The open file's inode as it is now: another descriptor (or program) may
 * have changed the file. False if the file is gone. */
static bool refresh(struct open_file *f)
{
    if (f->kind != FILE_RITTERFS)
        return true;
    return vfs_get_inode(f->fs, f->object_id, &f->inode) && f->inode.type != RFS_TYPE_FREE;
}

/* Raw bytes of a disk or partition, from the descriptor's position (reads
 * of whole blocks; the device may be gone: EIO). */
static int64_t read_device(struct process *p, struct open_file *f, uint64_t buffer, uint64_t size)
{
    struct block_device *d = f->device;
    uint64_t total = d->block_count * d->block_size;
    if (f->offset >= total)
        return 0;
    if (size > total - f->offset)
        size = total - f->offset;
    uint32_t blocks = d->block_size < RFS_BLOCK_SIZE ? RFS_BLOCK_SIZE / d->block_size : 1;
    uint64_t chunk_bytes = (uint64_t)blocks * d->block_size;
    uint8_t *chunk = kmalloc(chunk_bytes);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t lba = f->offset / d->block_size, within = f->offset % d->block_size;
        uint32_t n = d->block_count - lba < blocks ? (uint32_t)(d->block_count - lba) : blocks;
        if (!d->read(d, lba, n, chunk)) {
            error = -ERR_IO;
            break;
        }
        uint64_t take = (uint64_t)n * d->block_size - within;
        if (take > size - done)
            take = size - done;
        if (!copy_user(p, buffer + done, chunk + within, take, true)) {
            error = -ERR_FAULT;
            break;
        }
        f->offset += take;
        done += take;
    }
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

/* A USB serial port (50e): what came, or a wait for it; then out to the
 * program. Writes go to the device a page at a time. */
static int64_t read_usbserial(struct process *p, struct open_file *f, uint64_t buffer,
                              uint64_t size)
{
    uint8_t *chunk = kmalloc(4096);
    if (!chunk)
        return -ERR_NOMEM;
    int64_t r = usbserial_read(f->serial, chunk, size < 4096 ? size : 4096);
    if (r > 0 && !copy_user(p, buffer, chunk, (uint64_t)r, true))
        r = -ERR_FAULT;
    kfree(chunk);
    return r;
}

static int64_t write_usbserial(struct process *p, struct open_file *f, uint64_t buffer,
                               uint64_t size)
{
    if (!f->writable)
        return -ERR_BADF;
    uint8_t *chunk = kmalloc(4096);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t n = size - done < 4096 ? size - done : 4096;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        int64_t r = usbserial_write(f->serial, chunk, n);
        if (r < 0) {
            error = r;
            break;
        }
        done += (uint64_t)r;
    }
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

/* (38a) /dev/tapN: one frame per read, one per write. */
static int64_t read_tap(struct process *p, struct open_file *f, uint64_t buffer, uint64_t size)
{
    uint8_t frame[ETH_FRAME_MAX];
    int64_t r = tap_read(f->tap, frame, size < sizeof(frame) ? size : sizeof(frame));
    if (r > 0 && !copy_user(p, buffer, frame, (uint64_t)r, true))
        r = -ERR_FAULT;
    return r;
}

static int64_t write_tap(struct process *p, struct open_file *f, uint64_t buffer, uint64_t size)
{
    if (!f->writable)
        return -ERR_BADF;
    uint8_t frame[ETH_FRAME_MAX];
    if (size > sizeof(frame))
        return -ERR_INVAL;
    if (!copy_user(p, buffer, frame, size, false))
        return -ERR_FAULT;
    return tap_write(f->tap, frame, size);
}

static int64_t read_file(struct process *p, const struct fd_use *u, uint64_t buffer,
                         uint64_t size)
{
    struct open_file *f = u->f;
    if (!f->readable)
        return -ERR_BADF;
    if (f->kind == FILE_DEVICE)
        return read_device(p, f, buffer, size);
    if (u->kind == FILE_PTY)
        return read_pty(p, u->end, u->master, buffer, size);
    if (u->kind == FILE_PIPE)
        return read_pipe(p, u->end, buffer, size);
    if (f->kind == FILE_USBSERIAL)
        return read_usbserial(p, f, buffer, size);
    if (f->kind == FILE_TAP)
        return read_tap(p, f, buffer, size);
    if (f->kind == FILE_NULL)
        return 0;
    if (f->kind == FILE_ZERO)
        return read_zero(p, buffer, size);
    if (f->kind == FILE_DEVICES)
        return -ERR_ISDIR;
    if (!refresh(f))
        return 0;                                   /* removed meanwhile: at its end */
    if (f->kind == FILE_SNAPSHOTS || f->kind == FILE_VOLUMES || f->inode.type == RFS_TYPE_DIR)
        return -ERR_ISDIR;
    uint8_t *chunk = kmalloc(RFS_BLOCK_SIZE);
    if (!chunk)
        return -ERR_IO;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t want = size - done < RFS_BLOCK_SIZE ? size - done : RFS_BLOCK_SIZE;
        int64_t n = vfs_read_now(f->fs, f->object_id, &f->inode, f->offset, chunk, want);
        if (n < 0) {
            error = -ERR_IO;                        /* e.g. a block failed its checksum */
            break;
        }
        if (n == 0)
            break;                                  /* the end of the file */
        if (!copy_user(p, buffer + done, chunk, (uint64_t)n, true)) {
            error = -ERR_FAULT;
            break;
        }
        f->offset += (uint64_t)n;
        done += (uint64_t)n;
    }
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

static int64_t sys_read(struct process *p, const struct fd_use *u, uint64_t buffer, uint64_t size)
{
    if (u->sock)
        return socket_read(p, u->sock, buffer, size, 0, 0, false);
    return u->kind == FILE_TERMINAL ? read_terminal(p, buffer, size)
                                    : read_file(p, u, buffer, size);
}

static int64_t fs_error(int r);

/* Sockets: data between the program and the kernel's socket, in pieces. */
/* (64c) An address of the program's (struct sys_inaddr) and back. */
static int64_t inaddr_from_user(struct process *p, uint64_t addr, struct inet_addr *out,
                                uint16_t *port)
{
    struct sys_inaddr a;
    if (!copy_user(p, addr, &a, sizeof(a), false))
        return -ERR_FAULT;
    if (a.family == 2) {
        *out = inet_v4((uint32_t)a.address[12] << 24 | (uint32_t)a.address[13] << 16 |
                       (uint32_t)a.address[14] << 8 | a.address[15]);
    } else if (a.family == 10) {
        memcpy(out->b, a.address, 16);
        out->scope = a.scope;
        if (a.scope > (uint32_t)net_count())
            return -ERR_NODEV;
    } else {
        return -ERR_AFNOSUPPORT;
    }
    *port = a.port;
    return 0;
}

static bool inaddr_to_user(struct process *p, uint64_t addr, const struct inet_addr *in,
                           uint16_t port, bool six)
{
    struct sys_inaddr a;
    memset(&a, 0, sizeof(a));
    a.family = six ? 10 : 2;
    a.port = port;
    a.scope = in->scope;
    memcpy(a.address, in->b, 16);
    return copy_user(p, addr, &a, sizeof(a), true);
}

#define READ_CHUNK 9000                     /* socket.c's DATAGRAM_MAX and more */

/* six: from_addr is a struct sys_inaddr (recvfrom6), else the IPv4 form
 * address << 16 | port (0 for an IPv6 sender). */
static int64_t socket_read(struct process *p, struct socket *s, uint64_t buffer, uint64_t size,
                           uint64_t from_addr, uint32_t timeout_ms, bool six)
{
    uint8_t *chunk = kmalloc(READ_CHUNK);
    if (!chunk)
        return -ERR_NOMEM;
    struct inet_addr ip = inet_v4(0);
    uint16_t port = 0;
    int64_t n = socket_recvfrom(s, chunk, size < READ_CHUNK ? size : READ_CHUNK, &ip, &port,
                                timeout_ms);
    if (n > 0 && !copy_user(p, buffer, chunk, (uint64_t)n, true))
        n = -ERR_FAULT;
    uint64_t from = (uint64_t)inet_v4_of(&ip) << 16 | port;
    if (n >= 0 && from_addr &&
        !(six ? inaddr_to_user(p, from_addr, &ip, port, socket_six(s))
              : copy_user(p, from_addr, &from, sizeof(from), true)))
        n = -ERR_FAULT;
    kfree(chunk);
    return n;
}

/* to 0: the connected peer (TCP's stream: everything, in pieces). */
static int64_t socket_write(struct process *p, struct socket *s, uint64_t buffer, uint64_t size,
                            const struct inet_addr *to, uint16_t port)
{
    uint8_t *chunk = kmalloc(IP_PAYLOAD_MAX);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    do {
        uint64_t n = size - done < IP_PAYLOAD_MAX - 8 ? size - done : IP_PAYLOAD_MAX - 8;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        int64_t r = socket_sendto(s, chunk, n, to, port);
        if (r < 0) {
            error = r;
            break;
        }
        done += (uint64_t)r;
    } while (done < size && !to);                   /* a datagram is sent whole, once */
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

static int64_t write_file(struct process *p, struct open_file *f, uint64_t buffer, uint64_t size)
{
    if (!f->writable)
        return -ERR_BADF;
    if (!refresh(f))
        return -ERR_NOENT;                          /* removed meanwhile */
    if (f->append)
        f->offset = f->inode.size;                  /* (each piece below: at the end as it is then) */
    uint8_t *chunk = kmalloc(RFS_BLOCK_SIZE);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t n = size - done < RFS_BLOCK_SIZE ? size - done : RFS_BLOCK_SIZE;
        if (!copy_user(p, buffer + done, chunk, n, false)) {
            error = -ERR_FAULT;
            break;
        }
        /* (65e-36) An append finds the end and writes there in one hold of
         * the file system's lock: with the size read before, two appenders
         * wrote at one place (eight shells appending a line each kept seven). */
        uint64_t at = f->offset;
        int r = f->append ? vfs_append(f->fs, f->object_id, chunk, n, &at)
                          : vfs_write(f->fs, f->object_id, f->offset, chunk, n);
        f->offset = at;
        if (r != RFS_OK) {
            error = fs_error(r);
            break;
        }
        f->offset += n;
        f->written = true;
        f->unsaved += n;
        done += n;
        if (f->unsaved >= WRITE_COMMIT_BYTES) {     /* do not keep too much in memory */
            if ((r = vfs_commit(f->fs)) != RFS_OK) {
                error = fs_error(r);
                break;
            }
            f->unsaved = 0;
        }
    }
    kfree(chunk);
    refresh(f);
    return done > 0 ? (int64_t)done : error;
}

/* Raw bytes to a disk or partition at the descriptor's position. Blocks
 * written in part are read first; blocks of a mounted file system are
 * refused (EBUSY), the end of the device is ENOSPC. */
static int64_t write_device(struct process *p, struct open_file *f, uint64_t buffer,
                            uint64_t size)
{
    if (!f->writable)
        return -ERR_BADF;
    struct block_device *d = f->device;
    uint64_t total = d->block_count * d->block_size;
    if (size == 0)
        return 0;
    if (f->offset >= total)
        return -ERR_NOSPC;
    if (size > total - f->offset)
        size = total - f->offset;
    if (!d->write)
        return -ERR_ROFS;
    uint32_t blocks = d->block_size < RFS_BLOCK_SIZE ? RFS_BLOCK_SIZE / d->block_size : 1;
    uint64_t chunk_bytes = (uint64_t)blocks * d->block_size;
    uint8_t *chunk = kmalloc(chunk_bytes);
    if (!chunk)
        return -ERR_NOMEM;
    uint64_t done = 0;
    int64_t error = 0;
    while (done < size) {
        uint64_t lba = f->offset / d->block_size, within = f->offset % d->block_size;
        uint64_t take = chunk_bytes - within;
        if (take > size - done)
            take = size - done;
        uint32_t n = (uint32_t)((within + take + d->block_size - 1) / d->block_size);
        if (devices_busy(d, lba, n)) {
            error = -ERR_BUSY;
            break;
        }
        /* The first and last block keep what is not written. */
        if ((within && !d->read(d, lba, 1, chunk)) ||
            ((within + take) % d->block_size &&
             !d->read(d, lba + n - 1, 1, chunk + (uint64_t)(n - 1) * d->block_size))) {
            error = -ERR_IO;
            break;
        }
        if (!copy_user(p, buffer + done, chunk + within, take, false)) {
            error = -ERR_FAULT;
            break;
        }
        if (!d->write(d, lba, n, chunk)) {
            error = -ERR_IO;
            break;
        }
        f->written = true;
        f->offset += take;
        done += take;
    }
    kfree(chunk);
    return done > 0 ? (int64_t)done : error;
}

static int64_t sys_write(struct process *p, uint64_t fd, const struct fd_use *u, uint64_t buffer,
                         uint64_t size)
{
    if (u->sock)
        return socket_write(p, u->sock, buffer, size, 0, 0);
    struct open_file *f = u->f;
    return u->kind == FILE_TERMINAL ? write_terminal(p, fd, buffer, size)
              : f->kind == FILE_DEVICE ? write_device(p, f, buffer, size)
              : u->kind == FILE_PTY ? write_pty(p, u->end, u->master, buffer, size)
              : u->kind == FILE_PIPE ? (u->writable ? write_pipe(p, u->end, buffer, size)
                                                    : -ERR_BADF)
              : f->kind == FILE_PCM ? write_pcm(p, f->pcm, buffer, size)
              : f->kind == FILE_USBSERIAL ? write_usbserial(p, f, buffer, size)
              : f->kind == FILE_TAP ? write_tap(p, f, buffer, size)
              : f->kind == FILE_NULL || f->kind == FILE_ZERO ?
                    (f->writable ? (int64_t)size : -ERR_BADF)        /* (52c: taken, gone) */
              : f->kind != FILE_RITTERFS ? -ERR_BADF
              : write_file(p, f, buffer, size);
}

/* A path from the program, made absolute against its current directory,
 * its links followed (57a; the last one too unless !follow). */
static int64_t user_path_as(struct process *p, uint64_t addr, char *out, bool follow)
{
    char path[PATH_MAX];
    int64_t error = copy_string_from_user(p, addr, path, sizeof(path));
    if (error)
        return error;
    return -(int64_t)resolve_path(p, path, out, follow);
}

static int64_t user_path(struct process *p, uint64_t addr, char *out)
{
    return user_path_as(p, addr, out, true);
}

/* Build an open_file for path/flags into *f: all the file-system work (49b,
 * split out of sys_open). The caller holds a claimed slot, so creating a
 * file here only happens once a descriptor is reserved for it. */
static int64_t open_build(struct process *p, uint64_t path_addr, uint64_t flags,
                          uint64_t mode, struct open_file *f)
{
    char path[PATH_MAX];
    int64_t error = user_path(p, path_addr, path);
    if (error)
        return error;
    uint64_t access = flags & OPEN_ACCESS;
    if (access > OPEN_READ_WRITE ||
        (flags & ~(uint64_t)(OPEN_ACCESS | OPEN_CREATE | OPEN_TRUNCATE | OPEN_APPEND | OPEN_MODE |
                             OPEN_EXCLUSIVE | OPEN_NONBLOCK)))
        return -ERR_INVAL;
    bool writing = access != OPEN_READ;
    if (writing && in_snapshots(path))
        return -ERR_ROFS;
    bool exclusive = (flags & OPEN_CREATE) && (flags & OPEN_EXCLUSIVE);    /* (52j) */
    if (in_devices(path)) {
        if (exclusive && (special_device(path) || device_at(path) || !strcmp(path, DEVICE_DIR) ||
                          pcm_number(path) >= 0 || serial_number(path) >= 0 ||
                          tap_number(path) >= 0))
            return -ERR_EXIST;
        int special = special_device(path);
        if (special == SPECIAL_TTY)                     /* (52c) the caller's terminal */
            return open_tty(p, access, f);
        if (special) {                                  /* /dev/null, /dev/zero: anyone's */
            f->kind = special == SPECIAL_NULL ? FILE_NULL : FILE_ZERO;
            f->readable = access != OPEN_WRITE;
            f->writable = writing;
            return 0;
        }
        if (strcmp(path, DEVICE_DIR) == 0) {
            f->kind = FILE_DEVICES;
        } else if (pcm_number(path) >= 0) {             /* /dev/pcmN: a sound device */
            if (!sound_present(pcm_number(path)))
                return -ERR_NOENT;
            if (access == OPEN_READ)
                return -ERR_ACCES;                  /* audio output: write-only */
            if (p->uid != 0 && !console_user(p))
                return -ERR_ACCES;                  /* the speakers are the console's (49k) */
            f->kind = FILE_PCM;
            f->pcm = pcm_number(path);
            f->readable = false;
            f->writable = true;
            return 0;
        } else if (serial_number(path) >= 0) {          /* /dev/usbserialN (50e) */
            struct usbserial *port = usbserial_find(serial_number(path));
            if (!port)
                return -ERR_NOENT;
            if (p->uid != 0 && !console_user(p))
                return -ERR_ACCES;                  /* root's and the console user's, as pcm */
            if (flags & OPEN_APPEND)
                return -ERR_INVAL;
            f->kind = FILE_USBSERIAL;
            f->serial = port;
            f->readable = access != OPEN_WRITE;
            f->writable = writing;
            return 0;
        } else if (tap_number(path) >= 0) {             /* /dev/tapN (38a) */
            if (flags & OPEN_APPEND)
                return -ERR_INVAL;
            int r = tap_open(tap_number(path), p->uid, &f->tap);
            if (r < 0)
                return r;                           /* none, another user's, held already */
            f->kind = FILE_TAP;
            f->readable = access != OPEN_WRITE;
            f->writable = writing;
            return 0;
        } else if (!(f->device = device_at(path))) {
            return -ERR_NOENT;
        } else if (p->uid != 0) {
            return -ERR_ACCES;                      /* raw disks: root only */
        } else {
            f->kind = FILE_DEVICE;
        }
        if (writing && f->kind == FILE_DEVICES)
            return -ERR_ISDIR;
        if (writing && (flags & OPEN_APPEND))
            return -ERR_INVAL;                      /* a device has no end to append to */
        /* A partition with a mounted file system (or a disk that is one
         * whole) is never written; on a disk, writes check each block. */
        if (writing && partition_disk(f->device) != f->device &&
            devices_busy(f->device, 0, f->device->block_count))
            return -ERR_BUSY;
        f->readable = access != OPEN_WRITE;
        f->writable = writing;
        return 0;
    }
    bool snapshots, created = false;
    int64_t found = find(p, path, &f->fs, &f->object_id, &f->inode, &snapshots);
    if (found == -ERR_ACCES)
        return found;
    if (!found && exclusive)
        return -ERR_EXIST;                          /* (52j: O_EXCL, the name is taken) */
    if (found) {
        if (!writing || !(flags & OPEN_CREATE))
            return -ERR_NOENT;
        const char *inner;
        if ((error = parent_allows(p, path)) || (error = target(p, path, &f->fs, &inner)))
            return error;
        int r = vfs_create(f->fs, inner, RFS_TYPE_FILE, &f->object_id);
        if (r == RFS_OK)
            r = set_creator(p, f->fs, f->object_id,         /* (52b: the mode asked for) */
                            flags & OPEN_MODE ? (unsigned)mode & 0777 & ~p->umask : 0644);
        if (r != RFS_OK)
            return fs_error(r);
        created = true;
        if (!vfs_get_inode(f->fs, f->object_id, &f->inode))
            return -ERR_IO;
    } else if (!snapshots && !may(p, &f->inode, (access != OPEN_WRITE ? MAY_READ : 0) |
                                                   (writing ? MAY_WRITE : 0))) {
        return -ERR_ACCES;
    }
    if (writing && (snapshots || f->inode.type == RFS_TYPE_DIR))
        return snapshots ? -ERR_ROFS : -ERR_ISDIR;
    if (!created && (f->inode.flags & RFS_FLAG_FIXED))
        return -ERR_BUSY;                           /* (35a) the swap file: the kernel's alone */
    if (writing && !created && vfs_read_only(f->fs))
        return -ERR_ROFS;                           /* a snapshot's view, FAT (so far) */
    if (writing && (flags & OPEN_TRUNCATE) && !created) {
        int r = vfs_truncate(f->fs, f->object_id);
        if (r != RFS_OK)
            return fs_error(r);
        refresh(f);
        f->written = true;
    }
    f->kind = !snapshots ? FILE_RITTERFS
             : strcmp(path, VOLUMES_DIR) == 0 ? FILE_VOLUMES : FILE_SNAPSHOTS;
    f->readable = access != OPEN_WRITE;
    f->writable = writing;
    f->append = writing && (flags & OPEN_APPEND);
    f->written |= created;                          /* the new file is committed at close */
    return 0;
}

static int64_t sys_open(struct process *p, uint64_t path_addr, uint64_t flags, uint64_t mode)
{
    int fd = fd_claim(p);
    if (fd < 0)
        return fd;
    struct open_file f = {0};
    int64_t error = open_build(p, path_addr, flags, mode, &f);
    if (error) {
        fd_release(p, fd);
        return error;
    }
    f.nonblock = flags & OPEN_NONBLOCK;             /* (53b) */
    fd_install(p, fd, &f);
    return fd;
}

/* Close a descriptor; a file written through it is committed. */
static int64_t close_file(struct open_file *f)
{
    if (f->kind == FILE_SOCKET) {
        f->kind = FILE_CLOSED;
        socket_close(f->socket);
        return 0;
    }
    if (f->kind == FILE_PTY) {
        f->kind = FILE_CLOSED;
        pty_close(f->pty, f->master);
        return 0;
    }
    if (f->kind == FILE_PIPE) {
        f->kind = FILE_CLOSED;
        pipe_close(f->pipe, f->writable);
        return 0;
    }
    if (f->kind == FILE_PCM) {
        f->kind = FILE_CLOSED;
        sound_drain(f->pcm);                        /* play out the rest, then silence */
        return 0;
    }
    if (f->kind == FILE_USBSERIAL) {
        f->kind = FILE_CLOSED;                      /* the port stays as it is */
        return 0;
    }
    if (f->kind == FILE_TAP) {
        f->kind = FILE_CLOSED;
        tap_close(f->tap);                          /* its link goes down */
        return 0;
    }
    if (f->kind == FILE_SHARE) {
        f->kind = FILE_CLOSED;
        if (f->shbuf_va) {                          /* unmap, but the pages are not ours */
            struct process *p = current_process();
            /* (65e) No copy_user into them under way: they may be freed below. */
            mmap_change_begin(&p->mm);
            for (unsigned i = 0; i < shbuf_pages(f->shbuf); i++)
                paging_user_unmap(p->space, f->shbuf_va + (uint64_t)i * PAGE_SIZE);
            mmap_change_end(&p->mm);
            f->shbuf_va = 0;
        }
        shbuf_unref(f->shbuf);
        return 0;
    }
    if (f->kind == FILE_DEVICE) {
        /* A written disk: on the medium, and its table and file systems
         * read again (a program may have changed them). */
        struct block_device *disk = partition_disk(f->device);
        bool written = f->written;
        f->kind = FILE_CLOSED;
        f->written = false;
        if (written) {
            bool flushed = !disk->flush || disk->flush(disk);
            mounts_forget(disk);
            devices_rescan(disk, f->device);
            if (!flushed)
                return -ERR_IO;
        }
        return 0;
    }
    int r = f->written ? vfs_commit(f->fs) : RFS_OK;
    f->kind = FILE_CLOSED;
    f->written = false;
    return r == RFS_OK ? 0 : fs_error(r);
}

static int64_t sys_close(struct process *p, uint64_t fd)
{
    /* Detach the slot under the lock so two threads cannot both tear the
     * same descriptor down (double socket_close/commit), then do the slow
     * teardown on the detached copy (49b). */
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    if (!f) {
        spin_unlock(&p->fds, s);
        return -ERR_BADF;
    }
    fds_change_begin(p);                            /* (65e-42) */
    __atomic_store_n(&p->files[fd], 0, __ATOMIC_RELEASE);
    fds_change_end(p);
    p->cloexec[fd / 8] &= (uint8_t)~(1u << (fd % 8));       /* (53b) */
    spin_unlock(&p->fds, s);
    return file_put(f);                             /* (51a: closed when the last goes) */
}

static int64_t dup_to(struct process *p, uint64_t old, int64_t new, int64_t min);

/* fcntl (53b): FCNTL_* on a descriptor (see common/syscalls.h). */
static int64_t sys_fcntl(struct process *p, uint64_t fd, uint64_t cmd, uint64_t arg)
{
    if (cmd == FCNTL_DUPFD || cmd == FCNTL_DUPFD_CLOEXEC) {
        int64_t n = dup_to(p, fd, -1, (int64_t)arg);
        if (n >= 0 && cmd == FCNTL_DUPFD_CLOEXEC)
            p->cloexec[n / 8] |= (uint8_t)(1u << (n % 8));
        return n;
    }
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    int64_t r = 0;
    if (!f) {
        r = -ERR_BADF;
    } else if (cmd == FCNTL_GETFD) {
        r = p->cloexec[fd / 8] >> (fd % 8) & 1 ? FCNTL_CLOEXEC : 0;
    } else if (cmd == FCNTL_SETFD) {
        if (arg & FCNTL_CLOEXEC)
            p->cloexec[fd / 8] |= (uint8_t)(1u << (fd % 8));
        else
            p->cloexec[fd / 8] &= (uint8_t)~(1u << (fd % 8));
    } else if (cmd == FCNTL_GETFL) {
        r = (f->readable && f->writable ? OPEN_READ_WRITE : f->writable ? OPEN_WRITE : OPEN_READ) |
            (f->append ? OPEN_APPEND : 0) | (f->nonblock ? OPEN_NONBLOCK : 0);
    } else if (cmd == FCNTL_SETFL) {
        f->nonblock = arg & OPEN_NONBLOCK;
        if (f->kind == FILE_RITTERFS && f->writable)
            f->append = arg & OPEN_APPEND;
    } else {
        r = -ERR_INVAL;
    }
    spin_unlock(&p->fds, s);
    return r;
}

/* (53b) Is this call not to wait: the descriptor's O_NONBLOCK? */
static bool fd_nonblock(struct process *p, uint64_t fd)
{
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    bool r = f && f->nonblock;
    spin_unlock(&p->fds, s);
    return r;
}

/* dup2(old, new) (52a): another descriptor for old's open file. The slot
 * changes under p->fds; a file that was open at new is let go afterwards
 * (its last close may commit, which sleeps). A slot another thread has
 * claimed but not installed yet is -ERR_BUSY, as Linux's. */
static int64_t sys_dup2(struct process *p, uint64_t old, int64_t new)
{
    return dup_to(p, old, new, 0);
}

/* dup2 (min 0), or with new -1 the lowest free descriptor from min on
 * (fcntl's F_DUPFD, 53b). */
static int64_t dup_to(struct process *p, uint64_t old, int64_t new, int64_t min)
{
    if (new >= FD_MAX || new < -1 || min < 0 || min >= FD_MAX)
        return new >= FD_MAX || new < -1 ? -ERR_BADF : -ERR_INVAL;
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, old);
    if (!f) {
        spin_unlock(&p->fds, s);
        return -ERR_BADF;
    }
    if (new == (int64_t)old) {
        spin_unlock(&p->fds, s);
        return new;
    }
    if (new < 0) {
        new = min;
        while (new < FD_MAX && p->files[new])
            new++;
        if (new == FD_MAX) {
            spin_unlock(&p->fds, s);
            return -ERR_MFILE;
        }
    }
    struct open_file *before = p->files[new];
    if (before && before->kind == FILE_RESERVED) {
        spin_unlock(&p->fds, s);
        return -ERR_BUSY;
    }
    p->cloexec[new / 8] &= (uint8_t)~(1u << (new % 8));    /* (53b: a copy is without it) */
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
    if (before)
        fds_change_begin(p);                        /* (65e-42: before may be held by a look) */
    __atomic_store_n(&p->files[new], f, __ATOMIC_RELEASE);
    if (before)
        fds_change_end(p);
    spin_unlock(&p->fds, s);
    if (before && before->kind != FILE_CLOSED)
        file_put(before);
    return new;
}

/* --- system calls for processes ------------------------------------------------ */

/* Strings from a program's array of pointers (ended by a null pointer) into
 * one buffer: their count, or a negative error. */
static int copy_strings(struct process *p, uint64_t array, char *strings, size_t size,
                        const char **out, int max)
{
    int count = 0;
    size_t used = 0;
    for (;; count++) {
        uint64_t s;
        if (!copy_user(p, array + (uint64_t)count * 8, &s, 8, false))
            return -ERR_FAULT;
        if (!s)
            return count;
        if (count == max || used >= size)
            return -ERR_2BIG;
        int64_t error = copy_string_from_user(p, s, strings + used, size - used);
        if (error)
            return error == -ERR_NAMETOOLONG ? -ERR_2BIG : (int)error;
        out[count] = strings + used;
        used += strlen(strings + used) + 1;
    }
}

/* (35e) copy_strings into a buffer of its own, *strings, and (37g-5a-2)
 * its pointers into an array of their own, *out, with extra more and the
 * null pointer's room (both the caller's to free, also after an error):
 * first ones that fit in a page each, the whole sizes only for strings that
 * do not fit there. A block of more than a page is a run of pages, and
 * while swap is busy the free pages lie scattered: with ARGS_MAX's 33
 * every program start failed under swaptest on x86 (133-176 pages free,
 * none 33 in a row), even `kill`. */
#define STRINGS_FIRST 4000              /* bytes: one page with the heap's header */
static int copy_strings_sized(struct process *p, uint64_t array, char **strings, size_t size,
                              const char ***out, int max, int extra)
{
    size_t first = size < STRINGS_FIRST ? size : STRINGS_FIRST;
    int first_max = (int)(STRINGS_FIRST / sizeof(char *)) - extra - 1;
    if (first_max > max)
        first_max = max;
    *strings = kmalloc(first);
    *out = kmalloc(sizeof(char *) * (size_t)(first_max + extra + 1));
    if (!*strings || !*out)
        return -ERR_NOMEM;
    int r = copy_strings(p, array, *strings, first, *out, first_max);
    if (r != -ERR_2BIG || (first == size && first_max == max))
        return r;
    kfree(*strings);
    kfree(*out);
    *strings = kmalloc(size);
    *out = kmalloc(sizeof(char *) * (size_t)(max + extra + 1));
    if (!*strings || !*out)
        return -ERR_NOMEM;
    return copy_strings(p, array, *strings, size, *out, max);
}

/* (51c) A script: a file starting with "#!" names its interpreter (an
 * absolute path) and at most one argument after it, as on Unix. true if
 * path is one the caller may run (its x bit counts, as a program's). */
static bool script_interpreter(struct process *p, const char *path, char *interp, size_t isize,
                               char *arg, size_t asize)
{
    char full[PATH_MAX];
    if (resolve_path(p, path, full, true))
        return false;
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if (find(p, full, &fs, &id, &inode, &snapshots) || inode.type != RFS_TYPE_FILE ||
        !may_run(p, &inode))
        return false;
    char head[256];
    int64_t n = vfs_read(fs, id, &inode, 0, head, sizeof(head) - 1);
    if (n < 3 || head[0] != '#' || head[1] != '!')
        return false;
    head[n] = 0;
    char *line = head + 2, *nl = line;
    while (*nl && *nl != '\n')
        nl++;
    if (!*nl)
        return false;                               /* (a line too long for a #! line) */
    *nl = 0;
    while (*line == ' ' || *line == '\t')
        line++;
    char *e = line;
    while (*e && *e != ' ' && *e != '\t' && *e != '\r')
        e++;
    size_t il = (size_t)(e - line);
    if (!il || il >= isize)
        return false;
    memcpy(interp, line, il);
    interp[il] = 0;
    while (*e == ' ' || *e == '\t')
        e++;
    char *end = e + strlen(e);
    while (end > e && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
    size_t al = (size_t)(end - e);
    if (al >= asize)
        return false;
    memcpy(arg, e, al);
    arg[al] = 0;
    return true;
}

static int64_t sys_spawn(struct process *p, uint64_t path_addr, uint64_t argv_addr,
                         uint64_t flags, uint64_t fds_addr, uint64_t ids, uint64_t envp_addr)
{
    if ((flags & SPAWN_AS) && p->uid != 0)
        return -ERR_PERM;                           /* starting programs as others: root */
    int join = 0;
    if (flags & SPAWN_JOIN_GROUP) {
        /* (51a) A group of the caller's session: a pipeline's later commands
         * join its first one's, which may have ended but not been waited
         * for yet (still listed, as a zombie: the group is still there). */
        join = (int)(flags >> 32);
        if ((flags & SPAWN_NEW_GROUP) || join <= 0)
            return -ERR_INVAL;
        bool found = false;
        uint64_t state = spin_lock(&table);
        for (struct process *q = all_processes; q; q = q->next)
            found |= q->group == join && q->session == p->session;
        spin_unlock(&table, state);
        if (!found)
            return -ERR_SRCH;
    }
    /* The child's standard input, output and error: the caller's (copies:
     * each has its own position). */
    int32_t fds[3] = { FD_STDIN, FD_STDOUT, FD_STDERR };
    if (fds_addr && !copy_user(p, fds_addr, fds, sizeof(fds), false))
        return -ERR_FAULT;
    for (int i = 0; i < 3; i++) {
        if (fds[i] < 0)
            fds[i] = i;
        if (!file(p, (uint64_t)fds[i]))
            return -ERR_BADF;
    }
    char path[PATH_MAX];
    int64_t error = copy_string_from_user(p, path_addr, path, sizeof(path));
    if (error)
        return error;
    /* The arguments and (22t) the environment: arrays of pointers, ended by
     * a null pointer, to strings that go into a buffer each here. */
    char *strings = 0, *env_strings = 0;
    const char **argv = 0, **envp = 0;
    int argc = copy_strings_sized(p, argv_addr, &strings, ARGS_MAX, &argv, ARGC_MAX, 2);
    int envc = argc >= 0 && envp_addr ? copy_strings_sized(p, envp_addr, &env_strings, ENV_MAX,
                                                           &envp, ENVC_MAX, 0) : 0;
    error = argc < 0 ? argc : envc < 0 ? envc : 0;
    int result = (int)error;
    if (!error) {
        int code;
        int group = flags & SPAWN_NEW_GROUP ? 0 : join ? join : p->group;
        /* The program is loaded with the rights of the user it runs as. */
        uint32_t saved_uid = p->uid, saved_ruid = p->ruid, saved_gid = p->gid;
        if (flags & SPAWN_AS) {
            p->uid = p->ruid = (uint32_t)ids;       /* the user it runs as, through and through */
            p->gid = (uint32_t)(ids >> 32);
        }
        /* (51c) A script: its interpreter runs, with the script's path and
         * the arguments after it (INTERPRETER [ARG] PATH ARGV[1]...; in
         * argv's two extra places, as execve does). */
        char interp[PATH_MAX], arg[128];
        const char *run_path = path;
        struct process *child = 0;
        code = 0;
        if (script_interpreter(p, path, interp, sizeof(interp), arg, sizeof(arg))) {
            int k = arg[0] ? 2 : 1;
            if (argc + k + 1 > ARGC_MAX) {
                code = ERR_2BIG;
            } else {
                memmove(argv + k + 1, argv + 1, (size_t)(argc > 0 ? argc - 1 : 0) * sizeof(char *));
                argv[0] = interp;
                if (arg[0])
                    argv[1] = arg;
                argv[k] = path;
                argc = argc > 0 ? argc + k : k + 1;
                argv[argc] = 0;
                run_path = interp;
            }
        }
        if (!code)
            child = process_create(p->fs, run_path, argc, argv, envc, envp, p, group, &code);
        p->uid = saved_uid;
        p->ruid = saved_ruid;
        p->gid = saved_gid;
        if (child && (flags & SPAWN_NEW_SESSION) && (flags & SPAWN_NEW_GROUP)) {
            uint64_t state = spin_lock(&table);
            child->session = child->pid;            /* it leads a session of its own */
            spin_unlock(&table, state);
        }
        /* A job in the foreground has the terminal before it can read: a
         * read in the background would find the input at its end. */
        bool foreground = child && (flags & SPAWN_FOREGROUND) && (flags & SPAWN_NEW_GROUP) &&
                          !p->no_terminal &&
                          (p->pid == p->session || p->group == foreground_of(p));
        if (foreground)
            set_foreground_of(p, child->group);
        if (child) {
            /* The caller's three open files themselves (51a: shared, one
             * position: what the child writes follows what the caller and its
             * other children wrote), each with a reference of the child's,
             * taken under the descriptor lock (no close in between). A shared
             * buffer is not passed on (its mapping is the caller's). */
            uint64_t s = spin_lock(&p->fds);
            for (int i = 0; i < 3; i++) {
                struct open_file *f = file(p, (uint64_t)fds[i]);
                if (f && f->kind != FILE_SHARE) {
                    __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
                    child->files[i] = f;
                } else {
                    child->files[i] = 0;
                }
            }
            spin_unlock(&p->fds, s);
            /* A new session whose input is a pseudo-terminal's slave side:
             * that is its terminal, and it is in the foreground there. One
             * whose input is the console (login's shell, the elves) keeps
             * the console. Any other has no terminal (49k): a command run
             * over SSH on pipes shares sshd's console terminal, and a new
             * session made from it could otherwise take the console's
             * foreground from the user there, and with it the keyboard. */
            struct open_file *in = child->files[0] ? child->files[0] : &console_terminal;
            if ((flags & SPAWN_NEW_SESSION) && (flags & SPAWN_NEW_GROUP)) {
                if (in->kind == FILE_PTY && !in->master) {
                    if (child->tty)
                        pty_release_terminal(child->tty);
                    child->tty = in->pty;
                    child->no_terminal = false;
                    pty_hold_terminal(in->pty);
                    pty_set_session(in->pty, child->pid);
                    pty_set_foreground(in->pty, child->group);
                } else if (!(in->kind == FILE_TERMINAL && !p->tty && !p->no_terminal)) {
                    if (child->tty)
                        pty_release_terminal(child->tty);
                    child->tty = 0;
                    child->no_terminal = true;
                }
            }
        }
        if (!child)
            result = -code;
        else if (!process_start(child)) {
            if (foreground)
                set_foreground_of(p, p->group);     /* it never ran */
            result = -ERR_NOMEM;
        } else {
            result = child->pid;
        }
    }
    kfree(strings);
    kfree(env_strings);
    kfree(argv);
    kfree(envp);
    return result;
}

static int64_t sys_wait(struct process *p, int64_t pid, uint64_t status_addr, uint64_t flags)
{
    int status = 0;
    int id = process_wait(p, (int)pid, &status, flags & WAIT_NO_HANG);
    if (id > 0 && status_addr && !copy_user(p, status_addr, &status, sizeof(status), true))
        return -ERR_FAULT;
    return id;
}

static int64_t sys_sleep(uint64_t ms)
{
    if (ms == 0) {                              /* (22y: 0 gives the processor away, thrd_yield) */
        thread_yield();
        return 0;
    }
    if (ms > 86400000ULL * 365 * 100)           /* (a century: the sums below cannot overflow) */
        ms = 86400000ULL * 365 * 100;
    struct thread *self = thread_current();
    uint64_t hz = arch_counter_hz();
    if (!hz) {                                  /* (no counter rate yet: by ticks alone) */
        uint64_t deadline = timer_ticks() + (ms * TIMER_HZ + 999) / 1000 + 1;
        wait_event_until(&sleepers, self->stop_requested, deadline);
        return self->stop_requested ? -ERR_INTR : 0;
    }
    /* (68b) It ends by the CPU's counter, the clock programs read: ticks
     * may come in a burst (QEMU catching up after a stall), and counted
     * alone they ended a 60 ms sleep early; each tick only wakes it to
     * look again. */
    uint64_t end = arch_counter() + ms / 1000 * hz + (ms % 1000 * hz + 999) / 1000;
    for (;;) {
        uint64_t now = arch_counter();
        if (self->stop_requested)
            return -ERR_INTR;
        if (now >= end)
            return 0;
        uint64_t left = end - now;
        uint64_t wait = left / hz * TIMER_HZ + (left % hz * TIMER_HZ + hz - 1) / hz;
        wait_event_until(&sleepers, self->stop_requested, timer_ticks() + (wait ? wait : 1));
    }
}

static void fill_stat(struct sys_stat *st, uint64_t id, const struct rfs_inode *inode)
{
    memset(st, 0, sizeof(*st));
    st->id = id;
    st->size = inode->size;
    st->type = inode->type == RFS_TYPE_DIR       ? FILE_TYPE_DIR
             : inode->type == RFS_TYPE_SYMLINK ? FILE_TYPE_SYMLINK : FILE_TYPE_FILE;
    st->links = inode->link_count;
    st->created = inode->created;
    st->modified = inode->modified;
    st->uid = inode->uid;
    st->gid = inode->gid;
    st->mode = inode->type == RFS_TYPE_SYMLINK ? 0777 : rfs_mode(inode->mode);   /* (lrwxrwxrwx) */
}

static int64_t sys_stat(struct process *p, uint64_t path_addr, uint64_t out, bool follow)
{
    char path[PATH_MAX];
    int64_t error = user_path_as(p, path_addr, path, follow);
    if (error)
        return error;
    uint64_t id;
    struct rfs_inode inode;
    struct vfs *fs;
    bool snapshots;
    struct sys_stat st;
    if (in_devices(path) && (special_device(path) ||
                             (pcm_number(path) >= 0 && sound_present(pcm_number(path))) ||
                             usbserial_present(serial_number(path)) ||
                             tap_present(tap_number(path)))) {
        memset(&st, 0, sizeof(st));                 /* (52c; 52l: the sound devices too) */
        st.type = FILE_TYPE_CHAR;
        st.mode = special_device(path) ? 0666 : 0600;
        st.links = 1;
        return copy_user(p, out, &st, sizeof(st), true) ? 0 : -ERR_FAULT;
    }
    if (in_devices(path)) {
        struct block_device *d = device_at(path);
        if (!d && strcmp(path, DEVICE_DIR) != 0)
            return -ERR_NOENT;
        memset(&st, 0, sizeof(st));
        st.type = d ? FILE_TYPE_BLOCK : FILE_TYPE_DIR;
        st.mode = d ? 0600 : 0755;                  /* root's */
        st.links = 1;
        st.size = d ? d->block_count * d->block_size : 0;
        return copy_user(p, out, &st, sizeof(st), true) ? 0 : -ERR_FAULT;
    }
    if ((error = find(p, path, &fs, &id, &inode, &snapshots)))
        return error;
    fill_stat(&st, id, &inode);
    st.device = mounts_fs_number(fs);
    return copy_user(p, out, &st, sizeof(st), true) ? 0 : -ERR_FAULT;
}

/* (53a) Not a socket: ENOTSOCK for an open descriptor, else EBADF. */
static int64_t file_get_kind_error(struct process *p, uint64_t fd)
{
    struct open_file *f = file_get(p, fd);
    if (!f)
        return -ERR_BADF;
    file_release(f);
    return -ERR_NOTSOCK;
}

/* (53a) shutdown, sockname and sockopt: on a socket descriptor, held
 * (socket_grab) while the call runs. */
static int64_t sys_shutdown(struct process *p, uint64_t fd, uint64_t how)
{
    struct socket *s = socket_grab(p, fd);
    if (!s)
        return file_get_kind_error(p, fd);
    int r = socket_shutdown(s, (int)how);
    socket_close(s);
    return r ? -r : 0;
}

static int64_t sys_sockname(struct process *p, uint64_t fd, uint64_t which, uint64_t out)
{
    struct socket *s = socket_grab(p, fd);
    if (!s)
        return file_get_kind_error(p, fd);
    struct sys_sockname n;
    int r = which > SOCKNAME_PEER ? ERR_INVAL : socket_name(s, which == SOCKNAME_PEER, &n);
    socket_close(s);
    if (r)
        return -r;
    return copy_user(p, out, &n, sizeof(n), true) ? 0 : -ERR_FAULT;
}

static int64_t sys_sockopt(struct process *p, uint64_t fd, uint64_t op, uint64_t option,
                           uint64_t addr)
{
    uint64_t value = 0;
    if (op > SOCKOPT_SET)
        return -ERR_INVAL;
    if (op == SOCKOPT_SET && !copy_user(p, addr, &value, sizeof(value), false))
        return -ERR_FAULT;
    struct socket *s = socket_grab(p, fd);
    if (!s)
        return file_get_kind_error(p, fd);
    int r = socket_option(s, op == SOCKOPT_SET, (int)option, &value);
    socket_close(s);
    if (r)
        return -r;
    if (op == SOCKOPT_GET && !copy_user(p, addr, &value, sizeof(value), true))
        return -ERR_FAULT;
    return 0;
}

/* termios(fd, op, modes) (52e): on the terminal fd is (the console or a
 * pseudo-terminal, either end); changing it is its foreground job's,
 * as switching raw input is. */
static int64_t sys_termios(struct process *p, uint64_t fd, uint64_t op, uint64_t addr)
{
    if (op > TERMIOS_FLUSH)
        return -ERR_INVAL;
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, fd);
    if (!f || (f->kind != FILE_TERMINAL && f->kind != FILE_PTY)) {
        spin_unlock(&p->fds, s);
        return f ? -ERR_NOTTY : -ERR_BADF;
    }
    __atomic_add_fetch(&f->holds, 1, __ATOMIC_SEQ_CST);
    struct pty *t = f->kind == FILE_PTY ? f->pty : 0;
    if (t)
        pty_hold_terminal(t);
    spin_unlock(&p->fds, s);
    struct sys_termios m;
    int64_t r = 0;
    if (op == TERMIOS_GET) {
        if (t)
            pty_termios_get(t, &m);
        else
            terminal_termios_get(&m);
        if (!copy_user(p, addr, &m, sizeof(m), true))
            r = -ERR_FAULT;
    } else if (p->group != (t ? pty_foreground(t) : terminal_foreground())) {
        r = -ERR_PERM;
    } else if (op == TERMIOS_FLUSH) {
        if (t)
            pty_flush(t);
        else
            terminal_flush();
    } else if (!copy_user(p, addr, &m, sizeof(m), false)) {
        r = -ERR_FAULT;
    } else if (t) {
        pty_termios_set(t, &m, p->pid, op == TERMIOS_SET_FLUSH);
    } else {
        terminal_termios_set(&m, p->pid, op == TERMIOS_SET_FLUSH);
    }
    if (t)
        pty_release_terminal(t);
    file_release(f);
    return r;
}

/* What one descriptor would report to poll (52d), interrupts on. */
static int poll_one(struct process *p, int fd)
{
    struct socket *sock = socket_grab(p, (uint64_t)fd);
    if (sock) {
        int r = socket_poll(sock);
        socket_close(sock);
        return r;
    }
    uint64_t s = spin_lock(&p->fds);
    struct open_file *f = file(p, (uint64_t)fd);
    if (!f) {
        spin_unlock(&p->fds, s);
        return POLLNVAL;
    }
    __atomic_add_fetch(&f->holds, 1, __ATOMIC_SEQ_CST);
    if (f->kind == FILE_PIPE)                       /* (held as read and write hold them) */
        pipe_hold(f->pipe);
    else if (f->kind == FILE_PTY)
        pty_hold_terminal(f->pty);
    spin_unlock(&p->fds, s);
    int r;
    switch (f->kind) {
    case FILE_PIPE:
        r = pipe_poll(f->pipe, f->writable);
        pipe_unhold(f->pipe);
        break;
    case FILE_PTY:
        r = pty_poll(f->pty, f->master);
        pty_release_terminal(f->pty);
        break;
    case FILE_TERMINAL:
        r = POLLOUT | (terminal_poll() ? POLLIN : 0);
        break;
    case FILE_USBSERIAL:
        r = usbserial_poll(f->serial) & ((f->readable ? POLLIN | POLLHUP : 0) |
                                         (f->writable ? POLLOUT | POLLHUP : 0));
        break;
    case FILE_TAP:
        r = tap_poll(f->tap) & ((f->readable ? POLLIN : 0) | (f->writable ? POLLOUT : 0));
        break;
    default:                                        /* files and devices never wait */
        r = (f->readable ? POLLIN : 0) | (f->writable ? POLLOUT : 0);
        break;
    }
    file_release(f);
    return r;
}

static int64_t poll_loop(struct process *p, uint64_t addr, struct sys_pollfd *fds, uint64_t count,
                         int64_t timeout_ms);

/* poll (52d): look at every descriptor; if none has anything, sleep until
 * a pollable queue is woken (the count moved since the look began), the
 * time is up or a signal comes, and look again. */
static int64_t sys_poll(struct process *p, uint64_t addr, uint64_t count, int64_t timeout_ms)
{
    if (count > POLL_MAX)
        return -ERR_INVAL;
    size_t bytes = count * sizeof(struct sys_pollfd);
    struct sys_pollfd *fds = kmalloc(bytes ? bytes : 1);    /* (52k: 2 KiB at most, not on the stack) */
    if (!fds)
        return -ERR_NOMEM;
    int64_t r = poll_loop(p, addr, fds, count, timeout_ms);
    kfree(fds);
    return r;
}

static int64_t poll_loop(struct process *p, uint64_t addr, struct sys_pollfd *fds, uint64_t count,
                         int64_t timeout_ms)
{
    size_t bytes = count * sizeof(fds[0]);
    if (count && !copy_user(p, addr, fds, bytes, false))
        return -ERR_FAULT;
    uint64_t deadline = timeout_ms > 0 ? timer_ticks() +
                        ((uint64_t)timeout_ms * TIMER_HZ + 999) / 1000 : 0;
    /* (65e) The queues the descriptors would be woken on: at most 4 each. */
    struct poll_table *pt = poll_table_new((int)count * 4 + 4);
    if (!pt)
        return -ERR_NOMEM;
    struct thread *self = thread_current();
    self->poll_table = pt;
    int64_t result;
    for (;;) {
        poll_table_reset(pt);
        int ready = 0;
        for (uint64_t i = 0; i < count; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            int found = poll_one(p, fds[i].fd);
            int asked = fds[i].events | (fds[i].events & POLLRDNORM ? POLLIN : 0) |
                        (fds[i].events & POLLWRNORM ? POLLOUT : 0);
            int r = found & (asked | POLLERR | POLLHUP | POLLNVAL);
            if (r & POLLIN)
                r |= fds[i].events & POLLRDNORM;
            if (r & POLLOUT)
                r |= fds[i].events & POLLWRNORM;
            fds[i].revents = (int16_t)r;
            ready += r != 0;
        }
        if (ready || timeout_ms == 0 || (deadline && timer_ticks() >= deadline)) {
            result = ready;
            break;
        }
        if (!poll_table_wait(pt, deadline)) {
            if (self->stop_requested) {
                result = -ERR_INTR;
                goto out;
            }
            if (deadline && timer_ticks() >= deadline) {
                result = 0;
                break;
            }
        }
    }
    if (count && !copy_user(p, addr, fds, bytes, true))
        result = -ERR_FAULT;
out:
    self->poll_table = 0;
    poll_table_free(pt);
    return result;
}

/* fstat(fd) (52b): a file's inode read again (another descriptor or
 * program may have changed it since the open), the rest by kind. */
static int64_t sys_fstat(struct process *p, uint64_t fd, uint64_t out)
{
    struct open_file *f = file_get(p, fd);
    if (!f)
        return -ERR_BADF;
    struct sys_stat st;
    memset(&st, 0, sizeof(st));
    st.links = 1;
    st.mode = 0600;
    int64_t r = 0;
    switch (f->kind) {
    case FILE_RITTERFS: {
        struct rfs_inode inode;
        if (!vfs_get_inode(f->fs, f->object_id, &inode) || inode.type == RFS_TYPE_FREE) {
            r = -ERR_NOENT;                         /* (removed since) */
            break;
        }
        fill_stat(&st, f->object_id, &inode);
        st.device = mounts_fs_number(f->fs);
        break;
    }
    case FILE_SNAPSHOTS:
    case FILE_VOLUMES:
    case FILE_DEVICES:
        st.type = FILE_TYPE_DIR;
        st.mode = 0755;
        break;
    case FILE_DEVICE:
        st.type = FILE_TYPE_BLOCK;
        st.size = f->device->block_count * f->device->block_size;
        break;
    case FILE_TERMINAL:
    case FILE_PTY:
        st.type = FILE_TYPE_CHAR;
        st.mode = 0620;
        break;
    case FILE_PCM:
    case FILE_USBSERIAL:
    case FILE_TAP:
        st.type = FILE_TYPE_CHAR;                   /* (root's, and the console user's) */
        st.mode = 0600;
        break;
    case FILE_NULL:
    case FILE_ZERO:
        st.type = FILE_TYPE_CHAR;
        st.mode = 0666;
        break;
    case FILE_PIPE:
        st.type = FILE_TYPE_FIFO;
        break;
    case FILE_SOCKET:
        st.type = FILE_TYPE_SOCKET;
        st.mode = 0777;
        break;
    default:
        st.type = FILE_TYPE_FILE;                   /* (a shared buffer) */
        break;
    }
    if (!r && f->kind != FILE_RITTERFS)
        st.uid = p->uid, st.gid = p->gid;           /* (not files: the caller's own) */
    file_release(f);
    if (r)
        return r;
    return copy_user(p, out, &st, sizeof(st), true) ? 0 : -ERR_FAULT;
}

/* The next entry of an open directory; the descriptor's offset is the
 * position in the directory. */
/* /.snapshots: entry number `index` of the snapshot list. */
struct snapshot_entry {
    uint64_t index, seen;
    struct sys_dirent *e;
    bool found;
};

static bool snapshot_entry(void *ctx, const char *name, uint64_t generation, uint64_t created)
{
    struct snapshot_entry *s = ctx;
    (void)generation;
    (void)created;
    if (s->seen++ != s->index)
        return true;
    size_t n = strlen(name);
    memcpy(s->e->name, name, n + 1);
    s->e->name_length = (uint32_t)n;
    s->e->type = FILE_TYPE_DIR;
    s->found = true;
    return false;
}

/* After the system's root directory's own entries: the directories the
 * kernel makes up there (/boot if it is mounted, /dev, /volumes), so that
 * ls / shows them. /.snapshots stays hidden, as names starting with a dot
 * are. 1 for an entry, 0 at the end. */
static int64_t root_extra(struct process *p, struct open_file *f, uint64_t out)
{
    static const char *const names[] = { "boot", "dev", "volumes" };
    if (f->fs != p->fs || f->object_id != RFS_OBJ_ROOT)
        return 0;
    while (f->made_up < sizeof(names) / sizeof(names[0])) {
        const char *name = names[f->made_up++];
        char path[16];
        ksnprintf(path, sizeof(path), "/%s", name);
        uint64_t id;
        struct rfs_inode inode;
        if (vfs_lookup(p->fs, path, &id, &inode))
            continue;                               /* the disk has one of that name */
        if (strcmp(name, "boot") == 0 && !mounts_get(path, 0, 0, 0, 0))
            continue;                               /* no EFI partition mounted there */
        struct sys_dirent e;
        memset(&e, 0, sizeof(e));
        e.name_length = (uint32_t)strlen(name);
        memcpy(e.name, name, e.name_length + 1);
        e.type = FILE_TYPE_DIR;
        return copy_user(p, out, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
    }
    return 0;
}

static int64_t readdir_file(struct process *p, struct open_file *f, uint64_t out);

static int64_t sys_readdir(struct process *p, uint64_t fd, uint64_t out)
{
    struct open_file *f = file_get(p, fd);
    int64_t r = readdir_file(p, f, out);
    if (f)
        file_release(f);
    return r;
}

static int64_t readdir_file(struct process *p, struct open_file *f, uint64_t out)
{
    if (f && f->kind == FILE_VOLUMES) {
        struct sys_dirent e;
        memset(&e, 0, sizeof(e));
        if (!mounts_volume((int)f->offset, e.name, sizeof(e.name)))
            return 0;
        e.name_length = (uint32_t)strlen(e.name);
        e.type = FILE_TYPE_DIR;
        f->offset++;
        return copy_user(p, out, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
    }
    if (f && f->kind == FILE_DEVICES) {
        struct sys_device *d = kmalloc(sizeof(*d));
        if (!d)
            return -ERR_NOMEM;
        struct sys_dirent e;
        memset(&e, 0, sizeof(e));
        bool found = !f->made_up && devices_info((int)f->offset, d);
        if (found) {
            size_t n = strlen(d->name);
            memcpy(e.name, d->name, n + 1);
            e.name_length = (uint32_t)n;
            e.type = FILE_TYPE_BLOCK;
            f->offset++;
        }
        kfree(d);
        /* (52l) After the disks: null, zero, tty, then each sound device,
         * then (50e) each USB serial port, (38a) then each tap. */
        static const char *const specials[] = { "null", "zero", "tty" };
        while (!found && f->made_up < 3 + 8 + 8 + TAP_MAX) {
            int k = f->made_up++;
            if (k < 3)
                ksnprintf(e.name, sizeof(e.name), "%s", specials[k]);
            else if (k < 3 + 8 && sound_present(k - 3))
                ksnprintf(e.name, sizeof(e.name), "pcm%d", k - 3);
            else if (k >= 3 + 8 && k < 3 + 8 + 8 && usbserial_present(k - 3 - 8))
                ksnprintf(e.name, sizeof(e.name), "usbserial%d", k - 3 - 8);
            else if (k >= 3 + 8 + 8 && tap_present(k - 3 - 8 - 8))
                ksnprintf(e.name, sizeof(e.name), "tap%d", k - 3 - 8 - 8);
            else
                continue;
            e.name_length = (uint32_t)strlen(e.name);
            e.type = FILE_TYPE_CHAR;
            found = true;
        }
        if (!found)
            return 0;
        return copy_user(p, out, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
    }
    if (f && f->kind == FILE_SNAPSHOTS) {
        struct sys_dirent e;
        memset(&e, 0, sizeof(e));
        struct snapshot_entry s = { f->offset, 0, &e, false };
        ritterfs_snapshot_list(vfs_ritterfs(f->fs), snapshot_entry, &s);
        if (!s.found)
            return 0;
        f->offset++;
        return copy_user(p, out, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
    }
    if (!f || f->kind != FILE_RITTERFS)
        return -ERR_BADF;
    if (f->inode.type != RFS_TYPE_DIR)
        return -ERR_NOTDIR;
    struct sys_dirent e;
    memset(&e, 0, sizeof(e));
    uint8_t len, type;
    int r = vfs_dir_next(f->fs, f->object_id, &f->inode, &f->offset, e.name, &len, &e.id,
                              &type);
    if (r < 0)
        return -ERR_IO;
    if (r == 0)
        return root_extra(p, f, out);
    e.name_length = len;
    e.type = type == RFS_TYPE_DIR ? FILE_TYPE_DIR
           : type == RFS_TYPE_SYMLINK ? FILE_TYPE_SYMLINK : FILE_TYPE_FILE;
    return copy_user(p, out, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
}

static int64_t sys_chdir(struct process *p, uint64_t path_addr)
{
    char path[PATH_MAX];
    int64_t error = user_path(p, path_addr, path);
    if (error)
        return error;
    uint64_t id;
    struct rfs_inode inode;
    struct vfs *fs;
    bool snapshots;
    if (in_devices(path)) {
        if (strcmp(path, DEVICE_DIR) != 0)
            return device_at(path) ? -ERR_NOTDIR : -ERR_NOENT;
        memcpy(p->cwd, path, sizeof(p->cwd));
        return 0;
    }
    if ((error = find(p, path, &fs, &id, &inode, &snapshots)))
        return error;
    if (inode.type != RFS_TYPE_DIR)
        return -ERR_NOTDIR;
    if (!may(p, &inode, MAY_EXEC))
        return -ERR_ACCES;
    memcpy(p->cwd, path, sizeof(p->cwd));
    return 0;
}

static int64_t sys_getcwd(struct process *p, uint64_t buffer, uint64_t size)
{
    size_t n = strlen(p->cwd) + 1;
    if (size < n)
        return -ERR_INVAL;
    return copy_user(p, buffer, p->cwd, n, true) ? (int64_t)n - 1 : -ERR_FAULT;
}

/* The terminal's foreground group: the session's leader (the shell) may
 * hand it to a group of its session, and so may the foreground job and
 * (65e, as POSIX) any process of the session the terminal belongs to: a
 * shell started from the shell takes it back after each command. Until
 * 65e it could not (neither leader nor, its command ended, foreground): it
 * read at the end of its input and ended after its first command. Another
 * session sharing the console may not (sshd's commands on pipes, 49k). */
static int64_t sys_foreground(struct process *p, int64_t group)
{
    if (p->no_terminal)
        return -ERR_PERM;                           /* no terminal to hand around (49k) */
    if (group == 0)
        return foreground_of(p);
    if (p->pid != p->session && p->group != foreground_of(p) && !terminal_is_sessions(p))
        return -ERR_PERM;
    bool found = false;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q; q = q->next)
        found |= q->group == group && q->session == p->session && q->state != PROCESS_ZOMBIE;
    spin_unlock(&table, state);
    if (!found)
        return -ERR_SRCH;
    set_foreground_of(p, (int)group);
    return group;
}

static int64_t sys_signal(struct process *p, uint64_t signal, uint64_t action)
{
    if ((signal != SIGINT && signal != SIGTERM) || action > SIGNAL_IGNORE)
        return -ERR_INVAL;                          /* only Ctrl-C and SIGTERM can be ignored */
    uint64_t state = spin_lock(&table);
    if (action == SIGNAL_IGNORE)
        p->ignored |= 1u << signal;
    else
        p->ignored &= ~(1u << signal);
    spin_unlock(&table, state);
    return 0;
}

/* SYS_SIGACTION (22r): a signal's handler, ignored or default; the old one
 * into *old_action. SIGKILL keeps its default. */
static int64_t sys_sigaction(struct process *p, uint64_t signal, uint64_t new_action,
                             uint64_t old_action, uint64_t restorer)
{
    if (signal == 0 || signal >= SIGNALS || signal == SIGKILL)
        return -ERR_INVAL;
    struct sys_sigaction now = { 0 }, want;
    if (new_action) {
        if (!copy_user(p, new_action, &want, sizeof(want), false))
            return -ERR_FAULT;
        if (want.handler > SIGNAL_IGNORE && (want.handler >= USER_TOP || !restorer ||
                                             restorer >= USER_TOP))
            return -ERR_INVAL;
    }
    uint64_t state = spin_lock(&table);
    now.handler = p->handlers[signal] ? p->handlers[signal]
                : (p->ignored & (1u << signal)) ? SIGNAL_IGNORE : SIGNAL_DEFAULT;
    now.mask = p->handler_masks[signal];
    now.flags = p->handler_flags[signal];
    if (new_action) {
        p->handlers[signal] = want.handler > SIGNAL_IGNORE ? want.handler : 0;
        p->handler_masks[signal] = want.mask & ~(1ULL << SIGKILL);
        p->handler_flags[signal] = (uint8_t)(want.flags & (SIGACTION_RESETHAND | SIGACTION_NODEFER));
        if (want.handler == SIGNAL_IGNORE)
            p->ignored |= 1u << signal;
        else
            p->ignored &= ~(1u << signal);
        if (want.handler > SIGNAL_IGNORE)
            p->restorer = restorer;
        else
            p->pending &= ~(1ULL << signal);        /* (one waiting goes with its handler) */
    }
    spin_unlock(&table, state);
    if (old_action && !copy_user(p, old_action, &now, sizeof(now), true))
        return -ERR_FAULT;
    return 0;
}

/* A kernel report (the rescue console's ps, threads, pci, usb) as text. */
static int64_t sys_report(struct process *p, uint64_t which, uint64_t buffer, uint64_t size)
{
    /* (37g-5c-2: a register snapshot, up to 662 lines, needs more; 38a:
     * the network's report, with up to 64 cards) */
    const size_t max = which == REPORT_DISPLAY_SLEEP || which == REPORT_NET ? 64 * 1024
                                                                            : 32 * 1024;
    char *text = kmalloc(max);
    if (!text)
        return -ERR_NOMEM;
    long length = report_text((int)which, text, max);
    int64_t result = length;
    if (length < 0) {
        result = -ERR_INVAL;
    } else if (size > 0) {
        uint64_t n = (uint64_t)length < size - 1 ? (uint64_t)length : size - 1;
        n = n < max - 1 ? n : max - 1;
        text[n] = 0;
        if (!copy_user(p, buffer, text, n + 1, true))
            result = -ERR_FAULT;
    }
    kfree(text);
    return result;
}

static int64_t seek_file(struct open_file *f, int64_t offset, uint64_t whence);

static int64_t sys_seek(struct process *p, uint64_t fd, int64_t offset, uint64_t whence)
{
    struct open_file *f = file_get(p, fd);
    if (!f)
        return -ERR_BADF;
    int64_t r = seek_file(f, offset, whence);
    file_release(f);
    return r;
}

static int64_t seek_file(struct open_file *f, int64_t offset, uint64_t whence)
{
    if (f->kind == FILE_NULL || f->kind == FILE_ZERO)
        return 0;                           /* (52c: always at 0, as Unix's) */
    if (f->kind != FILE_RITTERFS && f->kind != FILE_DEVICE)
        return -ERR_INVAL;                  /* the terminal and the directories have none */
    int64_t end = f->kind == FILE_DEVICE ? (int64_t)(f->device->block_count *
                                                     f->device->block_size)
                                         : (int64_t)f->inode.size;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->offset
                 : whence == SEEK_END ? end : -1;
    if (base < 0 || base + offset < 0 || f->inode.type == RFS_TYPE_DIR)
        return -ERR_INVAL;
    f->offset = (uint64_t)(base + offset);
    return (int64_t)f->offset;
}

/* --- snapshots ------------------------------------------------------------------- */

static int64_t snapshot_error(int r)
{
    switch (r) {
    case RFS_ERR_EXISTS:    return -ERR_EXIST;
    case RFS_ERR_NOT_FOUND: return -ERR_NOENT;
    case RFS_ERR_BAD_NAME:  return -ERR_INVAL;
    case RFS_ERR_READ_ONLY: return -ERR_ROFS;
    default:                return -ERR_IO;
    }
}

struct snapshot_fill {
    struct process *p;
    uint64_t buffer, room, count;
    const char *find;               /* only look for this name's generation */
    uint64_t generation;
    bool fault;
};

static bool snapshot_fill(void *ctx, const char *name, uint64_t generation, uint64_t created)
{
    struct snapshot_fill *l = ctx;
    if (l->find) {
        if (strcmp(name, l->find) == 0)
            l->generation = generation;
        return true;
    }
    if ((l->count + 1) * sizeof(struct sys_snapshot) <= l->room) {
        struct sys_snapshot s;
        memset(&s, 0, sizeof(s));
        size_t n = strlen(name);
        memcpy(s.name, name, n < sizeof(s.name) ? n : sizeof(s.name) - 1);
        s.generation = generation;
        s.created = created;
        if (!copy_user(l->p, l->buffer + l->count * sizeof(s), &s, sizeof(s), true))
            l->fault = true;
    }
    l->count++;
    return !l->fault;
}

/* Snapshots of the program's file system; they write to the disk only
 * because the user asked (the snapshot program). */
static int64_t sys_snapshot(struct process *p, uint64_t op, uint64_t name_addr, uint64_t buffer,
                            uint64_t size)
{
    struct snapshot_fill l = { p, buffer, size, 0, 0, 0, false };
    struct ritterfs *fs = vfs_ritterfs(p->fs);
    if (!fs)
        return -ERR_NOTSUP;
    if (op == SNAPSHOT_LIST) {
        ritterfs_snapshot_list(fs, snapshot_fill, &l);
        return l.fault ? -ERR_FAULT : (int64_t)l.count;
    }
    if (op != SNAPSHOT_CREATE && op != SNAPSHOT_DELETE)
        return -ERR_INVAL;
    if (p->uid != 0)
        return -ERR_PERM;                           /* they cover everyone's files */
    char name[RFS_SNAPSHOT_NAME_MAX + 1];
    int64_t error = copy_string_from_user(p, name_addr, name, sizeof(name));
    if (error)
        return error == -ERR_NAMETOOLONG ? -ERR_INVAL : error;
    if (op == SNAPSHOT_DELETE) {
        uint64_t freed;
        int r = ritterfs_snapshot_delete(fs, name, &freed);
        return r == RFS_OK ? (int64_t)freed : snapshot_error(r);
    }
    int r = ritterfs_snapshot_create(fs, name);
    if (r != RFS_OK)
        return snapshot_error(r);
    l.find = name;
    ritterfs_snapshot_list(fs, snapshot_fill, &l);
    return (int64_t)l.generation;
}

/* --- attributes ------------------------------------------------------------------ */

struct attr_keys {
    struct process *p;
    uint64_t buffer, room, need;
    bool fault;
};

static bool list_key(void *ctx, uint64_t object, const char *key, size_t key_length,
                     const uint8_t *value, size_t value_length)
{
    struct attr_keys *l = ctx;
    char name[RFS_ATTR_KEY_MAX + 1];
    (void)object, (void)value, (void)value_length;
    memcpy(name, key, key_length);
    name[key_length] = 0;
    if (l->need + key_length + 1 <= l->room &&
        !copy_user(l->p, l->buffer + l->need, name, key_length + 1, true))
        l->fault = true;
    l->need += key_length + 1;
    return !l->fault;
}

static int64_t attr_error(int r, uint64_t op)
{
    switch (r) {
    case RFS_ERR_NOT_FOUND: return op == ATTR_REMOVE ? -ERR_NODATA : -ERR_NOENT;
    case RFS_ERR_BAD_NAME:  return -ERR_INVAL;
    case RFS_ERR_READ_ONLY: return -ERR_ROFS;
    default:                return -ERR_IO;
    }
}

/* Attributes of a path; changes are committed at once (the user asked). */
static int64_t sys_attr(struct process *p, uint64_t op, uint64_t path_addr, uint64_t key_addr,
                        uint64_t buffer, uint64_t size)
{
    char path[PATH_MAX];
    int64_t error = user_path(p, path_addr, path);
    if (error)
        return error;
    struct vfs *v;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if ((error = find(p, path, &v, &id, &inode, &snapshots)))
        return error;
    struct ritterfs *fs = vfs_ritterfs(v);
    if (!fs && !snapshots)
        return -ERR_NOTSUP;                         /* attributes are RitterFS's */
    /* Reading attributes needs r, changing them w (the snapshots: none). */
    if (!snapshots && !may(p, &inode, op == ATTR_SET || op == ATTR_REMOVE ? MAY_WRITE : MAY_READ))
        return -ERR_ACCES;
    if (op == ATTR_LIST) {
        struct attr_keys l = { p, buffer, size, 0, false };
        if (!snapshots && !ritterfs_attr_each(fs, id, list_key, &l))
            return -ERR_IO;
        return l.fault ? -ERR_FAULT : (int64_t)l.need;
    }
    char key[RFS_ATTR_KEY_MAX + 1];
    if ((error = copy_string_from_user(p, key_addr, key, sizeof(key))))
        return error == -ERR_NAMETOOLONG ? -ERR_INVAL : error;
    if (op == ATTR_GET) {
        if (snapshots)
            return -ERR_NODATA;                     /* /.snapshots itself has none */
        uint8_t *value = kmalloc(RFS_ATTR_VALUE_MAX);
        int64_t n = value ? ritterfs_attr_get(fs, id, key, value, RFS_ATTR_VALUE_MAX) : RFS_ERR_IO;
        bool copied = n < 0 ||
                      copy_user(p, buffer, value, (uint64_t)n < size ? (uint64_t)n : size, true);
        kfree(value);
        if (!copied)
            return -ERR_FAULT;
        return n == RFS_ERR_NOT_FOUND ? -ERR_NODATA : n < 0 ? -ERR_IO : n;
    }
    if (op != ATTR_SET && op != ATTR_REMOVE)
        return -ERR_INVAL;
    if (snapshots || fs->read_only)
        return -ERR_ROFS;
    int r;
    if (op == ATTR_SET) {
        if (size > RFS_ATTR_VALUE_MAX)
            return -ERR_INVAL;
        uint8_t *value = kmalloc(size ? size : 1);
        if (!value)
            return -ERR_NOMEM;
        if (!copy_user(p, buffer, value, size, false)) {
            kfree(value);
            return -ERR_FAULT;
        }
        r = ritterfs_attr_set(fs, id, key, value, size);
        kfree(value);
    } else {
        r = ritterfs_attr_remove(fs, id, key);
    }
    if (r == RFS_OK)
        r = ritterfs_commit(fs);
    return r == RFS_OK ? 0 : attr_error(r, op);
}

/* --- changing the file system ---------------------------------------------------- */

static int64_t fs_error(int r)
{
    switch (r) {
    case RFS_ERR_EXISTS:    return -ERR_EXIST;
    case RFS_ERR_NOT_FOUND: return -ERR_NOENT;
    case RFS_ERR_NOT_DIR:   return -ERR_NOTDIR;
    case RFS_ERR_IS_DIR:    return -ERR_ISDIR;
    case RFS_ERR_NOT_EMPTY: return -ERR_NOTEMPTY;
    case RFS_ERR_BAD_NAME:  return -ERR_INVAL;
    case RFS_ERR_NO_SPACE:  return -ERR_NOSPC;
    case RFS_ERR_READ_ONLY: return -ERR_ROFS;
    case RFS_ERR_NOT_SUPPORTED: return -ERR_PERM;
    case RFS_ERR_BUSY:      return -ERR_BUSY;
    default:                return -ERR_IO;
    }
}

/* A path a program wants to change: absolute, and not in /.snapshots;
 * follow: a link in its last component means what it names (chmod,
 * chown), else the name itself (mkdir, remove, rename, a copy's name). */
static int64_t change_path(struct process *p, uint64_t addr, char *out, bool follow)
{
    int64_t error = user_path_as(p, addr, out, follow);
    if (error)
        return error;
    return in_snapshots(out) || in_devices(out) ? -ERR_ROFS : 0;
}

/*
 * Finish a change: commit it (the user asked for it, so it is written at
 * once). The file system functions check everything before they change
 * anything, so a refused change left nothing behind; only a failure in the
 * middle (the disk, no space) throws the open transaction away.
 */
static int64_t finish_change(struct vfs *fs, int r)
{
    if (r == RFS_OK)
        r = vfs_commit(fs);
    else if (r == RFS_ERR_IO || r == RFS_ERR_NO_SPACE)
        vfs_abort(fs);
    return r == RFS_OK ? 0 : fs_error(r);
}

static int64_t sys_mkdir(struct process *p, uint64_t path_addr, uint64_t mode)
{
    char path[PATH_MAX];
    int64_t error = change_path(p, path_addr, path, false);
    if (!error)
        error = parent_allows(p, path);
    if (error)
        return error;
    struct vfs *fs;
    const char *inner;
    if ((error = target(p, path, &fs, &inner)))
        return error;
    uint64_t id;
    int r = vfs_create(fs, inner, RFS_TYPE_DIR, &id);
    if (r == RFS_OK)
        r = set_creator(p, fs, id, (unsigned)mode & RFS_MODE_BITS & ~p->umask);   /* (52h) */
    return finish_change(fs, r);
}

/* rmdir (directory) or unlink (file). */
static int64_t sys_remove(struct process *p, uint64_t path_addr, bool directory)
{
    char path[PATH_MAX];
    int64_t error = change_path(p, path_addr, path, false);
    if (error)
        return error;
    uint64_t id;
    struct rfs_inode inode;
    if (strcmp(path, "/") == 0)
        return -ERR_INVAL;
    struct vfs *fs;
    bool snapshots;
    if ((error = find(p, path, &fs, &id, &inode, &snapshots)) || (error = parent_allows(p, path)) ||
        (error = sticky_allows(p, path)))
        return error;
    if (directory && inode.type != RFS_TYPE_DIR)
        return -ERR_NOTDIR;
    if (!directory && inode.type == RFS_TYPE_DIR)
        return -ERR_ISDIR;
    const char *inner;
    if ((error = target(p, path, &fs, &inner)))
        return error;
    if (strcmp(inner, "/") == 0)
        return -ERR_BUSY;                           /* a mount point */
    return finish_change(fs, vfs_remove(fs, inner));
}

static int64_t sys_rename(struct process *p, uint64_t from_addr, uint64_t to_addr)
{
    char from[PATH_MAX], to[PATH_MAX];
    int64_t error = change_path(p, from_addr, from, false);
    if (!error)
        error = change_path(p, to_addr, to, false);
    if (!error)
        error = parent_allows(p, from);
    if (!error)
        error = sticky_allows(p, from);
    if (!error)
        error = parent_allows(p, to);
    if (!error)
        error = sticky_allows(p, to);               /* (68a: an entry there is replaced) */
    struct vfs *fs, *to_fs;
    const char *inner_from, *inner_to;
    if (!error)
        error = target(p, from, &fs, &inner_from);
    if (!error)
        error = target(p, to, &to_fs, &inner_to);
    if (error)
        return error;
    if (strcmp(inner_from, "/") == 0 || strcmp(inner_to, "/") == 0)
        return -ERR_BUSY;                           /* a mount point */
    if (fs != to_fs)
        return -ERR_XDEV;                           /* another volume: the program copies */
    return finish_change(fs, vfs_rename(fs, inner_from, inner_to));
}

/* (57a) symlink(target, path): target is kept as given (resolved when the
 * link is used), the link is the caller's. */
static int64_t sys_symlink(struct process *p, uint64_t target_addr, uint64_t path_addr)
{
    char target_text[256], path[PATH_MAX];
    int64_t error = copy_string_from_user(p, target_addr, target_text, sizeof(target_text));
    if (error)
        return error == -ERR_NAMETOOLONG ? -ERR_NAMETOOLONG : error;
    if (!target_text[0])
        return -ERR_NOENT;                          /* (POSIX: an empty target) */
    if ((error = change_path(p, path_addr, path, false)) || (error = parent_allows(p, path)))
        return error;
    struct vfs *fs;
    const char *inner;
    if ((error = target(p, path, &fs, &inner)))
        return error;
    uint64_t id;
    int r = vfs_symlink(fs, inner, target_text, strlen(target_text), &id);
    if (r == RFS_OK)
        r = vfs_chown(fs, id, p->uid, p->gid);
    return finish_change(fs, r);
}

/* (57a) readlink(path, buffer, size): what the link at path names. */
static int64_t sys_readlink(struct process *p, uint64_t path_addr, uint64_t buffer,
                            uint64_t size)
{
    char path[PATH_MAX];
    int64_t error = user_path_as(p, path_addr, path, false);
    if (error)
        return error;
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if (in_devices(path))
        return -ERR_INVAL;
    if ((error = find(p, path, &fs, &id, &inode, &snapshots)))
        return error;
    if (snapshots || inode.type != RFS_TYPE_SYMLINK)
        return -ERR_INVAL;
    char text[256];
    uint64_t n = inode.size < sizeof(text) ? inode.size : sizeof(text);
    if (vfs_read(fs, id, &inode, 0, text, n) != (int64_t)n)
        return -ERR_IO;
    if (n > size)
        n = size;
    return copy_user(p, buffer, text, n, true) ? (int64_t)n : -ERR_FAULT;
}

/* (57a) link(from, to): a hard link. As Linux's protected hard links: the
 * caller owns the object, or may read and write it, or is root. */
static int64_t sys_link(struct process *p, uint64_t from_addr, uint64_t to_addr)
{
    char from[PATH_MAX], to[PATH_MAX];
    int64_t error = user_path_as(p, from_addr, from, false);
    if (!error)
        error = change_path(p, to_addr, to, false);
    if (error)
        return error;
    struct vfs *source, *fs;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if (in_devices(from))
        return -ERR_PERM;
    if ((error = find(p, from, &source, &id, &inode, &snapshots)))
        return error;
    if (snapshots || inode.type == RFS_TYPE_DIR)
        return -ERR_PERM;                           /* a folder has one name */
    if (p->uid != 0 && p->uid != inode.uid && !may(p, &inode, MAY_READ | MAY_WRITE))
        return -ERR_PERM;
    const char *inner_from, *inner_to;
    if ((error = parent_allows(p, to)) || (error = target(p, from, &source, &inner_from)) ||
        (error = target(p, to, &fs, &inner_to)))
        return error;
    if (fs != source)
        return -ERR_XDEV;
    return finish_change(fs, vfs_link(fs, inner_from, inner_to));
}

/* An instant copy; the source may be in a snapshot (restoring an old
 * version shares its blocks too). */
/* May p read everything in the tree at id (a directory: read and search
 * on it and every directory inside, read on every file)? An instant copy
 * belongs to its maker, so without this a readable directory handed over
 * the files inside it that its maker could not read (49a). 0, -ERR_ACCES,
 * or -ERR_XDEV for a tree deeper than the walk keeps (the programs then
 * copy byte by byte, file by file, with the usual checks). */
#define COPY_CHECK_DEPTH 32
static int64_t tree_readable(const struct process *p, struct vfs *fs, uint64_t id,
                             const struct rfs_inode *inode)
{
    if (p->uid == 0)
        return 0;
    if (inode->type != RFS_TYPE_DIR)
        return may(p, inode, MAY_READ) ? 0 : -ERR_ACCES;
    struct level {
        uint64_t id, pos;
        struct rfs_inode inode;
    } *stack = kmalloc(sizeof(*stack) * COPY_CHECK_DEPTH);
    if (!stack)
        return -ERR_NOMEM;
    int depth = 0;
    int64_t r = 0;
    stack[0].id = id, stack[0].pos = 0, stack[0].inode = *inode;
    if (!may(p, inode, MAY_READ | MAY_EXEC))
        r = -ERR_ACCES;
    while (!r && depth >= 0) {
        struct level *l = &stack[depth];
        char name[RFS_NAME_MAX + 1];
        uint8_t length, type;
        uint64_t child;
        int n = vfs_dir_next(fs, l->id, &l->inode, &l->pos, name, &length, &child, &type);
        if (n < 0) {
            r = -ERR_IO;
        } else if (n == 0) {
            depth--;
        } else {
            struct rfs_inode c;
            if (!vfs_get_inode(fs, child, &c))
                r = -ERR_IO;
            else if (c.type != RFS_TYPE_DIR)
                r = may(p, &c, MAY_READ) ? 0 : -ERR_ACCES;
            else if (!may(p, &c, MAY_READ | MAY_EXEC))
                r = -ERR_ACCES;
            else if (depth + 1 == COPY_CHECK_DEPTH)
                r = -ERR_XDEV;
            else
                depth++, stack[depth].id = child, stack[depth].pos = 0, stack[depth].inode = c;
        }
    }
    kfree(stack);
    return r;
}

static int64_t sys_copy(struct process *p, uint64_t from_addr, uint64_t to_addr)
{
    char from[PATH_MAX], to[PATH_MAX];
    int64_t error = user_path(p, from_addr, from);
    if (!error)
        error = change_path(p, to_addr, to, false);
    if (error)
        return error;
    struct vfs *source;
    const char *inner;
    bool snapshots;
    uint64_t id;
    struct rfs_inode inode;
    if ((error = find(p, from, &source, &id, &inode, &snapshots)))
        return error;
    if (snapshots)
        return -ERR_INVAL;                          /* /.snapshots itself */
    /* The copy belongs to the caller: everything in it must be readable. */
    if ((error = tree_readable(p, source, id, &inode)))
        return error;
    struct vfs *fs;
    const char *inner_to;
    if ((error = parent_allows(p, to)) || (error = target(p, to, &fs, &inner_to)))
        return error;
    locate(p->fs, from, &source, &inner, &snapshots);
    /* Blocks are shared only within one RitterFS: elsewhere the programs
     * copy bytes. */
    if (!vfs_ritterfs(fs) || !vfs_ritterfs(source) || vfs_device(source) != vfs_device(fs))
        return -ERR_XDEV;
    struct rfs_owner owner = { p->uid, p->gid };
    return finish_change(fs, ritterfs_copy_from(vfs_ritterfs(fs), vfs_ritterfs(source), inner,
                                                inner_to, &owner));
}

/* --- mounting ------------------------------------------------------------------- */

/* Whether a program has the disk, or one of its partitions, open for
 * writing. */
static bool disk_open_for_writing(struct block_device *disk)
{
    bool found = false;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q && !found; q = q->next) {
        uint64_t s = spin_lock(&q->fds);        /* (65a-4: no close meanwhile; table, then fds) */
        for (int fd = 0; fd < FD_MAX && !found && q->state != PROCESS_ZOMBIE; fd++) {
            const struct open_file *f = q->files[fd];
            found = f && f->kind == FILE_DEVICE && f->writable && partition_disk(f->device) == disk;
        }
        spin_unlock(&q->fds, s);
    }
    spin_unlock(&table, state);
    return found;
}

/* mount(device, path or 0): a RitterFS at path, or at /volumes/<label>.
 * The path must be a directory in /volumes (one that is not there yet)
 * or an existing directory elsewhere; what it holds is hidden meanwhile. */
static int64_t sys_mount(struct process *p, uint64_t device_addr, uint64_t path_addr,
                         uint64_t out, uint64_t size)
{
    if (p->uid != 0)
        return -ERR_PERM;
    char name[32], path[PATH_MAX];
    int64_t error = copy_string_from_user(p, device_addr, name, sizeof(name));
    if (error)
        return error;
    struct block_device *dev = devices_by_name(name);
    if (!dev)
        return -ERR_NOENT;
    if (disk_open_for_writing(partition_disk(dev)))
        return -ERR_BUSY;                           /* a program is changing the disk */
    if (path_addr) {
        if ((error = user_path(p, path_addr, path)))
            return error;
        size_t n = sizeof(VOLUMES_DIR) - 1;
        if (strcmp(path, "/") == 0 || in_devices(path) || in_snapshots(path))
            return -ERR_BUSY;
        if (in_volumes(path)) {
            for (const char *c = path + n + 1; path[n] && *c; c++)
                if (*c == '/')
                    return -ERR_INVAL;              /* only directly in /volumes */
            if (!path[n])
                return -ERR_BUSY;                   /* /volumes itself */
        } else {
            struct vfs *fs;
            uint64_t id;
            struct rfs_inode inode;
            bool virtual;
            if ((error = find(p, path, &fs, &id, &inode, &virtual)))
                return error;
            if (virtual || inode.type != RFS_TYPE_DIR)
                return virtual ? -ERR_BUSY : -ERR_NOTDIR;
        }
    }
    char used[MOUNT_PATH_MAX];
    int r = mounts_add(dev, path_addr ? path : 0, used, sizeof(used));
    if (r)
        return -r;
    size_t n = strlen(used) + 1;
    if (size < n)
        return -ERR_INVAL;
    return copy_user(p, out, used, n, true) ? 0 : -ERR_FAULT;
}

/* Whether a program uses the volume at mount_path: an open file in it (or
 * in a snapshot of it), or its current directory there. */
static bool volume_busy(const char *mount_path, struct vfs *fs)
{
    size_t n = strlen(mount_path);
    bool busy = false;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q && !busy; q = q->next) {
        if (q->state == PROCESS_ZOMBIE)
            continue;
        busy = strncmp(q->cwd, mount_path, n) == 0 && (q->cwd[n] == 0 || q->cwd[n] == '/');
        uint64_t s = spin_lock(&q->fds);        /* (65a-4: no close meanwhile; table, then fds) */
        for (int fd = 0; fd < FD_MAX && !busy; fd++) {
            const struct open_file *f = q->files[fd];
            busy = f && (f->kind == FILE_RITTERFS || f->kind == FILE_SNAPSHOTS) && f->fs &&
                   vfs_device(f->fs) == vfs_device(fs);
        }
        spin_unlock(&q->fds, s);
    }
    spin_unlock(&table, state);
    return busy;
}

/* unmount(path or device). */
static int64_t sys_unmount(struct process *p, uint64_t addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    char arg[PATH_MAX], path[PATH_MAX], mount_path[MOUNT_PATH_MAX];
    int64_t error = copy_string_from_user(p, addr, arg, sizeof(arg));
    if (error)
        return error;
    struct vfs *fs;
    bool found = mounts_get(arg, mount_path, sizeof(mount_path), &fs, 0);
    if (!found && !resolve_path(p, arg, path, true))
        found = mounts_get(path, mount_path, sizeof(mount_path), &fs, 0);
    if (!found)
        return -ERR_INVAL;                          /* not mounted */
    if (volume_busy(mount_path, fs))
        return -ERR_BUSY;
    mounts_remove(mount_path);
    return 0;
}

/* --- the network ---------------------------------------------------------------- */

static struct netdev *card_named(const char *name);

/* forward: forwarding between cards: FORWARD_ASK, or switched (root only). */
static int64_t sys_forward(struct process *p, uint64_t mode)
{
    if (mode == FORWARD_ON || mode == FORWARD_OFF) {
        if (p->uid != 0)
            return -ERR_PERM;
        ip_set_forwarding(mode == FORWARD_ON);
    } else if (mode != FORWARD_ASK) {
        return -ERR_INVAL;
    }
    return ip_forwarding() ? 1 : 0;
}

/* masquerade: mark the private cards for source NAT (49i; root only). */
static int64_t sys_masquerade(struct process *p, uint64_t op, uint64_t name_addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    if (op == MASQ_RESET) {
        ip_masquerade_reset();
        return 0;
    }
    if (op == MASQ_ADD) {
        char name[8];
        if (!copy_user(p, name_addr, name, sizeof(name), false))
            return -ERR_FAULT;
        name[sizeof(name) - 1] = 0;
        struct netdev *d = card_named(name);
        if (!d)
            return -ERR_NOENT;
        return ip_masquerade_add(d) < 0 ? -ERR_NOMEM : 0;
    }
    return -ERR_INVAL;
}

/* (38a) tap(TAP_*, value): make or remove a virtual card (root only). */
static int64_t sys_tap(struct process *p, uint64_t op, uint64_t value)
{
    if (p->uid != 0)
        return -ERR_PERM;
    if (op == TAP_ADD) {
        if (value > 0xFFFFFFFFu)
            return -ERR_INVAL;
        int want = (int)(value >> 24 & 0xFF) - 1;   /* (38c) tapN itself, or -1 */
        return want >= TAP_MAX ? -ERR_NOSPC : tap_add((uint32_t)value & 0xFFFFFF, want);
    }
    if (op == TAP_REMOVE)
        return tap_remove(value < TAP_MAX ? (int)value : -1);
    return -ERR_INVAL;
}

/* (38b) bridge(BRIDGE_*, number, card): ports in and out (root only). */
static int64_t sys_bridge(struct process *p, uint64_t op, int64_t number, uint64_t name_addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    /* (38c) -2 - N: a new bridge brN itself */
    if (number < -1 - BRIDGE_MAX || number >= BRIDGE_MAX || (op == BRIDGE_REMOVE && number < 0))
        return -ERR_NOENT;
    struct netdev *d = 0;
    if (name_addr) {
        char name[8];
        if (!copy_user(p, name_addr, name, sizeof(name), false))
            return -ERR_FAULT;
        name[sizeof(name) - 1] = 0;
        if (!(d = card_named(name)))
            return -ERR_NOENT;
    }
    if (op == BRIDGE_ADD)
        return d ? bridge_add(number < -1 ? -1 : (int)number, number < -1 ? (int)(-2 - number) : -1, d)
                 : -ERR_INVAL;
    if (op == BRIDGE_REMOVE)
        return bridge_remove((int)number, d);
    return -ERR_INVAL;
}

/* (50g) default_route(DEFAULT_ROUTE_*, name): the default route's cards. */
static int64_t sys_default_route(struct process *p, uint64_t op, uint64_t name_addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    if (op == DEFAULT_ROUTE_RESET) {
        ip_default_reset();
        return 0;
    }
    if (op == DEFAULT_ROUTE_ADD) {
        char name[8];
        int64_t e = copy_string_from_user(p, name_addr, name, sizeof(name));
        if (e)
            return e;
        /* ethN, wlanN, (38b) tapN or brN: any card's name, there or not */
        static const char *const kinds[] = { "eth", "wlan", "tap", "br" };
        const char *digits = 0;
        for (int k = 0; k < 4 && !digits; k++)
            if (!strncmp(name, kinds[k], strlen(kinds[k])))
                digits = name + strlen(kinds[k]);
        if (!digits || !*digits)
            return -ERR_INVAL;
        for (const char *c = digits; *c; c++)
            if (*c < '0' || *c > '9')
                return -ERR_INVAL;
        return ip_default_add(name) < 0 ? -ERR_NOMEM : 0;
    }
    return -ERR_INVAL;
}

/* The card of this name, or 0 (38d: a removed tap or bridge is none). */
static struct netdev *card_named(const char *name)
{
    for (int i = 0; i < net_count(); i++) {
        struct netdev *d = net_at(i);
        if (strcmp(d->name, name) == 0 && !(d->virtual_card && d->gone))
            return d;
    }
    return 0;
}

/* netset: a card's IPv4 settings (root only): fixed (net up) or from DHCP
 * (NETSET_LEASED: the elf dhclient, which logs them itself). */
static int64_t sys_netset(struct process *p, uint64_t addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    struct sys_netset s;
    if (!copy_user(p, addr, &s, sizeof(s), false))
        return -ERR_FAULT;
    s.name[sizeof(s.name) - 1] = 0;
    if (s.prefix > 32 || s.dns_port > 65535 || (s.flags & ~(uint32_t)NETSET_LEASED))
        return -ERR_INVAL;
    struct netdev *d = card_named(s.name);
    if (!d)
        return -ERR_NOENT;
    if (d->bridge)
        return -ERR_BUSY;                       /* (38b) a bridge's port: the bridge's address */
    d->address = s.address;
    d->prefix = s.address ? s.prefix : 0;
    d->gateway = s.gateway;
    d->dns = s.dns;
    d->dns_port = (uint16_t)s.dns_port;
    d->lease_seconds = (s.flags & NETSET_LEASED) && s.address ? s.lease : 0;
    if (!(s.flags & NETSET_LEASED)) {
        char text[16], gateway[16];
        ip_text(s.address, text, sizeof(text));
        ip_text(s.gateway, gateway, sizeof(gateway));
        kprintf("  %s:          address %s/%u, gateway %s\n", d->name, text, s.prefix,
                s.gateway ? gateway : "none");
    }
    return 0;
}

/* netset6 (64b): a card's IPv6 settings. */
static int64_t sys_netset6(struct process *p, uint64_t addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    struct sys_netset6 s;
    if (!copy_user(p, addr, &s, sizeof(s), false))
        return -ERR_FAULT;
    s.name[sizeof(s.name) - 1] = 0;
    struct netdev *d = card_named(s.name);
    if (!d)
        return -ERR_NOENT;
    return -ip6_configure(d, &s);
}

/* netinfo: a card's MAC address, link and settings. */
/* The logs (log.c): a process's log name, a line added, what they hold. */
static int64_t sys_log(struct process *p, uint64_t op, uint64_t a, uint64_t b)
{
    if (p->uid != 0)
        return -ERR_PERM;
    if (op == LOG_AS || op == LOG_WRITE) {
        char name[LOG_NAME_SIZE];
        int64_t error = copy_string_from_user(p, a, name, sizeof(name));
        if (error)
            return error == -ERR_NAMETOOLONG ? -ERR_INVAL : error;
        if (op == LOG_AS) {
            memcpy(p->log, name, sizeof(name));
            return 0;
        }
        if (!name[0])
            return -ERR_INVAL;
        char *text = kmalloc(1024);
        if (!text)
            return -ERR_NOMEM;
        error = copy_string_from_user(p, b, text, 1024);
        if (!error)
            log_append(name, text, strlen(text));
        kfree(text);
        return error == -ERR_NAMETOOLONG ? -ERR_INVAL : error;
    }
    if (op != LOG_TAKE)
        return -ERR_INVAL;
    if (b > 128 * 1024)
        b = 128 * 1024;
    uint8_t *buffer = kmalloc(b ? b : 1);
    if (!buffer)
        return -ERR_NOMEM;
    size_t n = log_take(buffer, b);
    int64_t r = copy_user(p, a, buffer, n, true) ? (int64_t)n : -ERR_FAULT;
    kfree(buffer);
    return r;
}

/* The firewall (guard.c): its table loaded (root), switched off (root),
 * or reported. */
static int64_t sys_guard(struct process *p, uint64_t op, uint64_t address)
{
    if (op != GUARD_STATUS && op != GUARD_LOAD && op != GUARD_OFF && op != GUARD_SET6 &&
        op != GUARD_GET6)
        return -ERR_INVAL;
    if (op != GUARD_STATUS && op != GUARD_GET6 && p->uid != 0)
        return -ERR_PERM;
    if (op == GUARD_SET6 || op == GUARD_GET6) {     /* (64e) IPv6's table */
        struct sys_guard6 *g6 = kmalloc(sizeof(*g6));
        if (!g6)
            return -ERR_NOMEM;
        int64_t r = 0;
        if (op == GUARD_GET6) {
            guard_get6(g6);
            if (!copy_user(p, address, g6, sizeof(*g6), true))
                r = -ERR_FAULT;
        } else if (!copy_user(p, address, g6, sizeof(*g6), false)) {
            r = -ERR_FAULT;
        } else {
            r = -(int64_t)guard_set6(g6);
        }
        kfree(g6);
        return r;
    }
    if (op == GUARD_OFF) {
        guard_off();
        return 0;
    }
    struct sys_guard *g = kmalloc(sizeof(*g));
    if (!g)
        return -ERR_NOMEM;
    int64_t r = 0;
    if (op == GUARD_STATUS) {
        guard_status(g);
        if (!copy_user(p, address, g, sizeof(*g), true))
            r = -ERR_FAULT;
    } else if (!copy_user(p, address, g, sizeof(*g), false)) {
        r = -ERR_FAULT;
    } else {
        r = -(int64_t)guard_load(g);
    }
    kfree(g);
    return r;
}

/* Firmware variables (boot entries: osinstall), root only. */
static int64_t sys_efivar(struct process *p, uint64_t op, uint64_t address)
{
    if (op != EFIVAR_GET && op != EFIVAR_SET)
        return -ERR_INVAL;
    if (p->uid != 0)
        return -ERR_PERM;
    struct sys_efivar *v = kmalloc(sizeof(*v));
    if (!v)
        return -ERR_NOMEM;
    int64_t r;
    if (!copy_user(p, address, v, sizeof(*v), false)) {
        r = -ERR_FAULT;
    } else if (op == EFIVAR_GET) {
        v->name[sizeof(v->name) - 1] = 0;
        uint64_t size = sizeof(v->data);
        r = uefi_variable_get(v->name, v->guid, &v->attributes, v->data, &size);
        v->size = (uint32_t)size;
        if ((r == 0 || r == -ERR_2BIG) && !copy_user(p, address, v, sizeof(*v), true))
            r = -ERR_FAULT;
    } else {
        v->name[sizeof(v->name) - 1] = 0;
        r = v->size > sizeof(v->data) ? -ERR_INVAL
                                      : uefi_variable_set(v->name, v->guid, v->attributes,
                                                          v->data, v->size);
    }
    kfree(v);
    return r;
}

static int64_t sys_netinfo(struct process *p, uint64_t addr)
{
    struct sys_netinfo info;
    if (!copy_user(p, addr, &info, sizeof(info), false))
        return -ERR_FAULT;
    info.name[sizeof(info.name) - 1] = 0;
    struct netdev *d;
    if (info.name[0] == 0) {                    /* by place: programs list every card */
        if (info.index >= net_count())
            return -ERR_NODEV;
        d = net_at(info.index);
        for (size_t k = 0; k + 1 < sizeof(info.name) && d->name[k]; k++)
            info.name[k] = d->name[k];
    } else if (!(d = card_named(info.name)))
        return -ERR_NOENT;
    memcpy(info.mac, d->mac, 6);
    info.link_speed = net_link(d);              /* (38b) a bridge's port: none */
    info.address = d->address, info.prefix = d->prefix, info.gateway = d->gateway;
    info.dns = d->dns, info.dns_port = d->dns_port, info.lease = d->lease_seconds;
    return copy_user(p, addr, &info, sizeof(info), true) ? 0 : -ERR_FAULT;
}

/* netlease6 (64g): a DHCPv6 lease from the elf dhclient. */
/* (35a) swap(SWAP_*, struct sys_swap *). */
/* (37b-1) SYS_POWER's POWER_TABLE: ACPI table number index (root only: the
 * firmware's code and the machine's details), at most size bytes copied;
 * its full length, so a caller with too small a buffer can ask again. */
static int64_t sys_power_table(struct process *p, uint64_t index, uint64_t addr, uint64_t size)
{
    if (p->uid != 0)
        return -ERR_PERM;
    const struct acpi_header *t = index < 256 ? acpi_table_at((int)index) : 0;
    if (!t)
        return -ERR_NOENT;
    uint64_t n = t->length < size ? t->length : size;
    if (n && !copy_user(p, addr, (void *)t, n, true))   /* (to the program: read only) */
        return -ERR_FAULT;
    return t->length;
}

/* (37b-2) SYS_POWER's POWER_NAMES: the namespace's lines, copied to the
 * program a chunk at a time (the whole is ~150 KB on the laptops); the full
 * length, so a caller with too small a buffer can ask again. (37b-3)
 * POWER_EVAL's value the same way. */
struct text_copy {
    struct process *p;
    uint64_t addr, size, at;            /* at: every byte so far, copied or not */
    bool fault;
    uint32_t used;
    char chunk[1024];
};

static void text_flush(struct text_copy *c)
{
    uint64_t start = c->at - c->used;
    if (c->size && start < c->size - 1) {
        uint64_t n = c->size - 1 - start < c->used ? c->size - 1 - start : c->used;
        if (!copy_user(c->p, c->addr + start, c->chunk, n, true))
            c->fault = true;
    }
    c->used = 0;
}

static void text_line(const char *line, void *ctx)
{
    struct text_copy *c = ctx;
    size_t n = strlen(line);
    for (size_t i = 0; i <= n; i++) {
        if (c->used == sizeof(c->chunk))
            text_flush(c);
        c->chunk[c->used++] = i < n ? line[i] : '\n';
        c->at++;
    }
}

/* The text's end: what was not flushed, then its 0. */
static int64_t text_end(struct text_copy *c)
{
    text_flush(c);
    char end = 0;
    if (c->size && !copy_user(c->p, c->addr + (c->at < c->size - 1 ? c->at : c->size - 1),
                              &end, 1, true))
        c->fault = true;
    int64_t result = c->fault ? -ERR_FAULT : (int64_t)c->at;
    kfree(c);
    return result;
}

static int64_t sys_power_names(struct process *p, uint64_t addr, uint64_t size)
{
    if (p->uid != 0)
        return -ERR_PERM;
    struct text_copy *c = kzalloc(sizeof(*c));
    if (!c)
        return -ERR_NOMEM;
    c->p = p, c->addr = addr, c->size = size;
    acpi_aml_names(text_line, c);
    return text_end(c);
}

/* (37b-3) POWER_EVAL: the object at the path, evaluated, as text. */
static int64_t sys_power_eval(struct process *p, uint64_t path_addr, uint64_t addr, uint64_t size)
{
    if (p->uid != 0)
        return -ERR_PERM;
    char path[256];
    int64_t error = copy_string_from_user(p, path_addr, path, sizeof(path));
    if (error)
        return error;
    struct text_copy *c = kzalloc(sizeof(*c));
    if (!c)
        return -ERR_NOMEM;
    c->p = p, c->addr = addr, c->size = size;
    int found = acpi_aml_eval(path, text_line, c);
    if (found < 0) {
        kfree(c);
        return found == -1 ? -ERR_NOENT : -ERR_IO;
    }
    return text_end(c);
}

static int64_t sys_swap(struct process *p, uint64_t op, uint64_t addr)
{
    if (op == SWAP_STATUS) {
        struct sys_swap s;
        swap_status(&s);
        return copy_user(p, addr, &s, sizeof(s), true) ? 0 : -ERR_FAULT;
    }
    if (op != SWAP_ON && op != SWAP_OFF)
        return -ERR_INVAL;
    if (p->uid != 0)
        return -ERR_PERM;
    return op == SWAP_ON ? swap_on() : swap_off();
}

static int64_t sys_netlease6(struct process *p, uint64_t addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    struct sys_netlease6 lease;
    if (!copy_user(p, addr, &lease, sizeof(lease), false))
        return -ERR_FAULT;
    lease.name[sizeof(lease.name) - 1] = 0;
    struct netdev *d = card_named(lease.name);
    if (!d)
        return -ERR_NOENT;
    return -ip6_lease(d, &lease);
}

/* netinfo6 (64d): a card's IPv6 side, by name or place as netinfo. */
static int64_t sys_netinfo6(struct process *p, uint64_t addr)
{
    struct sys_netinfo6 info;
    if (!copy_user(p, addr, &info, sizeof(info), false))
        return -ERR_FAULT;
    char name[8];
    memcpy(name, info.name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    uint16_t index = info.index;
    memset(&info, 0, sizeof(info));
    struct netdev *d;
    if (name[0] == 0) {
        if (index >= net_count())
            return -ERR_NODEV;
        d = net_at(index);
    } else if (!(d = card_named(name))) {
        return -ERR_NOENT;
    }
    for (size_t k = 0; k + 1 < sizeof(info.name) && d->name[k]; k++)
        info.name[k] = d->name[k];
    info.index = (uint16_t)net_index(d);
    ip6_info(d, &info);
    return copy_user(p, addr, &info, sizeof(info), true) ? 0 : -ERR_FAULT;
}

/* SYS_WLAN (24c): a wireless card's radio and link, root only; the buffer
 * goes through a kernel copy (filled back for WLAN_INFO and WLAN_RECEIVE). */
static int64_t sys_wlan(struct process *p, uint64_t op, uint64_t name_addr, uint64_t addr,
                        uint64_t length, uint64_t value)
{
    if (p->uid != 0)
        return -ERR_PERM;
    char name[8];
    if (!copy_user(p, name_addr, name, sizeof(name), false))
        return -ERR_FAULT;
    name[sizeof(name) - 1] = 0;
    struct netdev *d = card_named(name);
    if (!d)
        return -ERR_NOENT;
    if (!d->wlan)
        return -ERR_NOTSUP;
    if (length > sizeof(struct sys_wlan_frame) + WLAN_FRAME_MAX)
        length = sizeof(struct sys_wlan_frame) + WLAN_FRAME_MAX;
    void *buffer = length ? kmalloc(length) : 0;
    if (length && !buffer)
        return -ERR_NOMEM;
    bool out = op == WLAN_INFO || op == WLAN_RECEIVE;
    if (length && !out && !copy_user(p, addr, buffer, length, false)) {
        kfree(buffer);
        return -ERR_FAULT;
    }
    int64_t r = wlan_call(d, (int)op, buffer, length, value);
    if (out && r >= 0) {
        size_t n = op == WLAN_INFO ? sizeof(struct sys_wlan_info) : (size_t)r;
        if (!copy_user(p, addr, buffer, n, true))
            r = -ERR_FAULT;
    }
    kfree(buffer);
    return r;
}

/* A free descriptor for a socket: its number, or -ERR_MFILE. */
static int64_t socket_fd(struct process *p, struct socket *s)
{
    int fd = fd_claim(p);
    if (fd < 0) {
        socket_close(s);
        return fd;
    }
    struct open_file f = {0};
    f.kind = FILE_SOCKET;
    f.socket = s;
    f.readable = f.writable = true;
    fd_install(p, fd, &f);
    return fd;
}

/* Install a shared buffer (SYS_SHARE) as a descriptor; it takes over the
 * reference the caller holds. The descriptor, or -ERR_* (then the reference
 * is dropped). */
static int64_t shbuf_fd(struct process *p, struct shbuf *b)
{
    int fd = fd_claim(p);
    if (fd < 0) {
        shbuf_unref(b);
        return fd;
    }
    struct open_file f = {0};
    f.kind = FILE_SHARE;
    f.shbuf = b;
    fd_install(p, fd, &f);
    return fd;
}

/* SHARE_*: shared memory. CREATE a buffer (its fd), MAP it into this program
 * (the address), its SIZE, or SEND / RECV it over a local socket. */
/* mmap (22a): what file the descriptor is, then the mapping (mmap.c), which
 * may sleep reading the file, so without the descriptor table's lock. */
static int64_t sys_mmap(struct process *p, uint64_t addr, uint64_t length, uint64_t access,
                        uint64_t flags, uint64_t fd, uint64_t offset)
{
    struct mmap_file from;
    if (!(flags & MMAP_ANONYMOUS)) {
        uint64_t state = spin_lock(&p->fds);
        struct open_file *f = file(p, fd);
        int64_t error = !f ? -ERR_BADF : f->kind != FILE_RITTERFS ? -ERR_ACCES : 0;
        if (!error) {
            from.fs = f->fs;
            from.object_id = f->object_id;
            from.readable = f->readable;
            from.writable = f->writable;
        }
        spin_unlock(&p->fds, state);
        if (error)
            return error;
    }
    return mmap_map(&p->mm, p->space, addr, length, (unsigned)access, (unsigned)flags,
                    (flags & MMAP_ANONYMOUS) ? 0 : &from, offset);
}

static int64_t sys_share(struct process *p, uint64_t op, uint64_t a, uint64_t b)
{
    switch (op) {
    case SHARE_CREATE: {
        struct shbuf *buf = shbuf_create(a);
        return buf ? shbuf_fd(p, buf) : -ERR_INVAL;
    }
    case SHARE_MAP: {
        uint64_t state = spin_lock(&p->fds);
        struct open_file *f = file(p, a);
        if (!f || f->kind != FILE_SHARE) {
            spin_unlock(&p->fds, state);
            return -ERR_BADF;
        }
        if (f->shbuf_va) {                          /* already mapped in this program */
            uint64_t va = f->shbuf_va;
            spin_unlock(&p->fds, state);
            return (int64_t)va;
        }
        struct shbuf *buf = f->shbuf;
        unsigned pages = shbuf_pages(buf);
        uint64_t va = p->share_next;
        if (va + (uint64_t)pages * PAGE_SIZE + PAGE_SIZE > MMAP_WINDOW_BASE) {
            spin_unlock(&p->fds, state);            /* (49l: never into mmap's window, */
            return -ERR_NOMEM;                      /* whose pages mmap would free) */
        }
        bool ok = true;
        for (unsigned i = 0; i < pages && ok; i++)
            ok = paging_user_map(p->space, va + (uint64_t)i * PAGE_SIZE, shbuf_page(buf, i),
                                 USER_READ | USER_WRITE);
        if (!ok) {
            for (unsigned i = 0; i < pages; i++)    /* undo a partial mapping */
                paging_user_unmap(p->space, va + (uint64_t)i * PAGE_SIZE);
            spin_unlock(&p->fds, state);
            return -ERR_NOMEM;
        }
        f->shbuf_va = va;
        p->share_next = va + (uint64_t)pages * PAGE_SIZE + PAGE_SIZE;    /* a gap between buffers */
        spin_unlock(&p->fds, state);
        return (int64_t)va;
    }
    case SHARE_SIZE: {
        uint64_t state = spin_lock(&p->fds);
        struct open_file *f = file(p, a);
        int64_t r = f && f->kind == FILE_SHARE ? (int64_t)shbuf_size(f->shbuf) : -ERR_BADF;
        spin_unlock(&p->fds, state);
        return r;
    }
    case SHARE_SEND: {
        struct socket *s = socket_grab(p, a);
        if (!s)
            return -ERR_BADF;
        uint64_t state = spin_lock(&p->fds);
        struct open_file *f = file(p, b);
        struct shbuf *buf = f && f->kind == FILE_SHARE ? f->shbuf : 0;
        if (buf)
            shbuf_hold(buf);                        /* the queue's reference */
        spin_unlock(&p->fds, state);
        int64_t result;
        if (!buf) {
            result = -ERR_BADF;
        } else {
            int r = socket_send_buffer(s, buf);
            if (r)
                shbuf_unref(buf);                   /* not queued after all */
            result = r ? -r : 0;
        }
        socket_close(s);
        return result;
    }
    case SHARE_RECV: {
        struct socket *s = socket_grab(p, a);
        if (!s)
            return -ERR_BADF;
        struct shbuf *buf = socket_recv_buffer(s);
        socket_close(s);
        return buf ? shbuf_fd(p, buf) : -ERR_AGAIN;
    }
    default:
        return -ERR_INVAL;
    }
}

/* The program that owns the screen (SYS_DISPLAY), or 0. One at a time. */
static int display_owner;
static struct mutex display_changes = MUTEX_INIT("display changes");    /* (66b-2) */

/* Copy a rectangle of the owner's pixels to the screen (DISPLAY_FLUSH),
 * one row at a time through a small bounce buffer. */
static int64_t display_flush(struct process *p, const struct sys_display_flush *f)
{
    uint32_t sw, sh, stride;
    uint8_t rs, gs, bs;
    if (!console_screen_format(&sw, &sh, &stride, &rs, &gs, &bs))
        return -ERR_NOENT;                          /* no screen */
    if (f->x >= sw || f->y >= sh || f->w == 0 || f->h == 0)
        return 0;                                   /* nothing visible */
    uint32_t w = f->w, h = f->h;
    if (w > sw - f->x)
        w = sw - f->x;
    if (h > sh - f->y)
        h = sh - f->y;
    uint32_t *row = kmalloc((uint64_t)w * 4);
    if (!row)
        return -ERR_NOMEM;
    int64_t result = 0;
    for (uint32_t r = 0; r < h; r++) {
        if (!copy_user(p, f->pixels + (uint64_t)r * f->stride, row, (uint64_t)w * 4, false)) {
            result = -ERR_FAULT;
            break;
        }
        console_display_row(f->x, f->y + r, row, w);
    }
    kfree(row);
    return result;
}

/* DISPLAY_*: the screen. ACQUIRE parks the console so this program owns the
 * screen; RELEASE gives it back; INFO reports its size and format; FLUSH
 * copies pixels to it. */
/* Is p in the session at the machine's own screen and keyboard? Its terminal
 * is the console, and its session is the one the console's foreground group
 * belongs to: the user logged in there, and their background jobs (windowd
 * &). Commands over SSH without a terminal share sshd's console terminal but
 * not that session; SSH logins have a pseudo-terminal; elves sessions of
 * their own. */
static bool at_console(struct process *p)
{
    if (p->tty || p->no_terminal)
        return false;
    int foreground = terminal_foreground();
    bool same = false;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q; q = q->next)
        if (q->group == foreground && q->state != PROCESS_ZOMBIE) {
            same = q->session == p->session;
            break;
        }
    spin_unlock(&table, state);
    return same;
}

/* Is p's user the one at the console: the user of the console's foreground
 * job (their shell, or windowd started from it)? Their programs may play
 * sound wherever they run (term's shells have pseudo-terminals); another
 * user's, logged in over SSH, may not (49k: like a desktop's seat). */
static bool console_user(struct process *p)
{
    int foreground = terminal_foreground();
    bool same = false;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q; q = q->next)
        if (q->group == foreground && q->state != PROCESS_ZOMBIE) {
            same = q->uid == p->uid;
            break;
        }
    spin_unlock(&table, state);
    return same;
}

static int64_t sys_display(struct process *p, uint64_t op, uint64_t a, uint64_t b)
{
    switch (op) {
    case DISPLAY_ACQUIRE:
        /* Root, or the user at the console (15k, the user's): the screen's
         * owner also gets every key typed on the machine, so not a user
         * logged in from elsewhere -- the person at the keyboard could be
         * typing a password. */
        if (p->uid != 0 && !at_console(p))
            return -ERR_PERM;
        mutex_lock(&display_changes);               /* (66b-2) not while a mode changes */
        if (display_owner && display_owner != p->pid) {
            mutex_unlock(&display_changes);
            return -ERR_BUSY;
        }
        if (!console_display_acquire()) {
            mutex_unlock(&display_changes);
            return -ERR_NOENT;                      /* no screen */
        }
        display_owner = p->pid;
        mutex_unlock(&display_changes);
        winput_capture();                           /* input comes here now, not the terminal */
        return 0;
    case DISPLAY_SET_MODE: {                        /* (66b-2) as ACQUIRE: who may */
        if (p->uid != 0 && !at_console(p))
            return -ERR_PERM;
        struct sys_display_set_mode m;
        if (!copy_user(p, a, &m, sizeof(m), false))
            return -ERR_FAULT;
        m.output[sizeof(m.output) - 1] = 0;
        memset(m.said, 0, sizeof(m.said));
        mutex_lock(&display_changes);
        int64_t r = gpu_set_mode(m.output, m.width, m.height, m.refresh_mhz, m.said,
                                 sizeof(m.said));
        uint32_t w, h, stride;
        uint8_t rs, gs, bs;
        if (r == 0 && display_owner && console_screen_format(&w, &h, &stride, &rs, &gs, &bs))
            winput_screen(w, h);                    /* (66b-5) the owner redraws at the new size */
        mutex_unlock(&display_changes);
        if (!copy_user(p, a + offsetof(struct sys_display_set_mode, said), m.said, sizeof(m.said),
                       true))
            return -ERR_FAULT;
        return r;
    }
    case DISPLAY_RELEASE:
        if (display_owner != p->pid)
            return -ERR_PERM;
        gpu_screen_back();                          /* (66d-1) the console's buffer shown */
        screen_unmap(p);                            /* (66d-2) */
        winput_release();
        console_display_release();
        display_owner = 0;
        return 0;
    case DISPLAY_EVENT: {
        if (display_owner != p->pid)
            return -ERR_PERM;
        struct sys_input_event e;
        int r = winput_next(&e, (uint32_t)b);
        if (r <= 0)
            return r;                               /* 0: none; <0: interrupted */
        return copy_user(p, a, &e, sizeof(e), true) ? 1 : -ERR_FAULT;
    }
    case DISPLAY_BUFFERS: {                       /* (66d-2) the screen's owner */
        if (display_owner != p->pid)
            return -ERR_PERM;
        struct sys_display_buffers d = { 0 };
        uint64_t phys, slot;
        int r = gpu_buffers(&d, &phys, &slot);
        if (r < 0)
            return r;
        if (!p->screen_va) {                        /* mapped once: the slots never move */
            uint64_t pages = d.count * slot / PAGE_SIZE;
            uint64_t state = spin_lock(&p->fds);    /* (share_next's guard) */
            uint64_t va = p->share_next;
            bool room = va + pages * PAGE_SIZE + PAGE_SIZE <= MMAP_WINDOW_BASE;
            if (room)
                p->share_next = va + pages * PAGE_SIZE + PAGE_SIZE;
            spin_unlock(&p->fds, state);
            if (!room)
                return -ERR_NOMEM;
            for (uint64_t i = 0; i < pages; i++)
                if (!paging_user_map(p->space, va + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                                     USER_READ | USER_WRITE | USER_COMBINE)) {
                    for (uint64_t k = 0; k < i; k++)
                        paging_user_unmap(p->space, va + k * PAGE_SIZE);
                    return -ERR_NOMEM;
                }
            p->screen_va = va;
            p->screen_pages = pages;
        }
        for (uint32_t k = 0; k < d.count; k++)
            d.address[k] = p->screen_va + k * slot;
        return copy_user(p, a, &d, sizeof(d), true) ? 0 : -ERR_FAULT;
    }
    case DISPLAY_FLIP: {                          /* (66d-1) the screen's owner */
        if (display_owner != p->pid)
            return -ERR_PERM;
        uint64_t frame = 0;
        int r = gpu_flip((int)a, &frame);
        if (r > 0 && b && !copy_user(p, b, &frame, sizeof(frame), true))
            return -ERR_FAULT;
        return r;
    }
    case DISPLAY_VBLANK: {                        /* (66c-3) anyone may wait */
        uint64_t frame = 0;
        int r = gpu_wait_vblank(&frame, (uint32_t)(b ? b : 1000));
        if (thread_current()->stop_requested)
            return -ERR_INTR;
        if (r > 0 && !copy_user(p, a, &frame, sizeof(frame), true))
            return -ERR_FAULT;
        return r;
    }
    case DISPLAY_MODES: {                         /* (65e-34) anyone may ask */
        struct sys_display_modes *d = kmalloc(sizeof(*d));
        uint32_t w, h, stride;
        uint8_t rs, gs, bs;
        if (!d)
            return -ERR_NOMEM;
        if (!console_screen_format(&w, &h, &stride, &rs, &gs, &bs)) {
            kfree(d);
            return -ERR_NOENT;
        }
        display_modes(d, w, h);
        bool ok = copy_user(p, a, d, sizeof(*d), true);
        kfree(d);
        return ok ? 0 : -ERR_FAULT;
    }
    case DISPLAY_INFO: {
        struct sys_display d = {0};
        if (!console_screen_format(&d.width, &d.height, &d.stride, &d.red_shift, &d.green_shift,
                                   &d.blue_shift))
            return -ERR_NOENT;
        d.bytes_per_pixel = 4;
        return copy_user(p, a, &d, sizeof(d), true) ? 0 : -ERR_FAULT;
    }
    case DISPLAY_FLUSH: {
        if (display_owner != p->pid)
            return -ERR_PERM;
        struct sys_display_flush f;
        if (!copy_user(p, a, &f, sizeof(f), false))
            return -ERR_FAULT;
        return display_flush(p, &f);
    }
    default:
        return -ERR_INVAL;
    }
}

/* (66d-2) Take the screen buffers out of p (released, ended, a new
 * program): device memory, never the space's to free. */
static void screen_unmap(struct process *p)
{
    if (!p->screen_va)
        return;
    mmap_change_begin(&p->mm);
    for (uint64_t i = 0; i < p->screen_pages; i++)
        paging_user_unmap(p->space, p->screen_va + i * PAGE_SIZE);
    mmap_change_end(&p->mm);
    p->screen_va = 0;
    p->screen_pages = 0;
}

/* A process ended: if it owned the screen, give it back to the console. */
static void display_process_ended(int pid)
{
    screen_unmap(current_process());                /* (66d-2; it ends on its own thread) */
    if (display_owner == pid) {
        gpu_screen_back();                          /* (66d-1) */
        winput_release();
        console_display_release();
        display_owner = 0;
    }
}

/* Ctrl-Alt-Delete (from the keyboard interrupt): end the screen's owner. Its
 * own teardown (display_process_ended, in its thread context) gives the
 * screen back to the text console -- so this never repaints from interrupt
 * context, and works even if the owner is stuck in a loop (SIGKILL is noticed
 * at the next return to user mode). display_owner is an aligned int: its read
 * here does not tear against the owner's own syscalls. */
void display_escape(void)
{
    int pid = display_owner;
    if (pid)
        process_signal(pid, SIGKILL, 0);
}

/* PTY_OPEN: a new pseudo-terminal, its master and slave side as two
 * descriptors (into int fds[2]); PTY_SIZE: its size in characters. */
static int64_t sys_pty(struct process *p, uint64_t op, uint64_t a, uint64_t b, uint64_t c)
{
    if (op == PTY_SIZE) {
        uint64_t s = spin_lock(&p->fds);
        struct open_file *f = file(p, a);
        struct pty *t = f && f->kind == FILE_PTY ? f->pty : 0;
        if (t)
            pty_set_size(t, (uint32_t)b, (uint32_t)c);
        spin_unlock(&p->fds, s);
        return t ? 0 : -ERR_BADF;
    }
    if (op != PTY_OPEN)
        return -ERR_INVAL;
    int fds[2];
    fds[0] = fd_claim(p);                           /* two slots, each claimed atomically (49b) */
    if (fds[0] < 0)
        return fds[0];
    fds[1] = fd_claim(p);
    if (fds[1] < 0) {
        fd_release(p, fds[0]);
        return -ERR_MFILE;
    }
    struct pty *t = pty_new();
    if (!t) {
        fd_release(p, fds[0]);
        fd_release(p, fds[1]);
        return -ERR_NOMEM;
    }
    if (!copy_user(p, a, fds, sizeof(fds), true)) {
        pty_close(t, true);
        pty_close(t, false);
        fd_release(p, fds[0]);
        fd_release(p, fds[1]);
        return -ERR_FAULT;
    }
    for (int i = 0; i < 2; i++) {
        struct open_file f = {0};
        f.kind = FILE_PTY;
        f.pty = t;
        f.master = i == 0;
        f.readable = f.writable = true;
        fd_install(p, fds[i], &f);
    }
    return 0;
}

/* pipe(int fds[2]): fds[0] the read end, fds[1] the write end. */
static int64_t sys_pipe(struct process *p, uint64_t out)
{
    int fds[2];
    fds[0] = fd_claim(p);                           /* two slots, each claimed atomically (49b) */
    if (fds[0] < 0)
        return fds[0];
    fds[1] = fd_claim(p);
    if (fds[1] < 0) {
        fd_release(p, fds[0]);
        return -ERR_MFILE;
    }
    struct pipe *pp = pipe_new();
    if (!pp) {
        fd_release(p, fds[0]);
        fd_release(p, fds[1]);
        return -ERR_NOMEM;
    }
    if (!copy_user(p, out, fds, sizeof(fds), true)) {
        pipe_close(pp, false);                      /* the read and write references */
        pipe_close(pp, true);
        fd_release(p, fds[0]);
        fd_release(p, fds[1]);
        return -ERR_FAULT;
    }
    for (int i = 0; i < 2; i++) {
        struct open_file f = {0};
        f.kind = FILE_PIPE;
        f.pipe = pp;
        f.readable = i == 0;                        /* fds[0] reads, fds[1] writes */
        f.writable = i == 1;
        fd_install(p, fds[i], &f);
    }
    return 0;
}

/* execve (55b): the calling process becomes another program. Everything
 * that can fail without harm is checked first (the arguments, the file:
 * a program for this processor, or a #! script's interpreter); then the
 * other threads end, and from there on there is no way back: the old
 * address space goes, the new program is loaded into a fresh one (a
 * failure then ends the process, as Linux's past its point of no return),
 * and the thread jumps to it. */
static int64_t sys_execve(struct process *p, uint64_t path_addr, uint64_t argv_addr,
                          uint64_t envp_addr)
{
    char path[PATH_MAX];
    int64_t error = copy_string_from_user(p, path_addr, path, sizeof(path));
    if (error)
        return error;
    char *strings = 0, *env_strings = 0;
    const char **argv = 0, **envp = 0;
    char *full = kmalloc(PATH_MAX), *interp = kmalloc(PATH_MAX), *arg = kmalloc(128);
    if (!full || !interp || !arg) {
        error = -ERR_NOMEM;
        goto fail;
    }
    int argc = copy_strings_sized(p, argv_addr, &strings, ARGS_MAX, &argv, ARGC_MAX, 2);
    int envc = argc < 0 ? 0 : envp_addr ? copy_strings_sized(p, envp_addr, &env_strings, ENV_MAX,
                                                             &envp, ENVC_MAX, 0) : 0;
    if (argc < 0 || envc < 0) {
        error = argc < 0 ? argc : envc;
        goto fail;
    }
    const char *run_path = path;
    if (script_interpreter(p, path, interp, PATH_MAX, arg, 128)) {
        int k = arg[0] ? 2 : 1;                     /* INTERPRETER [ARG] PATH ARGV[1]... */
        if (argc + k + 1 > ARGC_MAX) {
            error = -ERR_2BIG;
            goto fail;
        }
        memmove(argv + k + 1, argv + 1, (size_t)(argc > 0 ? argc - 1 : 0) * sizeof(char *));
        argv[0] = interp;
        if (arg[0])
            argv[1] = arg;
        argv[k] = path;
        argc = argc > 0 ? argc + k : k + 1;
        run_path = interp;
    }
    if ((error = -resolve_path(p, run_path, full, true)))
        goto fail;
    {
        struct vfs *fs;
        uint64_t id;
        struct rfs_inode inode;
        Elf64_Ehdr eh;
        Elf64_Phdr ph[MAX_SEGMENTS];
        int r = read_program(p, full, &fs, &id, &inode, &eh, ph);   /* (it can be run: or not) */
        if (r) {
            error = -r;
            goto fail;
        }
    }

    /* The other threads end (as process_exit has them), the process stays. */
    uint64_t state = spin_lock(&table);
    if (p->exiting) {
        spin_unlock(&table, state);
        error = -ERR_INTR;
        goto fail;
    }
    p->exiting = true;
    p->exiter = thread_current();
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (p->threads[i] && p->threads[i] != thread_current())
            sched_request_stop(p->threads[i]);
    spin_unlock(&table, state);
    wait_event(&p->thread_events, p->live == 1);
    state = spin_lock(&table);
    p->exiting = false;
    p->exiter = 0;
    int self = thread_slot(p);
    for (int i = 0; i < PROCESS_THREADS; i++)
        if (i != self && p->slots[i] != SLOT_RUNNING)
            p->slots[i] = SLOT_FREE;                /* (ended ones nobody joins now) */
    if (self > 0) {                                 /* the caller becomes thread 0, as POSIX says */
        p->threads[0] = p->threads[self];
        p->threads[self] = 0;
        p->slots[0] = SLOT_RUNNING;
        p->slots[self] = SLOT_FREE;
    }
    /* Handled signals go back to the default; ignored ones stay ignored. */
    for (int i = 0; i < SIGNALS; i++) {
        p->handlers[i] = 0;
        p->handler_masks[i] = 0;
        p->handler_flags[i] = 0;
    }
    p->restorer = 0;
    spin_unlock(&table, state);

    /* FD_CLOEXEC descriptors close; shared buffers leave the old space. */
    for (int fd = 0; fd < FD_MAX; fd++) {
        uint64_t s = spin_lock(&p->fds);
        struct open_file *f = p->files[fd];
        bool close_it = f && (p->cloexec[fd / 8] >> (fd % 8) & 1);
        if (close_it) {
            fds_change_begin(p);                    /* (65e-42) */
            __atomic_store_n(&p->files[fd], 0, __ATOMIC_RELEASE);
            fds_change_end(p);
            p->cloexec[fd / 8] &= (uint8_t)~(1u << (fd % 8));
        }
        spin_unlock(&p->fds, s);
        if (close_it) {
            file_put(f);
        } else if (f && f->kind == FILE_SHARE && f->shbuf_va) {
            for (unsigned i = 0; i < shbuf_pages(f->shbuf); i++)
                paging_user_unmap(p->space, f->shbuf_va + (uint64_t)i * PAGE_SIZE);
            f->shbuf_va = 0;
        }
    }
    screen_unmap(p);                                /* (66d-2) not the new program's */

    /* The old program's memory goes; the new one gets a fresh space.
     * (35b) Not while pageout writes its pages: its mappings' state (the
     * lock too) is made new, and pageout leaves it alone until loaded. */
    uint64_t pstate = spin_lock(&table);
    p->execing = true;
    spin_unlock(&table, pstate);
    wait_event(&paging_done, !swap_at(p));
    uint64_t old = p->space;
    mmap_release_all(&p->mm, old);
    uint64_t space = paging_user_create();
    if (!space) {
        kfree(strings), kfree(env_strings), kfree(argv), kfree(envp), kfree(full), kfree(interp),
            kfree(arg);
        process_exit(STATUS_SIGNALED(SIGKILL));
    }
    uint64_t irq = arch_irq_save();
    p->space = space;
    thread_current()->space = space;
    sched_set_space(space);
    arch_irq_restore(irq);
    cpus_wait_space_unused(old);                    /* (65c-2) every core off the old tables */
    paging_user_destroy(old);
    uint64_t r[2];
    random_bytes(r, sizeof(r));
    p->share_next = SHARE_BASE + (r[0] % (SHARE_SHIFT_MAX / PAGE_SIZE)) * PAGE_SIZE;
    p->stack_top = USER_STACK_TOP - (r[1] % (STACK_SHIFT_MAX / PAGE_SIZE)) * PAGE_SIZE;
    mmap_init(&p->mm);
    const char *name = full;
    for (const char *c = full; *c; c++)
        if (*c == '/')
            name = c + 1;
    size_t n = strlen(name) < sizeof(p->name) - 1 ? strlen(name) : sizeof(p->name) - 1;
    memcpy(p->name, name, n);
    p->name[n] = 0;
    p->carry_length = 0;
    p->execed = true;
    struct start_info start;
    int r2 = load(p, full, &start);
    if (!r2 && !(p->user_sp = push_arguments(p, argc, argv, envc, envp, &start)))
        r2 = ERR_2BIG;
    kfree(strings), kfree(env_strings), kfree(argv), kfree(envp), kfree(full), kfree(interp),
        kfree(arg);
    if (r2)
        process_exit(STATUS_SIGNALED(SIGKILL));     /* (past the point of no return) */
    p->mm.pageable = true;                          /* (35b) loaded: pageout may take pages */
    pstate = spin_lock(&table);
    p->execing = false;
    spin_unlock(&table, pstate);

    /* A clean thread: no per-thread data pointer, fresh FP/SIMD registers. */
    struct thread *t = thread_current();
    irq = arch_irq_save();
    t->tp = 0;
    arch_set_thread_pointer(0);
    t->enter_at = 0;
    arch_fpu_clean(t->fpu);
    arch_fpu_restore(t->fpu);
    arch_irq_restore(irq);
    arch_enter_user(p->entry, p->user_sp, thread_stack_top(t));

fail:
    kfree(strings), kfree(env_strings), kfree(argv), kfree(envp), kfree(full), kfree(interp),
        kfree(arg);
    return error;
}

/* fork (56a): what the child's thread starts from. */
struct fork_start {
    struct process *p;
    int slot;                                       /* the caller's thread number: the child's */
    uint64_t tp, blocked;
    uint64_t size;
    uint8_t fpu[ARCH_FPU_STATE_SIZE];
    uint8_t frame[ARCH_FRAME_MAX];
};

/* The child's thread: the caller's FP/SIMD registers, thread pointer and
 * blocked set, then back to the program as from the system call, with 0. */
static void fork_thread(void *arg)
{
    struct fork_start *s = arg;
    uint8_t frame[ARCH_FRAME_MAX] __attribute__((aligned(16)));
    struct thread *t = thread_current();
    if (t->process != s->p || !t->fpu) {           /* (65c-3) */
        uint64_t here = (uint64_t)&frame, top = thread_stack_top(t);
        panic("fork: the child (pid %d) runs as thread %d '%s' of pid %d (core %d), on %s stack "
              "(sp %p, its top %p)", s->p->pid, t->id, t->name, t->process ? t->process->pid : -1,
              arch_this_cpu()->index, here < top && here >= top - 32768 ? "its" : "another",
              (void *)here, (void *)top);
    }
    memcpy(frame, s->frame, s->size);
    uint64_t state = spin_lock(&table);             /* (no switch in between: it would save
                                                     * the live registers over these) */
    memcpy(t->fpu, s->fpu, ARCH_FPU_STATE_SIZE);
    arch_fpu_restore(t->fpu);
    arch_set_thread_pointer(s->tp);
    t->tp = s->tp;
    t->sig_blocked = s->blocked;
    s->p->threads[s->slot] = t;
    spin_unlock(&table, state);
    kfree(s);
    arch_interrupts_disable();
    process_before_return(frame);                   /* (a signal sent to it meanwhile) */
    arch_resume_user(frame, thread_stack_top(t));
}

/* Undo a child not started: what it holds goes, it was never listed. */
static void fork_undo(struct process *c)
{
    if (c->space) {
        mmap_release_all(&c->mm, c->space);
        paging_user_destroy(c->space);
    }
    if (c->tty)
        pty_release_terminal(c->tty);
    for (int fd = 0; fd < FD_MAX; fd++)
        if (c->files[fd])
            file_put(c->files[fd]);
    kfree(c);
}

int64_t process_fork(const void *frame, uint64_t size)
{
    struct process *p = current_process();
    if (!p || size > ARCH_FRAME_MAX)
        return -ERR_INVAL;
    if (p->exiting)
        return -ERR_INTR;                           /* (exit or exec in another thread) */
    struct process *c = kzalloc(sizeof(*c));
    struct fork_start *s = kmalloc(sizeof(*s));
    if (!c || !s) {
        kfree(c);
        kfree(s);
        return -ERR_NOMEM;
    }
    memcpy(c->name, p->name, sizeof(c->name));
    c->fs = p->fs;
    c->fds.name = "fds";
    c->uid = p->uid;
    c->ruid = p->ruid;
    c->gid = p->gid;
    c->umask = p->umask;
    memcpy(c->cwd, p->cwd, sizeof(c->cwd));
    c->ignored = p->ignored;                        /* signals: handled and ignored as here, */
    memcpy(c->handlers, p->handlers, sizeof(c->handlers));     /* none pending (POSIX) */
    memcpy(c->handler_masks, p->handler_masks, sizeof(c->handler_masks));
    memcpy(c->handler_flags, p->handler_flags, sizeof(c->handler_flags));
    c->restorer = p->restorer;
    memcpy(c->log, p->log, sizeof(c->log));
    c->tty = p->tty;
    c->no_terminal = p->no_terminal;
    c->share_next = p->share_next;
    c->stack_top = p->stack_top;
    c->bias = p->bias;
    c->image_lo = p->image_lo;
    c->image_hi = p->image_hi;
    c->entry = p->entry;
    c->user_sp = p->user_sp;
    if (c->tty)
        pty_hold_terminal(c->tty);

    /* The memory: copy-on-write (mmap_fork). */
    mmap_init(&c->mm);
    c->space = paging_user_create();
    int64_t r = c->space ? mmap_fork(&p->mm, p->space, &c->mm, c->space) : -ERR_NOMEM;
    if (r) {
        kfree(s);
        fork_undo(c);
        return r;
    }

    /* The descriptors: the same open files, a reference each, FD_CLOEXEC as
     * here. A shared buffer is not passed on (its mapping is this process's). */
    uint64_t lock = spin_lock(&p->fds);
    for (int fd = 0; fd < FD_MAX; fd++) {
        struct open_file *f = p->files[fd];
        if (!f || f->kind == FILE_SHARE || f->kind == FILE_RESERVED)
            continue;
        if (f != &console_terminal)
            __atomic_add_fetch(&f->refs, 1, __ATOMIC_SEQ_CST);
        c->files[fd] = f;
        c->cloexec[fd / 8] |= p->cloexec[fd / 8] & (uint8_t)(1u << (fd % 8));
    }
    spin_unlock(&p->fds, lock);

    /* The calling thread, under its number; the FP/SIMD registers are live
     * (the kernel never uses them): saved into its own area, then copied. */
    struct thread *self = thread_current();
    arch_fpu_save(self->fpu);
    memcpy(s->fpu, self->fpu, ARCH_FPU_STATE_SIZE);
    s->tp = self->tp;
    s->blocked = self->sig_blocked;
    s->size = size;
    memcpy(s->frame, frame, size);
    s->p = c;

    uint64_t state = spin_lock(&table);
    int slot = thread_slot(p);
    s->slot = slot < 0 ? 0 : slot;
    c->pid = next_pid++;
    c->group = p->group;
    c->session = p->session;
    c->parent = p;
    c->state = PROCESS_ALIVE;
    c->slots[s->slot] = SLOT_RUNNING;
    c->live = 1;
    c->next = all_processes;
    all_processes = c;
    spin_unlock(&table, state);
    int pid = c->pid;
    struct thread *t = thread_create_for(c->name, fork_thread, s, c, c->space);
    if (!t) {
        state = spin_lock(&table);
        unlink_process(c);
        spin_unlock(&table, state);
        kfree(s);
        fork_undo(c);
        return -ERR_NOMEM;
    }
    return pid;
}

/* getppid (56b): the parent's ID, 0 for the kernel's children and orphans. */
static int64_t sys_getppid(struct process *p)
{
    uint64_t state = spin_lock(&table);
    int ppid = p->parent && !p->orphan ? p->parent->pid : 0;
    spin_unlock(&table, state);
    return ppid;
}

/* setpgid(pid, group) (56b), POSIX's rules: pid 0 or the caller's own, or a
 * child of it in its session that has not run execve (EACCES); group 0:
 * the process's own ID (a new group), else a group of the session (EPERM);
 * a session leader stays in its group (EPERM). group -1 asks for the
 * process's group instead (getpgid; pid 0 the caller, any process). */
static int64_t sys_setpgid(struct process *p, int64_t pid, int64_t group)
{
    if (pid < 0 || group < -1)
        return -ERR_INVAL;
    uint64_t state = spin_lock(&table);
    struct process *t = 0;
    for (struct process *q = all_processes; q && !t; q = q->next)
        if (q->pid == (pid ? pid : p->pid) && q->state != PROCESS_ZOMBIE)
            t = q;
    int64_t r = 0;
    if (!t) {
        r = -ERR_SRCH;
    } else if (group == -1) {
        r = t->group;
    } else if (t != p && (t->parent != p || t->orphan)) {
        r = -ERR_SRCH;
    } else if (t != p && t->execed) {
        r = -ERR_ACCES;
    } else if (t->session != p->session || t->pid == t->session) {
        r = -ERR_PERM;
    } else {
        int want = group ? (int)group : t->pid;
        bool found = want == t->pid;
        for (struct process *q = all_processes; q && !found; q = q->next)
            found = q->group == want && q->session == p->session;
        if (!found)
            r = -ERR_PERM;
        else
            t->group = want;
    }
    spin_unlock(&table, state);
    return r;
}

/* setsid (56b): a new session and group led by the caller, without a
 * terminal; EPERM for a process that leads a group already. */
static int64_t sys_setsid(struct process *p)
{
    uint64_t state = spin_lock(&table);
    bool leads = false;
    for (struct process *q = all_processes; q && !leads; q = q->next)
        leads = q->group == p->pid;
    if (!leads) {
        p->group = p->session = p->pid;
    }
    spin_unlock(&table, state);
    if (leads)
        return -ERR_PERM;
    if (p->tty) {
        struct pty *tty = p->tty;
        p->tty = 0;
        pty_release_terminal(tty);
    }
    p->no_terminal = true;
    return p->pid;
}

/* (68j) SYS_GROW: the RitterFS on a partition grown in place (disks grow)
 * takes the new room. */
static int64_t sys_grow(struct process *p, uint64_t addr)
{
    if (p->uid != 0)
        return -ERR_PERM;
    char name[32];
    int64_t error = copy_string_from_user(p, addr, name, sizeof(name));
    if (error < 0)
        return error;
    return devices_grow(name);
}

/* (68i) SYS_UNAME: the system's names (release.c). */
static int64_t sys_uname(struct process *p, uint64_t addr)
{
    struct sys_utsname u;
    release_uname(&u);
    return copy_user(p, addr, &u, sizeof(u), true) ? 0 : -ERR_FAULT;
}

/* (68e) SYS_SESSION_STAMP: sudo's memory on the caller's session leader
 * (a live process leading the caller's session), for the caller's real
 * user. STAMP_AGE: its age in seconds, or ERR_NOENT; STAMP_SET (root:
 * sudo, setuid): now; STAMP_FORGET: gone (the caller's own). */
static int64_t sys_session_stamp(struct process *p, uint64_t op)
{
    if (op > STAMP_FORGET)
        return -ERR_INVAL;
    if (op == STAMP_SET && p->uid != 0)
        return -ERR_PERM;
    uint64_t now = arch_counter(), hz = arch_counter_hz();
    int64_t r = -ERR_NOENT;
    uint64_t state = spin_lock(&table);
    for (struct process *q = all_processes; q; q = q->next) {
        if (q->pid != p->session || q->session != q->pid || q->state != PROCESS_ALIVE)
            continue;
        if (op == STAMP_SET) {
            q->stamped = true, q->stamp_uid = p->ruid, q->stamp_at = now;
            r = 0;
        } else if (q->stamped && q->stamp_uid == p->ruid) {
            if (op == STAMP_FORGET)
                q->stamped = false, r = 0;
            else
                r = (int64_t)((now - q->stamp_at) / hz);
        } else if (op == STAMP_FORGET) {
            r = 0;                                  /* (none to forget) */
        }
        break;
    }
    spin_unlock(&table, state);
    return r;
}

/* --- users ----------------------------------------------------------------------- */

/* setuid / setgid: root may become anyone (login does); others only stay. */
static int64_t sys_setid(struct process *p, uint64_t id, bool group)
{
    if (id > 0xFFFFFFFF)
        return -ERR_INVAL;
    uint32_t *mine = group ? &p->gid : &p->uid;
    if (p->uid != 0 && id != *mine)
        return -ERR_PERM;
    *mine = (uint32_t)id;
    if (!group)
        p->ruid = (uint32_t)id;                     /* setuid sets both users (as Unix's root) */
    return 0;
}

static int64_t sys_chmod(struct process *p, uint64_t path_addr, uint64_t mode)
{
    char path[PATH_MAX];
    int64_t error = change_path(p, path_addr, path, true);
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if (error || (error = find(p, path, &fs, &id, &inode, &snapshots)))
        return error;
    if (mode & ~(uint64_t)RFS_MODE_BITS)
        return -ERR_INVAL;
    if (p->uid != 0 && p->uid != inode.uid)
        return -ERR_PERM;
    return finish_change(fs, vfs_chmod(fs, id, (unsigned)mode));
}

/* (63m) A file's times: see SYS_UTIMENS. Only the last change is kept
 * (RitterFS, FAT and exFAT alike); the access time is checked and taken. */
static int64_t sys_utimens(struct process *p, uint64_t path_addr, uint64_t fd, uint64_t times_addr,
                           uint64_t flags)
{
    if (flags & ~(uint64_t)UTIMENS_NOFOLLOW)
        return -ERR_INVAL;
    struct sys_timespec t[2] = { { 0, UTIMENS_NOW }, { 0, UTIMENS_NOW } };
    if (times_addr && !copy_user(p, times_addr, t, sizeof(t), false))
        return -ERR_FAULT;
    for (int i = 0; i < 2; i++)
        if (t[i].nsec != UTIMENS_NOW && t[i].nsec != UTIMENS_OMIT &&
            (t[i].nsec < 0 || t[i].nsec > 999999999 || t[i].sec < 0 || t[i].sec > 9000000000LL))
            return -ERR_INVAL;                      /* (before 1970 or far after: not kept) */
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    if (path_addr) {
        char path[PATH_MAX];
        bool snapshots;
        int64_t error = change_path(p, path_addr, path, !(flags & UTIMENS_NOFOLLOW));
        if (error || (error = find(p, path, &fs, &id, &inode, &snapshots)))
            return error;
    } else {
        struct open_file *f = file_get(p, fd);
        if (!f)
            return -ERR_BADF;
        bool file = f->kind == FILE_RITTERFS;
        if (file) {
            fs = f->fs, id = f->object_id;
            file = vfs_get_inode(fs, id, &inode);
        }
        file_release(f);
        if (!file)
            return -ERR_INVAL;                      /* (a terminal, pipe, socket: no times kept) */
    }
    bool now_only = (t[0].nsec == UTIMENS_NOW || t[0].nsec == UTIMENS_OMIT) &&
                    (t[1].nsec == UTIMENS_NOW || t[1].nsec == UTIMENS_OMIT);
    if (p->uid != 0 && p->uid != inode.uid) {
        if (!now_only)
            return -ERR_PERM;                       /* another time: the owner's or root's */
        if (!may(p, &inode, MAY_WRITE))
            return -ERR_ACCES;                      /* now: also whoever may write it */
    }
    if (t[1].nsec == UTIMENS_OMIT)
        return 0;                                   /* (nothing kept changes) */
    uint64_t when = t[1].nsec == UTIMENS_NOW ? uefi_clock_ns()
                  : (uint64_t)t[1].sec * 1000000000ULL + (uint64_t)t[1].nsec;
    return finish_change(fs, vfs_set_time(fs, id, when));
}

static int64_t sys_chown(struct process *p, uint64_t path_addr, uint64_t uid, uint64_t gid)
{
    char path[PATH_MAX];
    int64_t error = change_path(p, path_addr, path, true);
    struct vfs *fs;
    uint64_t id;
    struct rfs_inode inode;
    bool snapshots;
    if (error || (error = find(p, path, &fs, &id, &inode, &snapshots)))
        return error;
    if (uid > 0xFFFFFFFF || gid > 0xFFFFFFFF)
        return -ERR_INVAL;
    if (p->uid != 0)
        return -ERR_PERM;                           /* giving files away: root only */
    /* A program given to another owner loses its setuid bit (50b): it must
     * not go on running as someone it no longer belongs to. */
    unsigned mode = rfs_mode(inode.mode);
    if ((mode & RFS_MODE_SETUID) && (inode.mode & RFS_MODE_SET)) {
        int r = vfs_chmod(fs, id, mode & ~(unsigned)RFS_MODE_SETUID);
        if (r)
            return finish_change(fs, r);
    }
    return finish_change(fs, vfs_chown(fs, id, (uint32_t)uid, (uint32_t)gid));
}

static int64_t sys_sysinfo(struct process *p, uint64_t out)
{
    /* Count the page cache's unused files (22g) and the heap's cached pages
     * as free: they are given back first, so the count is exact; (65e-55)
     * ended threads' stacks too. */
    sched_reap();
    mmap_cache_drop_idle();
    heap_trim();
    struct sys_info info = {
        .ticks = timer_ticks(), .ticks_per_second = TIMER_HZ, .free_pages = pmm_free_count(),
        .usable_pages = pmm_usable_count(), .page_size = PMM_PAGE_SIZE,
    };
    return copy_user(p, out, &info, sizeof(info), true) ? 0 : -ERR_FAULT;
}

/* --- threads ------------------------------------------------------------------ */

struct thread_start {
    struct process *p;
    int slot;
    uint64_t start, sp;
    uint64_t blocked;                   /* its creator's blocked signals (52f, POSIX's) */
};

/* A new thread of a program: record itself, enter user mode at start. */
static void program_thread(void *arg)
{
    struct thread_start s = *(struct thread_start *)arg;
    kfree(arg);
    uint64_t state = spin_lock(&table);
    s.p->threads[s.slot] = thread_current();
    thread_current()->sig_blocked = s.blocked;
    spin_unlock(&table, state);
    arch_enter_user(s.start, s.sp, thread_stack_top(thread_current()));
}

static int64_t sys_thread_create(struct process *p, uint64_t start, uint64_t fn, uint64_t arg,
                                 uint64_t stack_size)
{
    if (start < USER_LOAD_MIN || start >= USER_STACK_TOP)
        return -ERR_FAULT;
    if (stack_size > USER_STACK_MAX)
        return -ERR_INVAL;
    stack_size = stack_size ? (stack_size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1)
                            : USER_STACK_SIZE;
    uint64_t state = spin_lock(&table);
    int slot = -1;
    for (int i = 1; i < PROCESS_THREADS && slot < 0 && !p->exiting; i++)
        if (p->slots[i] == SLOT_FREE)
            slot = i;
    if (slot > 0) {
        p->slots[slot] = SLOT_RUNNING;
        p->live++;
    }
    spin_unlock(&table, state);
    if (slot < 0)
        return p->exiting ? -ERR_INTR : -ERR_AGAIN;

    /* Its stack (54d: a fresh mapping of the size asked, its pages made
     * when touched; what the number's thread before left goes), the start
     * frame on top, and the kernel thread. */
    int64_t error = 0;
    uint64_t top = THREAD_STACK_TOP_AT(p->stack_top, slot), frame[2] = { fn, arg };
    mmap_unmap(&p->mm, p->space, top - USER_STACK_SLOT + PAGE_SIZE, USER_STACK_SLOT - PAGE_SIZE);
    if (mmap_map_segment(&p->mm, p->space, top - stack_size, stack_size, MMAP_READ | MMAP_WRITE,
                         0, 0) < 0)
        error = -ERR_NOMEM;
    if (!error && !copy_user(p, top - 16, frame, sizeof(frame), true))
        error = -ERR_FAULT;
    struct thread_start *s = error ? 0 : kmalloc(sizeof(*s));
    if (!error && !s)
        error = -ERR_NOMEM;
    char name[32];
    ksnprintf(name, sizeof(name), "%s:%d", p->name, slot);
    if (!error) {
        *s = (struct thread_start){ p, slot, start, top - 16, thread_current()->sig_blocked };
        if (!thread_create_for(name, program_thread, s, p, p->space)) {
            kfree(s);
            error = -ERR_NOMEM;
        }
    }
    if (error) {
        state = spin_lock(&table);
        p->slots[slot] = SLOT_FREE;
        p->live--;
        spin_unlock(&table, state);
        return error;
    }
    return slot;
}

static int64_t sys_thread_join(struct process *p, uint64_t number, uint64_t out)
{
    if (number >= PROCESS_THREADS)
        return -ERR_INVAL;
    uint64_t state = spin_lock(&table);
    bool self = thread_slot(p) == (int)number;
    uint8_t slot_state = p->slots[number];
    spin_unlock(&table, state);
    if (self)
        return -ERR_INVAL;                          /* it would wait for itself */
    if (slot_state == SLOT_FREE)
        return -ERR_SRCH;
    if (!wait_event_interruptible(&p->thread_events, p->slots[number] != SLOT_RUNNING))
        return -ERR_INTR;
    state = spin_lock(&table);
    bool ended = p->slots[number] == SLOT_ENDED;
    int64_t value = p->results[number];
    if (ended)
        p->slots[number] = SLOT_FREE;               /* the number can be used again */
    spin_unlock(&table, state);
    if (!ended)
        return -ERR_SRCH;                           /* another thread joined it */
    if (out && !copy_user(p, out, &value, sizeof(value), true))
        return -ERR_FAULT;
    return 0;
}

static int64_t sys_futex(struct process *p, uint64_t addr, uint64_t op, uint64_t value,
                         uint64_t timeout_ns)
{
    if (addr & 3)
        return -ERR_INVAL;                      /* (a word: 4-byte aligned) */
    if (op == FUTEX_WAIT)
        return futex_wait(p, addr, (uint32_t)value, timeout_ns);
    if (op == FUTEX_WAKE)
        return futex_wake(p, addr, (int64_t)value);
    return -ERR_INVAL;
}

static int64_t sys_thread_self(struct process *p)
{
    uint64_t state = spin_lock(&table);
    int slot = thread_slot(p);
    spin_unlock(&table, state);
    return slot;
}

static int64_t syscall_on(struct process *p, uint64_t number, const uint64_t args[6]);

int64_t process_syscall(uint64_t number, const uint64_t args[6])
{
    struct process *p = current_process();
    bool may_wait = number == SYS_READ || number == SYS_WRITE || number == SYS_ACCEPT ||
                    number == SYS_CONNECT || number == SYS_SENDTO || number == SYS_RECVFROM ||
                    number == SYS_ACCEPT6 || number == SYS_CONNECT6 || number == SYS_SENDTO6 ||
                    number == SYS_RECVFROM6;      /* (64c's: a non-blocking socket's too) */
    if (!may_wait || !p)
        return syscall_on(p, number, args);
    struct thread *t = thread_current();            /* (53b) */
    if (number == SYS_READ || number == SYS_WRITE) {
        /* (65e-40) The descriptor looked at once, O_NONBLOCK with it. */
        struct fd_use u;
        if (!fd_use_get(p, args[0], &u))
            return -ERR_BADF;
        t->dont_wait = u.nonblock;
        int64_t r = number == SYS_READ ? sys_read(p, &u, args[1], args[2])
                                       : sys_write(p, args[0], &u, args[1], args[2]);
        t->dont_wait = false;
        fd_use_put(&u);
        return r;
    }
    t->dont_wait = fd_nonblock(p, args[0]) ||
                   ((number == SYS_SENDTO || number == SYS_RECVFROM || number == SYS_SENDTO6 ||
                     number == SYS_RECVFROM6) && (args[5] & SOCKET_DONTWAIT));
    int64_t r = syscall_on(p, number, args);
    t->dont_wait = false;
    return r;
}

static int64_t syscall_on(struct process *p, uint64_t number, const uint64_t args[6])
{
    switch (number) {
    case SYS_EXIT:
        process_exit(STATUS_EXITED((int)args[0]));
    case SYS_WRITE:                                 /* (65e-40: in process_syscall) */
    case SYS_READ:
        return -ERR_BADF;
    case SYS_OPEN:
        return sys_open(p, args[0], args[1], args[2]);
    case SYS_CLOSE:
        return sys_close(p, args[0]);
    case SYS_SPAWN:
        return sys_spawn(p, args[0], args[1], args[2], args[3], args[4], 0);
    case SYS_SPAWN_ENV:                             /* (path, argv, envp, flags, fds, ids) */
        return sys_spawn(p, args[0], args[1], args[3], args[4], args[5], args[2]);
    case SYS_WAIT:
        return sys_wait(p, (int64_t)args[0], args[1], args[2]);
    case SYS_KILL:
        if ((int64_t)args[1] <= 0 || args[1] > 64)
            return -ERR_INVAL;
        return process_signal((int)args[0], (int)args[1], p->uid);
    case SYS_GETPID:
        return p->pid;
    case SYS_SLEEP:
        return sys_sleep(args[0]);
    case SYS_STAT:
        return sys_stat(p, args[0], args[1], true);
    case SYS_LSTAT:
        return sys_stat(p, args[0], args[1], false);
    case SYS_SYMLINK:
        return sys_symlink(p, args[0], args[1]);
    case SYS_READLINK:
        return sys_readlink(p, args[0], args[1], args[2]);
    case SYS_LINK:
        return sys_link(p, args[0], args[1]);
    case SYS_READDIR:
        return sys_readdir(p, args[0], args[1]);
    case SYS_CHDIR:
        return sys_chdir(p, args[0]);
    case SYS_GETCWD:
        return sys_getcwd(p, args[0], args[1]);
    case SYS_GETPGRP:
        return p->group;
    case SYS_GETPPID:
        return sys_getppid(p);
    case SYS_SETPGID:
        return sys_setpgid(p, (int64_t)args[0], (int64_t)args[1]);
    case SYS_SETSID:
        return sys_setsid(p);
    case SYS_UTIMENS:
        return sys_utimens(p, args[0], args[1], args[2], args[3]);
    case SYS_FOREGROUND:
        return sys_foreground(p, (int64_t)args[0]);
    case SYS_SIGNAL:
        return sys_signal(p, args[0], args[1]);
    case SYS_SYSINFO:
        return sys_sysinfo(p, args[0]);
    case SYS_REPORT:
        return sys_report(p, args[0], args[1], args[2]);
    case SYS_SEEK:
        return sys_seek(p, args[0], (int64_t)args[1], args[2]);
    case SYS_THREAD_CREATE:
        return sys_thread_create(p, args[0], args[1], args[2], args[3]);
    case SYS_THREAD_EXIT:
        leave_thread(p, (int64_t)args[0]);
    case SYS_THREAD_JOIN:
        return sys_thread_join(p, args[0], args[1]);
    case SYS_FUTEX:
        return sys_futex(p, args[0], args[1], args[2], args[3]);
    case SYS_UMASK: {                               /* (52h) the old mask */
        uint32_t old = p->umask;
        p->umask = (uint32_t)args[0] & 0777;
        return old;
    }
    case SYS_FCNTL:
        return sys_fcntl(p, args[0], args[1], args[2]);
    case SYS_SHUTDOWN:
        return sys_shutdown(p, args[0], args[1]);
    case SYS_SOCKNAME:
        return sys_sockname(p, args[0], args[1], args[2]);
    case SYS_SOCKOPT:
        return sys_sockopt(p, args[0], args[1], args[2], args[3]);
    case SYS_THREAD_INTERRUPT:
        return sys_thread_interrupt(p, args[0], args[1], args[2]);
    case SYS_EXECVE:
        return sys_execve(p, args[0], args[1], args[2]);
    case SYS_SIGPENDING:
        return sys_sigpending(p, args[0]);
    case SYS_SIGSUSPEND:
        return sys_sigsuspend(p, args[0]);
    case SYS_SIGPROCMASK:
        return sys_sigprocmask(p, args[0], args[1], args[2]);
    case SYS_TERMIOS:
        return sys_termios(p, args[0], args[1], args[2]);
    case SYS_POLL:
        return sys_poll(p, args[0], args[1], (int64_t)args[2]);
    case SYS_FSTAT:
        return sys_fstat(p, args[0], args[1]);
    case SYS_DUP2:
        return sys_dup2(p, args[0], (int64_t)args[1]);
    case SYS_THREAD_SELF:
        return sys_thread_self(p);
    case SYS_SNAPSHOT:
        return sys_snapshot(p, args[0], args[1], args[2], args[3]);
    case SYS_ATTR:
        return sys_attr(p, args[0], args[1], args[2], args[3], args[4]);
    case SYS_MKDIR:
        return sys_mkdir(p, args[0], args[1]);
    case SYS_RMDIR:
        return sys_remove(p, args[0], true);
    case SYS_UNLINK:
        return sys_remove(p, args[0], false);
    case SYS_RENAME:
        return sys_rename(p, args[0], args[1]);
    case SYS_COPY:
        return sys_copy(p, args[0], args[1]);
    case SYS_RANDOM: {
        uint8_t bytes[256];
        if (args[1] > sizeof(bytes))
            return -ERR_INVAL;
        random_bytes(bytes, args[1]);
        bool ok = copy_user(p, args[0], bytes, args[1], true);
        memset(bytes, 0, sizeof(bytes));
        return ok ? (int64_t)args[1] : -ERR_FAULT;
    }
    case SYS_ECHO:
        if (p->group != foreground_of(p))
            return -ERR_PERM;
        if (p->tty)
            pty_set_echo(p->tty, args[0] != 0, p->pid);
        else
            terminal_set_echo(args[0] != 0, p->pid);
        return 0;
    case SYS_PTY:
        return sys_pty(p, args[0], args[1], args[2], args[3]);
    case SYS_PIPE:
        return sys_pipe(p, args[0]);
    case SYS_SHARE:
        return sys_share(p, args[0], args[1], args[2]);
    case SYS_MMAP:
        return sys_mmap(p, args[0], args[1], args[2], args[3], args[4], args[5]);
    case SYS_MUNMAP:
        return mmap_unmap(&p->mm, p->space, args[0], args[1]);
    case SYS_MPROTECT:
        return mmap_protect(&p->mm, p->space, args[0], args[1], (unsigned)args[2]);
    case SYS_MSYNC:
        if (args[2] & ~(uint64_t)(MSYNC_ASYNC | MSYNC_INVALIDATE | MSYNC_SYNC))
            return -ERR_INVAL;
        return mmap_sync(&p->mm, args[0], args[1]);
    case SYS_THREAD_POINTER:
        if (args[0] >= USER_TOP)
            return -ERR_INVAL;                      /* (x86: a non-canonical FS base faults) */
        arch_set_thread_pointer(args[0]);           /* (first: a switch in between saves it) */
        thread_current()->tp = args[0];
        return 0;
    case SYS_DISPLAY:
        return sys_display(p, args[0], args[1], args[2]);
    case SYS_TIME:
        return (int64_t)uefi_clock_ns();
    case SYS_NETSET:
        return sys_netset(p, args[0]);
    case SYS_NETSET6:
        return sys_netset6(p, args[0]);
    case SYS_FORWARD:
        return sys_forward(p, args[0]);
    case SYS_MASQUERADE:
        return sys_masquerade(p, args[0], args[1]);
    case SYS_DEFAULT_ROUTE:
        return sys_default_route(p, args[0], args[1]);
    case SYS_WLAN:
        return sys_wlan(p, args[0], args[1], args[2], args[3], args[4]);
    case SYS_NETINFO:
        return sys_netinfo(p, args[0]);
    case SYS_NETINFO6:
        return sys_netinfo6(p, args[0]);
    case SYS_NETLEASE6:
        return sys_netlease6(p, args[0]);
    case SYS_SWAP:
        return sys_swap(p, args[0], args[1]);
    case SYS_TAP:
        return sys_tap(p, args[0], args[1]);
    case SYS_BRIDGE:
        return sys_bridge(p, args[0], (int64_t)args[1], args[2]);
    case SYS_SESSION_STAMP:
        return sys_session_stamp(p, args[0]);
    case SYS_UNAME:
        return sys_uname(p, args[0]);
    case SYS_GROW:
        return sys_grow(p, args[0]);
    case SYS_GUARD:
        return sys_guard(p, args[0], args[1]);
    case SYS_LOG:
        return sys_log(p, args[0], args[1], args[2]);
    case SYS_EFIVAR:
        return sys_efivar(p, args[0], args[1]);
    case SYS_BINDCARD: {
        struct socket *s = socket_grab(p, args[0]);  /* held across the call (49b) */
        if (!s)
            return -ERR_BADF;
        int64_t result;
        if (p->uid != 0) {
            result = -ERR_PERM;                       /* broadcasts from 0.0.0.0 */
        } else {
            char name[8];
            if (!copy_user(p, args[1], name, sizeof(name), false)) {
                result = -ERR_FAULT;
            } else {
                name[sizeof(name) - 1] = 0;
                struct netdev *d = card_named(name);
                int r = d ? socket_bind_card(s, d) : ERR_NOENT;
                result = r ? -r : 0;
            }
        }
        socket_close(s);
        return result;
    }
    case SYS_SOCKET: {
        struct socket *s = socket_new((int)args[0]);
        return s ? socket_fd(p, s) : -ERR_INVAL;
    }
    case SYS_BIND: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int64_t result;
        if (socket_type(s) == SOCKET_LOCAL) {         /* bind(fd, name, length) */
            char name[LOCAL_PATH_MAX];
            int r = copy_local_name(p, args[1], args[2], name);
            result = r ? -r : (r = socket_bind_local(s, name, p->uid)) ? -r : 0;
        } else if (args[1] && args[1] < 1024 && p->uid != 0) {
            result = -ERR_ACCES;                      /* ports below 1024: root's (49a) */
        } else {
            int r = socket_bind(s, (uint16_t)args[1]);
            result = r ? -r : socket_port(s);
        }
        socket_close(s);
        return result;
    }
    case SYS_CONNECT: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int r;
        if (socket_type(s) == SOCKET_LOCAL) {         /* connect(fd, name, length) */
            char name[LOCAL_PATH_MAX];
            r = copy_local_name(p, args[1], args[2], name);
            if (!r)
                r = socket_connect_local(s, name, p->uid);
        } else {
            struct inet_addr to = inet_v4((uint32_t)args[1]);
            r = socket_connect(s, &to, (uint16_t)args[2]);
        }
        socket_close(s);
        return r ? -r : 0;
    }
    case SYS_SENDTO: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        struct inet_addr to = inet_v4((uint32_t)args[3]);
        int64_t n = socket_write(p, s, args[1], args[2], args[3] || args[4] ? &to : 0,
                                 (uint16_t)args[4]);
        socket_close(s);
        return n;
    }
    case SYS_RECVFROM: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int64_t n = socket_read(p, s, args[1], args[2], args[3], (uint32_t)args[4], false);
        socket_close(s);
        return n;
    }
    case SYS_LISTEN: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int r = socket_listen(s, (int)args[1]);
        socket_close(s);
        return r ? -r : 0;
    }
    case SYS_PEER_UID: {                              /* the user at a local socket's other end */
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int uid = socket_peer_uid(s);
        socket_close(s);
        return uid < 0 ? -ERR_NOTCONN : uid;
    }
    case SYS_ACCEPT: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int error;
        struct socket *c = socket_accept(s, &error);
        socket_close(s);                              /* the listener: done with it */
        if (!c)
            return -error;
        if (args[1]) {
            uint16_t port;
            struct inet_addr peer = socket_peer(c, &port);
            uint64_t from = (uint64_t)inet_v4_of(&peer) << 16 | port;
            if (!copy_user(p, args[1], &from, sizeof(from), true)) {
                socket_close(c);
                return -ERR_FAULT;
            }
        }
        return socket_fd(p, c);
    }
    case SYS_CONNECT6: {                              /* (64c) */
        struct inet_addr to;
        uint16_t port;
        int64_t e = inaddr_from_user(p, args[1], &to, &port);
        if (e)
            return e;
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int r = socket_type(s) == SOCKET_LOCAL ? ERR_AFNOSUPPORT : socket_connect(s, &to, port);
        socket_close(s);
        return r ? -r : 0;
    }
    case SYS_SENDTO6: {
        struct inet_addr to;
        uint16_t port = 0;
        if (args[3]) {
            int64_t e = inaddr_from_user(p, args[3], &to, &port);
            if (e)
                return e;
        }
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int64_t n = socket_write(p, s, args[1], args[2], args[3] ? &to : 0, port);
        socket_close(s);
        return n;
    }
    case SYS_RECVFROM6: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int64_t n = socket_read(p, s, args[1], args[2], args[3], (uint32_t)args[4], true);
        socket_close(s);
        return n;
    }
    case SYS_ACCEPT6: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return -ERR_BADF;
        int error;
        struct socket *c = socket_accept(s, &error);
        socket_close(s);
        if (!c)
            return -error;
        if (args[1]) {
            uint16_t port;
            struct inet_addr peer = socket_peer(c, &port);
            if (!inaddr_to_user(p, args[1], &peer, port, socket_six(c))) {
                socket_close(c);
                return -ERR_FAULT;
            }
        }
        return socket_fd(p, c);
    }
    case SYS_SOCKNAME6: {
        struct socket *s = socket_grab(p, args[0]);
        if (!s)
            return file_get_kind_error(p, args[0]);
        struct inet_addr a;
        uint16_t port;
        int r = args[1] > SOCKNAME_PEER ? ERR_INVAL : socket_type(s) == SOCKET_LOCAL ? ERR_AFNOSUPPORT
              : socket_name6(s, args[1] == SOCKNAME_PEER, &a, &port);
        bool six = socket_six(s);
        socket_close(s);
        if (r)
            return -r;
        return inaddr_to_user(p, args[2], &a, port, six) ? 0 : -ERR_FAULT;
    }
    case SYS_PING:
        return icmp_ping((uint32_t)args[0], (uint16_t)(args[1] >> 16), (uint16_t)args[1],
                         (size_t)args[2], (uint32_t)args[3]);
    case SYS_PING6: {
        struct sys_inaddr to;
        if (!copy_user(p, args[0], &to, sizeof(to), false))
            return -ERR_FAULT;
        static const uint8_t mapped[12] = { [10] = 0xFF, [11] = 0xFF };
        if (to.family == 2 || (to.family == 10 && !memcmp(to.address, mapped, 12)))
            return icmp_ping((uint32_t)to.address[12] << 24 | (uint32_t)to.address[13] << 16 |
                             (uint32_t)to.address[14] << 8 | to.address[15],
                             (uint16_t)(args[1] >> 16), (uint16_t)args[1], (size_t)args[2],
                             (uint32_t)args[3]);
        if (to.family != 10)
            return -ERR_AFNOSUPPORT;
        struct netdev *card = 0;
        if (to.scope && !(card = net_at((int)to.scope - 1)))
            return -ERR_NODEV;
        return icmp6_ping(to.address, card, (uint16_t)(args[1] >> 16), (uint16_t)args[1],
                          (size_t)args[2], (uint32_t)args[3]);
    }
    case SYS_MOUNT:
        return sys_mount(p, args[0], args[1], args[2], args[3]);
    case SYS_UNMOUNT:
        return sys_unmount(p, args[0]);
    case SYS_DEVICE: {
        struct sys_device *d = kmalloc(sizeof(*d));
        if (!d)
            return -ERR_NOMEM;
        bool found = devices_info((int)args[0], d);
        bool ok = !found || copy_user(p, args[1], d, sizeof(*d), true);
        kfree(d);
        return !ok ? -ERR_FAULT : found ? 1 : 0;
    }
    case SYS_TERMINAL:
        if (args[0] == TERMINAL_ISATTY) {
            uint64_t s = spin_lock(&p->fds);
            struct open_file *f = file(p, args[1]);
            bool tty = f && (f->kind == FILE_TERMINAL || f->kind == FILE_PTY);
            spin_unlock(&p->fds, s);
            return tty ? 1 : 0;
        }
        if (args[0] == TERMINAL_SIZE) {
            uint32_t rows, cols;
            if (p->tty)
                pty_size(p->tty, &rows, &cols);
            else if (!console_size(&rows, &cols))
                rows = 24, cols = 80;
            return (int64_t)rows << 16 | cols;
        }
        if (args[0] != TERMINAL_RAW)
            return -ERR_INVAL;
        if (p->group != foreground_of(p))
            return -ERR_PERM;
        if (p->tty)
            pty_set_raw(p->tty, args[1] != 0, p->pid);
        else
            terminal_set_raw(args[1] != 0, args[1] == 2, p->pid);
        return 0;
    case SYS_GETUID:
        return p->ruid;                             /* the real user (50b) */
    case SYS_GETEUID:
        return p->uid;                              /* the one whose rights apply */
    case SYS_CPUTIME:
        return (int64_t)process_cpu_ns(p);
    case SYS_SIGACTION:
        return sys_sigaction(p, args[0], args[1], args[2], args[3]);
    case SYS_GETGID:
        return p->gid;
    case SYS_SETUID:
        return sys_setid(p, args[0], false);
    case SYS_SETGID:
        return sys_setid(p, args[0], true);
    case SYS_CHMOD:
        return sys_chmod(p, args[0], args[1]);
    case SYS_CHOWN:
        return sys_chown(p, args[0], args[1], args[2]);
    case SYS_POWER:
        if (args[0] == POWER_TABLE)
            return sys_power_table(p, args[1], args[2], args[3]);
        if (args[0] == POWER_NAMES)
            return sys_power_names(p, args[1], args[2]);
        if (args[0] == POWER_EVAL)
            return sys_power_eval(p, args[1], args[2], args[3]);
        if (p->uid != 0)
            return -ERR_PERM;
        if (args[0] == POWER_SLEEP) {               /* (37g-4; 37g-7: 0 until woken) */
            if (args[1] > 86399)
                return -ERR_INVAL;
            return sleep_for((unsigned)args[1]) == 0 ? 0 : -ERR_IO;
        }
        if (args[0] == POWER_OFF)
            arch_power_off();
        else if (args[0] == POWER_REBOOT)
            arch_reboot();
        else
            return -ERR_INVAL;
        return -ERR_IO;                             /* it did not work */
    default:
        return -ERR_NOSYS;
    }
}
