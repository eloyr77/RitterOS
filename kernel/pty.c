/*
 * Pseudo-terminals (see pty.h).
 *
 * Two rings: "typed" holds finished input (whole lines, or in raw mode
 * every byte), "shown" what programs printed, until the master reads it.
 * Typing goes through the line editor at once, in the writer's call (the
 * master's): echo goes straight into "shown", a finished line into
 * "typed". The editor does what the console's does (terminal.c):
 * Backspace erases a whole UTF-8 character, Ctrl-U the line, Ctrl-L
 * shows the prompt and the line again on an empty screen, escape
 * sequences (cursor keys) are skipped, Ctrl-D ends the input on an empty
 * line, Ctrl-C stops the foreground job if it takes SIGINT, else it
 * interrupts the reader (the shell then says "^C").
 */
#include "pty.h"
#include "blank.h"
#include "heap.h"
#include "process.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "termios.h"
#include "timer.h"

#define TYPED_SIZE  4096
#define SHOWN_SIZE  16384
#define EDIT_MAX    160                 /* as the console's line */

struct pty {
    int number;
    int masters, slaves, terminal_users;   /* references */
    struct mutex lock;
    uint8_t typed[TYPED_SIZE];
    uint32_t typed_head, typed_tail;
    uint8_t shown[SHOWN_SIZE];
    uint32_t shown_head, shown_tail;
    struct wait_queue readers;          /* slave readers: typed input */
    struct wait_queue master_readers;   /* the master: shown output */
    struct wait_queue writers;          /* slave writers: room in shown */
    int foreground, session;
    bool echo, raw, hung_up;
    int echo_pid, raw_pid;
    bool isig;                          /* Ctrl-C a signal (52e) */
    struct sys_termios mode;            /* what termios set (52e); ICANON, ECHO, ISIG live */
    int mode_pid;
    uint32_t rows, cols;
    /* The line editor. */
    char edit[EDIT_MAX + 1];
    size_t edit_length, character_start;
    int pending;                        /* UTF-8 continuation bytes still to come */
    bool in_escape, last_cr, interrupt;
    int ends;                           /* Ctrl-D on empty lines, not yet read */
    char line_so_far[160];              /* what was printed since the last line break */
    size_t line_so_far_length;
};

static int next_number;

struct pty *pty_new(void)
{
    struct pty *t = kzalloc(sizeof(*t));
    if (!t)
        return 0;
    t->number = __atomic_fetch_add(&next_number, 1, __ATOMIC_SEQ_CST);
    t->masters = t->slaves = 1;
    t->lock = (struct mutex)MUTEX_INIT("pty");
    t->echo = true;
    t->rows = 24, t->cols = 80;
    t->readers.pollable = t->master_readers.pollable = t->writers.pollable = true;
    t->isig = true;
    termios_default(&t->mode);
    return t;
}

int pty_number(struct pty *t)
{
    return t->number;
}

static void free_if_unused(struct pty *t)
{
    bool unused = t->masters == 0 && t->slaves == 0 &&
                  __atomic_load_n(&t->terminal_users, __ATOMIC_SEQ_CST) == 0;
    mutex_unlock(&t->lock);
    if (unused) {
        wait_queue_detach(&t->readers);     /* (65e) no poller left on them */
        wait_queue_detach(&t->master_readers);
        wait_queue_detach(&t->writers);
        kfree(t);
    }
}

void pty_hold(struct pty *t, bool master)
{
    mutex_lock(&t->lock);
    if (master)
        t->masters++;
    else
        t->slaves++;
    mutex_unlock(&t->lock);
}

