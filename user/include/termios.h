/* A terminal's modes (52e): POSIX's termios. RitterOS acts on ICANON
 * (off: bytes as typed, no line editing and no echo; VMIN and VTIME then
 * say how a read waits), ECHO (lines only), ISIG (Ctrl-C a signal, else a
 * byte 3) and ICRNL (without ICANON: Enter read as \n); the other flags
 * are kept as set but change nothing. A program that changed the modes
 * leaves the terminal as it found it when it ends. */
#ifndef RITTEROS_TERMIOS_H
#define RITTEROS_TERMIOS_H

#include "ritteros.h"           /* the TERMIOS_* numbers (common/syscalls.h) */

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS            TERMIOS_NCCS

struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
};

/* c_lflag */
#define ISIG            TERMIOS_ISIG
#define ICANON          TERMIOS_ICANON
#define ECHO            TERMIOS_ECHO
#define ECHOE           TERMIOS_ECHOE
#define ECHOK           TERMIOS_ECHOK
#define ECHONL          TERMIOS_ECHONL
#define NOFLSH          TERMIOS_NOFLSH
#define TOSTOP          TERMIOS_TOSTOP
#define IEXTEN          TERMIOS_IEXTEN
/* c_iflag */
#define BRKINT          TERMIOS_BRKINT
#define INPCK           TERMIOS_INPCK
#define ISTRIP          TERMIOS_ISTRIP
#define INLCR           TERMIOS_INLCR
#define IGNCR           TERMIOS_IGNCR
#define ICRNL           TERMIOS_ICRNL
#define IXON            TERMIOS_IXON
/* c_oflag */
#define OPOST           TERMIOS_OPOST
#define ONLCR           TERMIOS_ONLCR
/* c_cflag */
#define CSIZE           TERMIOS_CSIZE
#define CS8             TERMIOS_CS8
#define CREAD           TERMIOS_CREAD
#define PARENB          TERMIOS_PARENB
/* c_cc */
#define VINTR           TERMIOS_VINTR
#define VQUIT           TERMIOS_VQUIT
#define VERASE          TERMIOS_VERASE
#define VKILL           TERMIOS_VKILL
#define VEOF            TERMIOS_VEOF
#define VTIME           TERMIOS_VTIME
#define VMIN            TERMIOS_VMIN
#define VSTART          TERMIOS_VSTART
#define VSTOP           TERMIOS_VSTOP
#define VSUSP           TERMIOS_VSUSP

/* tcsetattr's when (TCSADRAIN as TCSANOW: output never waits) */
#define TCSANOW         0
#define TCSADRAIN       1
#define TCSAFLUSH       2
/* tcflush's which (only typed input waits to be dropped) */
#define TCIFLUSH        0
#define TCOFLUSH        1
#define TCIOFLUSH       2

/* Speeds are plain numbers (BSD's); they change nothing. */
#define B0              0
#define B9600           9600
#define B19200          19200
#define B38400          38400
#define B57600          57600
#define B115200         115200

/* 0, or -1 with errno set (ENOTTY: not a terminal, EPERM: changing a
 * terminal whose foreground job is another). */
int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int when, const struct termios *t);
int tcflush(int fd, int which);
int tcdrain(int fd);
/* Raw: no ICANON, ECHO, ISIG or input translation, 8 bits, one byte a read. */
void cfmakeraw(struct termios *t);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int cfsetispeed(struct termios *t, speed_t speed);
int cfsetospeed(struct termios *t, speed_t speed);

#endif
