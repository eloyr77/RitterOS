/*
 * Pseudo-terminals (kernel/pty.c): a terminal whose keyboard and screen
 * are a program (sshd). What the program writes to the master side is
 * typed; what programs write to the slave side is shown, to be read from
 * the master side. In between is the same line editing as on the
 * console: echo, Backspace, Ctrl-U, Ctrl-C for the foreground job, Ctrl-D,
 * raw input for full-screen programs.
 */
#ifndef RITTEROS_PTY_H
#define RITTEROS_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pty;

/* A new pseudo-terminal, with one master and one slave reference; 0 if
 * there is no memory. */
struct pty *pty_new(void);
int pty_number(struct pty *t);

/* References: descriptors of either side, and processes that have it as
 * their terminal. It is freed when all are gone. */
void pty_hold(struct pty *t, bool master);
void pty_close(struct pty *t, bool master);
void pty_hold_terminal(struct pty *t);
void pty_release_terminal(struct pty *t);

/* The master side: what was shown (waits for at least one byte; 0: the
 * slave side is closed by everyone), and typing (never waits). Return the
 * count or -ERR_*. */
int64_t pty_read_master(struct pty *t, uint8_t *buf, size_t size);
int64_t pty_write_master(struct pty *t, const uint8_t *buf, size_t size);

/* The slave side, for the processes on the terminal: an edited line (or,
 * raw, what was typed; 0 at its end: Ctrl-D, or the master is gone), and
 * output (a line break becomes CR LF; waits while the master is behind).
 * -ERR_INTR: stopped, or Ctrl-C reached a reader that ignores it. */
int64_t pty_read_slave(struct pty *t, uint8_t *buf, size_t size);
int64_t pty_write_slave(struct pty *t, const uint8_t *buf, size_t size);

/* As the console's (terminal.h): the foreground group, the session, echo,
 * raw input, the size in characters. */
int pty_foreground(struct pty *t);
void pty_set_foreground(struct pty *t, int group);
int pty_session(struct pty *t);
void pty_set_session(struct pty *t, int session);
void pty_set_echo(struct pty *t, bool on, int pid);
void pty_set_raw(struct pty *t, bool on, int pid);
void pty_process_ended(struct pty *t, int pid);
void pty_set_size(struct pty *t, uint32_t rows, uint32_t cols);
void pty_size(struct pty *t, uint32_t *rows, uint32_t *cols);

/* The master side is gone: true once that happened. */
bool pty_hung_up(struct pty *t);

/* (52e) termios, as the console's (terminal.h). */
struct sys_termios;
void pty_termios_get(struct pty *t, struct sys_termios *out);
void pty_termios_set(struct pty *t, const struct sys_termios *in, int pid, bool flush);
void pty_flush(struct pty *t);

/* (52d) What a read or write of this end would find: POLLIN, POLLOUT,
 * POLLHUP (the other side gone). */
int pty_poll(struct pty *t, bool master);

#endif
