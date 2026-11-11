/*
 * The sound layer (see sound.h). Drivers register their cards while the
 * modules load; sound_announce then numbers them by kind, stably (a
 * machine's cards keep their order between boots: the PCI order within a
 * kind), and from then on a new card gets the next number.
 */
#include "sound.h"
#include "arch.h"
#include "kprintf.h"
#include "sync.h"
#include "timer.h"

static struct sound_card *waiting[SOUND_DEVICES];   /* registered, not numbered yet */
static struct sound_card *devices[SOUND_DEVICES];   /* pcm0, pcm1, ... */
static int waiting_count;
static volatile int device_count;
static bool announced;
static struct mutex lock = MUTEX_INIT("sound");

/* The card becomes the next device, with its boot line. */
static void number(struct sound_card *card)
{
    if (device_count == SOUND_DEVICES) {
        kprintf("  sound:         more than %d sound devices: %s's card left out\n",
                SOUND_DEVICES, card->driver);
        return;
    }
    int d = device_count;
    if (card->pci)
        kprintf("  pcm%d:          %02x:%02x.%x %s; 48 kHz 16-bit stereo\n", d, card->pci->bus,
                card->pci->device, card->pci->function, card->outputs);
    else
        kprintf("  pcm%d:          %s; 48 kHz 16-bit stereo\n", d, card->outputs);
    devices[d] = card;
    arch_write_barrier();                   /* (the slot before the count: sound_present */
    device_count = d + 1;                   /* reads only the count, on any core) */
}

void sound_register(struct sound_card *card)
{
    mutex_lock(&lock);
    if (announced)
        number(card);
    else if (waiting_count < SOUND_DEVICES)
        waiting[waiting_count++] = card;
    mutex_unlock(&lock);
}

void sound_announce(void)
{
    mutex_lock(&lock);
    for (int kind = SOUND_SPEAKERS; kind <= SOUND_DIGITAL; kind++)
        for (int i = 0; i < waiting_count; i++)
            if (waiting[i]->kind == kind)
                number(waiting[i]);
    announced = true;
    mutex_unlock(&lock);
}

bool sound_present(int dev)
{
    return dev >= 0 && dev < device_count;
}

long sound_write(int dev, const void *buf, size_t n)
{
    if (!sound_present(dev))
        return -1;
    return devices[dev]->write(devices[dev], buf, n);
}

void sound_drain(int dev)
{
    if (sound_present(dev))
        devices[dev]->drain(devices[dev]);
}

void sound_test_tone(void)
{
    if (!sound_present(0))
        return;
    /* A 440 Hz square wave, ~0.4 s, stereo. */
    static int16_t block[2400 * 2];
    int period = SOUND_RATE / 440;
    for (int i = 0; i < 2400; i++) {
        int16_t v = (i % period) < period / 2 ? 8000 : -8000;
        block[i * 2] = v;
        block[i * 2 + 1] = v;
    }
    for (int r = 0; r < 8; r++)
        sound_write(0, block, sizeof(block));
    sound_drain(0);
    kprintf("sound: test tone played on pcm0\n");
}
