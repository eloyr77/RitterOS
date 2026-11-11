#!/usr/bin/env python3
"""
elfneeded.py FILE...: for each ELF file, its soname (a library) and the
libraries it needs (DT_SONAME, DT_NEEDED of its dynamic section), one line
each: "FILE: soname NAME; needs A B" ("static" for a file without one, or
whose dynamic section is only for its own relocations: no loader, no soname,
nothing needed -- ld.so since 22l, position-independent).
The test's check of how RitterOS's programs and libraries link (22b-22c).
"""
import struct
import sys

PT_DYNAMIC, PT_INTERP, DT_NULL, DT_NEEDED, DT_STRTAB, DT_SONAME = 2, 3, 0, 1, 5, 14


def describe(path):
    data = open(path, 'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return 'not a 64-bit ELF file'
    phoff, = struct.unpack_from('<Q', data, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    loads, dynamic, interp = [], None, False
    for i in range(phnum):
        p_type, _, p_offset, p_vaddr, _, p_filesz, _, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)
        elif p_type == PT_INTERP:
            interp = True
    if not dynamic:
        return 'static'

    def offset_of(vaddr):                   # a virtual address's place in the file
        for v, o, n in loads:
            if v <= vaddr < v + n:
                return o + vaddr - v
        raise ValueError('address 0x%x is not in the file' % vaddr)

    entries = []
    for k in range(dynamic[1] // 16):
        tag, value = struct.unpack_from('<qQ', data, dynamic[0] + 16 * k)
        if tag == DT_NULL:
            break
        entries.append((tag, value))
    strtab = offset_of(next(v for t, v in entries if t == DT_STRTAB))

    def string(at):
        end = data.index(b'\0', strtab + at)
        return data[strtab + at:end].decode()

    soname = [string(v) for t, v in entries if t == DT_SONAME]
    needed = [string(v) for t, v in entries if t == DT_NEEDED]
    if not interp and not soname and not needed:
        return 'static'
    return 'soname %s; needs %s' % (soname[0] if soname else '-', ' '.join(needed) or '-')


for f in sys.argv[1:]:
    print('%s: %s' % (f, describe(f)))
