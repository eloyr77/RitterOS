#include "kprintf.h"
#include "console.h"
#include "serial.h"
#include "sync.h"
#include "log.h"
#include "string.h"
#include "terminal.h"
#include <stdbool.h>
#include <stdint.h>

/* Output buffer that silently drops what does not fit. */
struct out {
    char *buf;
    size_t size;
    size_t len;          /* length of the full output, including dropped part */
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size)
        o->buf[o->len] = c;
    o->len++;
}

static void put_padded(struct out *o, const char *s, size_t n, int width, bool left, char pad)
{
    int fill = width > (int)n ? width - (int)n : 0;
    if (!left)
        while (fill-- > 0)
            put(o, pad);
    for (size_t i = 0; i < n; i++)
        put(o, s[i]);
    if (left)
        while (fill-- > 0)
            put(o, ' ');
}

static void put_number(struct out *o, uint64_t value, bool negative, unsigned base,
                       bool upper, int width, bool left, bool zero)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0);

    int len = n + (negative ? 1 : 0);
    if (zero && !left) {
        if (negative)
            put(o, '-');
        for (; len < width; len++)
            put(o, '0');
    } else {
        if (!left)
            for (; len < width; len++)
                put(o, ' ');
        if (negative)
            put(o, '-');
    }
    while (n > 0)
        put(o, tmp[--n]);
    if (left)
        for (; len < width; len++)
            put(o, ' ');
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                zero = true;
            else
                break;
        }
        int width = 0;
        if (*fmt == '*') {              /* the width is an argument; negative aligns left */
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = width == INT32_MIN ? 0 : -width;
            }
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        int precision = -1;            /* for %s: maximum characters to print */
        if (*fmt == '.') {
            precision = 0;
            fmt++;
            if (*fmt == '*') {          /* the precision is an argument */
                precision = va_arg(ap, int);
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9')
                precision = precision * 10 + (*fmt++ - '0');
        }
        int longs = 0;
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            longs = 2;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_number(&o, v < 0 ? 0 - (uint64_t)v : (uint64_t)v, v < 0, 10, false,
                       width, left, zero);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            put_number(&o, v, false, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, left, zero);
            break;
        }
        case 'p':
            put(&o, '0');
            put(&o, 'x');
            put_number(&o, (uint64_t)va_arg(ap, void *), false, 16, true, 16, false, true);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t n = 0;
            while ((precision < 0 || n < (size_t)precision) && s[n])
                n++;
            put_padded(&o, s, n, width, left, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(&o, &c, 1, width, left, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case '\0':
            fmt--;          /* lone '%' at the end */
            break;
        default:            /* unknown conversion: print it as is */
            put(&o, '%');
            put(&o, *fmt);
            break;
        }
    }

    if (size > 0)
        buf[o.len < size ? o.len : size - 1] = 0;
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf_self_test(void)
{
    /* (66a) %*s once shifted every later argument by one: the J1900's
     * graphics line read its device number (2) as a string and crashed. */
    static const char *const want[] = { "[  ab|c  |-7|x]", "[abc|00042|  -5|ff  ]" };
    char got[2][32];
    ksnprintf(got[0], sizeof(got[0]), "[%*s|%-*s|%d|%.*s]", 4, "ab", 3, "c", -7, 1, "xyz");
    ksnprintf(got[1], sizeof(got[1]), "[%s|%05u|%*d|%-*x]", "abc", 42, 4, -5, -4, 255);
    for (int i = 0; i < 2; i++)
        if (strcmp(got[i], want[i])) {
            kprintf("  kprintf:       self-test FAILED: '%s', not '%s'\n", got[i], want[i]);
            return;
        }
    kprintf("  kprintf:       self-test passed: widths and precisions as arguments\n");
}

static struct mutex output = MUTEX_INIT("kprintf");
static bool emergency;

bool kprintf_lock(void)
{
    if (emergency)
        return false;
    /* Not sleeping for it in an interrupt handler or under a spinlock
     * (65a-5): then the line goes out unlocked if another is printing. */
    uint64_t irq = arch_irq_save();         /* (65c-3: this core's count, not a torn read) */
    int held = arch_this_cpu()->locks_held;
    arch_irq_restore(irq);
    if (sched_in_interrupt() || held)
        return mutex_try_lock(&output);
    mutex_lock(&output);
    return true;
}

void kprintf_unlock(bool locked)
{
    if (locked)
        mutex_unlock(&output);
}

void kprintf_emergency(void)
{
    emergency = true;
    console_emergency();
}

void kprintf_capture(struct kprintf_capture *capture)
{
    if (capture && capture->size)
        capture->buf[0] = 0;
    thread_current()->capture = capture;
}

void kprintf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    struct kprintf_capture *c = emergency ? 0 : thread_current()->capture;
    if (c) {
        for (const char *s = buf; *s; s++, c->used++)
            if (c->used + 1 < c->size)
                c->buf[c->used] = *s;
        if (c->size)
            c->buf[c->used < c->size ? c->used : c->size - 1] = 0;
        return;
    }
    bool locked = kprintf_lock();
    if (!emergency) {
        terminal_message(buf);
        if (!thread_current()->log_quiet)
            log_append("kernel", buf, strlen(buf));
    }
    serial_write(buf);
    console_write(buf);
    kprintf_unlock(locked);
}
