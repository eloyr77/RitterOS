/* swap_shell_test's host part (35a): kernel/xts.c with common/aes.c, built
 * on the Mac. xtstest e|d KEYHEX UNIT < in > out: one data unit encrypted or
 * decrypted, to compare with `openssl enc -aes-256-xts`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xts.h"

int main(int argc, char **argv)
{
    static uint8_t buf[1 << 16];
    uint8_t key[64];
    if (argc != 4 || strlen(argv[2]) != 128 || (argv[1][0] != 'e' && argv[1][0] != 'd'))
        return 2;
    for (int i = 0; i < 64; i++) {
        unsigned b;
        if (sscanf(argv[2] + 2 * i, "%2x", &b) != 1)
            return 2;
        key[i] = (uint8_t)b;
    }
    size_t n = fread(buf, 1, sizeof(buf), stdin);
    if (n < 16 || n % 16)
        return 2;
    struct xts x;
    xts_init(&x, key);
    if (argv[1][0] == 'e')
        xts_encrypt(&x, strtoull(argv[3], 0, 10), buf, buf, n);
    else
        xts_decrypt(&x, strtoull(argv[3], 0, 10), buf, buf, n);
    fwrite(buf, 1, n, stdout);
    return 0;
}
