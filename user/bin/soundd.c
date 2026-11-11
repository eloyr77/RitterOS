/*
 * soundd: the sound mixer. It owns /dev/pcm0 and listens on the local socket
 * /run/sound; several programs can play at once by connecting and streaming
 * 48 kHz / 16-bit / stereo PCM. Each connection's samples are summed into a
 * shared accumulator ring by a reader thread; an output thread clamps the sum
 * and writes it to /dev/pcm0 (which paces everything to real time). Meant to
 * run as an elf (`elf add soundd soundd`); also runs as `soundd &`.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <thread.h>
#include "ritteros.h"

PROGRAM_HELP("soundd", "the sound mixer: lets several programs play at once");

#define SOUND_SOCKET "/run/sound"
#define RING    16384                   /* frames in the mix ring (~0.34 s) */
#define CHUNK   512                     /* frames per output write */
#define LEAD    2048                    /* a client writes this far ahead of the play point */

static int32_t ring[RING * 2];          /* stereo accumulator (sum of all clients) */
static volatile uint32_t play_pos;      /* frames the output has consumed */
static volatile int lock;

static void take(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) usleep(200); }
static void give(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

/* One connection: read its PCM and add it into the ring at its own cursor. */
static int client_thread(void *arg)
{
    int sock = (int)(long)arg;
    take();
    uint32_t cursor = play_pos + LEAD;   /* start a little ahead of the play point */
    give();
    int16_t buf[256 * 2];               /* on this thread's own stack (no TLS in RitterOS) */
    for (;;) {
        ssize_t n = read(sock, buf, sizeof(buf));
        if (n <= 0)
            break;
        int frames = (int)(n / 4);
        for (;;) {                       /* wait for room ahead; resync if we fell behind */
            take();
            int32_t ahead = (int32_t)(cursor - play_pos);
            if (ahead < 0) {             /* the output passed us: skip forward, do not play the past */
                cursor = play_pos + LEAD;
                give();
                continue;
            }
            if (ahead <= RING - frames)
                break;                   /* enough room, keep the lock and add below */
            give();
            usleep(2000);                /* too far ahead: let the output catch up */
        }
        for (int i = 0; i < frames; i++) {
            uint32_t s = (cursor + i) % RING;
            ring[s * 2] += buf[i * 2];
            ring[s * 2 + 1] += buf[i * 2 + 1];
        }
        give();
        cursor += frames;
    }
    close(sock);
    return 0;
}

static int16_t clamp(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Drain the ring to /dev/pcm0, clamping the sum and clearing what it plays. */
static int output_thread(void *arg)
{
    (void)arg;
    int fd = open("/dev/pcm0", O_WRONLY);
    if (fd < 0) {
        dprintf(2, "soundd: /dev/pcm0: %s\n", strerror(errno));
        return 1;
    }
    static int16_t out[CHUNK * 2];
    for (;;) {
        take();
        for (int i = 0; i < CHUNK; i++) {
            uint32_t s = (play_pos + i) % RING;
            out[i * 2] = clamp(ring[s * 2]);
            out[i * 2 + 1] = clamp(ring[s * 2 + 1]);
            ring[s * 2] = ring[s * 2 + 1] = 0;       /* consumed */
        }
        play_pos += CHUNK;
        give();
        if (write(fd, out, sizeof(out)) <= 0)        /* blocks: paces the mixer */
            break;
    }
    close(fd);
    return 1;
}

int main(void)
{
    int listener = ritteros_local_listen(SOUND_SOCKET, 8);
    if (listener < 0) {
        dprintf(2, "soundd: cannot listen on %s: %s\n", SOUND_SOCKET, strerror(errno));
        return 1;
    }
    if (thread_create(output_thread, 0) < 0) {
        dprintf(2, "soundd: no output thread\n");
        return 1;
    }
    printf("soundd: mixing on %s\n", SOUND_SOCKET);
    for (;;) {
        int sock = ritteros_accept(listener, 0, 0);
        if (sock < 0)
            continue;
        /* (49k) A user's soundd mixes its own user's and root's programs
         * only; root's (an elf: the system's mixer) everyone's. */
        int uid = ritteros_peer_uid(sock);
        if (getuid() != 0 && uid != (int)getuid() && uid != 0) {
            printf("soundd: refused a program of user %d (this is user %d's sound)\n", uid,
                   (int)getuid());
            close(sock);
            continue;
        }
        if (thread_create(client_thread, (void *)(long)sock) < 0)
            close(sock);
    }
    return 0;
}
