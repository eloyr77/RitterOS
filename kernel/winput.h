/*
 * Input for the display owner (the window server). While a program owns the
 * screen (SYS_DISPLAY), the pointer and keyboard events go to a queue here
 * instead of to the terminal; the owner reads them with DISPLAY_EVENT. The
 * mouse and keyboard drivers (interrupt context) fill the queue.
 */
#ifndef RITTEROS_WINPUT_H
#define RITTEROS_WINPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "syscalls.h"           /* struct sys_input_event */

/* Start / stop capturing input (SYS_DISPLAY acquire / release). Capture
 * clears the queue. */
void winput_capture(void);
void winput_release(void);

/* The mouse and keyboard ask whether their events go to the queue. */
bool winput_captured(void);

/* Add an event (interrupt context: no console, no heap). */
void winput_pointer(int x, int y, uint8_t buttons);
/* The mouse wheel turned (interrupt context): steps, negative up. */
void winput_wheel(int x, int y, int steps);

/* A physical key (its USB HID usage ID) pressed or released (interrupt
 * context): games need both, and the modifiers alone (15p). */
void winput_keycode(uint8_t usage, bool down);

void winput_key(uint8_t byte);

/* (66b-5) The screen has a new mode: the owner gets INPUT_SCREEN (thread
 * context: DISPLAY_SET_MODE). */
void winput_screen(uint32_t width, uint32_t height);

/* (37e) The screen goes off (true) or on again: the owner gets
 * INPUT_BLANK (the console's thread: kernel/blank.c). */
void winput_blank(bool off);

/* The next event for the owner (1: one in *out; 0: timed out or released;
 * -ERR_* if interrupted). timeout_ms 0: wait as long as it takes. */
int winput_next(struct sys_input_event *out, uint32_t timeout_ms);

#endif
