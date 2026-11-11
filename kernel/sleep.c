/*
 * (37g-1) Sleep, ACPI S3, around the processor's part (arch_sleep), in
 * the order of the ACPI specification (6.5 16.1, 7.4.2) and Linux's
 * acpi_suspend_enter: \_PTS(3) while everything still runs (the firmware
 * prepares: it may need the embedded controller); every PCI function's
 * configuration kept (pci.c); the ACPI events off but the RTC's alarm, the
 * wake-up; then the processor's part; awake, the events back (and what
 * woke the machine), \_WAK(3), the PCI functions' configuration back
 * (37g-5d: Linux's order, acpi_pm_finish before dpm_resume_noirq).
 *
 * (37g-2) The devices' own state (a disk's queues, a card's rings) is
 * lost in S3: their drivers' suspend and resume, Linux's order: the
 * drivers first (the modules' devices, then the kernel's own drivers, each
 * the last first), then \_PTS and the PCI configuration; awake, the PCI
 * configuration and \_WAK first, then the drivers (the kernel's, then the
 * modules', in their order). A device whose driver has no suspend (a
 * module's 0, a kernel driver registered without) calls the sleep off
 * before anything is touched.
 *
 * (37g-7) Without seconds (0) no alarm: the power button (or what the
 * firmware wakes on: a laptop's lid) wakes the machine; one sleep at a
 * time; after it the screen comes on and idleness counts from the wake.
 *
 * (37g-5d) Linux freezes every process and keeps its console from
 * drawing until the graphics driver is back; here the console and the
 * display layer (console_sleep, gpu_sleep) touch no screen and call no
 * graphics driver from before the drivers' suspend until they are all
 * back (a cold graphics device's registers and memory, before its driver
 * restores them, hung the Acer's wakes).
 */
#include "sleep.h"
#include "acpi.h"
#include "acpi_events.h"
#include "arch_api.h"
#include "blank.h"
#include "console.h"
#include "gpu.h"
#include "kprintf.h"
#include "module.h"
#include "pci.h"
#include "sync.h"

#define PM1_WAKE            (1u << 15)
#define PM1_RTC             (1u << 10)
#define PM1_PWRBTN          (1u << 8)

#define SLEEPERS 16

static struct sleeper {
    const char *name;
    bool (*suspend)(void *);
    bool (*resume)(void *);
    void *ctx;
} sleepers[SLEEPERS];
static int sleeper_count;
static struct spinlock busy_lock = SPINLOCK_INIT("sleep");
static bool busy;                       /* (37g-7) a sleep under way */

void sleep_register(const char *name, bool (*suspend)(void *ctx), bool (*resume)(void *ctx),
                    void *ctx)
{
    if (sleeper_count < SLEEPERS)
        sleepers[sleeper_count++] = (struct sleeper){ name, suspend, resume, ctx };
}

/* The kernel's drivers suspended, the last first: how many; on a refusal
 * those before resumed again and its name in *refused. */
static int drivers_suspend(const char **refused)
{
    for (int i = sleeper_count - 1; i >= 0; i--)
        if (!sleepers[i].suspend(sleepers[i].ctx)) {
            *refused = sleepers[i].name;
            for (int k = i + 1; k < sleeper_count; k++)
                sleepers[k].resume(sleepers[k].ctx);
            return -1;
        }
    return sleeper_count;
}

static int drivers_resume(void)
{
    int failed = 0;
    for (int i = 0; i < sleeper_count; i++)
        if (!sleepers[i].resume(sleepers[i].ctx)) {
            kprintf("sleep: %s did not come back\n", sleepers[i].name);
            failed++;
        }
    return failed;
}

static const char *ran(int r)
{
    return r > 0 ? "run" : r == 0 ? "none" : "failed";
}

static int sleep_now(unsigned seconds);

int sleep_for(unsigned seconds)
{
    if (seconds > 86399) {
        kprintf("sleep: %u s is not between 1 s and a day\n", seconds);
        return -1;
    }
    uint64_t s = spin_lock(&busy_lock);
    bool taken = busy;
    busy = true;
    spin_unlock(&busy_lock, s);
    if (taken) {
        kprintf("sleep: not asleep: a sleep is already under way\n");
        return -1;
    }
    int r = sleep_now(seconds);
    s = spin_lock(&busy_lock);
    busy = false;
    spin_unlock(&busy_lock, s);
    return r;
}

static int sleep_now(unsigned seconds)
{
    uint8_t a, b;
    if (!acpi_sleep_type(3, &a, &b)) {
        kprintf("sleep: the firmware offers no S3\n");
        return -1;
    }
    const char *no = modules_cannot_sleep();
    for (int i = 0; !no && i < sleeper_count; i++)
        if (!sleepers[i].suspend)
            no = sleepers[i].name;
    if (no) {
        kprintf("sleep: not asleep: %s cannot sleep yet (its driver has no suspend)\n", no);
        return -1;
    }
    /* (37g-5d) Nothing draws on the screen or asks a graphics driver
     * anything until every driver is back (Linux freezes its processes
     * and its console). */
    console_sleep(true);
    gpu_sleep(true);
    const char *refused = modules_suspend();
    int kernel_drivers = refused ? 0 : drivers_suspend(&refused);
    if (refused) {
        if (kernel_drivers < 0)
            modules_resume();
        gpu_sleep(false);
        console_sleep(false);
        kprintf("sleep: not asleep: %s refused to suspend\n", refused);
        return -1;
    }
    int pts = acpi_aml_sleep_method("\\_PTS", 3);
    pci_sleep_save();
    if (seconds)
        kprintf("sleep: S3 (sleep types %u, %u) for %u s, woken by the RTC's alarm; _PTS %s\n", a,
                b, seconds, ran(pts));
    else
        kprintf("sleep: S3 (sleep types %u, %u) until woken (the power button); _PTS %s\n", a, b,
                ran(pts));
    acpi_events_sleep(true, seconds != 0, 0);
    struct arch_sleep_result r;
    int got = arch_sleep(a, b, seconds, &r);
    uint32_t wake = 0;
    acpi_events_sleep(false, false, &wake);
    if (got < 0) {
        int wak = acpi_aml_sleep_method("\\_WAK", 3);
        drivers_resume();
        modules_resume();
        gpu_sleep(false);
        console_sleep(false);
        acpi_events_awake();
        kprintf("sleep: not asleep: %s; _WAK %s\n", r.why, ran(wak));
        return -1;
    }
    /* (37g-5d) Linux's order: _WAK (acpi_pm_finish), then the PCI
     * functions' configuration (dpm_resume_noirq), then the drivers */
    int wak = acpi_aml_sleep_method("\\_WAK", 3);
    int restored = pci_wake();
    int failed = drivers_resume();
    failed += modules_resume();
    gpu_sleep(false);
    console_sleep(false);
    acpi_events_awake();
    blank_woke();                               /* (37g-7) the screen on, idle from now */
    kprintf("sleep: asked on core %d; the boot processor entered S3\n", r.asked_on);
    kprintf("sleep: awake after %u s (%s%s); %d of %d cores back, the processor's part %lu ms; "
            "%d PCI functions' configuration back; _WAK %s; %d kernel drivers suspended and "
            "resumed, %d failed\n", r.slept_s,
            wake & PM1_RTC ? "the RTC's alarm" : wake & PM1_PWRBTN ? "the power button" :
            "something else", wake & PM1_WAKE ? "" : "; PM1 shows no wake", r.cores_back, r.cores,
            r.resume_ms, restored, ran(wak), kernel_drivers, failed);
    return 0;
}
