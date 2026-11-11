/* Test: the arguments arrive as argc and argv, ending with a null pointer. */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("args: argc %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("args: argv[%d] '%s'\n", i, argv[i]);
    printf("args: argv[%d] is %s\n", argc, argv[argc] ? "not null!" : "null");
    return 0;
}
