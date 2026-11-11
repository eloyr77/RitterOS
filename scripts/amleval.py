#!/usr/bin/env python3
"""(37b-3) Compare RitterOS's AML evaluations with ACPICA's.

    amleval.py [--allow PATH]... [--names FILE] AMLHOST ACPIEXEC TABLE...

Loads the tables into both interpreters (ours on the Mac, scripts/amlhost.c,
and Intel's acpiexec -di -dr -es, the oracle; operation regions simulated in both,
every byte 0 until written) and evaluates, in the same order, every object
whose value can be asked without arguments: data objects, fields, buffer
fields and methods of no arguments (or the paths in FILE). The values are
compared as trees: integers, strings, buffers byte for byte, packages
element by element, references by the name they name. Prints "amleval: N
evaluations, all the same" or each difference, and exits 1 on any.
--init runs the boot's _INI in both first (acpiexec without -di).
--allow PATH lets one evaluation differ (a method that reads the clock,
or a value ACPICA's start wrote with a method we do not run yet).
"""
import re
import subprocess
import sys

KINDS = {"Integer", "String", "Buffer", "Package", "RegionField", "IndexField",
         "BankField", "BufferField"}


def evaluations(amlhost, tables):
    out = subprocess.run([amlhost] + tables, capture_output=True, text=True,
                         errors="replace").stdout
    paths = []
    for line in out.splitlines():
        w = line.split(" ")
        if len(w) < 2 or w[0].startswith("\\_TI_"):
            continue
        if w[1] in KINDS or (w[1] == "Method" and w[2:4] == ["args", "0"]):
            paths.append(w[0])
    return paths


def parse_ours(text):
    """Ours: 'PATH: Kind ...' then indented lines; a dict of path -> tree."""
    results, stack, path = {}, [], None
    for line in text.splitlines():
        m = re.match(r"^(\\\S*): (.*)$", line)
        if m:
            path = m.group(1)
            body = m.group(2)
            if body == "Uninitialized":       # a method that returned nothing
                results[path] = ("none",)
                stack = []
                continue
            if body in ("failed", "not found"):
                results[path] = ("failed",)
                stack = []
                continue
            node = our_node(body)
            results[path] = node
            stack = [(0, node)]
            continue
        if path is None or not line.strip():
            continue
        depth = (len(line) - len(line.lstrip(" "))) // 2
        body = line.strip()
        if re.match(r"^[0-9A-F]{4}:", body):          # a buffer's bytes
            stack[-1][1][1].extend(int(b, 16) for b in body[5:].split())
            continue
        node = our_node(body)
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack and stack[-1][1][0] == "Package":
            stack[-1][1][1].append(node)
        stack.append((depth, node))
    return results


def our_node(body):
    w = body.split(" ")
    if body == "Uninitialized":                 # (an empty element: ACPICA's Null Object)
        return ("null",)
    if w[0] == "Integer":
        return ("Integer", int(w[1], 16))
    if w[0] == "String":
        return ("String", int(w[2]))
    if w[0] == "Buffer":
        return ("Buffer", [])
    if w[0] == "Package":
        return ("Package", [])
    if w[0] == "Reference" and w[1].startswith("\\"):
        return ("Reference", w[1].lstrip("\\").split(".")[-1].rstrip("_"))
    return ("Other", body)


def dump_bytes(text):
    """The bytes of an acpiexec dump line ("0000: 89 06 ... // text")."""
    m = re.search(r"[0-9A-F]{4}: ((?:[0-9A-F]{2} ?)+)", text.split("//")[0])
    return [int(x, 16) for x in m.group(1).split()] if m else []


