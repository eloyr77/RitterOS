/*
 * Network devices: the list of Ethernet cards (see net.h).
 */
#include "net.h"
#include "bridge.h"
#include "wlan.h"
#include "pci.h"
#include "ipv4.h"
#include "ipv6.h"
#include "heap.h"
#include "kprintf.h"
#include "mounts.h"
#include "string.h"
#include "sched.h"
#include "sync.h"
#include "tcp.h"
#include "udp.h"
#include "timer.h"
#include "vfs.h"

#define LINK_LOOK   100             /* ms between looks at the links */

static struct netdev *devs[MAX_NETDEVS];
static uint32_t link_shown[MAX_NETDEVS];    /* the speed last reported; 0: no link */
static int count;
static struct mutex lock = MUTEX_INIT("netdevs");
static bool announced, watching;         /* the boot's lines are out; the thread link runs */
static void start_link_watch(void);

/* --- names (50f) ---------------------------------------------------------
 * /system/config/network/names: "NAME = MAC" lines (and # comments), made
 * by the elf netnamesd as cards are found, edited by people and by
 * `net forget`. The kernel only reads it: at the mount and at each card
 * found later (so an edit counts from the next card plugged in). */

#define NAMES_FILE      "/system/config/network/names"
#define NAMES_MAX       64
#define NAMES_FILE_MAX  8192

struct name_entry {
    char name[8];
    uint8_t mac[6];
};

static struct name_entry names[NAMES_MAX];
static int name_count;
static bool names_loaded;               /* after the mount: names come from the file */

static int hex_value(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* "eth3" for a wired card, "wlan0" for a wireless one: the kinds stay apart. */
static bool name_fits(const char *name, bool wireless)
{
    const char *prefix = wireless ? "wlan" : "eth";
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n) || !name[n] || strlen(name + n) > 2)
        return false;
    for (const char *c = name + n; *c; c++)
        if (*c < '0' || *c > '9')
            return false;
    return name[n] != '0' || !name[n + 1];      /* no leading zero */
}

/* One line: NAME = 00:11:22:33:44:55; false for anything else. */
static bool parse_entry(const char *line, size_t len, struct name_entry *e)
{
    size_t i = 0, k = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '=' && k < sizeof(e->name) - 1)
        e->name[k++] = line[i++];
    e->name[k] = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    if (!k || i >= len || line[i++] != '=')
        return false;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    for (int b = 0; b < 6; b++) {
        int hi = i < len ? hex_value(line[i]) : -1, lo = i + 1 < len ? hex_value(line[i + 1]) : -1;
        if (hi < 0 || lo < 0)
            return false;
        e->mac[b] = (uint8_t)(hi << 4 | lo);
        i += 2;
        if (b < 5 && (i >= len || line[i++] != ':'))
            return false;
    }
    while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r'))
        i++;
    return i == len && (name_fits(e->name, false) || name_fits(e->name, true));
}

/* The file into the table (an absent file: an empty table). It sleeps:
 * never called with the card list's lock held. */
static void names_read(void)
{
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    static struct name_entry fresh[NAMES_MAX];
    static char text[NAMES_FILE_MAX];
    static struct mutex reading = MUTEX_INIT("netnames");
    mutex_lock(&reading);
    int n = 0;
    int64_t got = fs && vfs_lookup(fs, NAMES_FILE, &id, &inode)
                  ? vfs_read(fs, id, &inode, 0, text, sizeof(text)) : 0;
    for (int64_t at = 0; got > 0 && at < got && n < NAMES_MAX;) {
        int64_t end = at;
        while (end < got && text[end] != '\n')
            end++;
        if (text[at] != '#' && parse_entry(text + at, (size_t)(end - at), &fresh[n]))
            n++;
        at = end + 1;
    }
    mutex_lock(&lock);
    memcpy(names, fresh, sizeof(struct name_entry) * (size_t)n);
    name_count = n;
    mutex_unlock(&lock);
    mutex_unlock(&reading);
}

static bool listed(const char *name)
{
    for (int i = 0; i < name_count; i++)
        if (!strcmp(names[i].name, name))
            return true;
    return false;
}

/* The name taken[k] holds (k < n), or false: the cards named so far. */
static bool taken(char (*given)[8], int n, const char *name)
{
    for (int k = 0; k < n; k++)
        if (!strcmp(given[k], name))
            return true;
    return false;
}

/* d's name from the table, else the lowest free one of its kind; the
 * names in given[0..n) are the other cards'. With the lock held. */
