#include "devname.h"
#include "string.h"
#include "sync.h"

#define MAX_NAMES   64

struct entry {
    char kind[12];
    int number;
    uint16_t vendor, product;
    char serial[64];
    char path[32];
    bool connected;
};

static struct entry entries[MAX_NAMES];
static int entry_count;
static struct spinlock lock = SPINLOCK_INIT("devname");

static void copy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src) < size - 1 ? strlen(src) : size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* The reserved, unplugged entry this device is, or 0. */
static struct entry *find(const char *kind, const struct devname_id *id)
{
    struct entry *same_ids = 0, *same_path = 0;
    int candidates = 0;
    for (int i = 0; i < entry_count; i++) {
        struct entry *e = &entries[i];
        if (e->connected || strcmp(e->kind, kind) != 0 || e->vendor != id->vendor ||
            e->product != id->product)
            continue;
        if (id->serial[0] || e->serial[0]) {
            if (strcmp(e->serial, id->serial) == 0)
                return e;                   /* a serial number identifies it */
            continue;
        }
        candidates++;
        if (!same_ids || e->number < same_ids->number)
            same_ids = e;
        if (strcmp(e->path, id->path) == 0)
            same_path = e;
    }
    if (candidates > 1 && same_path)
        return same_path;
    return same_ids;
}

int devname_attach(const char *kind, const struct devname_id *id, bool *again)
{
    uint64_t state = spin_lock(&lock);
    struct entry *e = find(kind, id);
    *again = e != 0;
    if (!e && entry_count < MAX_NAMES) {
        int next = 0;
        for (int i = 0; i < entry_count; i++)
            if (strcmp(entries[i].kind, kind) == 0 && entries[i].number >= next)
                next = entries[i].number + 1;
        e = &entries[entry_count++];
        copy(e->kind, kind, sizeof(e->kind));
        e->number = next;
        e->vendor = id->vendor;
        e->product = id->product;
        copy(e->serial, id->serial, sizeof(e->serial));
    }
    int number = -1;
    if (e) {
        copy(e->path, id->path, sizeof(e->path));     /* where it is now */
        e->connected = true;
        number = e->number;
    }
    spin_unlock(&lock, state);
    return number;
}

void devname_detach(const char *kind, int number)
{
    uint64_t state = spin_lock(&lock);
    for (int i = 0; i < entry_count; i++)
        if (entries[i].number == number && strcmp(entries[i].kind, kind) == 0)
            entries[i].connected = false;
    spin_unlock(&lock, state);
}
