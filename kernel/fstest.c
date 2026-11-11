#include "fstest.h"
#include "crc32.h"
#include "rfs_dirindex.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define LIST_LIMIT 20           /* larger directories are summarized */
#define MANIFEST "/test/manifest.txt"

struct walk {
    struct ritterfs *fs;
    char path[256];
    size_t len;
};

static bool count_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    (void)name, (void)len, (void)id, (void)type;
    (*(uint64_t *)ctx)++;
    return true;
}

static bool list_entry(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    struct walk *w = ctx;
    size_t saved = w->len;
    if (w->len + 1 + len >= sizeof(w->path))
        return true;
    w->path[w->len++] = '/';
    memcpy(w->path + w->len, name, len);
    w->len += len;
    w->path[w->len] = 0;

    struct rfs_inode ino;
    if (!ritterfs_get_inode(w->fs, id, &ino)) {
        kprintf("    %s: cannot read inode %lu\n", w->path, id);
    } else if (type == RFS_TYPE_DIR) {
        uint64_t entries = 0;
        ritterfs_iterate_dir(w->fs, id, &ino, count_entry, &entries);
        if (entries > LIST_LIMIT) {
            kprintf("    %s/  (%lu entries)\n", w->path, entries);
        } else {
            kprintf("    %s/\n", w->path);
            ritterfs_iterate_dir(w->fs, id, &ino, list_entry, w);
        }
    } else if (ino.flags & RFS_FLAG_INLINE) {
        kprintf("    %-40s %8lu bytes, inline\n", w->path, ino.size);
    } else {
        kprintf("    %-40s %8lu bytes, tree height %u\n", w->path, ino.size, ino.height);
    }
    w->len = saved;
    w->path[w->len] = 0;
    return true;
}

/* Read a whole file into a new heap buffer; 0 on failure. */
static uint8_t *read_file(struct ritterfs *fs, const char *path, uint64_t *size)
{
    uint64_t id;
    struct rfs_inode ino;
    if (!ritterfs_lookup(fs, path, &id, &ino) || ino.type != RFS_TYPE_FILE)
        return 0;
    uint8_t *data = kmalloc(ino.size ? ino.size : 1);
    if (data && ritterfs_read(fs, id, &ino, 0, data, ino.size) != (int64_t)ino.size) {
        kfree(data);
        return 0;
    }
    *size = ino.size;
    return data;
}

static uint64_t parse_hex(const char **p)
{
    uint64_t v = 0;
    for (; **p && **p != ' ' && **p != '\n'; (*p)++)
        v = v * 16 + (uint64_t)(**p <= '9' ? **p - '0' : (**p | 0x20) - 'a' + 10);
    return v;
}

static uint64_t parse_dec(const char **p)
{
    uint64_t v = 0;
    for (; **p >= '0' && **p <= '9'; (*p)++)
        v = v * 10 + (uint64_t)(**p - '0');
    return v;
}

/* Verify every "CRC SIZE PATH" line of the manifest. */
static void verify_manifest(struct ritterfs *fs)
{
    uint64_t size;
    char *manifest = (char *)read_file(fs, MANIFEST, &size);
    if (!manifest) {
        kprintf("  RitterFS files: %s not found\n", MANIFEST);
        return;
    }
    uint64_t files = 0, bytes = 0, bad = 0;
    const char *p = manifest, *end = manifest + size;
    while (p < end) {
        uint32_t want_crc = (uint32_t)parse_hex(&p);
        p++;
        uint64_t want_size = parse_dec(&p);
        p++;
        char path[256];
        size_t n = 0;
        while (p < end && *p != '\n' && n < sizeof(path) - 1)
            path[n++] = *p++;
        path[n] = 0;
        p++;

        uint64_t got_size = 0;
        uint8_t *data = read_file(fs, path, &got_size);
        uint32_t got_crc = data ? crc32c(data, got_size) : 0;
        if (!data || got_size != want_size || got_crc != want_crc) {
            if (bad++ < 5)
                kprintf("    MISMATCH %s: size %lu crc %08X, expected size %lu crc %08X\n", path,
                        got_size, got_crc, want_size, want_crc);
        }
        files++;
        bytes += got_size;
        kfree(data);
    }
    kfree(manifest);
    kprintf("  RitterFS files: %lu verified against %s (%lu KiB): %s\n", files, MANIFEST,
            bytes / 1024, bad ? "MISMATCHES FOUND" : "all match");
}