static void choose_name(struct netdev *d, char (*given)[8], int n, char out[8])
{
    bool wireless = d->wlan != 0;
    for (int i = 0; i < name_count; i++)
        if (!memcmp(names[i].mac, d->mac, 6) && name_fits(names[i].name, wireless) &&
            !taken(given, n, names[i].name)) {
            memcpy(out, names[i].name, 8);
            return;
        }
    for (int k = 0; k < 100; k++) {
        ksnprintf(out, 8, wireless ? "wlan%d" : "eth%d", k);
        if (!listed(out) && !taken(given, n, out))
            return;
    }
}

/* (67b) A module's card (driver_net.h): struct net_card is the first
 * fields of struct netdev, field for field. */
#define SAME(field) _Static_assert(offsetof(struct netdev, field) == offsetof(struct net_card, field) && \
                                   sizeof(((struct netdev *)0)->field) == sizeof(((struct net_card *)0)->field), \
                                   "struct net_card's " #field " is not struct netdev's")
SAME(name); SAME(mac); SAME(driver); SAME(model); SAME(where); SAME(link_speed); SAME(send);
SAME(pci); SAME(details); SAME(set_multicast); SAME(priv); SAME(wlan); SAME(rx_frames);
SAME(rx_bytes); SAME(tx_frames); SAME(tx_bytes); SAME(tx_dropped); SAME(gone);
SAME(set_promiscuous);
#undef SAME

struct net_card *net_card_new(void)
{
    return (struct net_card *)kzalloc(sizeof(struct netdev));     /* the stack's part with it */
}

bool net_receiving(void)
{
    return thread_current()->net_rx;
}

static void link_text(struct netdev *d, char *out, size_t size);

void net_register(struct netdev *d)
{
    if (names_loaded)
        names_read();                           /* as the file is now (edits count) */
    mutex_lock(&lock);
    if (count < MAX_NETDEVS) {
        if (names_loaded) {
            static char given[MAX_NETDEVS][8];
            int n = 0;
            for (int i = 0; i < count; i++)
                if (!devs[i]->gone)             /* an unplugged card's name may be taken */
                    memcpy(given[n++], devs[i]->name, 8);
            choose_name(d, given, n, d->name);
        } else {
            int same = 0;                       /* eth0, eth1, ...; wlan0, wlan1, ... */
            for (int i = 0; i < count; i++)
                same += !devs[i]->wlan == !d->wlan;
            ksnprintf(d->name, sizeof(d->name), d->wlan ? "wlan%d" : "eth%d", same);
        }
        devs[count++] = d;
    }
    mutex_unlock(&lock);
    if (announced) {
        char label[24], link[24];               /* (67a) as net_announce's line */
        ksnprintf(label, sizeof(label), "%s:", d->name);
        link_text(d, link, sizeof(link));
        kprintf("  %-15s%s at %s (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x, %s\n", label, d->model,
                d->where, d->driver, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4],
                d->mac[5], link);
        start_link_watch();                     /* the first card came after the boot (a USB one, a module's) */
    }
}

bool net_register_virtual(struct netdev *d)
{
    mutex_lock(&lock);
    bool room = count < MAX_NETDEVS;
    if (room) {
        d->virtual_card = true;
        link_shown[count] = 0;
        devs[count++] = d;
    }
    mutex_unlock(&lock);
    if (room)
        start_link_watch();
    return room;
}

void net_virtual_gone(struct netdev *d)
{
    net_card_quiet(d);
    d->gone = true;
}

void net_virtual_back(struct netdev *d)
{
    d->gone = false;
}

void net_rename(struct netdev *d)
{
    if (!names_loaded)
        return;
    names_read();
    static char given[MAX_NETDEVS][8];
    mutex_lock(&lock);
    int n = 0;
    for (int i = 0; i < count; i++)
        if (devs[i] != d && !devs[i]->gone)
            memcpy(given[n++], devs[i]->name, 8);
    char before[8];
    memcpy(before, d->name, 8);
    choose_name(d, given, n, d->name);
    mutex_unlock(&lock);
    if (strcmp(before, d->name))
        kprintf("  %-15s%s is %s now (%s)\n", "net:", before, d->name, NAMES_FILE);
}

