/*
 * Input arrives byte by byte from the interrupt handlers of the serial port
 * and the keyboards into a ring buffer; the terminal reads it with interrupts briefly disabled and sleeps
 * while it is empty. Lines are edited as on a simple terminal: printable
 * text is echoed, Backspace (DEL or BS) erases, Ctrl-C cancels the line,
 * Ctrl-U clears it, Ctrl-L clears the screen and shows the prompt (what
 * was printed since the last line break) and the line again at its top,
 * Enter (CR or LF) ends it. UTF-8 characters are taken
 * whole (echoed when complete, erased whole by Backspace; malformed bytes
 * are dropped). Escape sequences (e.g. the
 * arrow keys: ESC [ A) are ignored. With echo off (passwords), nothing
 * typed is shown, not even the line break. In raw mode (full-screen
 * programs) the bytes go to the program as they are.
 *
 * The line on the screen belongs to the foreground program (or the rescue
 * console): its prompt and what was typed after it. Text from anything
 * else (the kernel, an elf, a job in the background) is a message: it
 * starts on a line of its own and never becomes part of the line, which
 * is shown again below it: when the owner reads, once nothing more has
 * come for 50 ms (or at a key), and at once when the owner prints more.
 * The line may be away before its read begins (a message right after the
 * prompt: login's at the boot, among the elves' first lines).
 */
#include "blank.h"
#include "terminal.h"
#include "arch.h"
#include "console.h"
#include "heap.h"
#include "kprintf.h"
#include "mmap.h"
#include "mouse.h"
#include "pci.h"
#include "process.h"
#include "sync.h"
#include "report.h"
#include "syscalls.h"
#include "termios.h"
#include "usb.h"
#include "pmm.h"
#include "ritterfs.h"
#include "serial.h"
#include "string.h"
#include "timer.h"
#include "sched.h"

#define INPUT_SIZE  256             /* a power of two */
#define LINE_MAX    160
#define PROMPT      "rescue> "
#define LOGIN       "/system/" ARCH_NAME "/bin/login"   /* it starts the users' shells */
#define MAX_ARGS    32              /* words passed to a program */
#define QUIET_MS    50              /* after a message, before the prompt comes again */

static volatile uint8_t input[INPUT_SIZE];
static volatile uint32_t input_head, input_tail;   /* written by the handler / the reader */
/* (65a-4) The ring's writers (the serial port's interrupt, the keyboards:
 * on several cores at once) and its readers. Until 65a-4 only interrupt
 * handlers wrote, which never ran at the same time on the one core. */
static struct spinlock input_lock = SPINLOCK_INIT("terminal input");
static volatile uint64_t input_dropped;
static struct wait_queue input_waiters = { .pollable = true };    /* threads waiting for input */
static volatile bool isig = true;   /* Ctrl-C is a signal (52e: ISIG) */
static struct ritterfs *filesystem;
#define MAX_INPUTS 10       /* serial0 and kbd0-kbd7 */
static const char *inputs[MAX_INPUTS] = { "serial0" };
static int input_count = 1;

bool terminal_has_input(void)
{
    return input_count > 0;
}

void terminal_add_input(const char *name)
{
    for (int i = 0; i < input_count; i++)
        if (strcmp(inputs[i], name) == 0)
            return;                         /* back after being unplugged */
    if (input_count < MAX_INPUTS)
        inputs[input_count++] = name;
}

void terminal_remove_input(const char *name)
{
    for (int i = 0; i < input_count; i++) {
        if (strcmp(inputs[i], name) == 0) {
            inputs[i] = inputs[--input_count];
            return;
        }
    }
}

void terminal_set_filesystem(struct ritterfs *fs)
{
    filesystem = fs;
}

/* What the line's owner printed since the line began (a line break, or a
 * carriage return: the shell's line editor redraws its line after one):
 * the prompt, for Ctrl-L and for showing the line again after a message.
 * A program editing its own line in raw mode (TERMINAL_RAW 2, the shell)
 * has its echo and cursor moves in it too: shown again, they give the
 * same line. */
static char line_so_far[512];
static size_t line_so_far_length;
static struct thread *console_thread;       /* the rescue console's */
static struct thread *volatile line_reader; /* reading the line now, if any */
static volatile int foreground;              /* process group of the foreground job */
static volatile int foreground_session;      /* (65e) the session that set it; 0: the kernel */

