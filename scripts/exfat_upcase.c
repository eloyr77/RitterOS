/*
 * The exFAT up-case table for format -t exfat, as a C header (made by the
 * Makefile into build/generated/; never edited by hand). Written in C in 63;
 * until then scripts/exfat_upcase.py, whose table this writes unit for unit.
 *
 * exfat_upcase OUT.h
 *
 * Each character of the Basic Multilingual Plane is mapped to its capital
 * where it has one that is a single character of the plane, stored
 * compressed (runs of four or more characters that are their own capitals
 * as 0xFFFF and the run's length), as the exFAT specification describes it;
 * its checksum is the one the volume's up-case entry carries. The capitals
 * come from the Unicode Character Database (13.0, as Python 3.9's
 * str.upper() gives them; scripts/exfat.py makes the same table for the
 * tests' images): below as runs of characters START, START + STEP, ...
 * whose capital is the character plus DELTA. They are Unicode's data,
 * under the Unicode License v3: its notice is scripts/UNICODE-LICENSE.txt.
 */
#include <stdint.h>
#include <stdio.h>

static const struct {
    uint16_t start, count;
    uint8_t step;
    int32_t delta;
} capitals[] = {
    { 0x0061, 26, 1, -32 },
    { 0x00B5, 1, 1, 743 },
    { 0x00E0, 23, 1, -32 },
    { 0x00F8, 7, 1, -32 },
    { 0x00FF, 1, 1, 121 },
    { 0x0101, 24, 2, -1 },
    { 0x0131, 1, 1, -232 },
    { 0x0133, 3, 2, -1 },
    { 0x013A, 8, 2, -1 },
    { 0x014B, 23, 2, -1 },
    { 0x017A, 3, 2, -1 },
    { 0x017F, 1, 1, -300 },
    { 0x0180, 1, 1, 195 },
    { 0x0183, 2, 2, -1 },
    { 0x0188, 1, 1, -1 },
    { 0x018C, 1, 1, -1 },
    { 0x0192, 1, 1, -1 },
    { 0x0195, 1, 1, 97 },
    { 0x0199, 1, 1, -1 },
    { 0x019A, 1, 1, 163 },
    { 0x019E, 1, 1, 130 },
    { 0x01A1, 3, 2, -1 },
    { 0x01A8, 1, 1, -1 },
    { 0x01AD, 1, 1, -1 },
    { 0x01B0, 1, 1, -1 },
    { 0x01B4, 2, 2, -1 },
    { 0x01B9, 1, 1, -1 },
    { 0x01BD, 1, 1, -1 },
    { 0x01BF, 1, 1, 56 },
    { 0x01C5, 1, 1, -1 },
    { 0x01C6, 1, 1, -2 },
    { 0x01C8, 1, 1, -1 },
    { 0x01C9, 1, 1, -2 },
    { 0x01CB, 1, 1, -1 },
    { 0x01CC, 1, 1, -2 },
    { 0x01CE, 8, 2, -1 },
    { 0x01DD, 1, 1, -79 },
    { 0x01DF, 9, 2, -1 },
    { 0x01F2, 1, 1, -1 },
    { 0x01F3, 1, 1, -2 },
    { 0x01F5, 1, 1, -1 },
    { 0x01F9, 20, 2, -1 },
    { 0x0223, 9, 2, -1 },
    { 0x023C, 1, 1, -1 },
    { 0x023F, 2, 1, 10815 },
    { 0x0242, 1, 1, -1 },
    { 0x0247, 5, 2, -1 },
    { 0x0250, 1, 1, 10783 },
    { 0x0251, 1, 1, 10780 },
    { 0x0252, 1, 1, 10782 },
    { 0x0253, 1, 1, -210 },
    { 0x0254, 1, 1, -206 },
    { 0x0256, 2, 1, -205 },
    { 0x0259, 1, 1, -202 },
    { 0x025B, 1, 1, -203 },
    { 0x025C, 1, 1, 42319 },
    { 0x0260, 1, 1, -205 },
    { 0x0261, 1, 1, 42315 },
    { 0x0263, 1, 1, -207 },
    { 0x0265, 1, 1, 42280 },
    { 0x0266, 1, 1, 42308 },
    { 0x0268, 1, 1, -209 },
    { 0x0269, 1, 1, -211 },
    { 0x026A, 1, 1, 42308 },
    { 0x026B, 1, 1, 10743 },
    { 0x026C, 1, 1, 42305 },
    { 0x026F, 1, 1, -211 },
    { 0x0271, 1, 1, 10749 },
    { 0x0272, 1, 1, -213 },
    { 0x0275, 1, 1, -214 },
    { 0x027D, 1, 1, 10727 },
    { 0x0280, 1, 1, -218 },
    { 0x0282, 1, 1, 42307 },
    { 0x0283, 1, 1, -218 },
    { 0x0287, 1, 1, 42282 },
    { 0x0288, 1, 1, -218 },
    { 0x0289, 1, 1, -69 },
    { 0x028A, 2, 1, -217 },
    { 0x028C, 1, 1, -71 },
    { 0x0292, 1, 1, -219 },
    { 0x029D, 1, 1, 42261 },
    { 0x029E, 1, 1, 42258 },
    { 0x0345, 1, 1, 84 },
    { 0x0371, 2, 2, -1 },
    { 0x0377, 1, 1, -1 },
    { 0x037B, 3, 1, 130 },
    { 0x03AC, 1, 1, -38 },
    { 0x03AD, 3, 1, -37 },
    { 0x03B1, 17, 1, -32 },
    { 0x03C2, 1, 1, -31 },
    { 0x03C3, 9, 1, -32 },
    { 0x03CC, 1, 1, -64 },
    { 0x03CD, 2, 1, -63 },
    { 0x03D0, 1, 1, -62 },
    { 0x03D1, 1, 1, -57 },
    { 0x03D5, 1, 1, -47 },
    { 0x03D6, 1, 1, -54 },
    { 0x03D7, 1, 1, -8 },
    { 0x03D9, 12, 2, -1 },
    { 0x03F0, 1, 1, -86 },
    { 0x03F1, 1, 1, -80 },
    { 0x03F2, 1, 1, 7 },
    { 0x03F3, 1, 1, -116 },
    { 0x03F5, 1, 1, -96 },
    { 0x03F8, 1, 1, -1 },
    { 0x03FB, 1, 1, -1 },
    { 0x0430, 32, 1, -32 },
    { 0x0450, 16, 1, -80 },
    { 0x0461, 17, 2, -1 },
    { 0x048B, 27, 2, -1 },
    { 0x04C2, 7, 2, -1 },
    { 0x04CF, 1, 1, -15 },
    { 0x04D1, 48, 2, -1 },
    { 0x0561, 38, 1, -48 },
    { 0x10D0, 43, 1, 3008 },
    { 0x10FD, 3, 1, 3008 },
    { 0x13F8, 6, 1, -8 },
    { 0x1C80, 1, 1, -6254 },
    { 0x1C81, 1, 1, -6253 },
    { 0x1C82, 1, 1, -6244 },
    { 0x1C83, 2, 1, -6242 },
    { 0x1C85, 1, 1, -6243 },
    { 0x1C86, 1, 1, -6236 },
    { 0x1C87, 1, 1, -6181 },
    { 0x1C88, 1, 1, 35266 },
    { 0x1D79, 1, 1, 35332 },
    { 0x1D7D, 1, 1, 3814 },
    { 0x1D8E, 1, 1, 35384 },
    { 0x1E01, 75, 2, -1 },
    { 0x1E9B, 1, 1, -59 },
    { 0x1EA1, 48, 2, -1 },
    { 0x1F00, 8, 1, 8 },
    { 0x1F10, 6, 1, 8 },
    { 0x1F20, 8, 1, 8 },
    { 0x1F30, 8, 1, 8 },
    { 0x1F40, 6, 1, 8 },
    { 0x1F51, 4, 2, 8 },
    { 0x1F60, 8, 1, 8 },
    { 0x1F70, 2, 1, 74 },
    { 0x1F72, 4, 1, 86 },
    { 0x1F76, 2, 1, 100 },
    { 0x1F78, 2, 1, 128 },
    { 0x1F7A, 2, 1, 112 },
    { 0x1F7C, 2, 1, 126 },
    { 0x1FB0, 2, 1, 8 },
    { 0x1FBE, 1, 1, -7205 },
    { 0x1FD0, 2, 1, 8 },
    { 0x1FE0, 2, 1, 8 },
    { 0x1FE5, 1, 1, 7 },
    { 0x214E, 1, 1, -28 },
    { 0x2170, 16, 1, -16 },
    { 0x2184, 1, 1, -1 },
    { 0x24D0, 26, 1, -26 },
    { 0x2C30, 47, 1, -48 },
    { 0x2C61, 1, 1, -1 },
    { 0x2C65, 1, 1, -10795 },
    { 0x2C66, 1, 1, -10792 },
    { 0x2C68, 3, 2, -1 },
    { 0x2C73, 1, 1, -1 },
    { 0x2C76, 1, 1, -1 },
    { 0x2C81, 50, 2, -1 },
    { 0x2CEC, 2, 2, -1 },
    { 0x2CF3, 1, 1, -1 },
    { 0x2D00, 38, 1, -7264 },
    { 0x2D27, 1, 1, -7264 },
    { 0x2D2D, 1, 1, -7264 },
    { 0xA641, 23, 2, -1 },
    { 0xA681, 14, 2, -1 },
    { 0xA723, 7, 2, -1 },
    { 0xA733, 31, 2, -1 },
    { 0xA77A, 2, 2, -1 },
    { 0xA77F, 5, 2, -1 },
    { 0xA78C, 1, 1, -1 },
    { 0xA791, 2, 2, -1 },
    { 0xA794, 1, 1, 48 },
    { 0xA797, 10, 2, -1 },
    { 0xA7B5, 6, 2, -1 },
    { 0xA7C3, 1, 1, -1 },
    { 0xA7C8, 2, 2, -1 },
    { 0xA7F6, 1, 1, -1 },
    { 0xAB53, 1, 1, -928 },
    { 0xAB70, 80, 1, -38864 },
    { 0xFF41, 26, 1, -32 },
};

