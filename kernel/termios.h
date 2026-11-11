/*
 * A terminal's modes (52e, SYS_TERMIOS): what the console and every
 * pseudo-terminal keep, and how a read without ICANON waits (VMIN, VTIME).
 */
#ifndef RITTEROS_TERMIOS_H
#define RITTEROS_TERMIOS_H

#include "string.h"
#include "syscalls.h"

/* Lines with echo and Ctrl-C; reads at least one byte when raw. */
static inline void termios_default(struct sys_termios *t)
{
    memset(t, 0, sizeof(*t));
    t->iflag = TERMIOS_ICRNL | TERMIOS_IXON | TERMIOS_BRKINT;
    t->oflag = TERMIOS_OPOST | TERMIOS_ONLCR;
    t->cflag = TERMIOS_CS8 | TERMIOS_CREAD;
    t->lflag = TERMIOS_ISIG | TERMIOS_ICANON | TERMIOS_ECHO | TERMIOS_ECHOE | TERMIOS_ECHOK |
               TERMIOS_IEXTEN;
    t->cc[TERMIOS_VINTR] = 0x03;
    t->cc[TERMIOS_VQUIT] = 0x1C;
    t->cc[TERMIOS_VERASE] = 0x7F;
    t->cc[TERMIOS_VKILL] = 0x15;
    t->cc[TERMIOS_VEOF] = 0x04;
    t->cc[TERMIOS_VMIN] = 1;
    t->cc[TERMIOS_VSTART] = 0x11;
    t->cc[TERMIOS_VSTOP] = 0x13;
    t->cc[TERMIOS_VSUSP] = 0x1A;
    t->ispeed = t->ospeed = 38400;
}

#endif
