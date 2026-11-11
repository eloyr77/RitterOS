/*
 * (67g) The sound class of the driver interface (driver.h): a sound chip's
 * driver gives the kernel's sound layer (kernel/sound.c, one for every
 * chip) a card that plays PCM; the layer numbers the cards as the sound
 * devices /dev/pcm0, pcm1, ... (speakers and headphones first, then line
 * outs, digital outputs last; in the order registered within a kind) and
 * hands the programs' writes to the card.
 *
 * A driver keeps a struct sound_card (in its own state), fills it in from
 * what its chip reports (which outputs are wired, and to what) and
 * registers it with sound_register once the chip plays. The format is
 * fixed for now: SOUND_RATE Hz, SOUND_BITS-bit, SOUND_CHANNELS channels,
 * interleaved.
 */
#ifndef RITTEROS_DRIVER_SOUND_H
#define RITTEROS_DRIVER_SOUND_H

#include "driver.h"

#define SOUND_RATE      48000
#define SOUND_CHANNELS  2
#define SOUND_BITS      16

/* What a card's outputs are, which decides its number. */
enum { SOUND_SPEAKERS, SOUND_LINE_OUT, SOUND_DIGITAL };

struct sound_card {
    const char *driver;                 /* "hda" */
    struct pci_device *pci;             /* where it sits, or 0 */
    int kind;                           /* SOUND_SPEAKERS, ... */
    char outputs[96];                   /* for the boot line: "codec 0: line out pin 3 (DAC 2)" */
    void *priv;                         /* the driver's own */
    /* Play n bytes of PCM (whole frames): wait while the card's buffer is
     * full, start playing if stopped. The bytes taken, or -1. From a
     * program's thread (a write to /dev/pcmN). */
    long (*write)(struct sound_card *card, const void *buf, size_t n);
    /* Play out everything written, then stop with silence (the player
     * closed the device); the next write starts again. */
    void (*drain)(struct sound_card *card);
};

#ifndef RITTEROS_KERNEL

/* The card plays: the layer gives it its number (pcm0, ...). */
void sound_register(struct sound_card *card);

#endif /* RITTEROS_KERNEL */
#endif
