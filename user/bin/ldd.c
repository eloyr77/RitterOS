/*
 * ldd PROGRAM...: the shared libraries a program needs (step 22d) -- the ones
 * it names and the ones those name, each where ld.so finds it in
 * /system/<arch>/lib -- and whether all the functions they need are there,
 * the program and the libraries read from their files (mapped, never run).
 * A program is a path, or a bare name looked up like the shell does (bin,
 * then apps). Exit code 0 when everything is there, 1 if anything is missing.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ritteros.h"
#include "../../common/elf.h"

PROGRAM_HELP("ldd PROGRAM...", "the shared libraries a program needs, and whether they are all there");

#if defined(__x86_64__)
#define ARCH "x86_64"
#else
#define ARCH "aarch64"
#endif
#define LIB_DIR "/system/" ARCH "/lib/"
#define MAX_OBJECTS 16

struct object {                         /* a program or a library, mapped from its file */
    char name[64], path[160];
    const uint8_t *data;
    uint64_t size;
    const Elf64_Phdr *ph;
    int phnum;
    const char *strtab;
    const Elf64_Sym *symtab;
    const uint32_t *hash;
    uint64_t needed[16];
    int needed_count;
};

static struct object objects[MAX_OBJECTS];
static int count;

/* Where a virtual address of the object lies in its file (0: nowhere). */
static const void *at(const struct object *o, uint64_t vaddr)
{
    for (int i = 0; i < o->phnum; i++) {
        const Elf64_Phdr *s = &o->ph[i];
        if (s->p_type == PT_LOAD && vaddr >= s->p_vaddr && vaddr < s->p_vaddr + s->p_filesz &&
            s->p_offset + (vaddr - s->p_vaddr) < o->size)
            return o->data + s->p_offset + (vaddr - s->p_vaddr);
    }
    return 0;
}

/* Map path and read its dynamic section. 1: dynamic, 0: static, -1: not an
 * ELF file for this CPU (errno for an open error). */
static int open_object(struct object *o, const char *path)
{
    snprintf(o->path, sizeof(o->path), "%s", path);
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || stat(path, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        if (fd >= 0)
            close(fd), errno = ENOEXEC;
        return -1;
    }
    o->size = (uint64_t)st.st_size;
    o->data = mmap(0, o->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (o->data == MAP_FAILED)
        return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)o->data;
    if (eh->e_magic != ELF_MAGIC || eh->e_class != ELFCLASS64 || eh->e_machine != ELF_MACHINE_NATIVE ||
        eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > o->size) {
        errno = ENOEXEC;
        return -1;
    }
    o->ph = (const Elf64_Phdr *)(o->data + eh->e_phoff);
    o->phnum = eh->e_phnum;
    const Elf64_Dyn *dyn = 0;
    for (int i = 0; i < o->phnum; i++)
        if (o->ph[i].p_type == PT_DYNAMIC && o->ph[i].p_offset < o->size)
            dyn = (const Elf64_Dyn *)(o->data + o->ph[i].p_offset);
    if (!dyn)
        return 0;
    uint64_t strtab = 0, symtab = 0, hash = 0;
    for (const Elf64_Dyn *d = dyn; (const uint8_t *)(d + 1) <= o->data + o->size && d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_STRTAB)
            strtab = d->d_val;
        else if (d->d_tag == DT_SYMTAB)
            symtab = d->d_val;
        else if (d->d_tag == DT_HASH)
            hash = d->d_val;
        else if (d->d_tag == DT_NEEDED && o->needed_count < 16)
            o->needed[o->needed_count++] = d->d_val;
    }
    o->strtab = at(o, strtab);
    o->symtab = at(o, symtab);
    o->hash = at(o, hash);
    if (!o->strtab || !o->symtab || !o->hash) {
        errno = ENOEXEC;
        return -1;
    }
    return 1;
}

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0;
    for (const unsigned char *s = (const unsigned char *)name; *s; s++) {
        h = (h << 4) + *s;
        uint32_t g = h & 0xF0000000u;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* Is name defined (and exported) by an object? The program first, then the
 * libraries in the order ld.so loads them -- the same search it makes. */
static bool defined(const char *name)
{
    uint32_t h = elf_hash(name);
    for (int i = 0; i < count; i++) {
        const struct object *o = &objects[i];
        uint32_t nbucket = o->hash[0];
        if (nbucket == 0)
            continue;
        const uint32_t *buckets = o->hash + 2, *chains = buckets + nbucket;
        for (uint32_t k = buckets[h % nbucket]; k; k = chains[k]) {
            const Elf64_Sym *s = &o->symtab[k];
            unsigned bind = s->st_info >> 4;
            if (s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK) &&
                strcmp(o->strtab + s->st_name, name) == 0)
                return true;
        }
    }
    return false;
}

static void release(void)
{
    for (int i = 0; i < count; i++)
        if (objects[i].data && objects[i].data != MAP_FAILED)
            munmap((void *)objects[i].data, objects[i].size);
    memset(objects, 0, sizeof(objects));
    count = 0;
}

/* One program: 0 when everything it needs is there, 1 if not. */
static int list(const char *arg)
{
    char path[160];
    struct stat st;
    if (strchr(arg, '/')) {
        snprintf(path, sizeof(path), "%s", arg);
    } else {                                    /* a bare name: bin, then apps */
        snprintf(path, sizeof(path), "/system/" ARCH "/bin/%s", arg);
        if (stat(path, &st) != 0)
            snprintf(path, sizeof(path), "/system/" ARCH "/apps/%s.program/%s", arg, arg);   /* (42b) */
    }
    struct object *p = &objects[count++];
    snprintf(p->name, sizeof(p->name), "%s", arg);
    int r = open_object(p, path);
    if (r < 0) {
        printf("ldd: %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("%s:\n", path);
    if (r == 0) {
        printf("    statically linked: it needs no libraries\n");
        return 0;
    }
    int missing = 0;
    for (int i = 0; i < count; i++)             /* the libraries, breadth first, each once */
        for (int k = 0; k < objects[i].needed_count; k++) {
            const char *name = objects[i].strtab + objects[i].needed[k];
            bool have = false;
            for (int j = 1; j < count && !have; j++)
                have = strcmp(objects[j].name, name) == 0;
            if (have)
                continue;
            if (count == MAX_OBJECTS) {
                printf("    (more than %d libraries)\n", MAX_OBJECTS - 1);
                return 1;
            }
            char lib[160];
            snprintf(lib, sizeof(lib), LIB_DIR "%s", name);
            struct object *o = &objects[count];
            snprintf(o->name, sizeof(o->name), "%s", name);
            if (strchr(name, '/') || open_object(o, lib) != 1) {
                printf("    %s => not found\n", name);
                missing++;
                memset(o, 0, sizeof(*o));
                continue;
            }
            count++;
            printf("    %s => %s\n", name, lib);
        }
    for (int i = 0; i < count; i++) {          /* every function they need, defined somewhere */
        const struct object *o = &objects[i];
        uint32_t nsyms = o->hash[1];
        for (uint32_t k = 1; k < nsyms; k++) {
            const Elf64_Sym *s = &o->symtab[k];
            const char *name = o->strtab + s->st_name;
            if (s->st_shndx == SHN_UNDEF && (s->st_info >> 4) == STB_GLOBAL && name[0] &&
                !defined(name)) {
                printf("    %s needs %s, which none of its libraries has\n", o->name, name);
                missing++;
            }
        }
    }
    return missing != 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: ldd PROGRAM...\n");
        return 2;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        failed |= list(argv[i]);
        release();
    }
    return failed;
}
