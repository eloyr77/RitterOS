/*
 * The terminal: commands typed on the first serial port (for example in the
 * terminal that runs `make run`, or any serial terminal program) or on a
 * keyboard. Answers go to the serial port and to the screen.
 */
#ifndef RITTEROS_TERMINAL_H
#define RITTEROS_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ritterfs;

/* Start receiving on the serial port (bytes typed from now on are kept).
 * Needs interrupts (timer_init). Returns false if there is no serial input. */
bool terminal_init(void);

/* A byte typed on another input device (a keyboard), as if it came from
 * the serial port. Interrupt context is fine. */
void terminal_receive(uint8_t byte);

/* Text a program printed to the terminal: the line it is on so far is kept
 * (the prompt), so that Ctrl-L can show it again. */
void terminal_wrote(const char *s, size_t n);

/* kprintf, with its lock held, before text goes out: text from any thread
 * but the one reading a line takes that line's place, which is shown again
 * below it (see terminal.c). */
void terminal_message(const char *text);

/*
 * Read one line, edited and echoed: printable ASCII, Backspace, Ctrl-U,
 * Enter; escape sequences (cursor keys) are skipped. Returns its length
 * (*newline: it ended with Enter), or one of:
 *   TERMINAL_EOF          Ctrl-D on an empty line (programs only)
 *   TERMINAL_INTERRUPTED  the reading program is being stopped, or Ctrl-C
 *                         reached it as input (it ignores the signal)
 * Ctrl-C at the terminal's own prompt cancels the line (length 0).
 */
#define TERMINAL_EOF            (-1)
#define TERMINAL_INTERRUPTED    (-2)
int terminal_read_line(char *buf, size_t size, bool *newline);

/* Echo of typed text on or off (off: passwords); a process that ends with
 * it off gets it switched on again. */
void terminal_set_echo(bool on, int pid);
void terminal_process_ended(int pid);

/* Raw input (full-screen programs): bytes as typed, no echo, no line
 * editing; a process that ends with it on gets lines again. line_editor:
 * the program edits a line of its own (the shell), which is shown again
 * after a message like the terminal's own lines. */
void terminal_set_raw(bool on, bool line_editor, int pid);
bool terminal_is_raw(void);

/* (52e) termios: the modes now (ICANON, ECHO, ISIG as they are), set them
 * (pid: whose they are; back to the start when it ends), drop typed input
 * not yet read. */
struct sys_termios;
void terminal_termios_get(struct sys_termios *out);
void terminal_termios_set(const struct sys_termios *in, int pid, bool flush);
void terminal_flush(void);

/* Wait for at least one typed byte, then take what is there, up to size.
 * Returns the number, or TERMINAL_INTERRUPTED. */
int terminal_read_raw(uint8_t *buf, size_t size);

/* The process group of the job in the foreground (0: none, the terminal's
 * own prompt): only it reads input, and Ctrl-C stops it (SIGINT). */
int terminal_foreground(void);
void terminal_set_foreground(int group, int session);
/* (65e) The session that gave the foreground out last (0: the kernel did,
 * for login or a rescue console job): its processes may hand it on. */
int terminal_foreground_session(void);

/* (52d) Typed bytes not read yet (for poll: a line read still waits for
 * its Enter). */
bool terminal_input_waiting(void);
bool terminal_poll(void);                   /* (65e) the same, for poll() (it watches) */

/* Is there any input device (serial port or keyboard)? */
bool terminal_has_input(void);

/* Name another input device in the terminal's start message, e.g. "kbd0". */
void terminal_add_input(const char *name);
void terminal_remove_input(const char *name);

/* The file system for ls and cat. */
void terminal_set_filesystem(struct ritterfs *fs);

/* Start the shell (/system/<arch>/bin/sh) as the foreground job unless
 * rescue_only; when it ends (or did not start), say why and run the rescue
 * console: the kernel's own commands for checking the hardware and the file
 * system, forever. */
__attribute__((noreturn)) void terminal_run(bool rescue_only);

#endif