/* Is the current thread the line's owner: its reader, else a thread of
 * the foreground job, else (no job) the rescue console? */
static bool owns_line(void)
{
    struct thread *t = thread_current();
    if (line_reader)
        return t == line_reader;
    if (t->process)
        return process_group(t->process) == foreground;
    return !foreground && t == console_thread;
}

void terminal_wrote(const char *s, size_t n)
{
    if (!owns_line())
        return;                                     /* a message: not part of the line */
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n' || s[i] == '\r') {
            line_so_far_length = 0;
        } else if (line_so_far_length + 1 < sizeof(line_so_far)) {
            line_so_far[line_so_far_length++] = s[i];
        }
    }
    line_so_far[line_so_far_length] = 0;
}

/* Interrupt context (or a USB event taken by a waiting thread): no console,
 * no heap. The ring is input_lock's. */
void terminal_receive(uint8_t byte)
{
    blank_activity();                       /* (37e) wakes the screen, never dropped */
    /* Ctrl-C stops a foreground job that takes the signal; the byte goes
     * nowhere. If the foreground ignores it (the shell at its prompt), it is
     * input like any other byte: as a signal it was lost when it came
     * before the shell started reading (typed right after the prompt). */
    if (byte == 0x03 && isig && foreground && process_signal_group(foreground, SIGINT)) {
        /* stopped */
    } else {
        uint64_t state = spin_lock(&input_lock);
        bool full = input_head - input_tail == INPUT_SIZE;
        if (full) {
            input_dropped++;
        } else {
            input[input_head % INPUT_SIZE] = byte;
            input_head++;
        }
        spin_unlock(&input_lock, state);
        if (full)
            return;
    }
    wait_queue_wake_all(&input_waiters);
}

bool terminal_input_waiting(void)
{
    return input_head != input_tail;
}

bool terminal_poll(void)
{
    poll_watch(&input_waiters);             /* (65e) a poller watches the input */
    return input_head != input_tail;
}

int terminal_foreground(void)
{
    return foreground;
}

void terminal_set_foreground(int group, int session)
{
    foreground = group;
    foreground_session = session;
}

int terminal_foreground_session(void)
{
    return foreground_session;
}

/* The line being read: the text typed so far (shown: the bytes echoed,
 * without an unfinished UTF-8 character). */
static const char *line_text;
static size_t line_shown;
static volatile bool line_away;     /* a message took the line's place */
static volatile bool message_ended_line;
static volatile uint64_t message_time;  /* timer ticks */
static volatile bool echo = true;

/* The line again, below the messages (the kprintf lock held). ESC [ 0 K
 * (erase to the line's end, a no-op at its start) marks it as shown
 * again, for the tests' prompt counts. Written directly: kprintf would
 * come back here. */
static void show_line(void)
{
    static char again[8 + sizeof(line_so_far) + LINE_MAX];
    ksnprintf(again, sizeof(again), "%s\033[0K%s%.*s", message_ended_line ? "" : "\n",
              line_so_far, line_reader && echo ? (int)line_shown : 0, line_reader ? line_text : "");
    serial_write(again);
    console_write(again);
    line_away = false;
}

void terminal_message(const char *text)
{
    if (!*text)
        return;
    if (owns_line()) {
        /* The owner prints more (a reader shows its line first itself:
         * read_byte). */
        if (line_away && !line_reader)
            show_line();
        return;
    }
    if (!line_away && (line_so_far_length || (line_reader && line_shown))) {
        serial_write("\n");                        /* the message below the line */
        console_write("\n");
        line_away = true;
    }
    if (line_away) {
        size_t n = strlen(text);
        message_ended_line = text[n - 1] == '\n';
        message_time = timer_ticks();
        wait_queue_wake_all(&input_waiters);
    }
}

/* After a message, for the reader: its line again, once it is quiet (or a
 * key is pressed). */
static void show_line_again(void)
{
    uint64_t quiet = (QUIET_MS * TIMER_HZ + 999) / 1000;
    while (line_away && input_head == input_tail && timer_ticks() - message_time < quiet)
        timer_sleep_ms(10);
    bool locked = kprintf_lock();
    if (line_away)
        show_line();
    kprintf_unlock(locked);
}

/* The next byte typed, or -1 if the reading program is being stopped.
 * The thread sleeps until there is one. */
