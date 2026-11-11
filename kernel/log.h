/*
 * Logs (the user's wish after step 45a): text kept in memory, one ring per
 * log, until the elf logd takes it (SYS_LOG) and writes it to
 * /system/logs/NAME.log. Each line begins with the time it was added.
 * "kernel" holds the kernel's own messages from the first boot line on;
 * the others are named by elvesd (an elf's output) and login.
 */
#ifndef RITTEROS_LOG_H
#define RITTEROS_LOG_H

#include <stddef.h>
#include <stdint.h>

#define LOG_NAME_MAX 15

/* Add text to the log name (made on first use; at most 16 logs): lines
 * get their time in front. Not in interrupt context. */
void log_append(const char *name, const char *text, size_t length);

/* Take what the logs hold into out, as struct sys_log_record + text, one
 * after another, as many as fit: the bytes used. */
size_t log_take(uint8_t *out, size_t size);

#endif
