/*
 * Virtual network cards (see tap.h). Frames the stack sends wait in a ring
 * of bytes, each a 2-byte length and the frame, until the program reads
 * them; the ring is made when the program opens the card and freed when it
 * closes it, so a card nobody holds costs only its structure. A removed
 * tap keeps its structure (and its place in the stack's list, marked gone)
 * for the next tap_add: senders may still hold its pointer.
 */
#include "tap.h"
#include "heap.h"
#include "kprintf.h"
#include "net.h"
#include "random.h"
#include "sched.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"

#define RING_SIZE       (256 * 1024)    /* bytes of frames waiting (a power of two) */
#define TAP_SPEED       10000           /* Mbit/s shown while a program holds it */

struct tap {
    struct netdev dev;                  /* tapN in the stack's list */
    int number;
    uint32_t uid;                       /* who may open /dev/tapN (root too) */
    bool removed;
    struct spinlock lock;               /* the ring, open */
    bool open;                          /* a program holds /dev/tapN */
    uint8_t *ring;                      /* RING_SIZE bytes while open, else 0 */
    uint32_t head, tail;                /* bytes tail..head (counting on, mod RING_SIZE) */
    struct wait_queue readers;
    uint64_t not_ours;                  /* written for another MAC: dropped */
    bool promiscuous;                   /* (38b) a bridge's port: no filter */
};

static struct tap *taps[TAP_MAX];
static struct mutex taps_lock = MUTEX_INIT("taps");

static uint32_t tap_link(struct netdev *d)
{
    struct tap *t = d->priv;
    return t->open ? TAP_SPEED : 0;
}

/* Bytes into the ring at head (room checked by the caller), under lock. */
static void put(struct tap *t, const uint8_t *from, size_t n)
{
    for (size_t i = 0; i < n; i++)
        t->ring[(t->head + i) % RING_SIZE] = from[i];
    t->head += (uint32_t)n;
}

/* The stack sends a frame: it waits for the program (dropped while nobody
 * holds the card or the ring is full). Never sleeps: callers may hold
 * locks. */
static bool tap_send(struct netdev *d, const void *frame, size_t length)
{
    struct tap *t = d->priv;
    if (length > ETH_FRAME_MAX) {
        d->tx_dropped++;
        return false;
    }
    uint64_t state = spin_lock(&t->lock);
    bool ok = t->open && RING_SIZE - (t->head - t->tail) >= length + 2;
    if (ok) {
        uint8_t size[2] = { (uint8_t)length, (uint8_t)(length >> 8) };
        put(t, size, 2);
        put(t, frame, length);
    }
    spin_unlock(&t->lock, state);
    if (!ok) {
        d->tx_dropped++;
        return false;
    }
    d->tx_frames++;
    d->tx_bytes += length;
    wait_queue_wake_all(&t->readers);
    return true;
}

/* (38b) A bridge's port takes frames for every address. */
static void tap_promiscuous(struct netdev *d, bool on)
{
    ((struct tap *)d->priv)->promiscuous = on;
}

static void tap_details(struct netdev *d)
{
    struct tap *t = d->priv;
    uint64_t state = spin_lock(&t->lock);
    uint32_t waiting = t->head - t->tail;
    bool open = t->open;
    spin_unlock(&t->lock, state);
    kprintf("       the program's side: /dev/tap%d, user %u, %s; %u bytes waiting, "
            "%lu frame(s) for other cards dropped\n", t->number, t->uid,
            open ? "open" : "not open", waiting, t->not_ours);
}

int tap_add(uint32_t uid, int want)
{
    mutex_lock(&taps_lock);
    int k = want >= 0 ? want : 0;
    while (want < 0 && k < TAP_MAX && taps[k] && !taps[k]->removed)
        k++;
    if (k >= TAP_MAX || (taps[k] && !taps[k]->removed)) {
        mutex_unlock(&taps_lock);
        return k >= TAP_MAX ? -ERR_NOSPC : -ERR_EXIST;
    }
    struct tap *t = taps[k];
    bool fresh = !t;
    if (fresh) {
        t = kzalloc(sizeof(*t));
        if (!t) {
            mutex_unlock(&taps_lock);
            return -ERR_NOMEM;
        }
        t->number = k;
        t->lock = (struct spinlock)SPINLOCK_INIT("tap");
        t->readers.pollable = true;
        t->dev.driver = "tap";
        ksnprintf(t->dev.model, sizeof(t->dev.model), "virtual card");
        ksnprintf(t->dev.where, sizeof(t->dev.where), "/dev/tap%d", k);
        ksnprintf(t->dev.name, sizeof(t->dev.name), "tap%d", k);
        t->dev.link_speed = tap_link;
        t->dev.send = tap_send;
        t->dev.details = tap_details;
        t->dev.set_promiscuous = tap_promiscuous;
        t->dev.priv = t;
    }
    t->uid = uid;
    t->not_ours = 0;
    random_bytes(t->dev.mac, 6);
    t->dev.mac[0] = (uint8_t)((t->dev.mac[0] & 0xFE) | 0x02);  /* one card's, locally made */
    if (fresh) {
        if (!net_register_virtual(&t->dev)) {
            mutex_unlock(&taps_lock);
            kfree(t);
            return -ERR_NOSPC;
        }
        taps[k] = t;
    } else {
        net_virtual_back(&t->dev);
    }
    t->removed = false;
    mutex_unlock(&taps_lock);
    const uint8_t *m = t->dev.mac;
    kprintf("tap%d: a virtual card for user %u, MAC %02x:%02x:%02x:%02x:%02x:%02x (/dev/tap%d)\n",
            k, uid, m[0], m[1], m[2], m[3], m[4], m[5], k);
    return k;
}

