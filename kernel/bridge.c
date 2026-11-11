/*
 * Bridges (see bridge.h). Each bridge has its ports and a table of the MAC
 * addresses heard on them, both under its spinlock; a port's `bridge`
 * pointer (in its struct netdev) sends its received frames here. Frames
 * are sent outside the lock: a driver's send may take locks of its own.
 * A removed bridge keeps its structure and its place in the card list for
 * the next bridge made (a receive thread may still be in bridge_input).
 */
#include "bridge.h"
#include "heap.h"
#include "ipv4.h"
#include "kprintf.h"
#include "net.h"
#include "string.h"
#include "sync.h"
#include "syscalls.h"
#include "timer.h"

#define TABLE           256             /* learned addresses (a power of two) */
#define PROBES          8               /* places an address may take in the table */
#define AGE_TICKS       (300 * TIMER_HZ)    /* unheard this long: forgotten (802.1D) */

struct learned {
    uint8_t mac[6];
    struct netdev *port;                /* 0: a free entry */
    uint64_t heard;                     /* timer ticks */
};

struct bridge {
    struct netdev dev;                  /* brN in the stack's list */
    int number;
    bool removed;
    struct spinlock lock;               /* ports, table */
    struct netdev *ports[BRIDGE_PORTS];
    int port_count;
    struct learned table[TABLE];
    uint64_t passed, flooded, same_port, looped;
};

static struct bridge *bridges[BRIDGE_MAX];
static struct mutex bridges_lock = MUTEX_INIT("bridges");

const char *bridge_name(const struct bridge *b)
{
    return b->dev.name;
}

static unsigned slot_of(const uint8_t *mac)
{
    uint32_t v = (uint32_t)mac[2] << 24 | (uint32_t)mac[3] << 16 | (uint32_t)mac[4] << 8 | mac[5];
    return (v * 2654435761u) >> 24 & (TABLE - 1);
}

/* The port mac was last heard on (0: not known, or too long ago), under lock. */
static struct netdev *find(struct bridge *b, const uint8_t *mac, uint64_t now)
{
    unsigned at = slot_of(mac);
    for (int k = 0; k < PROBES; k++) {
        struct learned *e = &b->table[(at + k) & (TABLE - 1)];
        if (e->port && !memcmp(e->mac, mac, 6))
            return now - e->heard < AGE_TICKS ? e->port : 0;
    }
    return 0;
}

/* mac was heard on port: its entry, or a free or the oldest one of its
 * places, under lock. */
static void learn(struct bridge *b, const uint8_t *mac, struct netdev *port, uint64_t now)
{
    unsigned at = slot_of(mac);
    struct learned *use = 0;
    for (int k = 0; k < PROBES; k++) {
        struct learned *e = &b->table[(at + k) & (TABLE - 1)];
        if (e->port && !memcmp(e->mac, mac, 6)) {
            use = e;
            break;
        }
        if (!use || (use->port && (!e->port || e->heard < use->heard)))
            use = e;
    }
    memcpy(use->mac, mac, 6);
    use->port = port;
    use->heard = now;
}

/* The ports to send a frame out (all but `except`) into out[]: their count. */
static int ports_but(struct bridge *b, const struct netdev *except, struct netdev **out)
{
    int n = 0;
    for (int i = 0; i < b->port_count; i++)
        if (b->ports[i] != except)
            out[n++] = b->ports[i];
    return n;
}

