#!/usr/bin/env python3
"""ctypetab.py UCD_DIR > user/lib/ctypetab.h

The C library's tables of Unicode characters (43a-2), for the C.UTF-8
locale: the classes of wctype.h (iswalpha ...), each character's width on a
terminal (wcwidth), and the simple upper and lower case (towupper,
towlower), from the Unicode Character Database: UnicodeData.txt (general
categories, case mappings), DerivedCoreProperties.txt (Alphabetic,
Uppercase, Lowercase), PropList.txt (White_Space) and EastAsianWidth.txt
(wide and fullwidth). The output is checked in, so the build needs neither
Python nor the files; run this again for a new Unicode version (17.0.0
now, as the JavaScript engine's casetab.h). The data is under the Unicode
License v3: scripts/UNICODE-LICENSE.txt.

The classes follow glibc's C.UTF-8: alpha is Alphabetic and the decimal
digits beyond 0-9; digit and xdigit are ASCII only; space is White_Space
without the no-break spaces; blank the spaces of a line (Zs, tab) without
them; cntrl Cc, Zl and Zp; graph every assigned character that is not a
space, a control or a surrogate; print graph and Zs; punct graph that is
neither alpha nor digit. The width: -1 for controls and what is not
assigned, 0 for combining marks, format characters (but the soft hyphen)
and Hangul's medial vowels and final consonants, 2 for wide and fullwidth,
else 1. ASCII is the library's own code, not these tables.
"""
import os
import sys

ALPHA, UPPER, LOWER, SPACE, BLANK, CNTRL, PUNCT, GRAPH, PRINT = (1 << i for i in range(9))
# width in bits 12-13: 0 -> width 1, 1 -> 0, 2 -> 2, 3 -> -1
W0, W2, WNONE = 1 << 12, 2 << 12, 3 << 12
TOP = 0x110000


def fields(path):
    for line in open(path, encoding='utf-8'):
        line = line.split('#')[0].strip()
        if line:
            yield [x.strip() for x in line.split(';')]


def code_range(text):
    a, _, b = text.partition('..')
    return int(a, 16), int(b or a, 16)


def main():
    ucd = sys.argv[1]
    cat = ['Cn'] * TOP
    upper_map, lower_map = {}, {}
    first = None
    for f in fields(os.path.join(ucd, 'UnicodeData.txt')):
        cp, name, gc = int(f[0], 16), f[1], f[2]
        if name.endswith(', First>'):
            first = cp
            continue
        if name.endswith(', Last>'):
            for c in range(first, cp + 1):
                cat[c] = gc
            continue
        cat[cp] = gc
        if f[12]:
            upper_map[cp] = int(f[12], 16)
        if f[13]:
            lower_map[cp] = int(f[13], 16)
    props = {'Alphabetic': set(), 'Uppercase': set(), 'Lowercase': set(), 'White_Space': set()}
    for name in ('DerivedCoreProperties.txt', 'PropList.txt'):
        for f in fields(os.path.join(ucd, name)):
            if f[1] in props:
                a, b = code_range(f[0])
                props[f[1]].update(range(a, b + 1))
    wide = set()
    for f in fields(os.path.join(ucd, 'EastAsianWidth.txt')):
        if f[1] in ('W', 'F'):
            a, b = code_range(f[0])
            wide.update(range(a, b + 1))
    nobreak = {0x00A0, 0x2007, 0x202F}

    bits = [0] * TOP
    for c in range(0x80, TOP):
        gc = cat[c]
        b = 0
        if c in props['Alphabetic'] or gc == 'Nd':
            b |= ALPHA
        if c in props['Uppercase']:
            b |= UPPER
        if c in props['Lowercase']:
            b |= LOWER
        if c in props['White_Space'] and c not in nobreak and c != 0x85:
            b |= SPACE
        if gc == 'Zs' and c not in nobreak:
            b |= BLANK
        if gc in ('Cc', 'Zl', 'Zp'):
            b |= CNTRL
        graph = gc not in ('Cn', 'Cc', 'Cs', 'Zs', 'Zl', 'Zp')
        if graph:
            b |= GRAPH | PRINT
            if not b & ALPHA:
                b |= PUNCT
        if gc == 'Zs':
            b |= PRINT
        if gc in ('Cn', 'Cs', 'Cc', 'Zl', 'Zp'):
            b |= WNONE
        elif (gc in ('Mn', 'Me', 'Cf') and c != 0x00AD) or 0x1160 <= c <= 0x11FF or c == 0x200B:
            b |= W0
        elif c in wide:
            b |= W2
        bits[c] = b

    ranges = []
    for c in range(0x80, TOP):
        if ranges and ranges[-1][1] == c - 1 and ranges[-1][2] == bits[c]:
            ranges[-1][1] = c
        else:
            ranges.append([c, c, bits[c]])

    def runs(mapping):
        """[lo, hi, delta, step]: lo..hi (every step-th) map to themselves + delta."""
        out = []
        for c in sorted(mapping):
            d = mapping[c] - c
            if out:
                lo, hi, delta, step = out[-1]
                if delta == d and step == 1 and c == hi + 1:
                    out[-1][1] = c
                    continue
                if delta == d and c == hi + 2 and (step == 2 or lo == hi):
                    out[-1][1], out[-1][3] = c, 2
                    continue
            out.append([c, c, d, 1])
        return out

    upper, lower = runs(upper_map), runs(lower_map)
    w = sys.stdout.write
    w('/* user/lib/ctypetab.h: made by scripts/ctypetab.py from Unicode 17.0.0\'s\n')
    w(' * character database (Unicode License v3, scripts/UNICODE-LICENSE.txt).\n')
    w(' * Do not edit: run the script again. */\n\n')
    w('#define CT_ALPHA 0x%x\n#define CT_UPPER 0x%x\n#define CT_LOWER 0x%x\n' % (ALPHA, UPPER, LOWER))
    w('#define CT_SPACE 0x%x\n#define CT_BLANK 0x%x\n#define CT_CNTRL 0x%x\n' % (SPACE, BLANK, CNTRL))
    w('#define CT_PUNCT 0x%x\n#define CT_GRAPH 0x%x\n#define CT_PRINT 0x%x\n' % (PUNCT, GRAPH, PRINT))
    w('#define CT_WIDTH(bits) ((int)((bits) >> 12 & 3))      /* 0: 1, 1: 0, 2: 2, 3: -1 */\n\n')
    w('/* Code points from 0x80 on in runs of the same classes and width. */\n')
    w('static const struct { uint32_t first, last; uint16_t bits; } ct_ranges[%d] = {\n' % len(ranges))
    for lo, hi, b in ranges:
        w('    { 0x%X, 0x%X, 0x%X },\n' % (lo, hi, b))
    w('};\n\n')
    for name, rs in (('ct_upper', upper), ('ct_lower', lower)):
        w('/* %s: first..last (every step-th) + delta. */\n' % ('towupper' if name == 'ct_upper' else 'towlower'))
        w('static const struct { uint32_t first, last; int32_t delta; uint8_t step; } %s[%d] = {\n' % (name, len(rs)))
        for lo, hi, d, step in rs:
            w('    { 0x%X, 0x%X, %d, %d },\n' % (lo, hi, d, step))
        w('};\n\n')


if __name__ == '__main__':
    main()