static int read_byte(void)
{
    for (;;) {
        bool mine = line_reader == thread_current();
        if (!wait_event_interruptible(&input_waiters,
                                      input_head != input_tail || (mine && line_away)))
            return -1;
        if (!mine || !line_away)
            break;
        show_line_again();
    }
    uint64_t state = spin_lock(&input_lock);
    int byte = -1;
    if (input_head != input_tail)
        byte = input[input_tail++ % INPUT_SIZE];
    spin_unlock(&input_lock, state);
    return byte >= 0 ? byte : read_byte();  /* (another reader took it meanwhile) */
}

bool terminal_init(void)
{
    if (arch_serial_input_init(terminal_receive))
        return true;
    input_count = 0;                /* no serial0 */
    return false;
}

static volatile int echo_off_by;    /* the process that switched it off */

void terminal_set_echo(bool on, int pid)
{
    echo = on;
    echo_off_by = on ? 0 : pid;
}

static volatile bool raw, raw_line;
static volatile int raw_by;         /* the process that switched it on */

/* (52e) The modes a program set (ICANON, ECHO and ISIG are the live ones
 * above), and the process that set them. */
static struct sys_termios mode;
static bool mode_made;
static volatile int mode_by;

static struct sys_termios *modes(void)
{
    if (!mode_made) {
        termios_default(&mode);
        mode_made = true;
    }
    return &mode;
}

void terminal_set_raw(bool on, bool line_editor, int pid)
{
    raw = on;
    raw_line = on && line_editor;
    raw_by = on ? pid : 0;
    struct sys_termios *m = modes();        /* (the old switch: one byte or more, CR as typed) */
    m->cc[TERMIOS_VMIN] = 1;
    m->cc[TERMIOS_VTIME] = 0;
    m->iflag &= ~(uint32_t)TERMIOS_ICRNL;
}

void terminal_termios_get(struct sys_termios *out)
{
    *out = *modes();
    out->lflag &= ~(uint32_t)(TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ISIG);
    out->lflag |= (raw ? 0 : TERMIOS_ICANON) | (echo ? TERMIOS_ECHO : 0) | (isig ? TERMIOS_ISIG : 0);
}

void terminal_termios_set(const struct sys_termios *in, int pid, bool flush)
{
    *modes() = *in;
    bool lines = in->lflag & TERMIOS_ICANON;
    raw = !lines;
    raw_line = false;
    raw_by = lines ? 0 : pid;
    echo = in->lflag & TERMIOS_ECHO;
    echo_off_by = echo ? 0 : pid;
    isig = in->lflag & TERMIOS_ISIG;
    mode_by = pid;
    if (flush)
        terminal_flush();
}

void terminal_flush(void)
{
    uint64_t state = spin_lock(&input_lock);
    input_tail = input_head;
    spin_unlock(&input_lock, state);
}

bool terminal_is_raw(void)
{
    return raw;
}

void terminal_process_ended(int pid)
{
    if (!echo && echo_off_by == pid)
        terminal_set_echo(true, 0);                 /* never leave it off */
    if (mode_by == pid && pid) {                    /* (52e) everything it set, back */
        termios_default(modes());
        isig = true;
        mode_by = 0;
    }
    if (raw && raw_by == pid) {
        terminal_set_raw(false, false, 0);          /* lines again */
        bool locked = kprintf_lock();
        console_reset_style();                      /* in case it left reverse on */
        kprintf_unlock(locked);
    }
}

/* Typed bytes into buf from n on, Enter as \n with ICRNL; the new n. */
static size_t take_input(uint8_t *buf, size_t n, size_t size)
{
    bool crnl = !raw_line && (modes()->iflag & TERMIOS_ICRNL);   /* (the shell: as typed) */
    uint64_t state = spin_lock(&input_lock);
    while (n < size && input_head != input_tail) {
        uint8_t c = input[input_tail % INPUT_SIZE];
        input_tail++;
        buf[n++] = crnl && c == '\r' ? '\n' : c;
    }
    spin_unlock(&input_lock, state);
    return n;
}

/* (52e) A raw read by VMIN and VTIME, as POSIX's: VMIN 0 takes what is
 * there (after up to VTIME tenths for a first byte); else it waits for a
 * first byte, then until VMIN have come or, with VTIME, no byte came for
 * that long. */