int tap_remove(int number)
{
    if (number < 0 || number >= TAP_MAX)
        return -ERR_NOENT;
    mutex_lock(&taps_lock);
    struct tap *t = taps[number];
    int r = !t || t->removed ? -ERR_NOENT : t->open ? -ERR_BUSY : 0;
    if (r == 0) {
        t->removed = true;
        net_virtual_gone(&t->dev);
    }
    mutex_unlock(&taps_lock);
    if (r == 0)
        kprintf("tap%d: removed\n", number);
    return r;
}

int tap_open(int number, uint32_t uid, struct tap **out)
{
    if (number < 0 || number >= TAP_MAX)
        return -ERR_NOENT;
    uint8_t *ring = kmalloc(RING_SIZE);         /* made before the locks (it may sleep) */
    if (!ring)
        return -ERR_NOMEM;
    mutex_lock(&taps_lock);
    struct tap *t = taps[number];
    int r = !t || t->removed ? -ERR_NOENT : uid != 0 && uid != t->uid ? -ERR_ACCES : 0;
    if (r == 0) {
        uint64_t state = spin_lock(&t->lock);
        if (t->open) {
            r = -ERR_BUSY;
        } else {
            t->open = true;
            t->ring = ring;
            t->head = t->tail = 0;
            ring = 0;
        }
        spin_unlock(&t->lock, state);
    }
    mutex_unlock(&taps_lock);
    kfree(ring);                                /* not taken */
    if (r == 0)
        *out = t;
    return r;
}

void tap_close(struct tap *t)
{
    uint64_t state = spin_lock(&t->lock);
    uint8_t *ring = t->ring;
    t->open = false;
    t->ring = 0;
    t->head = t->tail = 0;
    spin_unlock(&t->lock, state);
    wait_queue_wake_all(&t->readers);
    kfree(ring);
}

int64_t tap_read(struct tap *t, uint8_t *buf, size_t size)
{
    for (;;) {
        uint64_t state = spin_lock(&t->lock);
        if (t->open && t->head != t->tail) {
            size_t length = t->ring[t->tail % RING_SIZE] |
                            (size_t)t->ring[(t->tail + 1) % RING_SIZE] << 8;
            size_t n = length < size ? length : size;
            for (size_t i = 0; i < n; i++)
                buf[i] = t->ring[(t->tail + 2 + i) % RING_SIZE];
            t->tail += (uint32_t)(length + 2);
            spin_unlock(&t->lock, state);
            return (int64_t)n;
        }
        spin_unlock(&t->lock, state);
        if (thread_current()->dont_wait)
            return -ERR_AGAIN;
        if (!wait_event_interruptible(&t->readers, t->head != t->tail || !t->open))
            return -ERR_INTR;
        if (!t->open)
            return 0;                           /* (only after a close: never for its holder) */
    }
}

int64_t tap_write(struct tap *t, const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER || length > ETH_FRAME_MAX)
        return -ERR_INVAL;
    /* A card's filter: frames for its own MAC, broadcast and multicast (a
     * bridge's port: every frame). */
    if (!t->promiscuous && !(frame[0] & 1) && memcmp(frame, t->dev.mac, 6)) {
        t->not_ours++;
        return (int64_t)length;
    }
    net_batch_begin();
    net_receive(&t->dev, frame, length);
    net_batch_end();
    return (int64_t)length;
}

int tap_poll(struct tap *t)
{
    poll_watch(&t->readers);                    /* (65e) before the state is read */
    uint64_t state = spin_lock(&t->lock);
    bool waiting = t->head != t->tail;
    spin_unlock(&t->lock, state);
    return (waiting ? POLLIN : 0) | POLLOUT;
}

bool tap_present(int number)
{
    if (number < 0 || number >= TAP_MAX)
        return false;
    mutex_lock(&taps_lock);
    bool there = taps[number] && !taps[number]->removed;
    mutex_unlock(&taps_lock);
    return there;
}
