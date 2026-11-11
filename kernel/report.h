/*
 * System reports: the text the rescue console prints for ps, threads, pci,
 * usb and disks. Programs get the same text through a system call (SYS_REPORT),
 * so there is one implementation for both.
 */
#ifndef RITTEROS_REPORT_H
#define RITTEROS_REPORT_H

#include <stdbool.h>
#include <stddef.h>

/* Print report `which` (REPORT_*, common/syscalls.h) with kprintf. False if
 * there is no such report. */
bool report_print(int which);

/* The same into buf (at most size bytes, zero-terminated); returns the
 * length of the whole report (it was cut short if that is >= size), or -1
 * if there is no such report. */
long report_text(int which, char *buf, size_t size);

#endif