static int read_timed(uint8_t *buf, size_t size, unsigned vmin, unsigned vtime)
{
    uint64_t tenth = (TIMER_HZ + 9) / 10;
    struct thread *self = thread_current();
    if (vmin == 0) {
        if (vtime)
            wait_event_until(&input_waiters, input_head != input_tail || self->stop_requested,
                             timer_ticks() + vtime * tenth);
        if (self->stop_requested)
            return TERMINAL_INTERRUPTED;
        return (int)take_input(buf, 0, size);
    }
    size_t want = vmin < size ? vmin : size, n = 0;
    for (;;) {
        n = take_input(buf, n, size);
        if (n >= want)
            return (int)n;
        if (n > 0 && vtime) {
            if (!wait_event_until(&input_waiters, input_head != input_tail || self->stop_requested,
                                  timer_ticks() + vtime * tenth))
                return (int)n;                      /* quiet for VTIME: what came */
        } else if (!wait_event_interruptible(&input_waiters, input_head != input_tail)) {
            return n ? (int)n : TERMINAL_INTERRUPTED;
        }
        if (self->stop_requested)
            return n ? (int)n : TERMINAL_INTERRUPTED;
    }
}

int terminal_read_raw(uint8_t *buf, size_t size)
{
    size_t n = 0;
    bool locked = false;
    struct sys_termios *m = modes();
    if (!raw_line && (m->cc[TERMIOS_VMIN] != 1 || m->cc[TERMIOS_VTIME] != 0))
        return read_timed(buf, size, m->cc[TERMIOS_VMIN], m->cc[TERMIOS_VTIME]);
    if (raw_line) {
        /* A line editor's read: its line is shown again after a message,
         * like the kernel's own (what it printed is in line_so_far). */
        locked = kprintf_lock();
        line_shown = 0;
        line_text = "";
        line_reader = thread_current();
        kprintf_unlock(locked);
    }
    int c = read_byte();
    if (raw_line || line_reader == thread_current()) {
        locked = kprintf_lock();
        line_reader = 0;
        kprintf_unlock(locked);
    }
    if (c < 0)
        return TERMINAL_INTERRUPTED;
    buf[n++] = c == '\r' && !raw_line && (m->iflag & TERMIOS_ICRNL) ? '\n' : (uint8_t)c;
    return (int)take_input(buf, n, size);
}

/* Show typed text, unless echo is off. */
#define ECHO(...)                                                              \
    do {                                                                       \
        if (echo)                                                              \
            kprintf(__VA_ARGS__);                                              \
    } while (0)

/* Erase the line's last character (a UTF-8 one whole); true if there was one. */
static bool erase_last(char *buf, size_t *len)
{
    if (*len == 0)
        return false;
    do
        (*len)--;
    while (*len > 0 && ((uint8_t)buf[*len] & 0xC0) == 0x80);
    return true;
}

static int read_line(char *buf, size_t size, bool *newline)
{
    size_t len = 0;
    int previous = 0;
    size_t pending = 0;                 /* continuation bytes a UTF-8 character still needs */
    size_t start = 0;                   /* where that character began */
    *newline = false;
    for (;;) {
        line_shown = pending ? start : len;
        int c = read_byte();
        if (c < 0)
            return TERMINAL_INTERRUPTED;
        int before = previous;
        previous = c;
        if (c == '\n' && before == '\r')
            continue;                           /* CR LF counts as one Enter */
        if (c == '\r' || c == '\n') {
            ECHO("\n");
            if (echo)
                terminal_wrote("\n", 1);        /* a new line: the prompt is behind it */
            *newline = true;
            break;
        }
        if (c == 0x1B) {                        /* ESC: skip an "ESC [ ... final" sequence */
            int b = read_byte();
            while (b == '[' || (b >= 0 && b < 0x40))
                b = read_byte();
            if (b < 0)
                return TERMINAL_INTERRUPTED;
            continue;
        }
        if (c == 0x04 && thread_current()->process) {  /* Ctrl-D: end of input, or of this line */
            if (len == 0)
                return TERMINAL_EOF;
            break;
        }
        if (c >= 0x80) {
            /* UTF-8: a lead byte, then 1-3 continuation bytes. */
            if ((c & 0xC0) == 0x80) {
                if (pending == 0)
                    continue;                   /* a stray continuation byte */
                buf[len++] = (char)c;
                if (--pending == 0) {
                    buf[len] = 0;
                    ECHO("%s", buf + start);    /* the whole character at once */
                }
                continue;
            }
            if (pending)                        /* an unfinished one: dropped */
                len = start, pending = 0;
            size_t need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2
                        : (c & 0xF8) == 0xF0 ? 3 : 0;
            if (need == 0 || len + need + 1 >= size)
                continue;
            start = len;
            buf[len++] = (char)c;
            pending = need;
            continue;
        }
        if (pending)
            len = start, pending = 0;           /* an unfinished character: dropped */
        if (c == 0x7F || c == '\b') {
            if (erase_last(buf, &len))
                ECHO("\b \b");
        } else if (c == 0x03) {                 /* Ctrl-C */
            if (thread_current()->process)
                return TERMINAL_INTERRUPTED;    /* the program says "^C" itself */
            kprintf("^C\n");
            len = 0;
            break;
        } else if (c == 0x0C) {                 /* Ctrl-L: the prompt and the line at the top */
            buf[len] = 0;
            kprintf("\033[2J\033[H%s%s", line_so_far, echo ? buf : "");
        } else if (c == 0x15) {                 /* Ctrl-U */
            while (erase_last(buf, &len))
                ECHO("\b \b");
        } else if (c >= 0x20 && c < 0x7F && len + 1 < size) {
            buf[len++] = (char)c;
            ECHO("%c", c);
        }
    }
    buf[len] = 0;
    return (int)len;
}

