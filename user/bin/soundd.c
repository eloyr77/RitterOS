/*
 * soundd: the sound server. It listens on the local socket /run/sound;
 * several programs can play at once by connecting and streaming 48 kHz /
 * 16-bit / stereo PCM. Each connection's samples are summed into a shared
 * accumulator ring by a reader thread; an output thread clamps the sum and
 * writes it to /dev/pcm0. Meant to run as an elf (login makes its file,
 * 68w); also runs as `soundd &`.
 *
 * (68w) /dev/pcm0 takes one opener at a time: soundd opens it when a
 * program's sound comes and lets go after a second without any (as
 * PipeWire suspends an idle device), so a program may still have the
 * device alone meanwhile. It keeps the device's own buffer short (about
 * 43 ms, by SNDCTL_DSP_GETODELAY) so that a program's delay is right and a
 * reset drops nearly everything. A program that starts with the four bytes
 * "RSND" speaks in messages, each an 8-byte header (type, bytes) and its
 * bytes:
 *   1 sound   bytes of PCM (whole frames)
 *   2 delay   answered with 4 bytes: this program's bytes not yet heard
 *             (its share still in the mix, and the device's buffer)
 *   3 reset   this program's share not yet played taken out of the mix
 *   4 volume  (68x) 8 bytes, the master volume 0-100 and mute 0 or 1 (-1:
 *             as it is), answered with the two as they are now
 * Any other program's bytes are PCM, as before.
 *
 * (68x) The master volume scales the mix on a cubic curve (the ear's, as
 * PulseAudio's percentages); it is kept in /system/config/sound (volume,
 * mute), read at the start (written with its defaults when missing) and
 * changed line by line when a program sets it. Any program soundd plays
 * for may set it (the user's choice).
 *
 * (68z) As the elf it plays for root's programs and the console user's
 * only (SOUND_MAY_PLAY at each connection), the rule /dev/pcm0 keeps.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <time.h>
#include <unistd.h>
#include <thread.h>
#include "ritteros.h"
#include "settings_file.h"

PROGRAM_HELP("soundd", "the sound server: lets several programs play at once");

#define SOUND_SOCKET "/run/sound"
#define RING    16384                   /* frames in the mix ring (~0.34 s) */
#define CHUNK   512                     /* frames per output write */
#define LEAD    2048                    /* a client writes this far ahead of the play point */
#define DEVICE_KEEP (2048 * 4)          /* (68w) bytes the device may hold (~43 ms) */
#define IDLE_MS 1000                    /* (68w) the device let go after this without sound */

enum { MSG_SOUND = 1, MSG_DELAY = 2, MSG_RESET = 3, MSG_VOLUME = 4 };

static int32_t ring[RING * 2];          /* stereo accumulator (sum of all clients) */
static volatile uint32_t play_pos;      /* frames the output has consumed */
static volatile uint32_t added;         /* (68w) counts additions: sound came */
static volatile int device_delay;       /* (68w) the device's buffered bytes, last looked */
static volatile int lock;
static int wake[2] = { -1, -1 };        /* (68w) the idle output thread waits on wake[0] */
static volatile int idle_waits;
static volatile int volume = 100, muted;    /* (68x) */
static volatile int32_t gain = 65536;       /* (68x) the mix's factor, 16.16 */
static volatile int settings_lock;

