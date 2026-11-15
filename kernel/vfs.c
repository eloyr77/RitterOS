/*
 * The file systems behind struct vfs (see vfs.h): each call goes to
 * RitterFS, FAT or exFAT.
 */
#include "vfs.h"
#include "exfat.h"
#include "fat.h"
#include "ritterfs.h"
#include "kprintf.h"
#include "pmm.h"
#include "paging.h"
#include "memory.h"
#include "heap.h"
#include "string.h"

struct ritterfs *vfs_ritterfs(struct vfs *v)
{
    if (!v || v->kind != VFS_RITTERFS)
        return 0;
    return (struct ritterfs *)((char *)v - offsetof(struct ritterfs, vfs));
}

struct fat *vfs_fat(struct vfs *v)
{
    return v && v->kind == VFS_FAT ? (struct fat *)v : 0;     /* its first member */
}

struct exfat *vfs_exfat(struct vfs *v)
{
    return v && v->kind == VFS_EXFAT ? (struct exfat *)v : 0;   /* its first member */
}

struct vfs *vfs_mount(struct block_device *dev)
{
    struct ritterfs *fs = ritterfs_mount(dev);
    if (fs) {
        if (!ritterfs_check(fs)) {
            fs->read_only = true;
            kprintf("  %s: problems found: mounted read only\n", dev->name);
        }
        return &fs->vfs;
    }
    struct fat *f = fat_mount(dev);
    if (f)
        return fat_vfs(f);
    struct exfat *x = exfat_mount(dev);
    return x ? exfat_vfs(x) : 0;
}

const char *vfs_type(struct vfs *v)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return "exFAT";
    struct fat *f = vfs_fat(v);
    return f ? fat_type(f) : "RitterFS";
}

struct block_device *vfs_device(struct vfs *v)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_device(x);
    struct fat *f = vfs_fat(v);
    return f ? fat_device(f) : vfs_ritterfs(v)->dev;
}

bool vfs_read_only(struct vfs *v)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_read_only(x);
    struct fat *f = vfs_fat(v);
    if (f)
        return fat_read_only(f);
    struct ritterfs *fs = vfs_ritterfs(v);
    return fs->read_only || fs->view;
}

void vfs_set_read_only(struct vfs *v)
{
    struct exfat *x = vfs_exfat(v);
    if (x) {
        exfat_set_read_only(x);
        return;
    }
    struct fat *f = vfs_fat(v);
    if (f)
        fat_set_read_only(f);
    else
        vfs_ritterfs(v)->read_only = true;
}

bool vfs_is_view(struct vfs *v)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    return fs && fs->view;
}

void vfs_label(struct vfs *v, char *out, size_t size)
{
    struct exfat *x = vfs_exfat(v);
    if (x) {
        exfat_label(x, out, size);
        return;
    }
    struct fat *f = vfs_fat(v);
    if (f) {
        fat_label(f, out, size);
        return;
    }
    /* RitterFS: printable ASCII, trailing spaces dropped. */
    const char *label = vfs_ritterfs(v)->vol.sb.label;
    size_t n = 0;
    for (size_t i = 0; i < sizeof(vfs_ritterfs(v)->vol.sb.label) && label[i] && n + 1 < size; i++)
        out[n++] = label[i] >= 0x20 && label[i] < 0x7F ? label[i] : '?';
    while (n > 0 && out[n - 1] == ' ')
        n--;
    if (size)
        out[n] = 0;
}

uint64_t vfs_generation(struct vfs *v)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    return fs ? fs->vol.sb.generation : 0;
}

bool vfs_get_inode(struct vfs *v, uint64_t id, struct rfs_inode *out)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_get_inode(x, id, out);
    struct fat *f = vfs_fat(v);
    return f ? fat_get_inode(f, id, out) : ritterfs_get_inode(vfs_ritterfs(v), id, out);
}

bool vfs_lookup(struct vfs *v, const char *path, uint64_t *id, struct rfs_inode *inode)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_lookup(x, path, id, inode);
    struct fat *f = vfs_fat(v);
    return f ? fat_lookup(f, path, id, inode) : ritterfs_lookup(vfs_ritterfs(v), path, id, inode);
}

