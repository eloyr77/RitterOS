/* hello: the starting point of a RitterOS command-line program. */
#include <stdio.h>
#include "ritteros.h"

PROGRAM_HELP("hello [NAME]", "a greeting: the starting point of a new program");

int main(int argc, char **argv)
{
    printf("Hello, %s, from RitterOS!\n", argc > 1 ? argv[1] : "world");
    return 0;
}
