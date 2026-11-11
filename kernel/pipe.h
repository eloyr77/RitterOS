/*
 * Pipes (kernel/pipe.c): a one-way, binary-transparent byte channel between
 * a write end and a read end (the SYS_PIPE system call hands back one of
 * each). Bytes written appear unchanged at the read end; a read returns 0
 * (end of file) once every write end is closed, and a write fails with
 * ERR_PIPE once every read end is closed. Used to run a program's stdio on
 * raw pipes (sshd's non-interactive exec), and the groundwork for shell
 * pipelines later.
 */
#ifndef RITTEROS_PIPE_H
#define RITTEROS_PIPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pipe;

/* A new pipe, with one read reference and one write reference; 0 if there
 * is no memory. */
struct pipe *pipe_new(void);

/* Another descriptor onto one end (a child inheriting it), and dropping an
 * end. The pipe is freed when both ends have no references left. */
void pipe_ref(struct pipe *p, bool writer);
void pipe_close(struct pipe *p, bool writer);

/* Held across a blocking read/write, so another thread closing the
 * descriptor meanwhile cannot free the pipe (as pty_hold_terminal does). */
void pipe_hold(struct pipe *p);
void pipe_unhold(struct pipe *p);

/* Read from the read end: waits for at least one byte; 0 once every write
 * end is closed. Write to the write end: all of it, waiting for room; -ERR_PIPE
 * once every read end is closed. -ERR_INTR if the thread is stopped. */
int64_t pipe_read(struct pipe *p, uint8_t *buf, size_t size);
int64_t pipe_write(struct pipe *p, const uint8_t *buf, size_t size);

/* (52d) POLLIN (bytes to read), POLLHUP (no write end left) for the read
 * end; POLLOUT (room) or POLLERR (no read end left) for the write end. */
int pipe_poll(struct pipe *p, bool writer);

#endif
