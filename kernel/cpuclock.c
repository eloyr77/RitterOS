/*
 * The processor's clock setting (64t, the user's): /system/config/cpu,
 * "clock = performance | balanced | powersave | top | firmware" (the file's
 * own comments explain them; scripts/mkdisk.py's CPU), read once after the
 * system's file system is mounted. Without the file or the key:
 * performance. The architecture applies it where the processor lets it
 * (arch_clock_set: AMD's CPPC) and says what it did; elsewhere the boot log
 * says the setting is not used.
 *
 * (69a) The request is each core's own: the mode is kept here and every
 * core applies it (cpu_clock_core): when asked (a reread: SYS_POWER's
 * POWER_CLOCK, `power clock`), when it starts and after a sleep. Until 69a
 * only the boot core had it (the other cores kept the firmware's).
 */
#include "arch_api.h"
#include "cpuclock.h"
#include "cpus.h"
#include "kprintf.h"
#include "log.h"
#include "mounts.h"
#include "percpu.h"
#include "string.h"
#include "sync.h"
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

static struct mutex settings = MUTEX_INIT("cpu clock settings");
static volatile int kept_mode = -1;     /* the mode every core applies (-1: none yet) */
static bool cppc_on;                    /* a mode other than firmware was applied */

void cpu_clock_core(void)
{
    int mode = __atomic_load_n(&kept_mode, __ATOMIC_ACQUIRE);
    char done[120];
    if (mode >= 0 && mode != ARCH_CLOCK_FIRMWARE)
        arch_clock_set((enum arch_clock)mode, done, sizeof(done));
}

/* One line: to the log, and to out (a program's) when there is one (then
 * the log only, not the program's terminal). */
static void say(void (*out)(const char *line, void *ctx), void *ctx, const char *line)
{
    if (out) {
        char logged[250];
        ksnprintf(logged, sizeof(logged), "  %-15s%s\n", "cpu:", line);
        log_append("kernel", logged, strlen(logged));
        out(line, ctx);
    } else {
        kprintf("  %-15s%s\n", "cpu:", line);
    }
}

void cpu_clock_load(void (*out)(const char *line, void *ctx), void *ctx)
{
    mutex_lock(&settings);
    static char text[CPU_FILE_MAX];
    char line[220];
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
        ksnprintf(line, sizeof(line), "clock = %.*s in %s is not a setting: performance",
                  (int)(length < 32 ? length : 32), value, CPU_FILE);
        say(out, ctx, line);
        k = 0;
    }
    char done[120];
    if (modes[k].mode == ARCH_CLOCK_FIRMWARE && cppc_on) {
        /* CPPC_ENABLE stays on until a reset: the cores keep the mode they
         * have, and the firmware's own settings come at the next start. */
        ksnprintf(line, sizeof(line), "clock = firmware (%s): from the next start (CPPC, once on, "
                  "stays on until then); the cores keep clock = %s", source,
                  modes[kept_mode].name);
        say(out, ctx, line);
        mutex_unlock(&settings);
        return;
    }
    __atomic_store_n(&kept_mode, (int)modes[k].mode, __ATOMIC_RELEASE);
    if (arch_clock_set(modes[k].mode, done, sizeof(done))) {
        if (modes[k].mode != ARCH_CLOCK_FIRMWARE)
            cppc_on = true;
        int cores = cpus_apply_clock();
        ksnprintf(line, sizeof(line), "clock = %s (%s): %s, on %d core%s", modes[k].name, source,
                  done, cores, cores == 1 ? "" : "s");
    } else {
        ksnprintf(line, sizeof(line), "clock = %s (%s): not used, this processor's clock is the "
                  "firmware's", modes[k].name, source);
    }
    say(out, ctx, line);
    mutex_unlock(&settings);
}
