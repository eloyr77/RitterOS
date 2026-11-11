/* termios (52e): the calls over SYS_TERMIOS (common/syscalls.h). */
#include <errno.h>
#include <termios.h>
#include "ritteros.h"

_Static_assert(sizeof(struct termios) == sizeof(struct sys_termios), "the kernel's layout");

static int check(int64_t r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return (int)r;
}

int tcgetattr(int fd, struct termios *t)
{
    return check(syscall6(SYS_TERMIOS, fd, TERMIOS_GET, (int64_t)t, 0, 0, 0));
}

int tcsetattr(int fd, int when, const struct termios *t)
{
    if (when < TCSANOW || when > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    int op = when == TCSAFLUSH ? TERMIOS_SET_FLUSH : TERMIOS_SET;
    return check(syscall6(SYS_TERMIOS, fd, op, (int64_t)t, 0, 0, 0));
}

int tcflush(int fd, int which)
{
    if (which < TCIFLUSH || which > TCIOFLUSH) {
        errno = EINVAL;
        return -1;
    }
    if (which == TCOFLUSH) {                    /* (nothing waits to be written) */
        struct termios t;
        return tcgetattr(fd, &t);
    }
    return check(syscall6(SYS_TERMIOS, fd, TERMIOS_FLUSH, 0, 0, 0, 0));
}

int tcdrain(int fd)
{
    struct termios t;
    return tcgetattr(fd, &t);                   /* (a terminal? output is never held) */
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON | INLCR | IGNCR);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *t)
{
    return t->c_ispeed;
}

speed_t cfgetospeed(const struct termios *t)
{
    return t->c_ospeed;
}

int cfsetispeed(struct termios *t, speed_t speed)
{
    t->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *t, speed_t speed)
{
    t->c_ospeed = speed;
    return 0;
}
