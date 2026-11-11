/*
 * Test: configget FILE KEY prints the value config_get finds (or that it
 * finds none, or why it cannot read the file).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ritteros.h"

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    char value[128];
    int r = config_get(argv[1], argv[2], value, sizeof(value));
    if (r > 0)
        printf("configget: %s = '%s'\n", argv[2], value);
    else if (r == 0)
        printf("configget: %s: not set\n", argv[2]);
    else
        printf("configget: %s: %s\n", argv[1], strerror(errno));
    return 0;
}