void pty_close(struct pty *t, bool master)
{
    mutex_lock(&t->lock);
    bool hang_up = false;
    if (master) {
        if (--t->masters == 0) {
            t->hung_up = hang_up = true;
            wait_queue_wake_all(&t->readers);
            wait_queue_wake_all(&t->writers);
        }
    } else if (--t->slaves == 0) {
        wait_queue_wake_all(&t->master_readers);    /* the end of what is shown */
    }
    if (hang_up) {
        __atomic_add_fetch(&t->terminal_users, 1, __ATOMIC_SEQ_CST);  /* kept while its processes are told */
        mutex_unlock(&t->lock);
        process_hang_up(t);                         /* they end (SIGHUP) */
        mutex_lock(&t->lock);
        __atomic_sub_fetch(&t->terminal_users, 1, __ATOMIC_SEQ_CST);
    }
    free_if_unused(t);
}

/* Atomic, without the lock (49l): taken with the process's descriptor lock
 * held, before anything that may sleep (as pipe_hold). */
void pty_hold_terminal(struct pty *t)
{
    __atomic_add_fetch(&t->terminal_users, 1, __ATOMIC_SEQ_CST);
}

void pty_release_terminal(struct pty *t)
{
    mutex_lock(&t->lock);
    __atomic_sub_fetch(&t->terminal_users, 1, __ATOMIC_SEQ_CST);
    free_if_unused(t);
}

/* --- the rings (under the lock) ---------------------------------------------------- */

static uint32_t shown_room(struct pty *t)
{
    return SHOWN_SIZE - (t->shown_head - t->shown_tail);
}

int pty_poll(struct pty *t, bool master)
{
    poll_watch(&t->readers);                /* (65e) before the state is read */
    poll_watch(&t->master_readers);
    poll_watch(&t->writers);
    mutex_lock(&t->lock);
    int r;
    if (master)                                 /* (typing never waits) */
        r = POLLOUT | (t->shown_head != t->shown_tail ? POLLIN : 0) | (t->slaves == 0 ? POLLHUP : 0);
    else
        r = (t->typed_head != t->typed_tail || t->ends ? POLLIN : 0) |
            (shown_room(t) >= 2 ? POLLOUT : 0) | (t->hung_up ? POLLHUP : 0);
    mutex_unlock(&t->lock);
    return r;
}

/* Echo: dropped when the master is that far behind (it never waits). */
static void show(struct pty *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n && shown_room(t) > 0; i++)
        t->shown[t->shown_head++ % SHOWN_SIZE] = (uint8_t)s[i];
    wait_queue_wake_all(&t->master_readers);
}

static void show_text(struct pty *t, const char *s)
{
    show(t, s, strlen(s));
}

static void type(struct pty *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n && t->typed_head - t->typed_tail < TYPED_SIZE; i++)
        t->typed[t->typed_head++ % TYPED_SIZE] = (uint8_t)s[i];
    wait_queue_wake_all(&t->readers);
}

/* --- the line editor ------------------------------------------------------------ */

/* Erase the last character (a whole UTF-8 sequence); false if there is none. */
static bool erase_last(struct pty *t)
{
    if (t->edit_length == 0)
        return false;
    do
        t->edit_length--;
    while (t->edit_length > 0 && ((uint8_t)t->edit[t->edit_length] & 0xC0) == 0x80);
    return true;
}

static void finish_line(struct pty *t, bool newline)
{
    if (newline) {
        t->edit[t->edit_length++] = '\n';
        if (t->echo)
            show_text(t, "\r\n");
    }
    type(t, t->edit, t->edit_length);
    t->edit_length = 0;
    t->pending = 0;
}