int64_t vfs_read(struct vfs *v, uint64_t id, const struct rfs_inode *inode, uint64_t offset,
                 void *buf, uint64_t size)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_read(x, id, inode, offset, buf, size);
    struct fat *f = vfs_fat(v);
    return f ? fat_read(f, id, inode, offset, buf, size)
             : ritterfs_read(vfs_ritterfs(v), id, inode, offset, buf, size);
}

int64_t vfs_read_now(struct vfs *v, uint64_t id, struct rfs_inode *inode, uint64_t offset,
                     void *buf, uint64_t size)
{
    if (!vfs_exfat(v) && !vfs_fat(v))
        return ritterfs_read_now(vfs_ritterfs(v), id, inode, offset, buf, size);
    if (!vfs_get_inode(v, id, inode))
        return 0;
    return vfs_read(v, id, inode, offset, buf, size);
}

int vfs_dir_next(struct vfs *v, uint64_t dir_id, const struct rfs_inode *dir, uint64_t *pos,
                 char *name, uint8_t *name_len, uint64_t *id, uint8_t *type)
{
    struct exfat *x = vfs_exfat(v);
    if (x)
        return exfat_dir_next(x, dir_id, dir, pos, name, name_len, id, type);
    struct fat *f = vfs_fat(v);
    return f ? fat_dir_next(f, dir_id, dir, pos, name, name_len, id, type)
             : ritterfs_dir_next(vfs_ritterfs(v), dir_id, dir, pos, name, name_len, id, type);
}

int vfs_create(struct vfs *v, const char *path, int type, uint64_t *id)
{
    if (vfs_exfat(v))
        return exfat_create(vfs_exfat(v), path, type, id);
    struct fat *f = vfs_fat(v);
    return f ? fat_create(f, path, type, id) : ritterfs_create(vfs_ritterfs(v), path, type, id);
}

int vfs_write(struct vfs *v, uint64_t id, uint64_t offset, const void *data, uint64_t size)
{
    if (vfs_exfat(v))
        return exfat_write(vfs_exfat(v), id, offset, data, size);
    struct fat *f = vfs_fat(v);
    return f ? fat_write(f, id, offset, data, size)
             : ritterfs_write(vfs_ritterfs(v), id, offset, data, size);
}

int vfs_append(struct vfs *v, uint64_t id, const void *data, uint64_t size, uint64_t *at)
{
    if (!vfs_exfat(v) && !vfs_fat(v))
        return ritterfs_append(vfs_ritterfs(v), id, data, size, at);
    struct rfs_inode inode;
    if (!vfs_get_inode(v, id, &inode))
        return RFS_ERR_NOT_FOUND;
    *at = inode.size;
    return vfs_write(v, id, inode.size, data, size);
}

int vfs_truncate(struct vfs *v, uint64_t id)
{
    if (vfs_exfat(v))
        return exfat_truncate(vfs_exfat(v), id);
    struct fat *f = vfs_fat(v);
    return f ? fat_truncate(f, id) : ritterfs_truncate(vfs_ritterfs(v), id);
}

int vfs_remove(struct vfs *v, const char *path)
{
    if (vfs_exfat(v))
        return exfat_remove(vfs_exfat(v), path);
    struct fat *f = vfs_fat(v);
    return f ? fat_remove(f, path) : ritterfs_remove(vfs_ritterfs(v), path);
}

/* (57a) Links: RitterFS only (FAT and exFAT have none). */
int vfs_symlink(struct vfs *v, const char *path, const char *target, size_t length, uint64_t *id)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    return fs ? ritterfs_symlink(fs, path, target, length, id) : RFS_ERR_NOT_SUPPORTED;
}

int vfs_link(struct vfs *v, const char *from, const char *to)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    return fs ? ritterfs_link(fs, from, to) : RFS_ERR_NOT_SUPPORTED;
}

int vfs_rename(struct vfs *v, const char *from, const char *to)
{
    if (vfs_exfat(v))
        return exfat_rename(vfs_exfat(v), from, to);
    struct fat *f = vfs_fat(v);
    return f ? fat_rename(f, from, to) : ritterfs_rename(vfs_ritterfs(v), from, to);
}

int vfs_chmod(struct vfs *v, uint64_t id, unsigned mode)
{
    if (vfs_exfat(v))
        return exfat_chmod(vfs_exfat(v), id, mode);
    struct fat *f = vfs_fat(v);
    return f ? fat_chmod(f, id, mode) : ritterfs_chmod(vfs_ritterfs(v), id, mode);
}

