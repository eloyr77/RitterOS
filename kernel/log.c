/*
 * Logs (see log.h): a ring of text per log, under one spinlock. A full
 * ring drops its oldest bytes and counts them (logd notes the loss in the
 * file). Lines get the time in front ("2026-10-18 16:22:47 ", UTC; before
 * the clock runs, the time since the timer started: "boot +0.42 s ");
 * escape sequences (colors, cursor moves) and carriage returns are left
 * out, so a log file is plain text.
 */
#include "log.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"
#include "uefi.h"

#define LOGS            16
#define KERNEL_SIZE     (64 * 1024)
#define LOG_SIZE        (16 * 1024)

struct log {
    char name[LOG_NAME_MAX + 1];
    char *ring;
    size_t size, start, length;
    uint64_t lost;                      /* bytes dropped since the last take */
    bool in_line;                       /* the last text ended inside a line */
    uint8_t escape;                     /* 1: after ESC, 2: in ESC [ ... */
};

/* The kernel's ring is static: the boot's first lines come before the heap. */
static char kernel_ring[KERNEL_SIZE];
static struct log logs[LOGS] = {
    { .name = "kernel", .ring = kernel_ring, .size = KERNEL_SIZE },
};
static struct spinlock lock = SPINLOCK_INIT("log");

static void put(struct log *l, char c)
{
    if (l->length == l->size) {
        l->start = (l->start + 1) % l->size;
        l->length--;
        l->lost++;
    }
    l->ring[(l->start + l->length) % l->size] = c;
    l->length++;
}

/* The time in front of a line. */
static void stamp(struct log *l)
{
    char text[32];
    uint64_t ns = uefi_clock_ns_running();
    if (ns) {
        /* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
        int64_t secs = (int64_t)(ns / 1000000000ULL);
        int64_t days = secs / 86400, rem = secs % 86400;
        int64_t z = days + 719468, era = z / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t mp = (5 * doy + 2) / 153;
        int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
        int64_t y = yoe + era * 400 + (m <= 2);
        ksnprintf(text, sizeof(text), "%04ld-%02ld-%02ld %02ld:%02ld:%02ld ", y, m, d,
                  rem / 3600, rem / 60 % 60, rem % 60);
    } else {
        uint64_t t = timer_ticks();
        ksnprintf(text, sizeof(text), "boot +%lu.%02lu s ", t / TIMER_HZ,
                  t % TIMER_HZ * 100 / TIMER_HZ);
    }
    for (const char *c = text; *c; c++)
        put(l, *c);
}

static struct log *find(const char *name)
{
    for (int i = 0; i < LOGS; i++)
        if (logs[i].ring && strcmp(logs[i].name, name) == 0)
            return &logs[i];
    return 0;
}

void log_append(const char *name, const char *text, size_t length)
{
    if (!*name || strlen(name) > LOG_NAME_MAX || !length)
        return;
    uint64_t s = spin_lock(&lock);
    struct log *l = find(name);
    if (!l) {
        /* A new log: its ring from the heap, outside the lock. */
        spin_unlock(&lock, s);
        char *ring = kmalloc(LOG_SIZE);
        if (!ring)
            return;
        s = spin_lock(&lock);
        l = find(name);
        for (int i = 0; i < LOGS && !l; i++) {
            if (!logs[i].ring) {
                l = &logs[i];
                memset(l, 0, sizeof(*l));
                memcpy(l->name, name, strlen(name) + 1);
                l->ring = ring;
                l->size = LOG_SIZE;
                ring = 0;
            }
        }
        if (ring) {
            spin_unlock(&lock, s);
            kfree(ring);                            /* (taken meanwhile, or all logs used) */
            if (!l)
                return;
            s = spin_lock(&lock);
        }
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (l->escape == 1) {
            l->escape = c == '[' ? 2 : 0;
            continue;
        }
        if (l->escape == 2) {
            if (c >= 0x40 && c <= 0x7E)
                l->escape = 0;
            continue;
        }
        if (c == 0x1B) {
            l->escape = 1;
            continue;
        }
        if (c == '\r' || c == 0)
            continue;
        if (!l->in_line)
            stamp(l);
        put(l, c);
        l->in_line = c != '\n';
    }
    spin_unlock(&lock, s);
}

size_t log_take(uint8_t *out, size_t size)
{
    size_t used = 0;
    struct sys_log_record r;
    uint64_t s = spin_lock(&lock);
    for (int i = 0; i < LOGS; i++) {
        struct log *l = &logs[i];
        if (!l->ring || (!l->length && !l->lost))
            continue;
        if (used + sizeof(r) + 1 > size)
            break;
        size_t n = size - used - sizeof(r);
        if (n > l->length)
            n = l->length;
        memset(&r, 0, sizeof(r));
        memcpy(r.name, l->name, sizeof(l->name));
        r.length = (uint32_t)n;
        r.lost = (uint32_t)(l->lost > 0xFFFFFFFFu ? 0xFFFFFFFFu : l->lost);
        memcpy(out + used, &r, sizeof(r));
        used += sizeof(r);
        for (size_t k = 0; k < n; k++)
            out[used + k] = (uint8_t)l->ring[(l->start + k) % l->size];
        used += n;
        l->start = (l->start + n) % l->size;
        l->length -= n;
        l->lost = 0;
    }
    spin_unlock(&lock, s);
    return used;
}
