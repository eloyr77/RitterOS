#include "panic.h"
#include "arch.h"
#include "console.h"
#include "kprintf.h"

void halt_forever(void)
{
    arch_stop_other_cores();                /* (65c) a crash halts every core */
    arch_halt();
}

void panic(const char *fmt, ...)
{
    char message[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    arch_interrupts_disable();              /* nothing else runs from here on */
    arch_stop_other_cores();                /* (65c) on any core */
    kprintf_emergency();
    if (!console_is_shown())
        console_show();
    console_set_color(COLOR_ERROR);
    console_set_bold(true);
    kprintf("\n=== RitterOS panic: %s ===\n", message);
    console_set_bold(false);
    kprintf("System halted.\n");
    console_set_status(STATUS_CRASH);
    halt_forever();
}