void bridge_input(struct bridge *b, struct netdev *in, const uint8_t *frame, size_t length)
{
    if (length < ETH_HEADER)
        return;
    const uint8_t *to = frame, *from = frame + 6;
    bool group = to[0] & 1;
    bool ours = group || !memcmp(to, b->dev.mac, 6);
    struct netdev *out[BRIDGE_PORTS];
    int n = 0;
    uint64_t now = timer_ticks();
    uint64_t state = spin_lock(&b->lock);
    if (!memcmp(from, b->dev.mac, 6)) {
        b->looped++;                            /* our own frame come back: a loop */
        spin_unlock(&b->lock, state);
        return;
    }
    if (!(from[0] & 1))
        learn(b, from, in, now);
    if (group) {
        n = ports_but(b, in, out);
        b->flooded++;
    } else if (!ours) {
        struct netdev *port = find(b, to, now);
        if (port == in) {
            b->same_port++;                     /* its own segment's: nothing to do */
        } else if (port) {
            out[n++] = port;
            b->passed++;
        } else {
            n = ports_but(b, in, out);
            b->flooded++;
        }
    }
    spin_unlock(&b->lock, state);
    for (int i = 0; i < n; i++)
        out[i]->send(out[i], frame, length);
    if (ours) {
        b->dev.rx_frames++;
        b->dev.rx_bytes += length;
        ether_input(&b->dev, frame, length);
    }
}

/* The stack sends through brN: out the port its destination is on, else
 * out every port. */
static bool bridge_send(struct netdev *d, const void *frame, size_t length)
{
    struct bridge *b = d->priv;
    const uint8_t *to = frame;
    struct netdev *out[BRIDGE_PORTS];
    int n = 0;
    uint64_t state = spin_lock(&b->lock);
    struct netdev *port = to[0] & 1 ? 0 : find(b, to, timer_ticks());
    if (port)
        out[n++] = port;
    else
        n = ports_but(b, 0, out);
    spin_unlock(&b->lock, state);
    bool sent = false;
    for (int i = 0; i < n; i++)
        sent |= out[i]->send(out[i], frame, length);
    if (sent) {
        d->tx_frames++;
        d->tx_bytes += length;
    } else {
        d->tx_dropped++;
    }
    return sent;
}

/* Up when any port's link is (its fastest). (38d) The stack asks through
 * net_link, which keeps it for a timer tick: the ports' links (each a read
 * of its chip) are read at most once a tick, as every card's. */
static uint32_t bridge_link(struct netdev *d)
{
    struct bridge *b = d->priv;
    struct netdev *ports[BRIDGE_PORTS];
    uint64_t state = spin_lock(&b->lock);
    int n = ports_but(b, 0, ports);
    spin_unlock(&b->lock, state);
    uint32_t best = 0;
    for (int i = 0; i < n; i++) {
        uint32_t speed = ports[i]->link_speed(ports[i]);
        best = speed > best ? speed : best;
    }
    return best;
}

static void bridge_details(struct netdev *d)
{
    struct bridge *b = d->priv;
    struct netdev *ports[BRIDGE_PORTS];
    uint64_t now = timer_ticks();
    uint64_t state = spin_lock(&b->lock);
    int n = ports_but(b, 0, ports);
    int known = 0;
    for (int i = 0; i < TABLE; i++)
        known += b->table[i].port && now - b->table[i].heard < AGE_TICKS;
    uint64_t passed = b->passed, flooded = b->flooded, same = b->same_port, looped = b->looped;
    spin_unlock(&b->lock, state);
    char list[BRIDGE_PORTS * 9 + 1] = "";
    for (int i = 0; i < n; i++)
        ksnprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i ? " " : "",
                  ports[i]->name);
    kprintf("       bridge: ports %s; %d address(es) known; %lu frame(s) passed on, %lu sent to "
            "every port, %lu for their own port, %lu come back (a loop)\n", n ? list : "none",
            known, passed, flooded, same, looped);
}

/* Take port d out of b (its entries forgotten), under bridges_lock. */
static void leave(struct bridge *b, struct netdev *d)
{
    __atomic_store_n(&d->bridge, (struct bridge *)0, __ATOMIC_RELEASE);
    uint64_t state = spin_lock(&b->lock);
    for (int i = 0; i < b->port_count; i++)
        if (b->ports[i] == d) {
            b->ports[i] = b->ports[--b->port_count];
            break;
        }
    for (int i = 0; i < TABLE; i++)
        if (b->table[i].port == d)
            b->table[i].port = 0;
    spin_unlock(&b->lock, state);
    net_set_promiscuous(d, false);
    kprintf("%s: %s is no longer its port\n", b->dev.name, d->name);
}

