/*
 * Test: the terminal for full-screen programs (SYS_TERMINAL). Raw input
 * (bytes as typed, no echo), lines again, then the console's escape
 * sequences: scripts/test.sh takes screenshots at "term: drawn" and
 * "term: hidden" (scripts/check_screen.py, mode "term"). It ends with
 * reverse video on, the cursor hidden and raw input on: the kernel must
 * undo all three.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ritteros.h"

static void out(const char *s)
{
    write(STDOUT_FILENO, s, strlen(s));
}

/* Wait for one key in raw mode. */
static void key(void)
{
    char c;
    read(STDIN_FILENO, &c, 1);
}

int main(void)
{
    signal(SIGINT, SIG_IGN);                        /* Ctrl-C arrives as a byte */
    int rows, cols;
    ritteros_terminal_size(&rows, &cols);
    printf("term: size %d rows, %d columns\n", rows, cols);

    /* Raw: every byte as typed, until 'q'. */
    printf("term: raw %d\n", ritteros_terminal_raw(true));
    char got[64], text[3 * sizeof(got) + 1] = "";
    size_t n = 0;
    while (n < sizeof(got) && (n == 0 || got[n - 1] != 'q')) {
        ssize_t r = read(STDIN_FILENO, got + n, sizeof(got) - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    for (size_t i = 0, used = 0; i < n; i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%02x", i ? " " : "",
                                 (unsigned char)got[i]);
    printf("term: read %s\n", text);

    /* Lines again: edited and echoed by the kernel. */
    ritteros_terminal_raw(false);
    printf("term: lines again\n");
    char line[32];
    ssize_t r = read(STDIN_FILENO, line, sizeof(line) - 1);
    line[r > 0 ? r : 0] = 0;
    if (r > 0 && line[r - 1] == '\n')
        line[r - 1] = 0;
    printf("term: line '%s'\n", line);

    /* The screen (positions are checked by check_screen.py "term"). */
    ritteros_terminal_raw(true);
    char buf[64];
    out("\033[2J\033[H");
    out("\033[3;5Hhere");
    out("\033[5;1H\033[7mREV\033[0m");
    out("\033[6;1H\033[32mgreen\033[39m \033[1mbold\033[22m");
    out("\033[7;1HXXXXXXXX\033[7;3H\033[K");
    out("\033[8;1Hgone\033[2K");
    out("\033[");                                   /* a sequence split in two writes */
    out("9;1Hsplit");
    out("\033[12;10H\033[2A\033[3C\033[1D\033[1Bm");
    snprintf(buf, sizeof(buf), "\033[14;%dHW\033[0mZ", cols);   /* the wrap waits */
    out(buf);
    out("\033[1;1Hterm: drawn\033[16;20H");
    key();
    out("\033[?25l\033[1;1Hterm: hidden\033[16;20H");
    key();

    /* Leave a mess behind. */
    out("\033[2J\033[H\033[7m");
    return 0;
}