int terminal_read_line(char *buf, size_t size, bool *newline)
{
    bool locked = kprintf_lock();                   /* (messages wait meanwhile) */
    line_shown = 0;
    line_text = buf;
    line_reader = thread_current();                 /* (a line already away stays away) */
    kprintf_unlock(locked);
    thread_current()->log_quiet++;                  /* the echo of what is typed: no log */
    int r = read_line(buf, size, newline);
    thread_current()->log_quiet--;
    locked = kprintf_lock();
    line_reader = 0;
    if (!line_so_far_length)
        line_away = false;                          /* nothing left to show again */
    kprintf_unlock(locked);
    return r;
}

/* --- commands ------------------------------------------------------------ */

struct command {
    const char *name;
    const char *help;
    void (*run)(const char *args);
};

static void cmd_help(const char *args);

static void cmd_uptime(const char *args)
{
    (void)args;
    uint64_t t = timer_ticks();
    kprintf("up %lu.%02lu s (%lu timer ticks at %u Hz)\n", t / TIMER_HZ,
            t % TIMER_HZ * 100 / TIMER_HZ, t, TIMER_HZ);
}

static void cmd_mem(const char *args)
{
    (void)args;
    /* The heap keeps one empty page per block size for reuse; give those
     * back first, so that the count is exact (a leak shows at once). */
    sched_reap();
    mmap_cache_drop_idle();
    heap_trim();
    uint64_t free = pmm_free_count(), usable = pmm_usable_count();
    kprintf("memory: %lu MiB free of %lu MiB (%lu of %lu pages of 4 KiB)\n",
            free * PMM_PAGE_SIZE >> 20, usable * PMM_PAGE_SIZE >> 20, free, usable);
}

static bool have_filesystem(const char *command)
{
    if (!filesystem)
        kprintf("%s: no RitterFS mounted\n", command);
    return filesystem != 0;
}

/* One line per entry, in the format of scripts/ritterfs.py ls. */
static bool print_entry(void *ctx, const char *name, size_t name_len, uint64_t object_id,
                        uint8_t type)
{
    (void)ctx;
    char text[256];
    size_t n = name_len < sizeof(text) - 1 ? name_len : sizeof(text) - 1;
    memcpy(text, name, n);
    text[n] = 0;
    struct rfs_inode inode;
    if (type == RFS_TYPE_DIR)
        kprintf("     <dir>  %s/\n", text);
    else if (ritterfs_get_inode(filesystem, object_id, &inode))
        kprintf("%10lu  %s\n", inode.size, text);
    else
        kprintf("         ?  %s (unreadable)\n", text);
    return true;
}

static void cmd_ls(const char *args)
{
    if (!have_filesystem("ls"))
        return;
    const char *path = *args ? args : "/";
    uint64_t id;
    struct rfs_inode inode;
    if (!ritterfs_lookup(filesystem, path, &id, &inode)) {
        kprintf("ls: %s: not found\n", path);
        return;
    }
    if (inode.type != RFS_TYPE_DIR) {
        kprintf("%10lu  %s\n", inode.size, path);
        return;
    }
    if (!ritterfs_iterate_dir(filesystem, id, &inode, print_entry, 0))
        kprintf("ls: %s: directory unreadable\n", path);
}