def parse_theirs(text):
    results, stack, path = {}, [], None
    for line in text.splitlines():
        m = re.match(r"^No object was returned from evaluation of (\S+)", line)
        if m:
            results[m.group(1)] = ("none",)
            path = None
            continue
        m = re.match(r"^Evaluation of (\S+) (returned|failed)", line)
        if m:
            path = m.group(1)
            if m.group(2) == "failed":
                # (acpiexec's debugger prints a value through a 16 KiB
                # buffer: a larger one fails there alone)
                results[path] = ("too large",) if "AE_BUFFER_OVERFLOW" in line else ("failed",)
                path = None
            stack = []
            continue
        if line.startswith("Evaluating ") or path is None:
            continue
        if re.match(r"^\s+[0-9A-F]{4}: ", line) and stack and stack[-1][1][0] == "Buffer":
            stack[-1][1][1].extend(dump_bytes(line))
            continue
        m = re.match(r"^( +)\[([^\]]+)\](.*)$", line)
        if not m:
            continue
        depth = (len(m.group(1)) - 2) // 2
        kind, rest = m.group(2), m.group(3)
        if kind == "Integer":
            node = ("Integer", int(rest.split("=")[1].split()[0], 16))
        elif kind == "String":
            node = ("String", int(re.search(r"Length ([0-9A-F]+)", rest).group(1), 16))
        elif kind == "Buffer":                # (its first bytes may follow on the line)
            node = ("Buffer", dump_bytes(rest))
        elif kind == "Package":
            node = ("Package", [])
        elif kind.startswith("Null Object"):
            node = ("null",)
        elif kind == "Object Reference":
            n = re.search(r"Name (\S+)", rest)
            node = ("Reference", n.group(1).rstrip("_") if n else "?")
        else:
            node = ("Other", kind + rest)
        if depth == 0:
            results[path] = node
            stack = [(0, node)]
            continue
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack and stack[-1][1][0] == "Package":
            stack[-1][1][1].append(node)
        stack.append((depth, node))
    return results


def where(a, b, at="value"):
    """Where two trees first differ, and what each has there."""
    if a == b:
        return None
    if (a and b and a[0] == b[0] and a[0] in ("Package", "Buffer") and
            len(a[1]) == len(b[1])):
        for i, (x, y) in enumerate(zip(a[1], b[1])):
            if x != y:
                return where(x, y, "%s[%d]" % (at, i)) if a[0] == "Package" else \
                    "%s[%d]: ACPICA %r, ours %r" % (at, i, x, y)
    if a and b and a[0] == b[0] and a[0] in ("Package", "Buffer"):
        return "%s: ACPICA %d %s, ours %d" % (at, len(a[1]),
                                               "elements" if a[0] == "Package" else "bytes", len(b[1]))
    return "%s: ACPICA %s, ours %s" % (at, short(a), short(b))


def short(node):
    s = repr(node)
    return s if len(s) < 160 else s[:160] + "..."


def main():
    args = sys.argv[1:]
    allowed, names, init = set(), None, False
    while args and args[0] in ("--allow", "--names", "--init"):
        if args[0] == "--init":             # (37b-4) the boot's _INI run first, in both
            init = True
            args = args[1:]
            continue
        if args[0] == "--allow":
            allowed.add(args[1])
        else:
            names = args[1]
        args = args[2:]
    if len(args) < 3:
        sys.exit(__doc__)
    amlhost, acpiexec, tables = args[0], args[1], args[2:]
    paths = [l.strip() for l in open(names)] if names else evaluations(amlhost, tables)
    paths = [p for p in paths if p]
    listing = "amleval-paths.txt"
    open(listing, "w").write("\n".join(paths) + "\n")
    ours = parse_ours(subprocess.run([amlhost] + (["-i"] if init else []) + ["-a", listing] + tables, capture_output=True,
                                     text=True, errors="replace").stdout)
    # (acpiexec's -b line is too short for thousands: its commands on stdin)
    commands = "".join("evaluate %s\n" % p for p in paths) + "quit\n"
    # -dr: no repairs of predefined methods' values (ACPICA turns a _BIF's
    # buffer into a string, for one): the interpreter alone is compared;
    # -es: slack mode, as Linux runs ACPICA and as ours is (a missing
    # return value used as 0)
    theirs = parse_theirs(subprocess.run([acpiexec] + ([] if init else ["-di"]) + ["-dr", "-es"] + tables,
                                         input=commands,
                                         capture_output=True, text=True,
                                         errors="replace").stdout)
    differences = []
    large = 0
    for p in paths:
        a, b = theirs.get(p), ours.get(p)
        if a == ("too large",) and b not in (None, ("failed",)):
            large += 1
            continue
        if a != b and p not in allowed:
            differences.append("%s: %s" % (p, where(a, b)))
    for d in differences[:40]:
        print("amleval: " + d)
    if differences:
        print("amleval: %d evaluations, %d differences" % (len(paths), len(differences)))
        sys.exit(1)
    print("amleval: %d evaluations, all the same%s" % (len(paths), large and
          " (%d too large for acpiexec to print)" % large or ""))


if __name__ == "__main__":
    main()
