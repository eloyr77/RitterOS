/*
 * sys/soundcard.h (68v): OSS's requests on a sound device (/dev/pcmN), for
 * players and ported programs, through ioctl (the numbers are Linux's).
 * RitterOS's devices play 48 kHz, 16-bit, stereo PCM only.
 *   SNDCTL_DSP_GETODELAY  *(int *)arg: the bytes written and not yet played
 *                         (0 once everything was): where playing is
 *   SNDCTL_DSP_RESET      what is buffered dropped at once (silence); the
 *                         next write starts again, a close does not wait
 * Any other descriptor: -1 with ENOTTY.
 */
#ifndef RITTEROS_SYS_SOUNDCARD_H
#define RITTEROS_SYS_SOUNDCARD_H

#ifdef __cplusplus
extern "C" {
#endif

#define SNDCTL_DSP_RESET        0x00005000
#define SNDCTL_DSP_GETODELAY    0x80045017

#ifdef __cplusplus
}
#endif

#endif