int vfs_set_time(struct vfs *v, uint64_t id, uint64_t modified)
{
    if (vfs_exfat(v))
        return exfat_set_time(vfs_exfat(v), id, modified);
    struct fat *f = vfs_fat(v);
    return f ? fat_set_time(f, id, modified) : ritterfs_set_time(vfs_ritterfs(v), id, modified);
}

int vfs_chown(struct vfs *v, uint64_t id, uint32_t uid, uint32_t gid)
{
    if (vfs_exfat(v))
        return exfat_read_only(vfs_exfat(v)) ? RFS_ERR_READ_ONLY : RFS_OK;  /* no owners */
    struct fat *f = vfs_fat(v);
    if (f)
        return fat_read_only(f) ? RFS_ERR_READ_ONLY : RFS_OK;   /* FAT has no owners */
    return ritterfs_chown(vfs_ritterfs(v), id, uid, gid);
}

int vfs_commit(struct vfs *v)
{
    if (vfs_exfat(v))
        return exfat_commit(vfs_exfat(v));
    struct fat *f = vfs_fat(v);
    return f ? fat_commit(f) : ritterfs_commit(vfs_ritterfs(v));
}

void vfs_abort(struct vfs *v)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    if (fs)
        ritterfs_abort(fs);
}

/* (43b-1) see vfs.h */
int vfs_set_size(struct vfs *v, uint64_t id, uint64_t size)
{
    struct rfs_inode ino;
    if (!vfs_get_inode(v, id, &ino))
        return RFS_ERR_NOT_FOUND;
    if (ino.type != RFS_TYPE_FILE)
        return RFS_ERR_IS_DIR;
    uint64_t old = ino.size;
    if (size == old)
        return RFS_OK;
    if (size == 0)
        return vfs_truncate(v, id);
    if (size > old) {                           /* zeros after the end, a page at a time */
        uint64_t page = pmm_alloc_page();
        if (!page)
            return RFS_ERR_IO;
        uint8_t *zeros = phys_to_virt(page);
        memset(zeros, 0, PAGE_SIZE);
        int r = RFS_OK;
        for (uint64_t at = old; at < size && r == RFS_OK;) {
            uint64_t n = size - at < PAGE_SIZE ? size - at : PAGE_SIZE;
            r = vfs_write(v, id, at, zeros, n);
            at += n;
        }
        pmm_free_page(page);
        return r;
    }
    /* shorter: the first size bytes into pages, the file cut, written back */
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t *kept = kmalloc(pages * sizeof(uint64_t));
    if (!kept)
        return RFS_ERR_IO;
    uint64_t have = 0;
    int r = RFS_OK;
    for (; have < pages; have++) {
        kept[have] = pmm_alloc_page();
        if (!kept[have]) {
            r = RFS_ERR_IO;
            break;
        }
        uint64_t at = have * PAGE_SIZE, n = size - at < PAGE_SIZE ? size - at : PAGE_SIZE;
        if (vfs_read(v, id, &ino, at, phys_to_virt(kept[have]), n) != (int64_t)n) {
            r = RFS_ERR_IO;
            have++;
            break;
        }
    }
    if (r == RFS_OK)
        r = vfs_truncate(v, id);
    for (uint64_t i = 0; r == RFS_OK && i < pages; i++) {
        uint64_t at = i * PAGE_SIZE, n = size - at < PAGE_SIZE ? size - at : PAGE_SIZE;
        r = vfs_write(v, id, at, phys_to_virt(kept[i]), n);
    }
    for (uint64_t i = 0; i < have; i++)
        pmm_free_page(kept[i]);
    kfree(kept);
    return r;
}

bool vfs_space(struct vfs *v, uint64_t *block_size, uint64_t *blocks, uint64_t *free_blocks)
{
    struct ritterfs *fs = vfs_ritterfs(v);
    if (!fs || vfs_fat(v) || vfs_exfat(v))
        return false;
    uint64_t since = rfs_read_lock(fs);
    *block_size = fs->vol.sb.block_size;
    *blocks = fs->vol.sb.block_count;
    uint64_t used = fs->vol.sb.used_blocks;     /* (frees held back for snapshots stay used) */
    *free_blocks = used < *blocks ? *blocks - used : 0;
    rfs_read_unlock(fs, since);
    return true;
}