void fs_test(struct ritterfs *fs)
{
    struct walk w = { fs, "", 0 };
    struct rfs_inode root;
    kprintf("  RitterFS tree:\n");
    if (ritterfs_get_inode(fs, RFS_OBJ_ROOT, &root))
        ritterfs_iterate_dir(fs, RFS_OBJ_ROOT, &root, list_entry, &w);

    verify_manifest(fs);

    /* A read that starts in one block and ends in the next. */
    uint64_t size, id;
    struct rfs_inode ino;
    uint8_t *whole = read_file(fs, "/test/data/large.bin", &size);
    uint8_t part[20];
    bool partial_ok = whole && size > 4110 &&
                      ritterfs_lookup(fs, "/test/data/large.bin", &id, &ino) &&
                      ritterfs_read(fs, id, &ino, 4090, part, sizeof(part)) == sizeof(part) &&
                      memcmp(part, whole + 4090, sizeof(part)) == 0;
    kfree(whole);
    kprintf("                  partial read across a block boundary: %s\n",
            partial_ok ? "matches" : "WRONG");

    /* Paths that must not resolve. */
    bool missing_ok = !ritterfs_lookup(fs, "/test/does-not-exist", &id, &ino) &&
                      !ritterfs_lookup(fs, "/test/hello.txt/inside-a-file", &id, &ino);
    kprintf("                  lookups of missing paths: %s\n",
            missing_ok ? "correctly not found" : "WRONGLY FOUND");
    /* /test/many (300 entries) and the larger directories have an index. */
    uint64_t lookups, builds;
    rfs_dirindex_counts(fs, &lookups, &builds);
    kprintf("                  directory index: %lu lookups answered, %lu index(es) built\n",
            lookups, builds);
}

/* --- writing ------------------------------------------------------------------ */

#define PATTERN_SEED 0x5249545445524653ULL      /* "RITTERFS"; scripts/test.sh uses it too */