static void cmd_pci(const char *args)
{
    (void)args;
    report_print(REPORT_PCI);
}

/* Split text into words separated by spaces (in place); argv gets at most
 * max - 1 of them and a terminating 0. Returns their number. */
static int split_words(char *text, char *argv[], int max)
{
    int argc = 0;
    while (*text && argc < max - 1) {
        while (*text == ' ')
            *text++ = 0;
        if (!*text)
            break;
        argv[argc++] = text;
        while (*text && *text != ' ')
            text++;
    }
    argv[argc] = 0;
    return argc;
}

/* Run a program with the words of args as further arguments (argv[0] is
 * the name it was started by) and wait until it ends; say how it ended
 * unless that was normally (exit code 0). */
static void run_program(const char *path, const char *name, const char *args)
{
    char words[LINE_MAX];
    char *argv[MAX_ARGS + 1];
    argv[0] = (char *)name;
    size_t n = strlen(args) < sizeof(words) - 1 ? strlen(args) : sizeof(words) - 1;
    memcpy(words, args, n);
    words[n] = 0;
    int argc = 1 + split_words(words, argv + 1, MAX_ARGS);
    int error, status;
    struct process *p = process_create(&filesystem->vfs, path, argc, (const char *const *)argv, 0, 0, 0, 0,
                                       &error);
    if (!p) {
        kprintf("%s: %s\n", path, process_error_text(error));
        return;
    }
    /* The job leads its own group, which gets the keyboard until it ends. */
    int pid = process_id(p);
    foreground = process_group(p);
    foreground_session = 0;
    if (!process_start(p)) {
        foreground = 0;
        kprintf("%s: out of memory\n", path);
        return;
    }
    process_wait(0, pid, &status, false);
    foreground = 0;
    int signal = status & 0x7F;                 /* see common/syscalls.h */
    if (signal == SIGINT)
        kprintf("^C\n");
    else if (signal == SIGKILL)
        kprintf("%s: killed\n", name);
    else if (!signal && status >> 8)
        kprintf("%s: exit code %d\n", name, status >> 8);
    /* Faults were reported by the process itself. */
}

static void cmd_run(const char *args)
{
    if (!have_filesystem("run"))
        return;
    char path[LINE_MAX];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && n < sizeof(path) - 1) {
        path[n] = args[n];
        n++;
    }
    path[n] = 0;
    if (!n) {
        kprintf("usage: run PATH [ARGUMENTS]\n");
        return;
    }
    run_program(path, path, args + n);
}

static void cmd_ps(const char *args)
{
    (void)args;
    report_print(REPORT_PROCESSES);
}

static void cmd_threads(const char *args)
{
    (void)args;
    report_print(REPORT_THREADS);
}

static void cmd_cpus(const char *args)
{
    (void)args;
    report_print(REPORT_CPUS);
}

static void cmd_usb(const char *args)
{
    (void)args;
    report_print(REPORT_USB);
}

static void cmd_mouse(const char *args)
{
    (void)args;
    mouse_report();
}

static void cmd_disks(const char *args)
{
    (void)args;
    report_print(REPORT_DISKS);
}

static void cmd_drivers(const char *args)
{
    (void)args;
    report_print(REPORT_DRIVERS);
}

static void cmd_power(const char *args)
{
    (void)args;
    report_print(REPORT_POWER);             /* (37a) */
}

static void cmd_net(const char *args)
{
    (void)args;
    report_print(REPORT_NET);
}

static void cmd_poweroff(const char *args)
{
    (void)args;
    arch_power_off();
}

static void cmd_reboot(const char *args)
{
    (void)args;
    arch_reboot();
}

static const struct command commands[] = {
    { "help",     "list the commands",                      cmd_help },
    { "uptime",   "time since the timer started",           cmd_uptime },
    { "mem",      "free and usable memory",                 cmd_mem },
    { "ls",       "ls [PATH]: list a RitterFS directory",   cmd_ls },
    { "cpus",     "list the processor's cores",             cmd_cpus },
    { "pci",      "list the PCI devices",                   cmd_pci },
    { "usb",      "list the USB devices",                   cmd_usb },
    { "mouse",    "the mice and their pointer state",       cmd_mouse },
    { "disks",    "the disks and their partitions",         cmd_disks },
    { "net",      "the network cards",                      cmd_net },
    { "drivers",  "the kernel's modules (drivers)",         cmd_drivers },
    { "power",    "what the firmware offers for power",     cmd_power },
    { "threads",  "list the kernel's threads",              cmd_threads },
    { "ps",       "list the processes",                     cmd_ps },
    { "run",      "run PATH [ARGUMENTS]: run a program",     cmd_run },
    { "poweroff", "switch the machine off",                 cmd_poweroff },
    { "reboot",   "restart the machine",                    cmd_reboot },
};

