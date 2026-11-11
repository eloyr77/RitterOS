#include "crashtest.h"
#include "arch.h"
#include "kprintf.h"
#include "console.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"
#include "sync.h"
#include <stdint.h>

/* A return instruction in a writable, non-executable data page. */
static volatile uint8_t data_code[16] = ARCH_RETURN_INSTRUCTION;

extern char __stack_guard[];

/* Each call uses about half a KiB of stack and never ends: it runs into the
 * guard page below the stack. */
__attribute__((noinline)) static uint64_t recurse(uint64_t depth)
{
    volatile uint8_t frame[512];
    frame[0] = (uint8_t)depth;
    if (depth == ~0ULL)
        return 0;
    return recurse(depth + 1) + frame[0];
}


static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void crash_announce(const char *what)
{
    console_set_color(COLOR_WARNING);
    kprintf("  crash test: %s\n", what);
    console_set_color(COLOR_TEXT);
}

void crash_test(const char *kind)
{
    if (arch_crash_test(kind)) {
        /* handled (and normally not returned from) by the architecture */
    } else if (streq(kind, "pf")) {
        crash_announce("writing to unmapped address 0x0000700000001000");
        *(volatile uint64_t *)CRASH_UNMAPPED_ADDRESS = 0xDEAD;
    } else if (streq(kind, "null")) {
        crash_announce("reading through a NULL pointer");
        (void)*(volatile uint64_t *)0;
    } else if (streq(kind, "wp")) {
        crash_announce("writing into kernel code (read-only)");
        *(volatile uint8_t *)crash_test = 0x90;
    } else if (streq(kind, "nx")) {
        crash_announce("executing code in a data page (no-execute)");
        ((void (*)(void))(uintptr_t)data_code)();
    } else if (streq(kind, "stack")) {
        kprintf("  crash test: overflowing the stack (guard page at %p)\n", (void *)__stack_guard);
        recurse(0);
    } else if (streq(kind, "doublefree")) {
        uint64_t page = pmm_alloc_page();
        kprintf("  crash test: freeing page %p twice\n", (void *)page);
        pmm_free_page(page);
        pmm_free_page(page);
    } else if (streq(kind, "heapdouble")) {
        void *p = kmalloc(100);
        kprintf("  crash test: kfree(%p) twice\n", p);
        kfree(p);
        kfree(p);
    } else if (streq(kind, "heapoverflow")) {
        /* In a fresh slab the second 32-byte slot directly follows the first
         * and is next in the free list. Writing 48 bytes into the first one
         * damages it; the next kmalloc(32) must notice. */
        heap_trim();
        uint8_t *a = kmalloc(32);
        kprintf("  crash test: writing 48 bytes into a 32-byte block at %p\n", (void *)a);
        memset(a, 0x41, 48);
        (void)kmalloc(32);
    } else if (streq(kind, "spinlock")) {
        /* (65a-2) The ticket lock still knows its holder: this core taking
         * it again is a deadlock, reported instead of a hang. */
        static struct spinlock lock = SPINLOCK_INIT("crash test");
        kprintf("  crash test: taking a spinlock twice\n");
        uint64_t state = spin_lock(&lock);
        spin_lock(&lock);
        spin_unlock(&lock, state);
    } else if (streq(kind, "locksleep")) {
        /* (65a-5) A mutex may sleep: never while holding a spinlock. */
        static struct spinlock lock = SPINLOCK_INIT("crash test");
        static struct mutex m = MUTEX_INIT("crash test");
        kprintf("  crash test: taking a mutex while holding a spinlock\n");
        uint64_t state = spin_lock(&lock);
        mutex_lock(&m);
        mutex_unlock(&m);
        spin_unlock(&lock, state);
    } else if (streq(kind, "lockorder")) {
        /* (65a-5) A then B, later B then A: two cores doing one each would
         * wait for each other for good. The check is switched on here, as
         * "lockcheck" on the command line would. */
        static struct spinlock a = SPINLOCK_INIT("crash A"), b = SPINLOCK_INIT("crash B");
        lock_checking = true;
        kprintf("  crash test: taking 'crash A' then 'crash B', then the other way round\n");
        uint64_t state = spin_lock(&a);
        spin_lock(&b);
        spin_unlock_only(&b);
        spin_unlock(&a, state);
        state = spin_lock(&b);
        spin_lock(&a);
        spin_unlock_only(&a);
        spin_unlock(&b, state);
    } else {
        kprintf("  crash test: unknown kind '%s', use one of: %s pf null wp nx "
                "stack doublefree heapdouble heapoverflow spinlock locksleep lockorder\n", kind, arch_crash_kinds);
        return;
    }
    kprintf("  crash test: no exception happened!\n");
}
