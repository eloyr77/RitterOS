/*
 * sys/ioctl.h (46b): ioctl, for ported programs (OpenSSL and curl make
 * sockets non-blocking with FIONBIO). RitterOS has no ioctl system call:
 * the library does the requests below through fcntl (Linux's numbers);
 * any other request is -1 with ENOTTY, as for a device that has none.
 *   FIONBIO   *(int *)arg non-zero: O_NONBLOCK on, else off
 *   FIOCLEX   close-on-exec on (FD_CLOEXEC); FIONCLEX off
 * (68v) and sys/soundcard.h's on a sound device (SNDCTL_DSP_GETODELAY,
 * SNDCTL_DSP_RESET), through SYS_SOUND.
 */
#ifndef RITTEROS_SYS_IOCTL_H
#define RITTEROS_SYS_IOCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FIONBIO     0x5421
#define FIONCLEX    0x5450
#define FIOCLEX     0x5451
int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