void net_names_load(void)
{
    names_read();
    static char before[MAX_NETDEVS][8], given[MAX_NETDEVS][8];
    mutex_lock(&lock);
    names_loaded = true;
    /* The cards the file names first (in the order found), then the rest:
     * a listed card never loses its name to one found before it. */
    int n = 0;
    bool done[MAX_NETDEVS] = { false };
    for (int i = 0; i < count; i++)
        memcpy(before[i], devs[i]->name, 8);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < count; i++) {
            if (done[i] || devs[i]->virtual_card)
                continue;               /* (38a) a tap keeps the name it was given */
            bool in_file = false;
            for (int k = 0; k < name_count && !in_file; k++)
                in_file = !memcmp(names[k].mac, devs[i]->mac, 6) &&
                          name_fits(names[k].name, devs[i]->wlan != 0) &&
                          !taken(given, n, names[k].name);
            if (pass == 0 && !in_file)
                continue;
            choose_name(devs[i], given, n, given[n]);
            memcpy(devs[i]->name, given[n], 8);
            n++;
            done[i] = true;
        }
    int cards = count;
    mutex_unlock(&lock);
    for (int i = 0; i < cards; i++) {
        struct netdev *d = devs[i];
        if (strcmp(before[i], d->name))
            kprintf("  %-15s%s (%02x:%02x:%02x:%02x:%02x:%02x) is %s now (%s)\n", "net:", before[i],
                    d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], d->name,
                    NAMES_FILE);
    }
}

/* (38b) A card's filters change one at a time: the multicast groups (the
 * thread link) and promiscuous mode (a program's call) both rewrite the
 * driver's receive control register. */
static struct mutex filters = MUTEX_INIT("card filters");

uint32_t net_link(struct netdev *d)
{
    if (__atomic_load_n(&d->bridge, __ATOMIC_ACQUIRE) || !d->link_speed)
        return 0;
    uint64_t now = timer_ticks();
    __atomic_fetch_add(&d->link_asked, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&d->link_tick, __ATOMIC_ACQUIRE) == now + 1)
        return __atomic_load_n(&d->link_now, __ATOMIC_RELAXED);
    uint32_t speed = d->link_speed(d);
    __atomic_fetch_add(&d->link_reads, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&d->link_now, speed, __ATOMIC_RELAXED);
    __atomic_store_n(&d->link_tick, now + 1, __ATOMIC_RELEASE);
    return speed;
}

void net_set_promiscuous(struct netdev *d, bool on)
{
    mutex_lock(&filters);
    if (d->promiscuous != on && d->set_promiscuous)
        d->set_promiscuous(d, on);
    d->promiscuous = on;
    mutex_unlock(&filters);
}

void net_card_quiet(struct netdev *d)
{
    d->address = d->prefix = d->gateway = d->dns = 0;
    d->dns_port = 0;
    d->lease_seconds = 0;
}

void net_set_multicast(struct netdev *d, const uint8_t (*macs)[6], int count)
{
    if (count > NET_MULTICAST_MAX)
        macs = 0, count = -1;                   /* too many to list: all of them */
    if (count == d->multicast_count && (count <= 0 || !memcmp(d->multicast, macs, (size_t)count * 6)))
        return;                                 /* as before: the chip is not touched */
    d->multicast_count = count;
    if (count > 0)
        memcpy(d->multicast, macs, (size_t)count * 6);
    mutex_lock(&filters);
    if (d->set_multicast)
        d->set_multicast(d, macs, count);
    mutex_unlock(&filters);
}

void net_receive(struct netdev *d, const void *frame, size_t length)
{
    d->rx_frames++;
    d->rx_bytes += length;
    /* A reply generated while delivering this frame must not wait for ARP
     * in the card's receive thread (its ring would overflow and the TCP
     * mutex be held): send_on queues it instead (49c). */
    struct thread *self = thread_current();
    bool saved = self->net_rx;
    self->net_rx = true;
    struct bridge *b = __atomic_load_n(&d->bridge, __ATOMIC_ACQUIRE);
    if (b)
        bridge_input(b, d, frame, length);  /* (38b) a port: the bridge's */
    else
        ether_input(d, frame, length);
    self->net_rx = saved;
    /* (65e) A long burst (a ring that never runs dry) still acknowledges
     * every NET_BATCH_MAX frames. */
    if (self->net_batch && ++self->net_batch_frames >= NET_BATCH_MAX) {
        self->net_batch_frames = 0;
        tcp_batch_end();
    }
}

void net_batch_begin(void)
{
    thread_current()->net_batch++;
}

void net_batch_end(void)
{
    struct thread *self = thread_current();
    if (--self->net_batch == 0) {
        self->net_batch_frames = 0;
        tcp_batch_end();
    }
}

int net_count(void)
{
    return count;
}

int net_index(const struct netdev *d)
{
    for (int i = 0; i < count; i++)
        if (devs[i] == d)
            return i;
    return -1;
}

struct netdev *net_at(int index)
{
    return index >= 0 && index < count ? devs[index] : 0;
}

static void link_text(struct netdev *d, char *out, size_t size)
{
    uint32_t speed = d->link_speed(d);
    if (d->gone)
        ksnprintf(out, size, "unplugged");
    else if (speed)
        ksnprintf(out, size, "link up, %u Mbit/s", speed);
    else
        ksnprintf(out, size, "no link");
}

