"""swap_shell_test's host part (35a): the swap file's data blocks in an image.

    swapblocks.py IMAGE FIRST_LBA SECTORS PATH

prints "fixed: yes|no, N blocks in R run(s), Z all zeros": a fixed file's
data blocks are written in place by the kernel (encrypted), so after swap's
self-test none of them still holds the zeros `swap make` wrote; its runs are
counted as the kernel joins them (consecutive blocks, up or down: a commit
places a file's new blocks from the last one down), to compare with the
kernel's "pages in R run(s)"."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ritterfs as r  # noqa: E402


def main():
    image, first, sectors, path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    with open(image, "rb") as f:
        part = r.Partition(f, first, sectors)
        oid, inode = r.Reader(part).lookup(path)
        count = inode.size // r.BLOCK
        leaves = []

        def walk(ptr, level, start):
            if start >= count or not ptr.address:
                return
            if level == 0:
                leaves.append(ptr.address)
                return
            data = r.read_verified(part, ptr)
            for i in range(r.POINTERS_PER_BLOCK):
                walk(r.Pointer.unpack(data[i * 32:(i + 1) * 32]), level - 1,
                     start + i * r.POINTERS_PER_BLOCK ** (level - 1))

        walk(inode.root, inode.height, 0)
        zeros = sum(part.read(a) == bytes(r.BLOCK) for a in leaves)
        runs, step, length = 0, 0, 0
        for i, a in enumerate(leaves):
            d = a - leaves[i - 1] if i else 0
            if length and d in (1, -1) and (length == 1 or d == step):
                step, length = d, length + 1
            else:
                runs, step, length = runs + 1, 0, 1
        print(f"fixed: {'yes' if inode.flags & r.FLAG_FIXED else 'no'}, "
              f"{len(leaves)} blocks in {runs} run(s), {zeros} all zeros")


if __name__ == "__main__":
    main()