static void type_byte(struct pty *t, uint8_t c)
{
    if (c == 0x03 && t->isig) {                     /* Ctrl-C */
        if (t->foreground && process_signal_group(t->foreground, SIGINT)) {
            t->edit_length = 0;                     /* the job stops; the byte goes nowhere */
            t->pending = 0;
            return;
        }
        if (!t->raw) {
            t->edit_length = 0;
            t->pending = 0;
            t->interrupt = true;                    /* the reader says "^C" itself */
            wait_queue_wake_all(&t->readers);
            return;
        }
    }
    if (t->raw) {
        char b = (char)c;
        type(t, &b, 1);
        return;
    }
    if (t->in_escape) {                             /* ESC [ parameters, then a final byte */
        if (c != '[' && c >= 0x40)
            t->in_escape = false;
        return;
    }
    bool after_cr = t->last_cr;
    t->last_cr = c == '\r';
    if (c == '\n' && after_cr)
        return;                                     /* CR LF: one line break */
    if (c == 0x1B) {
        t->in_escape = true;
        return;
    }
    if (c == '\r' || c == '\n') {
        finish_line(t, true);
        return;
    }
    if (c == 0x04) {                                /* Ctrl-D */
        if (t->edit_length == 0) {
            t->ends++;
            wait_queue_wake_all(&t->readers);
        } else {
            finish_line(t, false);                  /* the line so far, without a break */
        }
        return;
    }
    if (c >= 0x80) {
        if ((c & 0xC0) == 0x80) {                   /* a continuation byte */
            if (t->pending == 0)
                return;
            t->edit[t->edit_length++] = (char)c;
            if (--t->pending == 0 && t->echo)
                show(t, t->edit + t->character_start, t->edit_length - t->character_start);
            return;
        }
        if (t->pending)
            t->edit_length = t->character_start, t->pending = 0;
        int need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
        if (need == 0 || t->edit_length + (size_t)need + 2 > EDIT_MAX)
            return;
        t->character_start = t->edit_length;
        t->edit[t->edit_length++] = (char)c;
        t->pending = need;
        return;
    }
    if (t->pending)
        t->edit_length = t->character_start, t->pending = 0;   /* unfinished: dropped */
    if (c == 0x7F || c == '\b') {
        if (erase_last(t) && t->echo)
            show_text(t, "\b \b");
    } else if (c == 0x15) {                         /* Ctrl-U */
        while (erase_last(t))
            if (t->echo)
                show_text(t, "\b \b");
    } else if (c == 0x0C) {                         /* Ctrl-L */
        show_text(t, "\033[2J\033[H");
        show(t, t->line_so_far, t->line_so_far_length);
        if (t->echo)
            show(t, t->edit, t->edit_length);
    } else if (c >= 0x20 && c < 0x7F && t->edit_length + 2 <= EDIT_MAX) {
        t->edit[t->edit_length++] = (char)c;
        if (t->echo)
            show(t, (const char *)&c, 1);
    }
}

/* --- the two sides ------------------------------------------------------------ */

int64_t pty_write_master(struct pty *t, const uint8_t *buf, size_t size)
{
    if (size)
        blank_used();                       /* (37g-7) typed: someone uses the machine */
    mutex_lock(&t->lock);
    for (size_t i = 0; i < size; i++)
        type_byte(t, buf[i]);
    mutex_unlock(&t->lock);
    return (int64_t)size;
}

int64_t pty_read_master(struct pty *t, uint8_t *buf, size_t size)
{
    for (;;) {
        mutex_lock(&t->lock);
        uint32_t have = t->shown_head - t->shown_tail;
        if (have > 0) {
            size_t n = have < size ? have : size;
            for (size_t i = 0; i < n; i++)
                buf[i] = t->shown[t->shown_tail++ % SHOWN_SIZE];
            wait_queue_wake_all(&t->writers);
            mutex_unlock(&t->lock);
            return (int64_t)n;
        }
        bool ended = t->slaves == 0;
        mutex_unlock(&t->lock);
        if (ended)
            return 0;
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;                      /* (53b) */
        if (!wait_event_interruptible(&t->master_readers,
                                      t->shown_head != t->shown_tail || t->slaves == 0))
            return -ERR_INTR;
    }
}

/* (52e) Typed bytes from n on, Enter as \n with ICRNL (raw only; lines
 * end in \n anyway); under the lock. */
static size_t take_typed(struct pty *t, uint8_t *buf, size_t n, size_t size)
{
    bool crnl = t->raw && (t->mode.iflag & TERMIOS_ICRNL);
    while (n < size && t->typed_head != t->typed_tail) {
        uint8_t c = t->typed[t->typed_tail++ % TYPED_SIZE];
        buf[n++] = crnl && c == '\r' ? '\n' : c;
    }
    return n;
}