static uint16_t table[65536];
static uint16_t packed[65536 * 2];

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: exfat_upcase OUT.h\n");
        return 2;
    }
    for (uint32_t c = 0; c < 65536; c++)
        table[c] = (uint16_t)c;
    for (size_t r = 0; r < sizeof(capitals) / sizeof(capitals[0]); r++)
        for (uint32_t k = 0; k < capitals[r].count; k++) {
            uint32_t c = capitals[r].start + capitals[r].step * k;
            table[c] = (uint16_t)(c + (uint32_t)capitals[r].delta);
        }
    /* the compressed form */
    size_t n = 0;
    for (uint32_t i = 0; i < 65536;) {
        uint32_t j = i;
        while (j < 65536 && table[j] == j)
            j++;
        if (j - i >= 4) {
            packed[n++] = 0xFFFF;
            packed[n++] = (uint16_t)(j - i);
            i = j;
        } else
            packed[n++] = table[i++];
    }
    /* the checksum over its bytes, little-endian */
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++)
        for (int b = 0; b < 2; b++)
            sum = ((sum & 1) << 31 | sum >> 1) + (uint8_t)(packed[i] >> (8 * b));
    FILE *out = fopen(argv[1], "w");
    if (!out) {
        fprintf(stderr, "exfat_upcase: cannot write %s\n", argv[1]);
        return 1;
    }
    fprintf(out, "/* Made by scripts/exfat_upcase.c: the exFAT up-case table, compressed. */\n");
    fprintf(out, "#define EXFAT_UPCASE_UNITS %zu\n", n);
    fprintf(out, "#define EXFAT_UPCASE_CHECKSUM 0x%08XU\n", sum);
    fprintf(out, "static const uint16_t exfat_upcase[EXFAT_UPCASE_UNITS] = {\n");
    for (size_t i = 0; i < n; i += 12) {
        fprintf(out, "   ");
        for (size_t k = i; k < n && k < i + 12; k++)
            fprintf(out, " 0x%04X,", packed[k]);
        fprintf(out, "\n");
    }
    fprintf(out, "};\n");
    return fclose(out) != 0;
}