static bool print_program(void *ctx, const char *name, size_t name_len, uint64_t object_id,
                          uint8_t type)
{
    (void)ctx, (void)object_id;
    if (type == RFS_TYPE_FILE)
        kprintf(" %.*s", (int)name_len, name);
    return true;
}

static void cmd_help(const char *args)
{
    (void)args;
    kprintf("RitterOS rescue console (%s), commands:\n", ARCH_NAME);
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        kprintf("  %-9s %s\n", commands[i].name, commands[i].help);
    /* Everything else is a program: started by typing its name. */
    uint64_t id;
    struct rfs_inode dir;
    if (filesystem && ritterfs_lookup(filesystem, "/system/" ARCH_NAME "/bin", &id, &dir)) {
        kprintf("programs in /system/%s/bin:", ARCH_NAME);
        ritterfs_iterate_dir(filesystem, id, &dir, print_program, 0);
        kprintf("\n");
    }
}

static void execute(char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;
    char *args = line;
    while (*args && *args != ' ')
        args++;
    if (*args)
        *args++ = 0;
    while (*args == ' ')
        args++;
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        if (strcmp(line, commands[i].name) == 0) {
            commands[i].run(args);
            return;
        }
    /* Not built in: a program of that name in the system's bin directory? */
    char path[LINE_MAX + 32];
    uint64_t id;
    struct rfs_inode inode;
    ksnprintf(path, sizeof(path), "/system/%s/bin/%s", ARCH_NAME, line);
    bool plain_name = true;
    for (const char *c = line; *c; c++)
        plain_name &= *c != '/';
    if (filesystem && plain_name && ritterfs_lookup(filesystem, path, &id, &inode) &&
        inode.type == RFS_TYPE_FILE) {
        run_program(path, line, args);
        return;
    }
    kprintf("unknown command '%s' (type 'help')\n", line);
}

/* login as the foreground job, until it ends (Ctrl-D at its prompt). */
static void run_login(void)
{
    const char *argv[] = { "login", 0 };
    int error, status;
    struct process *p = process_create(&filesystem->vfs, LOGIN, 1, argv, 0, 0, 0, 0, &error);
    if (!p) {
        kprintf("  login:         %s: %s\n", LOGIN, process_error_text(error));
        return;
    }
    int pid = process_id(p);
    foreground = process_group(p);
    foreground_session = 0;
    if (!process_start(p)) {
        foreground = 0;
        kprintf("  login:         %s: out of memory\n", LOGIN);
        return;
    }
    kprintf("  login:         %s, process %d\n", LOGIN, pid);
    process_wait(0, pid, &status, false);
    foreground = 0;
    int signal = status & 0x7F;
    if (signal)
        kprintf("  login:         login was stopped by signal %d\n", signal);
    else
        kprintf("  login:         login ended (exit code %d)\n", status >> 8);
}

void terminal_run(bool rescue_only)
{
    char line[LINE_MAX];
    console_thread = thread_current();
    kprintf("  input:         ");
    for (int i = 0; i < input_count; i++)
        kprintf("%s%s", i == 0 ? "" : i == input_count - 1 ? " and " : ", ", inputs[i]);
    kprintf("\n");
    if (rescue_only)
        kprintf("  login:         not started (rescue on the command line)\n");
    else if (!filesystem)
        kprintf("  login:         not started (no file system)\n");
    else
        run_login();
    kprintf("  rescue console: type 'help' for the commands\n");
    for (;;) {
        bool locked = kprintf_lock();
        console_set_color(COLOR_HEADING);
        thread_current()->log_quiet++;              /* (the prompt: not a message) */
        kprintf(PROMPT);
        thread_current()->log_quiet--;
        terminal_wrote("\n" PROMPT, sizeof(PROMPT));
        console_set_color(COLOR_TEXT);
        kprintf_unlock(locked);
        bool newline;
        if (terminal_read_line(line, sizeof(line), &newline) < 0)
            line[0] = 0;
        execute(line);
    }
}
