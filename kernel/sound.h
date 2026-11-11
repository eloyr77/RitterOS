/*
 * (67g) The sound layer, the same for every sound chip (as net.c is for
 * network cards): the drivers' cards (drivers/include/driver_sound.h) as
 * the sound devices /dev/pcm0, pcm1, ... (dev: their number): speakers and
 * headphones first, then line outs, digital (HDMI) last. The format is
 * fixed at 48 kHz / 16-bit / stereo for now.
 */
#ifndef RITTEROS_SOUND_H
#define RITTEROS_SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include "driver_sound.h"

#define SOUND_DEVICES 8                 /* /dev/pcm0 ... pcm7 */

/* After the modules are loaded: number the cards registered so far and
 * print a boot line for each (a card registered later gets the next number
 * and prints its own). */
void sound_announce(void);

/* Is dev a sound device? */
bool sound_present(int dev);

/* Write interleaved 16-bit stereo PCM to the device, blocking while its
 * buffer is full; the number of bytes taken, or -1. Starts it if stopped. */
long sound_write(int dev, const void *buf, size_t n);

/* Wait until the device has played everything written, then stop (silence).
 * Called when the player closes /dev/pcmN, so the last sound is not cut off. */
void sound_drain(int dev);

/* Play a short test tone on pcm0 (the `hdatone` boot option). */
void sound_test_tone(void);

#endif
