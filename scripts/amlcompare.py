#!/usr/bin/env python3
"""(37b-2) Compare RitterOS's AML namespace with ACPICA's.

    amlcompare.py [--allow PATH]... AMLHOST ACPIEXEC TABLE...

Loads the tables (a DSDT first, then SSDTs) into both: our interpreter on
the Mac (scripts/amlhost.c, built by the caller) and Intel's acpiexec (the
oracle, -di: no _INI or _STA run, `namespace` printed). Every object is
compared by its path, type and what both print of it: an integer's value,
a string's or buffer's length, a package's elements, a method's
arguments, a region's space, address and length, a field's bits. Prints
"amlcompare: N objects, all the same" or each difference, and exits 1 on
any. --allow PATH lets one object's value differ (a value a method ACPICA
runs while it starts writes: the J1900's \_SB_.SDHB._HRV, set by _STA).
"""
import re
import subprocess
import sys

SPACES = {"SystemMemory": 0, "SystemIO": 1, "PCI_Config": 2, "EmbeddedControl": 3,
          "SMBus": 4, "SystemCMOS": 5, "PCIBARTarget": 6, "IPMI": 7,
          "GeneralPurposeIo": 8, "GenericSerialBus": 9, "PCC": 10,
          "PlatformRtMechanism": 11}
# acpiexec's own test objects, not the tables'
ACPIEXEC_OWN = ("\\_TI_",)

LINE = re.compile(r"^\s*(\d+)\s+(\S{4})\s+(\w+)\s+0x[0-9a-fA-F]+\s+[0-9a-fA-F]+\s?(.*)$")


def acpiexec_names(acpiexec, tables):
    out = subprocess.run([acpiexec, "-di", "-b", "namespace"] + tables,
                         capture_output=True, text=True, errors="replace").stdout
    names = {}
    stack = []
    started = False
    for line in out.splitlines():
        if line.startswith("ACPI Namespace (from Namespace Root)"):
            started = True
            continue
        if not started:
            continue
        m = LINE.match(line)
        if not m:
            continue
        depth, seg, kind, rest = int(m.group(1)), m.group(2), m.group(3), m.group(4)
        del stack[depth:]
        stack.append(seg)
        path = "\\" + ".".join(stack)
        if path.startswith(ACPIEXEC_OWN):
            continue
        names[path] = (kind, acpiexec_detail(kind, rest))
    return names


def acpiexec_detail(kind, rest):
    if kind == "Integer":
        m = re.search(r"= ([0-9A-F]+)", rest)
        return ("value", int(m.group(1), 16)) if m else None
    if kind in ("String", "Buffer"):
        m = re.search(r"Len ([0-9A-F]+)", rest)
        return ("length", int(m.group(1), 16)) if m else None
    if kind == "Package":
        m = re.search(r"Elements ([0-9A-F]+)", rest)
        return ("elements", int(m.group(1), 16)) if m else None
    if kind == "Method":
        m = re.search(r"Args (\d+)", rest)
        return ("args", int(m.group(1))) if m else None
    if kind == "Region":
        m = re.search(r"\[(\w+)\] Addr ([0-9A-F]+) Len ([0-9A-F]+)", rest)
        if m:
            # spaces 0x80-0xFF are the OEM's: acpiexec names them all one way
            space = SPACES.get(m.group(1), "oem" if m.group(1) == "UserDefinedRegion"
                               else m.group(1))
            return ("region", space, int(m.group(2), 16), int(m.group(3), 16))
        return None
    if kind in ("RegionField", "IndexField", "BankField", "BufferField"):
        m = re.search(r"Off ([0-9A-F]+) Len ([0-9A-F]+)", rest)
        return ("bits", int(m.group(1), 16), int(m.group(2), 16)) if m else None
    if kind == "Processor":
        m = re.search(r"ID ([0-9A-F]+)", rest)
        return ("id", int(m.group(1), 16)) if m else None
    if kind == "Alias":
        m = re.search(r"Target (\S+)", rest)
        return ("target", m.group(1)) if m else None
    return None


def our_names(amlhost, tables):
    run = subprocess.run([amlhost] + tables, capture_output=True, text=True, errors="replace")
    if run.returncode != 0:
        sys.exit("amlcompare: amlhost failed:\n" + run.stderr[-2000:])
    names = {}
    for line in run.stdout.splitlines():
        parts = line.split(" ")
        path, kind, rest = parts[0], parts[1], " ".join(parts[2:])
        names[path] = (kind, our_detail(kind, rest))
    return names, run.stderr


def our_detail(kind, rest):
    words = rest.split()
    if kind == "Integer":
        return ("value", int(words[0], 16))
    if kind in ("String", "Buffer"):
        return ("length", int(words[1]))
    if kind == "Package":
        return ("elements", int(words[1]))
    if kind == "Method":
        return ("args", int(words[1]))
    if kind == "Region":
        space = int(words[1])
        return ("region", "oem" if space >= 0x80 else space, int(words[3], 16),
                int(words[5], 16))
    if kind in ("RegionField", "IndexField", "BankField", "BufferField"):
        offset, length = words[1].split("+")
        return ("bits", int(offset), int(length))
    if kind == "Processor":
        return ("id", int(words[1]))
    if kind == "Alias":
        return ("target", words[0].split(".")[-1])
    return None


def main():
    args = sys.argv[1:]
    allowed = set()
    while len(args) >= 2 and args[0] == "--allow":
        allowed.add(args[1])
        args = args[2:]
    if len(args) < 3:
        sys.exit(__doc__)
    amlhost, acpiexec, tables = args[0], args[1], args[2:]
    theirs = acpiexec_names(acpiexec, tables)
    ours, counters = our_names(amlhost, tables)
    differences = []
    for path in sorted(set(theirs) | set(ours)):
        a, b = theirs.get(path), ours.get(path)
        if a is None:
            differences.append("only ours: %s %s" % (path, b))
        elif b is None:
            differences.append("only ACPICA's: %s %s" % (path, a))
        elif a[0] != b[0] or (a[1] is not None and a[1] != b[1] and path not in allowed):
            differences.append("%s: ACPICA %s, ours %s" % (path, a, b))
    for d in differences[:60]:
        print("amlcompare: " + d)
    if len(differences) > 60:
        print("amlcompare: ... %d differences in all" % len(differences))
    sys.stderr.write(counters)
    if differences or not theirs:
        print("amlcompare: %d objects of ACPICA's, %d of ours, %d differences"
              % (len(theirs), len(ours), len(differences)))
        sys.exit(1)
    print("amlcompare: %d objects, all the same" % len(ours))


if __name__ == "__main__":
    main()
