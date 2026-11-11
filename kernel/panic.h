/* Fatal kernel errors that are not CPU exceptions. */
#ifndef RITTEROS_PANIC_H
#define RITTEROS_PANIC_H

/* Print the message as a crash report (console and serial), turn the status
 * strip red and halt. */
__attribute__((noreturn, format(printf, 1, 2)))
void panic(const char *fmt, ...);

/* Stop the CPU for good: interrupts off, HLT in a loop. */
__attribute__((noreturn))
void halt_forever(void);

#endif
