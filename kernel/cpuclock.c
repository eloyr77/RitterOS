/*
 * The processor's clock setting (64t, the user's): /system/config/cpu,
 * "clock = performance | balanced | powersave | top | firmware" (the file's
 * own comments explain them; scripts/mkdisk.py's CPU), read once after the
 * system's file system is mounted, so a change counts from the next start.
 * Without the file or the key: performance. The architecture applies it
 * where the processor lets it (arch_clock_set: AMD's CPPC) and says what
 * it did; elsewhere the boot log says the setting is not used.
 */
#include "arch_api.h"
#include "cpuclock.h"
#include "kprintf.h"
#include "mounts.h"
#include "string.h"
#include "vfs.h"

#define CPU_FILE        "/system/config/cpu"
#define CPU_FILE_MAX    4096

static const struct {
    const char *name;
    enum arch_clock mode;
} modes[] = {
    { "performance", ARCH_CLOCK_PERFORMANCE }, { "balanced", ARCH_CLOCK_BALANCED },
    { "powersave", ARCH_CLOCK_POWERSAVE },     { "top", ARCH_CLOCK_TOP },
    { "firmware", ARCH_CLOCK_FIRMWARE },
};

static bool blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* The value of `clock` in text (a "key = value" line; # starts a comment):
 * its place and length, or false. */
static bool clock_value(const char *text, size_t n, const char **value, size_t *length)
{
    bool found = false;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n')
            end++;
        size_t i = at;
        while (i < end && blank(text[i]))
            i++;
        if (end - i > 5 && !memcmp(text + i, "clock", 5)) {
            i += 5;
            while (i < end && blank(text[i]))
                i++;
            if (i < end && text[i] == '=') {
                i++;
                while (i < end && blank(text[i]))
                    i++;
                size_t stop = i;
                while (stop < end && !blank(text[stop]) && text[stop] != '#')
                    stop++;
                *value = text + i, *length = stop - i;
                found = true;           /* (the last one counts) */
            }
        }
        at = end + 1;
    }
    return found;
}

void cpu_clock_load(void)
{
    static char text[CPU_FILE_MAX];
    struct vfs *fs = mounts_root();
    uint64_t id;
    struct rfs_inode inode;
    int64_t got = fs && vfs_lookup(fs, CPU_FILE, &id, &inode)
                  ? vfs_read(fs, id, &inode, 0, text, sizeof(text)) : -1;
    const char *value = "performance";
    size_t length = strlen(value);
    const char *source = got < 0 ? "the default: no " CPU_FILE : CPU_FILE;
    if (got >= 0 && !clock_value(text, (size_t)got, &value, &length)) {
        value = "performance", length = strlen(value);
        source = "the default: no clock in " CPU_FILE;
    }
    int k = 0;
    while (k < (int)(sizeof(modes) / sizeof(modes[0])) &&
           (strlen(modes[k].name) != length || memcmp(modes[k].name, value, length)))
        k++;
    if (k == (int)(sizeof(modes) / sizeof(modes[0]))) {
        kprintf("  %-15sclock = %.*s in %s is not a setting: performance\n", "cpu:",
                (int)(length < 32 ? length : 32), value, CPU_FILE);
        k = 0;
    }
    char done[120];
    if (arch_clock_set(modes[k].mode, done, sizeof(done)))
        kprintf("  %-15sclock = %s (%s): %s\n", "cpu:", modes[k].name, source, done);
    else
        kprintf("  %-15sclock = %s (%s): not used, this processor's clock is the firmware's\n",
                "cpu:", modes[k].name, source);
}