static void take(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) usleep(200); }
static void give(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

static long long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int read_full(int fd, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

/* One connection's place in the mix: where its next frame goes, and (68w)
 * what it added at each place, so a reset can take its share out again. */
struct client {
    int sock;
    uint32_t cursor;
    int16_t *mine;                      /* RING frames, by ring slot */
    bool placed;                        /* (68w) false: the next sound goes a lead ahead */
};

/* Add frames of PCM at the client's cursor: wait for room ahead; a client
 * the output passed skips forward instead of playing the past. */
static void add(struct client *c, const int16_t *pcm, int frames)
{
    for (;;) {
        take();
        if (!c->placed) {
            c->cursor = play_pos + LEAD;
            c->placed = true;
        }
        int32_t ahead = (int32_t)(c->cursor - play_pos);
        if (ahead < 0) {
            c->cursor = play_pos + LEAD;
            give();
            continue;
        }
        if (ahead <= RING - frames)
            break;                       /* enough room, keep the lock and add below */
        give();
        usleep(2000);                    /* too far ahead: let the output catch up */
    }
    for (int i = 0; i < frames; i++) {
        uint32_t s = (c->cursor + i) % RING;
        ring[s * 2] += pcm[i * 2];
        ring[s * 2 + 1] += pcm[i * 2 + 1];
        c->mine[s * 2] = pcm[i * 2];
        c->mine[s * 2 + 1] = pcm[i * 2 + 1];
    }
    c->cursor += frames;
    __atomic_add_fetch(&added, 1, __ATOMIC_SEQ_CST);
    give();
    if (__atomic_load_n(&idle_waits, __ATOMIC_SEQ_CST)) {
        char one = 1;
        (void)!write(wake[1], &one, 1);  /* (non-blocking: one waiting byte is enough) */
    }
}

/* (68w) Bytes of this client's not yet heard: its frames still ahead of the
 * play point, and the device's buffer before them. */
static uint32_t delay_of(struct client *c)
{
    take();
    int64_t frames = (int32_t)(c->cursor - play_pos) + device_delay / 4;
    give();
    return frames > 0 ? (uint32_t)frames * 4 : 0;
}

/* (68w) Its share not yet played out of the mix: nothing of it pending
 * (the delay only the device's), its next sound a lead ahead again. */
static void reset(struct client *c)
{
    take();
    int32_t ahead = (int32_t)(c->cursor - play_pos);
    for (int32_t i = 0; i < ahead && i < RING; i++) {
        uint32_t s = (play_pos + (uint32_t)i) % RING;
        ring[s * 2] -= c->mine[s * 2];
        ring[s * 2 + 1] -= c->mine[s * 2 + 1];
        c->mine[s * 2] = c->mine[s * 2 + 1] = 0;
    }
    c->cursor = play_pos;
    c->placed = false;
    give();
}

static void volume_set(int32_t v, int32_t m);   /* (68x, below) */

/* PCM from the socket, n bytes (whole frames; a broken frame's bytes are
 * read and dropped), into the mix. */
static int sound_in(struct client *c, uint32_t n)
{
    int16_t buf[256 * 2];               /* on this thread's own stack */
    while (n > 0) {
        uint32_t k = n < sizeof(buf) ? n : (uint32_t)sizeof(buf);
        if (read_full(c->sock, buf, k) < 0)
            return -1;
        if (k / 4)
            add(c, buf, (int)(k / 4));
        n -= k;
    }
    return 0;
}

/* One connection: its messages after "RSND", else a stream of PCM. */
static int client_thread(void *arg)
{
    struct client c = { (int)(long)arg, 0, calloc(RING * 2, sizeof(int16_t)), false };
    char hello[4];
    if (!c.mine || read_full(c.sock, hello, 4) < 0)
        goto out;
    take();
    c.cursor = play_pos;                 /* (its first sound a lead ahead: add) */
    give();
    if (memcmp(hello, "RSND", 4) != 0) {
        int16_t buf[256 * 2];
        memcpy(buf, hello, 4);           /* (the first frame) */
        add(&c, buf, 1);
        for (;;) {
            ssize_t n = read(c.sock, buf, sizeof(buf));
            if (n <= 0)
                break;
            add(&c, buf, (int)(n / 4));
        }
        goto out;
    }
    for (;;) {
        uint32_t header[2];
        if (read_full(c.sock, header, sizeof(header)) < 0)
            break;
        if (header[0] == MSG_SOUND) {
            if (sound_in(&c, header[1]) < 0)
                break;
            continue;
        }
        int32_t asked[2] = { -1, -1 };   /* (a request's own bytes: 68x's volume) */
        char skip[64];
        uint32_t left = header[1];
        if (header[0] == MSG_VOLUME && left >= sizeof(asked)) {
            if (read_full(c.sock, asked, sizeof(asked)) < 0)
                goto out;
            left -= (uint32_t)sizeof(asked);
        }
        while (left > 0) {
            uint32_t k = left < sizeof(skip) ? left : (uint32_t)sizeof(skip);
            if (read_full(c.sock, skip, k) < 0)
                goto out;
            left -= k;
        }
        if (header[0] == MSG_DELAY) {
            uint32_t d = delay_of(&c);
            if (write(c.sock, &d, sizeof(d)) != sizeof(d))
                break;
        } else if (header[0] == MSG_RESET) {
            reset(&c);
        } else if (header[0] == MSG_VOLUME) {
            volume_set(asked[0], asked[1]);
            int32_t now[2] = { volume, muted };
            if (write(c.sock, now, sizeof(now)) != sizeof(now))
                break;
        }
    }
out:
    free(c.mine);                        /* (what it sent still plays) */
    close(c.sock);
    return 0;
}

/* (68x) The factor for a volume: (v / 100)^3, 16.16 fixed point. */
static int32_t gain_of(int v, int mute)
{
    return mute ? 0 : (int32_t)((int64_t)v * v * v * 65536 / 1000000);
}

/* (68x) /system/config/sound's `key` line made `key = value` (added if
 * missing): the file's other lines, its comments, stay (69c: the shared
 * settings_set). */
static void settings_put(const char *key, const char *value)
{
    if (settings_set(SETTINGS_SOUND, key, value) < 0)
        dprintf(2, "soundd: %s: %s\n", SETTINGS_SOUND, strerror(errno));   /* (a user's soundd) */
}

/* (68x) The settings at the start: volume and mute (the file made when
 * missing, as its reader does). */
static void settings_load(void)
{
    settings_make(SETTINGS_SOUND, SETTINGS_SOUND_TEXT);
    FILE *f = fopen(SETTINGS_SOUND, "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f)) {
        char key[16], value[16];
        if (line[0] == '#' || sscanf(line, " %15[a-z] = %15s", key, value) != 2)
            continue;
        if (!strcmp(key, "volume")) {
            int v = atoi(value);
            volume = v < 0 ? 0 : v > 100 ? 100 : v;
        } else if (!strcmp(key, "mute")) {
            muted = !strcmp(value, "yes");
        }
    }
    if (f)
        fclose(f);
    gain = gain_of(volume, muted);
}

/* (68x) A program's volume message: each of the two set unless -1; the
 * file's lines changed with it. */
static void volume_set(int32_t v, int32_t m)
{
    while (__atomic_exchange_n(&settings_lock, 1, __ATOMIC_ACQUIRE))
        usleep(200);
    char text[8];
    if (v >= 0 && v <= 100 && v != volume) {
        volume = v;
        snprintf(text, sizeof(text), "%d", (int)v);
        settings_put("volume", text);
    }
    if ((m == 0 || m == 1) && m != muted) {
        muted = m;
        settings_put("mute", m ? "yes" : "no");
    }
    gain = gain_of(volume, muted);
    __atomic_store_n(&settings_lock, 0, __ATOMIC_RELEASE);
}

static int16_t clamp(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* The next CHUNK frames of the mix, clamped; their slots cleared. */
static void mix(int16_t *out)
{
    int64_t g = gain;                           /* (68x) the master volume */
    take();
    for (int i = 0; i < CHUNK; i++) {
        uint32_t s = (play_pos + i) % RING;
        out[i * 2] = clamp((int32_t)(ring[s * 2] * g >> 16));
        out[i * 2 + 1] = clamp((int32_t)(ring[s * 2 + 1] * g >> 16));
        ring[s * 2] = ring[s * 2 + 1] = 0;       /* consumed */
    }
    play_pos += CHUNK;
    give();
}

/* The mix to /dev/pcm0 while sound comes (68w): opened when a program's
 * sound arrives, let go after IDLE_MS without any; its buffer kept near
 * DEVICE_KEEP. A device that cannot be opened (another program has it, or
 * none) plays nothing: the mix goes on at real time and is dropped. */
static int output_thread(void *arg)
{
    (void)arg;
    static int16_t out[CHUNK * 2];
    int fd = -1, last_error = 0;
    uint32_t seen = added;
    long long last_sound = 0;
    for (;;) {
        uint32_t now_added = added;
        if (now_added != seen) {
            seen = now_added;
            last_sound = ms_now();
        }
        if (fd < 0) {
            if (!last_sound || ms_now() - last_sound > IDLE_MS) {
                /* Nothing to play: the device is free, and this thread
                 * sleeps until sound comes (no polling: an idle machine). */
                __atomic_store_n(&idle_waits, 1, __ATOMIC_SEQ_CST);
                if (__atomic_load_n(&added, __ATOMIC_SEQ_CST) == seen) {
                    char b[16];
                    (void)!read(wake[0], b, sizeof(b));
                }
                __atomic_store_n(&idle_waits, 0, __ATOMIC_SEQ_CST);
                continue;
            }
            fd = open("/dev/pcm0", O_WRONLY);
            if (fd < 0) {
                if (errno != last_error)
                    dprintf(2, "soundd: /dev/pcm0: %s\n", strerror(errno));
                last_error = errno;
                mix(out);                /* (at real time, unheard) */
                usleep(CHUNK * 1000000 / 48000);
                continue;
            }
            last_error = 0;
        }
        if (ms_now() - last_sound > IDLE_MS) {
            close(fd);                   /* (its buffer is short: the close is quick) */
            fd = -1;
            device_delay = 0;
            continue;
        }
        int d = 0;
        if (ioctl(fd, SNDCTL_DSP_GETODELAY, &d) == 0) {
            device_delay = d;
            if (d > DEVICE_KEEP) {
                usleep((useconds_t)((int64_t)(d - DEVICE_KEEP) * 1000000 / (48000 * 4)));
                continue;
            }
        }
        mix(out);
        if (write(fd, out, sizeof(out)) <= 0) {
            close(fd);
            fd = -1;
        }
    }
    return 1;
}

int main(void)
{
    settings_load();                     /* (68x) */
    int listener = ritteros_local_listen(SOUND_SOCKET, 8);
    if (listener < 0) {
        dprintf(2, "soundd: cannot listen on %s: %s\n", SOUND_SOCKET, strerror(errno));
        return 1;
    }
    if (pipe(wake) < 0 || fcntl(wake[1], F_SETFL, O_NONBLOCK) < 0) {
        dprintf(2, "soundd: no pipe: %s\n", strerror(errno));
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
         * only; (68z, the user's) root's (an elf: the system's mixer) root's
         * and the console user's, as /dev/pcm0 opens for them: a user
         * logged in from elsewhere is not heard on this machine. */
        int uid = ritteros_peer_uid(sock);
        if (getuid() != 0 && uid != (int)getuid() && uid != 0) {
            printf("soundd: refused a program of user %d (this is user %d's sound)\n", uid,
                   (int)getuid());
            close(sock);
            continue;
        }
        if (getuid() == 0 && ritteros_sound_may_play(sock) != 1) {
            printf("soundd: refused a program of user %d (not the user at the console)\n", uid);
            close(sock);
            continue;
        }
        if (thread_create(client_thread, (void *)(long)sock) < 0)
            close(sock);
    }
    return 0;
}