static uint64_t xorshift(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

/* Fill buf with the next n bytes of the pattern stream. */
static void pattern(uint8_t *buf, uint64_t n, uint64_t *state)
{
    for (uint64_t i = 0; i < n; i += 8) {
        uint64_t v = xorshift(state);
        memcpy(buf + i, &v, n - i < 8 ? n - i : 8);
    }
}

static bool step(int r, const char *what)
{
    if (r != RFS_OK)
        kprintf("  write test:    %s: %s\n", what, ritterfs_error(r));
    return r == RFS_OK;
}

static bool commit_and_check(struct ritterfs *fs, int n)
{
    int r = ritterfs_commit(fs);
    if (r != RFS_OK) {
        kprintf("  write test:    commit %d failed: %s\n", n, ritterfs_error(r));
        return false;
    }
    kprintf("  write test:    commit %d ok: generation %lu, %lu blocks used\n", n,
            fs->vol.sb.generation, fs->vol.sb.used_blocks);
    return ritterfs_check(fs);
}

#define BIG_FIRST  (300 * 1024)
#define BIG_TOTAL  (600 * 1024)
#define OVERWRITE_AT 150000
#define HELLO_SIZE 200

void fs_write_test(struct ritterfs *fs)
{
    uint64_t id, big_id, state = PATTERN_SEED;
    uint8_t *big = kmalloc(BIG_TOTAL);
    uint8_t *back = kmalloc(BIG_TOTAL);
    char path[64];
    if (!big || !back)
        return;
    pattern(big, BIG_TOTAL, &state);
    memset(big + OVERWRITE_AT, 'X', 100);       /* what commit 2 does to the file */

    /* Commit 1: a directory with a small file, a 300 KiB file and 40 entries. */
    static const char hello1[] = "Hello, written by the RitterOS kernel!\n";
    bool ok = step(ritterfs_create(fs, "/work", RFS_TYPE_DIR, 0), "mkdir /work") &&
              step(ritterfs_create(fs, "/work/hello.txt", RFS_TYPE_FILE, &id), "create hello") &&
              step(ritterfs_write(fs, id, 0, hello1, sizeof(hello1) - 1), "write hello") &&
              step(ritterfs_create(fs, "/work/big.bin", RFS_TYPE_FILE, &big_id), "create big");
    uint8_t *original = kmalloc(BIG_FIRST);
    state = PATTERN_SEED;
    pattern(original, BIG_FIRST, &state);
    for (uint64_t off = 0; ok && off < BIG_FIRST; off += 10007) {    /* odd-sized pieces */
        uint64_t n = BIG_FIRST - off < 10007 ? BIG_FIRST - off : 10007;
        ok = step(ritterfs_write(fs, big_id, off, original + off, n), "write big");
    }
    kfree(original);
    ok = ok && step(ritterfs_create(fs, "/work/many", RFS_TYPE_DIR, 0), "mkdir many");
    for (int i = 0; ok && i < 40; i++) {
        char text[16];
        ksnprintf(path, sizeof(path), "/work/many/entry-%02d", i);
        ksnprintf(text, sizeof(text), "entry %02d\n", i);
        ok = step(ritterfs_create(fs, path, RFS_TYPE_FILE, &id), "create entry") &&
             step(ritterfs_write(fs, id, 0, text, strlen(text)), "write entry");
    }
    /* Reads see uncommitted data. */
    struct rfs_inode ino;
    ok = ok && ritterfs_get_inode(fs, big_id, &ino) &&
         ritterfs_read(fs, big_id, &ino, BIG_FIRST - 100, back, 100) == 100 &&
         memcmp(back, big + BIG_FIRST - 100, 100) == 0;
    if (ok)
        kprintf("  write test:    uncommitted data reads back correctly\n");
    ok = ok && commit_and_check(fs, 1);

    /* Commit 2: change the middle of big.bin, grow it to height 2, remove two files. */
    ok = ok && step(ritterfs_write(fs, big_id, OVERWRITE_AT, big + OVERWRITE_AT, 100),
                    "overwrite big") &&
         step(ritterfs_write(fs, big_id, BIG_FIRST, big + BIG_FIRST, BIG_TOTAL - BIG_FIRST),
              "append big") &&
         step(ritterfs_remove(fs, "/work/many/entry-07"), "remove entry-07") &&
         step(ritterfs_remove(fs, "/work/hello.txt"), "remove hello") &&
         commit_and_check(fs, 2);

    /* Commit 3: empty and remove the directory; a new, larger hello.txt. */
    for (int i = 0; ok && i < 40; i++) {
        if (i == 7)
            continue;
        ksnprintf(path, sizeof(path), "/work/many/entry-%02d", i);
        ok = step(ritterfs_remove(fs, path), "remove entry");
    }
    char hello2[HELLO_SIZE];
    for (int i = 0; i < HELLO_SIZE; i++)
        hello2[i] = (char)('a' + i % 26);
    hello2[HELLO_SIZE - 1] = '\n';
    ok = ok && step(ritterfs_remove(fs, "/work/many"), "remove many") &&
         step(ritterfs_create(fs, "/work/hello.txt", RFS_TYPE_FILE, &id), "create hello") &&
         step(ritterfs_write(fs, id, 0, hello2, HELLO_SIZE), "write hello") &&
         step(ritterfs_create(fs, "/work/empty-dir", RFS_TYPE_DIR, 0), "mkdir empty-dir") &&
         commit_and_check(fs, 3);

    /* Commit 4: reuse. Commit 3 freed 40 objects: new ones take their IDs
     * (below the next new ID). A removed entry's place takes a shorter
     * name: the directory does not grow. */
    struct rfs_inode work;
    uint64_t work_id, before_next = fs->vol.sb.next_object_id, reused = 0, size_before = 0;
    ok = ok && ritterfs_lookup(fs, "/work", &work_id, &work);
    size_before = work.size;
    for (int i = 0; ok && i < 3; i++) {
        ksnprintf(path, sizeof(path), "/work/reuse-%d", i);
        ok = step(ritterfs_create(fs, path, RFS_TYPE_FILE, &id), "create reuse");
        if (ok && id < before_next)
            reused++;
    }
    ok = ok && step(ritterfs_remove(fs, "/work/reuse-1"), "remove reuse-1") &&
         commit_and_check(fs, 4);
    ok = ok && ritterfs_lookup(fs, "/work", &work_id, &work);
    uint64_t size_middle = work.size;
    ok = ok && step(ritterfs_create(fs, "/work/r1", RFS_TYPE_FILE, &id), "create r1") &&
         commit_and_check(fs, 5) && ritterfs_lookup(fs, "/work", &work_id, &work);
    if (ok)
        kprintf("  write test:    reuse: %lu of 3 new objects took freed IDs; /work %lu -> %lu "
                "bytes, a new entry in a freed place: %lu bytes\n", reused, size_before,
                size_middle, work.size);
    ok = ok && reused == 3 && work.size == size_middle;
    if (fs->dev->discard)
        kprintf("  write test:    TRIM: %lu blocks freed by the commits reported to %s\n",
                fs->discarded, fs->dev->name);
    else
        kprintf("  write test:    TRIM: %s cannot discard\n", fs->dev->name);

    /* Read everything back from disk and compare. */
    if (ok) {
        uint64_t got;
        uint8_t *data = read_file(fs, "/work/big.bin", &got);
        bool big_ok = data && got == BIG_TOTAL && memcmp(data, big, BIG_TOTAL) == 0;
        ritterfs_lookup(fs, "/work/big.bin", &id, &ino);
        kprintf("  write test:    /work/big.bin: %lu bytes, tree height %u, crc32c %08X, %s\n",
                got, ino.height, data ? crc32c(data, got) : 0,
                big_ok ? "content correct" : "CONTENT WRONG");
        kfree(data);
        data = read_file(fs, "/work/hello.txt", &got);
        bool hello_ok = data && got == HELLO_SIZE && memcmp(data, hello2, HELLO_SIZE) == 0;
        kfree(data);
        bool gone = !ritterfs_lookup(fs, "/work/many", &id, &ino);
        kprintf("  write test:    /work/hello.txt %s, /work/many %s\n",
                hello_ok ? "correct" : "WRONG", gone ? "removed" : "STILL THERE");
        ok = big_ok && hello_ok && gone;
    }
    kprintf("  write test:    %s\n", ok ? "done" : "FAILED");
    kfree(big);
    kfree(back);
}

/* --- crash torture ---------------------------------------------------------- */

#define TORTURE_FILES 48
#define TORTURE_COMMITS 5000

static bool find_torture_file(void *ctx, const char *name, size_t len, uint64_t id, uint8_t type)
{
    bool *exists = ctx;
    (void)id;
    if (type == RFS_TYPE_FILE && len == 3 && name[0] == 'f') {
        int n = (name[1] - '0') * 10 + (name[2] - '0');
        if (n >= 0 && n < TORTURE_FILES)
            exists[n] = true;
    }
    return true;
}

void fs_torture(struct ritterfs *fs, uint64_t seed)
{
    bool exists[TORTURE_FILES] = {0};
    uint64_t state = seed ? seed : 1, id;
    struct rfs_inode ino;
    uint8_t *buf = kmalloc(20000);
    char path[16];
    if (!buf)
        return;

    if (ritterfs_lookup(fs, "/t", &id, &ino))
        ritterfs_iterate_dir(fs, id, &ino, find_torture_file, exists);
    else if (ritterfs_create(fs, "/t", RFS_TYPE_DIR, 0) != RFS_OK || ritterfs_commit(fs) != RFS_OK)
        return;
    kprintf("  torture: started at generation %lu\n", fs->vol.sb.generation);

    for (int commit = 1; commit <= TORTURE_COMMITS; commit++) {
        int ops = 1 + (int)(xorshift(&state) % 4);
        for (int op = 0; op < ops; op++) {
            int k = (int)(xorshift(&state) % TORTURE_FILES);
            uint64_t kind = xorshift(&state) % 10;
            ksnprintf(path, sizeof(path), "/t/f%02d", k);
            int r = RFS_OK;
            if (kind < 6) {                           /* create or write */
                if (!exists[k])
                    r = ritterfs_create(fs, path, RFS_TYPE_FILE, 0);
                exists[k] = true;
                if (r == RFS_OK && ritterfs_lookup(fs, path, &id, &ino)) {
                    uint64_t len = xorshift(&state) % 20000;
                    uint64_t off = xorshift(&state) % (ino.size + 1);
                    pattern(buf, len, &state);
                    r = ritterfs_write(fs, id, off, buf, len);
                }
            } else if (exists[k]) {                   /* remove */
                r = ritterfs_remove(fs, path);
                exists[k] = false;
            }
            if (r != RFS_OK) {
                kprintf("  torture: %s failed: %s\n", path, ritterfs_error(r));
                kfree(buf);
                return;
            }
        }
        int r = ritterfs_commit(fs);
        if (r != RFS_OK) {
            kprintf("  torture: commit failed: %s\n", ritterfs_error(r));
            break;
        }
        kprintf("  torture: commit %d ok, generation %lu\n", commit, fs->vol.sb.generation);
    }
    kprintf("  torture: done, generation %lu\n", fs->vol.sb.generation);
    ritterfs_check(fs);
    kfree(buf);
}

/* --- snapshots ------------------------------------------------------------------ */

static bool same_text(struct ritterfs *fs, const char *path, const char *want)
{
    uint64_t size;
    uint8_t *data = read_file(fs, path, &size);
    bool same = data && size == strlen(want) && memcmp(data, want, size) == 0;
    kfree(data);
    return same;
}

static bool print_snapshot(void *ctx, const char *name, uint64_t generation, uint64_t created)
{
    (void)ctx, (void)created;
    kprintf("  snapshot test:   listed: '%s' at generation %lu\n", name, generation);
    return true;
}

void fs_snapshot_test(struct ritterfs *fs)
{
    static const char v1[] = "version 1\n", v2[] = "version 2, written after the snapshot\n";
    uint64_t id, freed = 0;
    bool ok = step(ritterfs_create(fs, "/snap", RFS_TYPE_DIR, 0), "mkdir /snap") &&
              step(ritterfs_create(fs, "/snap/a.txt", RFS_TYPE_FILE, &id), "create a") &&
              step(ritterfs_write(fs, id, 0, v1, sizeof(v1) - 1), "write a") &&
              step(ritterfs_commit(fs), "commit") &&
              step(ritterfs_snapshot_create(fs, "before"), "snapshot before");
    uint64_t snapshot_generation = fs->vol.sb.generation - 1;
    if (ok)
        kprintf("  snapshot test:   'before' taken at generation %lu\n", snapshot_generation);
    /* Change after the snapshot: a.txt rewritten, b.txt new, much data. */
    uint64_t b;
    uint8_t *data = kmalloc(200 * 1024);
    ok = ok && data && step(ritterfs_write(fs, id, 0, v2, sizeof(v2) - 1), "rewrite a") &&
         step(ritterfs_create(fs, "/snap/b.txt", RFS_TYPE_FILE, &b), "create b");
    if (data)
        memset(data, 'b', 200 * 1024);
    ok = ok && step(ritterfs_write(fs, b, 0, data, 200 * 1024), "write b") &&
         step(ritterfs_commit(fs), "commit after the snapshot");
    kfree(data);
    struct ritterfs *before = ok ? ritterfs_snapshot_view(fs, "before") : 0;
    uint64_t b_id;
    struct rfs_inode ino;
    bool old_kept = before && same_text(before, "/snap/a.txt", v1) &&
                    !ritterfs_lookup(before, "/snap/b.txt", &b_id, &ino);
    bool live_new = same_text(fs, "/snap/a.txt", v2) && ritterfs_lookup(fs, "/snap/b.txt", &b_id, &ino);
    kprintf("  snapshot test:   in 'before' a.txt is %s and b.txt %s; live: a.txt %s\n",
            old_kept ? "version 1" : "WRONG", old_kept ? "absent" : "?",
            live_new ? "version 2, b.txt there" : "WRONG");
    bool refused = ritterfs_snapshot_create(fs, "before") == RFS_ERR_EXISTS;
    kprintf("  snapshot test:   a second 'before': %s\n", refused ? "refused (exists)" : "WRONG");
    ok = ok && old_kept && live_new && refused && ritterfs_check(fs);
    ok = ok && step(ritterfs_snapshot_create(fs, "after"), "snapshot after");
    ritterfs_snapshot_list(fs, print_snapshot, 0);
    ok = ok && step(ritterfs_snapshot_delete(fs, "before", &freed), "delete before");
    if (ok)
        kprintf("  snapshot test:   'before' deleted, the sweep freed %lu block(s)\n", freed);
    bool gone = !ritterfs_snapshot_view(fs, "before");
    ok = ok && gone && ritterfs_check(fs);
    kprintf("  snapshot test:   %s\n", ok ? "done ('after' stays)" : "FAILED");
}

/* --- instant copies ---------------------------------------------------------------- */

#define COPY_BIG   (300 * 1024)
#define COPY_AT    100000                       /* where the copy is changed */

/* The big file's content as /copy/src had it, and whether path holds it
 * (with "CHANGED" at COPY_AT if changed). */
static bool same_big(struct ritterfs *fs, const char *path, const uint8_t *want, bool changed)
{
    uint64_t size;
    uint8_t *data = read_file(fs, path, &size);
    bool same = data && size == COPY_BIG;
    for (uint64_t i = 0; same && i < size; i++) {
        bool in_change = changed && i >= COPY_AT && i < COPY_AT + 7;
        same = data[i] == (in_change ? (uint8_t)"CHANGED"[i - COPY_AT] : want[i]);
    }
    kfree(data);
    return same;
}

void fs_copy_test(struct ritterfs *fs)
{
    static const char small[] = "a small file, kept in its inode\n";
    static const char other[] = "the copy's own text\n";
    uint64_t seed = PATTERN_SEED, big, little, id, freed = 0;
    struct rfs_inode ino;
    uint8_t *data = kmalloc(COPY_BIG);
    if (!data)
        return;
    pattern(data, COPY_BIG, &seed);
    bool ok = step(ritterfs_create(fs, "/copy", RFS_TYPE_DIR, 0), "mkdir /copy") &&
              step(ritterfs_create(fs, "/copy/src", RFS_TYPE_DIR, 0), "mkdir src") &&
              step(ritterfs_create(fs, "/copy/src/sub", RFS_TYPE_DIR, 0), "mkdir sub") &&
              step(ritterfs_create(fs, "/copy/src/big.bin", RFS_TYPE_FILE, &big), "create big") &&
              step(ritterfs_write(fs, big, 0, data, COPY_BIG), "write big") &&
              step(ritterfs_create(fs, "/copy/src/sub/small.txt", RFS_TYPE_FILE, &little),
                   "create small") &&
              step(ritterfs_write(fs, little, 0, small, sizeof(small) - 1), "write small") &&
              step(ritterfs_commit(fs), "commit");

    /* The copy: only metadata is new (object table, directories, bitmap). */
    uint64_t used = fs->vol.sb.used_blocks;
    ok = ok && step(ritterfs_copy(fs, "/copy/src", "/copy/dst"), "copy");
    uint64_t grown = fs->vol.sb.used_blocks - used;
    bool same = ok && same_big(fs, "/copy/dst/big.bin", data, false) &&
                same_text(fs, "/copy/dst/sub/small.txt", small);
    kprintf("  copy test:      /copy/src copied to /copy/dst: %lu KiB shared, %lu new blocks "
            "(metadata), content %s\n", (uint64_t)COPY_BIG / 1024, grown,
            same ? "identical" : "WRONG");
    ok = ok && same && grown < COPY_BIG / RFS_BLOCK_SIZE / 2 && ritterfs_check(fs);

    bool refused = ritterfs_copy(fs, "/copy/src", "/copy/src/sub/inside") == RFS_ERR_BAD_NAME &&
                   ritterfs_copy(fs, "/copy/src", "/copy/dst") == RFS_ERR_EXISTS &&
                   ritterfs_copy(fs, "/copy/nothing", "/copy/x") == RFS_ERR_NOT_FOUND &&
                   !ritterfs_lookup(fs, "/copy/x", &id, &ino) && !rfs_has_changes(fs);
    kprintf("  copy test:      into itself, onto an existing name, from nothing: %s\n",
            refused ? "refused, nothing changed" : "WRONG");

    /* Changing the copy leaves the original alone. */
    uint64_t dst_big = 0, dst_small = 0;
    ok = ok && refused && ritterfs_lookup(fs, "/copy/dst/big.bin", &dst_big, &ino) &&
         ritterfs_lookup(fs, "/copy/dst/sub/small.txt", &dst_small, &ino) &&
         step(ritterfs_write(fs, dst_big, COPY_AT, "CHANGED", 7), "change the copy") &&
         step(ritterfs_write(fs, dst_small, 0, other, sizeof(other) - 1), "change small") &&
         step(ritterfs_commit(fs), "commit the change");
    bool apart = ok && same_big(fs, "/copy/src/big.bin", data, false) &&
                 same_text(fs, "/copy/src/sub/small.txt", small) &&
                 same_big(fs, "/copy/dst/big.bin", data, true);
    kprintf("  copy test:      the copy changed: the original %s\n",
            apart ? "is unchanged" : "WRONG");
    ok = ok && apart && ritterfs_check(fs);

    /* The original removed: a sweep frees what only it used; nothing is
     * shared any more, so nothing stays pinned. */
    ok = ok && step(ritterfs_remove(fs, "/copy/src/sub/small.txt"), "rm small") &&
         step(ritterfs_remove(fs, "/copy/src/sub"), "rmdir sub") &&
         step(ritterfs_remove(fs, "/copy/src/big.bin"), "rm big") &&
         step(ritterfs_remove(fs, "/copy/src"), "rmdir src") &&
         step(ritterfs_commit(fs), "commit the removal") &&
         step(ritterfs_sweep(fs, &freed), "sweep");
    if (ok)
        kprintf("  copy test:      /copy/src removed, the sweep freed %lu block(s); pinned "
                "generation now %lu\n", freed, fs->vol.sb.pinned_generation);
    ok = ok && same_big(fs, "/copy/dst/big.bin", data, true) && ritterfs_check(fs);

    /* One more copy that stays, so the host checks a state with sharing. */
    ok = ok && step(ritterfs_copy(fs, "/copy/dst", "/copy/again"), "copy again") &&
         same_big(fs, "/copy/again/big.bin", data, true) && ritterfs_check(fs);
    kfree(data);
    kprintf("  copy test:      %s\n", ok ? "done (/copy/dst and its copy /copy/again stay)"
                                        : "FAILED");
}

/* --- attributes -------------------------------------------------------------------- */

static bool attr_is(struct ritterfs *fs, uint64_t object, const char *key, const char *want)
{
    char value[RFS_ATTR_VALUE_MAX];
    int64_t n = ritterfs_attr_get(fs, object, key, value, sizeof(value));
    return want ? n == (int64_t)strlen(want) && memcmp(value, want, (size_t)n) == 0
                : n == RFS_ERR_NOT_FOUND;
}

static bool count_attr(void *ctx, uint64_t object, const char *key, size_t key_length,
                       const uint8_t *value, size_t value_length)
{
    (void)object, (void)key, (void)key_length, (void)value, (void)value_length;
    (*(uint64_t *)ctx)++;
    return true;
}

static uint64_t attr_count(struct ritterfs *fs, uint64_t object)
{
    uint64_t n = 0;
    ritterfs_attr_each(fs, object, count_attr, &n);
    return n;
}

#define ATTR_MANY 40

void fs_attr_test(struct ritterfs *fs)
{
    uint64_t a = 0, b = 0, d = 0, c = 0, a2 = 0, d2 = 0;
    struct rfs_inode ino;
    char long_value[1000], key[16], value[200];
    memset(long_value, 'x', sizeof(long_value));
    bool ok = step(ritterfs_create(fs, "/attr", RFS_TYPE_DIR, 0), "mkdir /attr") &&
              step(ritterfs_create(fs, "/attr/a.txt", RFS_TYPE_FILE, &a), "create a") &&
              step(ritterfs_create(fs, "/attr/b.txt", RFS_TYPE_FILE, &b), "create b") &&
              step(ritterfs_create(fs, "/attr/d", RFS_TYPE_DIR, &d), "mkdir d") &&
              step(ritterfs_attr_set(fs, a, "color", "blue", 4), "set color") &&
              step(ritterfs_attr_set(fs, a, "note", long_value, sizeof(long_value)), "set note") &&
              step(ritterfs_attr_set(fs, b, "color", "red", 3), "set b color") &&
              step(ritterfs_attr_set(fs, d, "kind", "folder", 6), "set d kind") &&
              step(ritterfs_commit(fs), "commit");
    bool read_back = ok && attr_is(fs, a, "color", "blue") && attr_is(fs, b, "color", "red") &&
                     attr_is(fs, d, "kind", "folder") && attr_is(fs, a, "size", 0);
    kprintf("  attribute test: a.txt color=blue, a 1000-byte note, b.txt color=red, d kind=folder: "
            "%s\n", read_back ? "read back" : "WRONG");

    /* Replace (larger), remove, and what is refused. */
    ok = ok && read_back &&
         step(ritterfs_attr_set(fs, a, "color", "green and bright", 16), "replace color") &&
         step(ritterfs_attr_remove(fs, a, "note"), "remove note") && step(ritterfs_commit(fs), "commit");
    bool changed = ok && attr_is(fs, a, "color", "green and bright") && attr_is(fs, a, "note", 0) &&
                   attr_count(fs, a) == 1;
    bool refused = ritterfs_attr_set(fs, a, "a=b", "x", 1) == RFS_ERR_BAD_NAME &&
                   ritterfs_attr_set(fs, a, "", "x", 1) == RFS_ERR_BAD_NAME &&
                   ritterfs_attr_set(fs, a, "big", long_value, RFS_ATTR_VALUE_MAX + 1) ==
                       RFS_ERR_BAD_NAME &&
                   ritterfs_attr_set(fs, 999999, "k", "v", 1) == RFS_ERR_NOT_FOUND &&
                   ritterfs_attr_remove(fs, a, "nothing") == RFS_ERR_NOT_FOUND &&
                   !rfs_has_changes(fs);
    kprintf("  attribute test: replaced and removed: %s; bad keys, a too long value, no such "
            "object or key: %s\n", changed ? "ok" : "WRONG", refused ? "refused" : "WRONG");
    ok = ok && changed && refused && ritterfs_check(fs);

    /* Many attributes: the table grows past a block (records never cross). */
    memset(value, 'v', sizeof(value));
    for (int i = 0; ok && i < ATTR_MANY; i++) {
        ksnprintf(key, sizeof(key), "k%d", i);
        value[0] = (char)('A' + i % 26);
        ok = step(ritterfs_attr_set(fs, b, key, value, sizeof(value)), "set many");
    }
    ok = ok && step(ritterfs_commit(fs), "commit many");
    bool many = ok && attr_count(fs, b) == ATTR_MANY + 1;
    for (int i = 0; many && i < ATTR_MANY; i++) {
        char got[sizeof(value)];
        ksnprintf(key, sizeof(key), "k%d", i);
        many = ritterfs_attr_get(fs, b, key, got, sizeof(got)) == (int64_t)sizeof(value) &&
               got[0] == 'A' + i % 26 && got[sizeof(got) - 1] == 'v';
    }
    uint64_t table_size = 0;
    if (ritterfs_get_inode(fs, fs->vol.sb.attribute_object, &ino))
        table_size = ino.size;
    kprintf("  attribute test: %d more on b.txt, table %lu bytes: %s\n", ATTR_MANY, table_size,
            many && table_size > RFS_BLOCK_SIZE ? "all read back" : "WRONG");
    ok = ok && many && table_size > RFS_BLOCK_SIZE && ritterfs_check(fs);

    /* A removed file's attributes go with it: a new file that takes its
     * object ID has none. */
    ok = ok && step(ritterfs_remove(fs, "/attr/b.txt"), "rm b") && step(ritterfs_commit(fs), "commit") &&
         step(ritterfs_create(fs, "/attr/c.txt", RFS_TYPE_FILE, &c), "create c") &&
         step(ritterfs_commit(fs), "commit c");
    bool dropped = ok && attr_count(fs, b) == 0 && c == b && attr_count(fs, c) == 0;
    kprintf("  attribute test: b.txt removed, its attributes with it; c.txt took its object ID "
            "and has %s\n", dropped ? "none" : "WRONG");
    ok = ok && dropped;

    /* Instant copies keep the attributes. */
    ok = ok && step(ritterfs_copy(fs, "/attr/a.txt", "/attr/a2.txt"), "copy a") &&
         step(ritterfs_copy(fs, "/attr/d", "/attr/d2"), "copy d") &&
         ritterfs_lookup(fs, "/attr/a2.txt", &a2, &ino) && ritterfs_lookup(fs, "/attr/d2", &d2, &ino);
    bool kept = ok && attr_is(fs, a2, "color", "green and bright") && attr_is(fs, d2, "kind", "folder");
    kprintf("  attribute test: copies keep them: %s\n", kept ? "yes" : "WRONG");
    ok = ok && kept && ritterfs_check(fs);
    kprintf("  attribute test: %s\n", ok ? "done (/attr stays)" : "FAILED");
}

/* --- transactions ------------------------------------------------------------------- */

void fs_abort_test(struct ritterfs *fs)
{
    uint64_t seed = PATTERN_SEED, id, gen = fs->vol.sb.generation, used = fs->vol.sb.used_blocks;
    struct rfs_inode ino;
    uint64_t size;
    uint8_t *readme = read_file(fs, "/readme.txt", &size), *data = kmalloc(100 * 1024);
    uint64_t readme_id = 0;
    ritterfs_lookup(fs, "/readme.txt", &readme_id, &ino);
    if (!readme || !data) {
        kfree(readme);
        kfree(data);
        return;
    }
    pattern(data, 100 * 1024, &seed);
    /* Changes of every kind, then abort. */
    bool ok = step(ritterfs_create(fs, "/txn", RFS_TYPE_DIR, 0), "mkdir /txn") &&
              step(ritterfs_create(fs, "/txn/big.bin", RFS_TYPE_FILE, &id), "create") &&
              step(ritterfs_write(fs, id, 0, data, 100 * 1024), "write") &&
              step(ritterfs_attr_set(fs, readme_id, "draft", "yes", 3), "set attribute") &&
              step(ritterfs_remove(fs, "/readme.txt"), "rm /readme.txt");
    bool pending = ok && rfs_has_changes(fs) && !ritterfs_lookup(fs, "/readme.txt", &id, &ino) &&
                   ritterfs_lookup(fs, "/txn/big.bin", &id, &ino);
    kprintf("  transaction:    uncommitted: /txn/big.bin (100 KiB) there, an attribute set, "
            "/readme.txt gone: %s\n", pending ? "yes" : "WRONG");
    ritterfs_abort(fs);
    uint64_t again;
    uint8_t *back = read_file(fs, "/readme.txt", &again);
    char value[8];
    bool undone = !rfs_has_changes(fs) && !ritterfs_lookup(fs, "/txn", &id, &ino) && back &&
                  again == size && memcmp(back, readme, size) == 0 &&
                  ritterfs_attr_get(fs, readme_id, "draft", value, sizeof(value)) < 0 &&
                  fs->vol.sb.generation == gen && fs->vol.sb.used_blocks == used;
    kfree(back);
    kprintf("  transaction:    aborted: all of it undone (generation %lu, %lu blocks used): %s\n",
            fs->vol.sb.generation, fs->vol.sb.used_blocks, undone ? "yes" : "WRONG");
    ok = ok && pending && undone && ritterfs_check(fs);

    /* Changes after an abort commit as usual. */
    ok = ok && step(ritterfs_create(fs, "/txn", RFS_TYPE_DIR, 0), "mkdir /txn again") &&
         step(ritterfs_create(fs, "/txn/after.txt", RFS_TYPE_FILE, &id), "create after") &&
         step(ritterfs_write(fs, id, 0, "after the abort\n", 16), "write after") &&
         step(ritterfs_commit(fs), "commit") && same_text(fs, "/txn/after.txt", "after the abort\n") &&
         ritterfs_check(fs);
    kfree(readme);
    kfree(data);
    kprintf("  transaction:    %s\n", ok ? "done (a commit after the abort: /txn/after.txt)"
                                       : "FAILED");
}