/* The thread link: a line whenever a card's link comes, goes or changes
 * its speed (a cable's negotiation takes a few seconds after the reset,
 * so most real cards come up after the boot's lines). */
static void link_watch(void *arg)
{
    (void)arg;
    for (;;) {
        timer_sleep_ms(LINK_LOOK);
        ip6_tick();                             /* IPv6's addresses follow the links (64a) */
        for (int i = 0; i < count; i++) {
            struct netdev *d = devs[i];
            uint32_t speed = d->link_speed(d);
            if (speed == link_shown[i])
                continue;
            link_shown[i] = speed;
            if (speed)
                kprintf("%s: link up, %u Mbit/s\n", d->name, speed);
            else
                kprintf("%s: link down\n", d->name);
        }
    }
}

static void start_link_watch(void)
{
    if (watching)
        return;
    watching = true;
    if (!thread_create("link", link_watch, 0)) {
        watching = false;
        kprintf("  net:           no thread to watch the links\n");
    }
}

void net_announce(void)
{
    for (int i = 0; i < count; i++) {
        struct netdev *d = devs[i];
        char label[24], link[24];
        ksnprintf(label, sizeof(label), "%s:", d->name);
        link_shown[i] = d->link_speed(d);
        link_text(d, link, sizeof(link));
        kprintf("  %-15s%s at %s (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x, %s\n", label, d->model,
                d->where, d->driver, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4],
                d->mac[5], link);
    }
    announced = true;
    if (count)
        start_link_watch();
}

void net_print(void)
{
    struct netdev *lo = ip_loopback();
    kprintf("lo     the loopback, 127.0.0.1/8\n");
    kprintf("       received %lu packets (%lu bytes), sent %lu packets (%lu bytes), %lu not sent\n",
            lo->rx_frames, lo->rx_bytes, lo->tx_frames, lo->tx_bytes, lo->tx_dropped);
    ip6_print_loopback();
    if (count == 0)
        kprintf("no network cards\n");
    for (int i = 0; i < count; i++) {
        struct netdev *d = devs[i];
        char link[24];
        if (d->virtual_card && d->gone)
            continue;                           /* (38a) a removed tap */
        link_text(d, link, sizeof(link));
        kprintf("%-6s %02x:%02x:%02x:%02x:%02x:%02x  %s, %s (%s at %s)\n", d->name, d->mac[0],
                d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5], link, d->model, d->driver,
                d->where);
        char address[16], gateway[16], dns[24], lease[32] = "";
        ip_text(d->address, address, sizeof(address));
        ip_text(d->gateway, gateway, sizeof(gateway));
        ip_text(d->dns, dns, sizeof(dns));
        if (d->dns_port)
            ksnprintf(dns + strlen(dns), sizeof(dns) - strlen(dns), ":%u", d->dns_port);
        if (d->lease_seconds)
            ksnprintf(lease, sizeof(lease), " (DHCP, lease %u s)", d->lease_seconds);
        struct bridge *b = __atomic_load_n(&d->bridge, __ATOMIC_ACQUIRE);
        if (b)
            kprintf("       a port of %s: its frames are the bridge's\n", bridge_name(b));
        else if (d->address)
            kprintf("       address %s/%u%s, gateway %s, DNS %s\n", address, d->prefix, lease,
                    d->gateway ? gateway : "none", d->dns ? dns : "none");
        else
            kprintf("       no address (net up applies /system/config/network/%s)\n", d->name);
        kprintf("       received %lu frames (%lu bytes), sent %lu frames (%lu bytes), %lu not sent\n",
                d->rx_frames, d->rx_bytes, d->tx_frames, d->tx_bytes, d->tx_dropped);
        kprintf("       its link asked %lu times by the stack, %lu of them read from the card\n",
                d->link_asked, d->link_reads);
        ip6_print_card(d);
        if (d->wlan)
            wlan_print(d);
        if (d->details)
            d->details(d);
        if (d->pci)
            pci_print_path(d->pci);
    }
    ip_print_routes();
    ip6_print_routes();
    ip_print_forwarding();
    uint32_t ip;
    uint8_t mac[6];
    for (int i = 0; arp_entry(i, &ip, mac); i++) {
        char text[16];
        ip_text(ip, text, sizeof(text));
        kprintf("arp: %-15s is %02x:%02x:%02x:%02x:%02x:%02x\n", text, mac[0], mac[1], mac[2],
                mac[3], mac[4], mac[5]);
    }
    ip6_print_neighbours();
    tcp_print();
    udp_print();
}