/* (52e) A raw read by VMIN and VTIME (as the console's read_timed). */
static int64_t read_timed(struct pty *t, uint8_t *buf, size_t size, unsigned vmin, unsigned vtime)
{
    uint64_t tenth = (TIMER_HZ + 9) / 10;
    struct thread *self = thread_current();
    size_t want = vmin < size ? vmin : size, n = 0;
    bool first = true;
    for (;;) {
        mutex_lock(&t->lock);
        n = take_typed(t, buf, n, size);
        bool gone = t->hung_up;
        mutex_unlock(&t->lock);
        if (gone || (vmin > 0 && n >= want) || (vmin == 0 && (n > 0 || vtime == 0 || !first)))
            return (int64_t)n;
        if (self->dont_wait)
            return n ? (int64_t)n : -ERR_AGAIN;     /* (53b) */
        bool ok;
        if (vmin == 0 || (n > 0 && vtime))
            ok = wait_event_until(&t->readers, t->typed_head != t->typed_tail || t->hung_up ||
                                               self->stop_requested,
                                  timer_ticks() + vtime * tenth);
        else
            ok = wait_event_interruptible(&t->readers, t->typed_head != t->typed_tail ||
                                                       t->hung_up);
        if (self->stop_requested)
            return n ? (int64_t)n : -ERR_INTR;
        if (!ok && n > 0)
            return (int64_t)n;                      /* quiet for VTIME: what came */
        first = false;
    }
}

int64_t pty_read_slave(struct pty *t, uint8_t *buf, size_t size)
{
    if (t->raw)                                 /* (52e: VMIN, VTIME and ICRNL) */
        return read_timed(t, buf, size, t->mode.cc[TERMIOS_VMIN], t->mode.cc[TERMIOS_VTIME]);
    for (;;) {
        mutex_lock(&t->lock);
        if (t->interrupt && !t->raw) {
            t->interrupt = false;
            mutex_unlock(&t->lock);
            return -ERR_INTR;
        }
        uint32_t have = t->typed_head - t->typed_tail;
        if (have > 0) {
            size_t n = 0;
            while (n < size && t->typed_head != t->typed_tail) {
                uint8_t c = t->typed[t->typed_tail++ % TYPED_SIZE];
                buf[n++] = c;
                if (c == '\n' && !t->raw)
                    break;                          /* one line at a time */
            }
            mutex_unlock(&t->lock);
            return (int64_t)n;
        }
        if (t->ends > 0 || t->hung_up) {
            if (t->ends > 0)
                t->ends--;
            mutex_unlock(&t->lock);
            return 0;
        }
        mutex_unlock(&t->lock);
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;                      /* (53b) */
        if (!wait_event_interruptible(&t->readers, t->typed_head != t->typed_tail || t->ends ||
                                                   t->hung_up || t->interrupt))
            return -ERR_INTR;
    }
}

int64_t pty_write_slave(struct pty *t, const uint8_t *buf, size_t size)
{
    size_t done = 0;
    mutex_lock(&t->lock);
    while (done < size) {
        if (t->hung_up) {
            mutex_unlock(&t->lock);
            return done ? (int64_t)done : -ERR_IO;
        }
        if (shown_room(t) < 2) {                    /* room for CR LF */
            mutex_unlock(&t->lock);
            if (thread_current()->dont_wait)
                return done ? (int64_t)done : -ERR_AGAIN;   /* (53b) */
            if (!wait_event_interruptible(&t->writers, shown_room(t) >= 2 || t->hung_up))
                return done ? (int64_t)done : -ERR_INTR;
            mutex_lock(&t->lock);
            continue;
        }
        while (done < size && shown_room(t) >= 2) {
            uint8_t c = buf[done++];
            if (c == '\n') {
                t->shown[t->shown_head++ % SHOWN_SIZE] = '\r';
                t->line_so_far_length = 0;
            } else if (c != '\r' && t->line_so_far_length < sizeof(t->line_so_far)) {
                t->line_so_far[t->line_so_far_length++] = (char)c;
            }
            t->shown[t->shown_head++ % SHOWN_SIZE] = c;
        }
        wait_queue_wake_all(&t->master_readers);
    }
    mutex_unlock(&t->lock);
    return (int64_t)done;
}

