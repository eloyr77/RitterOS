/*
 * Formatted output for the kernel.
 *
 * Supported: %s %c %d %i %u %x %X %p %%, flags '-' (left-align) and '0'
 * (zero padding), a field width (66a: also as an argument, %*s; a negative
 * one aligns left), a precision for %s (%.4s or %.*s: at most 4 characters,
 * for strings that are not NUL-terminated), and the length
 * modifiers l, ll and z for 64-bit values. %p prints "0x" and 16 uppercase
 * hex digits.
 */
#ifndef RITTEROS_KPRINTF_H
#define RITTEROS_KPRINTF_H

#include <stdarg.h>
#include <stddef.h>

/* Format into buf (always NUL-terminated if size > 0). Returns the length
 * the full output would have had. */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Print to the console and the serial port. Output longer than 1 KiB per
 * call is truncated. Threads take turns: one call's output is never mixed
 * with another's. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* (66a) Formats a few cases (widths and precisions as arguments too) and
 * compares them with what they must give; logs the result. "selftest" only. */
void kprintf_self_test(void);

/* Keep other threads' output out of several calls in a row (e.g. a color
 * change, text, the color back). Returns whether it locked: pass that to
 * kprintf_unlock. In an interrupt handler it only takes the lock if free. */
#include <stdbool.h>
bool kprintf_lock(void);
void kprintf_unlock(bool locked);

/* A crash or panic: from now on output never waits for the lock (the
 * thread holding it may be the one that crashed). */
void kprintf_emergency(void);

/* Capture the running thread's kprintf output in a buffer instead of
 * printing it (0: print again). used counts every character, also those
 * that did not fit; the text is always zero-terminated. */
struct kprintf_capture {
    char *buf;
    size_t size, used;
};
void kprintf_capture(struct kprintf_capture *capture);

#endif
