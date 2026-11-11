/* echo [WORD]...: the words, separated by spaces, and a newline. */
#include <stdio.h>
#include "ritteros.h"

PROGRAM_HELP("echo [WORD]...", "print the words");

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        printf("%s%s", i > 1 ? " " : "", argv[i]);
    printf("\n");
    return 0;
}
