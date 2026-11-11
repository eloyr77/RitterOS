/*
 * clear: an empty screen, the cursor in its top left corner (the next
 * prompt there). The terminal's escape sequences ESC [ 2 J (erase the
 * screen) and ESC [ H (cursor home): the console understands them, and so
 * does a terminal on the serial port.
 */
#include <unistd.h>
#include "ritteros.h"

PROGRAM_HELP("clear", "clear the screen");

int main(void)
{
    static const char sequence[] = "\033[2J\033[H";
    return write(STDOUT_FILENO, sequence, sizeof(sequence) - 1) < 0;
}