/* A new bridge brN (N want, or the lowest free one: -1), or a removed one
 * made again, with d's MAC address, under bridges_lock: its number, or
 * -ERR_*. */
static int make(int want, struct netdev *d)
{
    int k = want >= 0 ? want : 0;
    while (want < 0 && k < BRIDGE_MAX && bridges[k] && !bridges[k]->removed)
        k++;
    if (k >= BRIDGE_MAX)
        return -ERR_NOSPC;
    if (bridges[k] && !bridges[k]->removed)
        return -ERR_EXIST;
    struct bridge *b = bridges[k];
    bool fresh = !b;
    if (fresh) {
        b = kzalloc(sizeof(*b));
        if (!b)
            return -ERR_NOMEM;
        b->number = k;
        b->lock = (struct spinlock)SPINLOCK_INIT("bridge");
        b->dev.driver = "bridge";
        ksnprintf(b->dev.model, sizeof(b->dev.model), "bridge");
        ksnprintf(b->dev.name, sizeof(b->dev.name), "br%d", k);
        b->dev.link_speed = bridge_link;
        b->dev.send = bridge_send;
        b->dev.details = bridge_details;
        b->dev.priv = b;
    }
    uint64_t state = spin_lock(&b->lock);
    memset(b->table, 0, sizeof(b->table));
    b->passed = b->flooded = b->same_port = b->looped = 0;
    spin_unlock(&b->lock, state);
    memcpy(b->dev.mac, d->mac, 6);
    ksnprintf(b->dev.where, sizeof(b->dev.where), "its ports");
    if (fresh) {
        if (!net_register_virtual(&b->dev)) {
            kfree(b);
            return -ERR_NOSPC;
        }
        bridges[k] = b;
    } else {
        net_virtual_back(&b->dev);
    }
    b->removed = false;
    const uint8_t *m = b->dev.mac;
    kprintf("br%d: a bridge, MAC %02x:%02x:%02x:%02x:%02x:%02x (%s's)\n", k, m[0], m[1], m[2],
            m[3], m[4], m[5], d->name);
    return k;
}

int bridge_add(int number, int new_number, struct netdev *d)
{
    if (d->virtual_card && !strncmp(d->name, "br", 2))
        return -ERR_INVAL;                      /* a bridge in a bridge */
    if (d->wlan || !d->set_promiscuous)
        return -ERR_NOTSUP;
    if (number >= BRIDGE_MAX)
        return -ERR_NOENT;
    mutex_lock(&bridges_lock);
    int r = d->bridge ? -ERR_BUSY : number >= 0 && (!bridges[number] || bridges[number]->removed)
                                    ? -ERR_NOENT : 0;
    if (r == 0 && number >= 0 && bridges[number]->port_count == BRIDGE_PORTS)
        r = -ERR_NOSPC;
    if (r == 0 && number < 0)
        r = number = make(new_number, d);
    if (r >= 0) {
        struct bridge *b = bridges[number];
        net_set_promiscuous(d, true);           /* first: frames for others come in */
        net_card_quiet(d);                      /* its own addresses go (brN has them) */
        uint64_t state = spin_lock(&b->lock);
        b->ports[b->port_count++] = d;
        spin_unlock(&b->lock, state);
        __atomic_store_n(&d->bridge, b, __ATOMIC_RELEASE);
        kprintf("%s: %s is its port now\n", b->dev.name, d->name);
        r = number;
    }
    mutex_unlock(&bridges_lock);
    return r;
}

int bridge_remove(int number, struct netdev *d)
{
    if (number < 0 || number >= BRIDGE_MAX)
        return -ERR_NOENT;
    mutex_lock(&bridges_lock);
    struct bridge *b = bridges[number];
    int r = !b || b->removed || (d && d->bridge != b) ? -ERR_NOENT : 0;
    if (r == 0 && d) {
        leave(b, d);
    } else if (r == 0) {
        while (b->port_count > 0)
            leave(b, b->ports[b->port_count - 1]);
        b->removed = true;
        net_virtual_gone(&b->dev);
        kprintf("%s: removed\n", b->dev.name);
    }
    mutex_unlock(&bridges_lock);
    return r;
}
