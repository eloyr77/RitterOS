#!/usr/bin/env python3
"""Where is a test machine's CPU? Stops the guest through QEMU's GDB stub
(-gdb unix:SOCKET,server=on,wait=off), reads its registers and names the
kernel function the program counter is in (from the kernel's symbols).
For hangs: the tests run it when a machine stops reading its serial port.

Then it lets the guest run a second and reads the kernel's tick counter
(timer.c's `ticks`) again: unchanged means the timer stopped.

Usage: gdbprobe.py SOCKET x86_64|aarch64 KERNEL_ELF
"""
import bisect
import socket
import struct
import subprocess
import sys
import time


def packet(data):
    return b"$" + data + b"#" + b"%02x" % (sum(data) & 0xFF)


def read_reply(s):
    """The next packet's data (acknowledging it)."""
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            raise EOFError("the stub closed the connection")
        buf += chunk
        start = buf.find(b"$")
        end = buf.find(b"#", start + 1) if start >= 0 else -1
        if start >= 0 and end >= 0 and len(buf) >= end + 3:
            s.sendall(b"+")
            return buf[start + 1:end]


def ask(s, data):
    s.sendall(packet(data))
    return read_reply(s)


def symbols(elf, kinds="tTwW"):
    out = subprocess.run(["nm", "-n", elf], capture_output=True, text=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in kinds:
            table.append((int(parts[0], 16), parts[2]))
    return table


def connect(path):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(5)
    s.connect(path)                         # QEMU stops the guest on connecting
    ask(s, b"?")                            # why it stopped (it did, for us)
    return s


def read_u64(s, address):
    data = ask(s, b"m%x,8" % address)
    return None if data.startswith(b"E") else struct.unpack("<Q", bytes.fromhex(data.decode()))[0]


def name_of(table, address):
    i = bisect.bisect_right([a for a, _ in table], address) - 1
    return f"{table[i][1]}+0x{address - table[i][0]:x}" if i >= 0 else "?"


def main():
    sock_path, arch, elf = sys.argv[1:4]
    ticks = [a for a, n in symbols(elf, "bBdD") if n == "ticks"]
    counts = []
    for round in (0, 1):
        s = connect(sock_path)
        try:
            if round == 0:
                probe(s, arch, elf)
            if ticks:
                counts.append(read_u64(s, ticks[0]))
        finally:
            s.sendall(packet(b"D"))         # detach: it runs on
            s.close()
        if round == 0:
            time.sleep(1)
    if len(counts) == 2 and None not in counts:
        print(f"timer ticks: {counts[0]}, a second later {counts[1]}"
              f"{' (the timer stopped)' if counts[0] == counts[1] else ''}")


def probe(s, arch, elf):
    regs = bytes.fromhex(ask(s, b"g").decode())
    table = symbols(elf)
    if arch == "aarch64":
        x = struct.unpack_from("<31Q", regs, 0)
        sp, pc = struct.unpack_from("<QQ", regs, 31 * 8)
        cpsr = struct.unpack_from("<I", regs, 33 * 8)[0]
        print(f"pc 0x{pc:x} ({name_of(table, pc)}), sp 0x{sp:x}, cpsr 0x{cpsr:x} "
              f"(IRQs {'masked' if cpsr & 0x80 else 'unmasked'}), x30 0x{x[30]:x} "
              f"({name_of(table, x[30])})")
    else:
        r = struct.unpack_from("<16Q", regs, 0)
        rip = struct.unpack_from("<Q", regs, 16 * 8)[0]
        eflags = struct.unpack_from("<I", regs, 17 * 8)[0]
        print(f"rip 0x{rip:x} ({name_of(table, rip)}), rsp 0x{r[7]:x}, eflags 0x{eflags:x} "
              f"(interrupts {'on' if eflags & 0x200 else 'off'})")
    # A few return addresses from the stack (a rough backtrace).
    sp = struct.unpack_from("<Q", regs, 31 * 8)[0] if arch == "aarch64" else r[7]
    stack = ask(s, b"m%x,%x" % (sp, 256))
    if not stack.startswith(b"E"):
        words = struct.unpack("<32Q", bytes.fromhex(stack.decode()))
        text_start = table[0][0] if table else 0
        found = [f"0x{w:x} ({name_of(table, w)})" for w in words
                 if table and text_start <= w <= table[-1][0] + 0x10000]
        print("on the stack: " + (", ".join(found[:8]) or "nothing that looks like code"))


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"gdbprobe: {e}")
