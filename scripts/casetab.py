#!/usr/bin/env python3
"""casetab.py UCD_DIR > user/lib/js/casetab.h

The JavaScript engine's case tables (27s-2) from the Unicode Character
Database: UnicodeData.txt (simple upper and lower case), SpecialCasing.txt
(the unconditional full mappings, such as ß -> SS), CaseFolding.txt (simple
folding, C and S, for regular expressions with /u) and
DerivedCoreProperties.txt (Cased and Case_Ignorable, for the final sigma).
The output is checked in, so the build needs neither Python nor the files;
run this again for a new Unicode version (17.0.0 is what the Mac's
JavaScriptCore follows). The data is under the Unicode License v3:
scripts/UNICODE-LICENSE.txt.
"""
import sys


def parse_ranges(path, prop):
    out = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if f[1] != prop:
            continue
        a, _, b = f[0].partition("..")
        out.append((int(a, 16), int(b or a, 16)))
    out.sort()
    merged = []
    for a, b in out:
        if merged and a <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(b, merged[-1][1]))
        else:
            merged.append((a, b))
    return merged


def runs(mapping):
    """(lo, count, step, delta) runs: code points lo, lo + step, ... map to
    themselves + delta; runs never overlap (a step-2 run only skips code
    points the table does not map), so a binary search finds one"""
    keys = sorted(mapping)
    out = []
    i = 0
    while i < len(keys):
        c = keys[i]
        d = mapping[c] - c
        step = 1
        if c + 1 not in mapping and mapping.get(c + 2, None) == c + 2 + d:
            step = 2
        n = 1
        while n < 1024 and mapping.get(c + n * step) == c + n * step + d and \
                (step == 1 or c + n * step - 1 not in mapping):
            n += 1
        out.append((c, n, step, d))
        i = keys.index(c + (n - 1) * step) + 1
    return out


def main():
    d = sys.argv[1]
    upper, lower = {}, {}
    for line in open(d + "/UnicodeData.txt", encoding="utf-8"):
        f = line.split(";")
        c = int(f[0], 16)
        if f[12]:
            upper[c] = int(f[12], 16)
        if f[13]:
            lower[c] = int(f[13], 16)
    special = {}
    for line in open(d + "/SpecialCasing.txt", encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if f[4]:
            continue                    # conditional: language or context
        c = int(f[0], 16)
        lo = [int(x, 16) for x in f[1].split()]
        up = [int(x, 16) for x in f[3].split()]
        if lo == [lower.get(c, c)] and up == [upper.get(c, c)]:
            continue
        special[c] = (up, lo)
    fold = {}
    for line in open(d + "/CaseFolding.txt", encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(";")]
        if f[1] in ("C", "S"):
            fold[int(f[0], 16)] = int(f[2], 16)
    props = d + "/DerivedCoreProperties.txt"
    cased = parse_ranges(props, "Cased")
    ignorable = parse_ranges(props, "Case_Ignorable")

    # every code point some canonicalization may change, for /i classes
    def full_upper(c):
        return special[c][0] if c in special else [upper.get(c, c)]
    changing = set(upper) | set(lower) | set(fold) | set(special)
    for c in changing:
        u = full_upper(c)
        if len(u) == 1:
            assert full_upper(u[0]) == u, hex(c)      # canonicalizing is idempotent
        if c in fold:
            assert fold.get(fold[c], fold[c]) == fold[c], hex(c)
    for c, (up, lo) in special.items():
        assert len(up) <= 3 and len(lo) <= 3, hex(c)
    anyr = []
    for c in sorted(changing):
        if anyr and c == anyr[-1][1] + 1:
            anyr[-1] = (anyr[-1][0], c)
        else:
            anyr.append((c, c))

    w = sys.stdout.write
    w("/*\n * Case tables of the Unicode Character Database 17.0.0 (27s-2), made by\n"
      " * scripts/casetab.py from UnicodeData.txt, SpecialCasing.txt,\n"
      " * CaseFolding.txt and DerivedCoreProperties.txt: do not edit. Under the\n"
      " * Unicode License v3 (scripts/UNICODE-LICENSE.txt).\n *\n"
      " * A run is { first code point | (step - 1) << 21 | (count - 1) << 22,\n"
      " * delta }: first, first + step, ... map to themselves + delta.\n */\n\n")

    def emit_runs(name, mapping):
        r = runs(mapping)
        for lo, n, step, dl in r:
            for k in range(n):
                assert mapping[lo + k * step] == lo + k * step + dl
        w("static const struct case_run %s[%d] = {\n" % (name, len(r)))
        for i in range(0, len(r), 4):
            w("   " + "".join(" { 0x%08x, %d }," % (lo | (step - 1) << 21 | (n - 1) << 22, dl)
                          for lo, n, step, dl in r[i:i + 4]) + "\n")
        w("};\n\n")

    emit_runs("case_upper", upper)
    emit_runs("case_lower", lower)
    emit_runs("case_fold", fold)

    w("/* full mappings that are not one simple one: { code point, upper, lower } */\n")
    w("static const struct case_special case_special[%d] = {\n" % len(special))
    for c in sorted(special):
        up, lo = special[c]
        def arr(v):
            return "{ " + ", ".join("0x%x" % x for x in v + [0] * (3 - len(v))) + " }"
        w("    { 0x%x, %s, %s },\n" % (c, arr(up), arr(lo)))
    w("};\n\n")

    def emit_ranges(name, rs, what):
        w("/* %s */\n" % what)
        w("static const uint32_t %s[%d][2] = {\n" % (name, len(rs)))
        for i in range(0, len(rs), 5):
            w("   " + "".join(" { 0x%x, 0x%x }," % ab for ab in rs[i:i + 5]) + "\n")
        w("};\n\n")

    emit_ranges("case_cased", cased, "Cased")
    emit_ranges("case_ignorable", ignorable, "Case_Ignorable")
    emit_ranges("case_changing", anyr, "the code points any of the mappings above changes")


main()