/* --- settings --------------------------------------------------------------------- */

int pty_foreground(struct pty *t)
{
    return t->foreground;
}

void pty_set_foreground(struct pty *t, int group)
{
    t->foreground = group;
}

int pty_session(struct pty *t)
{
    return t->session;
}

void pty_set_session(struct pty *t, int session)
{
    t->session = session;
}

void pty_set_echo(struct pty *t, bool on, int pid)
{
    mutex_lock(&t->lock);
    t->echo = on;
    t->echo_pid = on ? 0 : pid;
    mutex_unlock(&t->lock);
}

void pty_set_raw(struct pty *t, bool on, int pid)
{
    mutex_lock(&t->lock);
    t->raw = on;
    t->raw_pid = on ? pid : 0;
    t->mode.cc[TERMIOS_VMIN] = 1;               /* (the old switch: as typed) */
    t->mode.cc[TERMIOS_VTIME] = 0;
    t->mode.iflag &= ~(uint32_t)TERMIOS_ICRNL;
    t->edit_length = 0;
    t->pending = 0;
    t->in_escape = false;
    mutex_unlock(&t->lock);
}

void pty_termios_get(struct pty *t, struct sys_termios *out)
{
    mutex_lock(&t->lock);
    *out = t->mode;
    out->lflag &= ~(uint32_t)(TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ISIG);
    out->lflag |= (t->raw ? 0 : TERMIOS_ICANON) | (t->echo ? TERMIOS_ECHO : 0) |
                  (t->isig ? TERMIOS_ISIG : 0);
    mutex_unlock(&t->lock);
}

void pty_termios_set(struct pty *t, const struct sys_termios *in, int pid, bool flush)
{
    mutex_lock(&t->lock);
    t->mode = *in;
    bool lines = in->lflag & TERMIOS_ICANON;
    if (t->raw != !lines) {                     /* (a line half typed goes, as pty_set_raw) */
        t->edit_length = 0;
        t->pending = 0;
        t->in_escape = false;
    }
    t->raw = !lines;
    t->raw_pid = lines ? 0 : pid;
    t->echo = in->lflag & TERMIOS_ECHO;
    t->echo_pid = t->echo ? 0 : pid;
    t->isig = in->lflag & TERMIOS_ISIG;
    t->mode_pid = pid;
    if (flush)
        t->typed_tail = t->typed_head, t->edit_length = 0;
    mutex_unlock(&t->lock);
}

void pty_flush(struct pty *t)
{
    mutex_lock(&t->lock);
    t->typed_tail = t->typed_head;
    t->edit_length = 0;
    mutex_unlock(&t->lock);
}

void pty_process_ended(struct pty *t, int pid)
{
    mutex_lock(&t->lock);
    if (t->mode_pid == pid && pid) {            /* (52e) everything it set, back */
        termios_default(&t->mode);
        t->isig = true;
        t->mode_pid = 0;
    }
    if (!t->echo && t->echo_pid == pid)
        t->echo = true;
    if (t->raw && t->raw_pid == pid)
        t->raw = false;
    mutex_unlock(&t->lock);
}

void pty_set_size(struct pty *t, uint32_t rows, uint32_t cols)
{
    t->rows = rows ? rows : 24;
    t->cols = cols ? cols : 80;
}

void pty_size(struct pty *t, uint32_t *rows, uint32_t *cols)
{
    *rows = t->rows;
    *cols = t->cols;
}

bool pty_hung_up(struct pty *t)
{
    return t->hung_up;
}
