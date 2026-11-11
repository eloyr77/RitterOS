#!/usr/bin/env python3
"""Compare a RitterFS made by RitterOS's program format with what
scripts/ritterfs.py's format makes for the same partition, label, UUID and
time (scripts/test.sh runs it on images RitterOS formatted).

Usage: samefs.py IMAGE FIRST_SECTOR SECTORS

The host formats an overlay of the partition: it reads RitterOS's bytes
and keeps what the host writes in memory, so any difference comes from a
block the host writes otherwise (large disks need little memory). Prints
"identical: N blocks" (those the host wrote) or the first blocks that
differ (exit status 1).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ritterfs  # noqa: E402


class Overlay(ritterfs.Partition):
    """The partition as written by the host, without changing the image."""

    def __init__(self, f, first, sectors):
        super().__init__(f, first, sectors)
        self.written = {}

    def read(self, block):
        return self.written.get(block) or super().read(block)

    def write(self, block, data):
        self.written[block] = data

    def write_super(self, slot, data):
        self.written[slot] = data


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    path, first, sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    with open(path, "rb") as f:
        made = ritterfs.Partition(f, first, sectors)
        sb, _ = ritterfs.mount(made)
        if not sb:
            sys.exit("no RitterFS there")
        host = Overlay(f, first, sectors)
        ritterfs.format(host, sb.label, sb.uuid, sb.created_time)
        differ = sorted(b for b, data in host.written.items() if made.read(b) != data)
    if differ:
        print("differ in blocks", differ[:10])
        sys.exit(1)
    print(f"identical: {len(host.written)} blocks")


if __name__ == "__main__":
    main()
