/*
 * ld.so: the dynamic loader (step 22b). The kernel starts it instead of the
 * program for a program whose ELF file names it (PT_INTERP), with the program
 * already loaded and an auxiliary vector on the stack telling where the
 * program's headers and entry point are. ld.so maps every library the program
 * needs from /system/<arch>/lib (and every one those need) with mmap: their
 * code and read-only data come from the page cache, one copy shared with
 * every program that maps the same library, their writable data is a private
 * copy. It then resolves every symbol at once -- a missing library or
 * function stops the program at its start with a message, never half way --
 * applies the relocations, makes the relocated data of each library (22f)
 * and of the program (22g) read-only (PT_GNU_RELRO), runs the libraries' initializers and jumps to the
 * program's entry with the stack the kernel made. ld.so is a static program
 * at a fixed address of its own (user/ldso.ld) with its own copy of the C
 * library, so it needs nothing it loads.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../common/elf.h"
#include "ritteros.h"

#if defined(__x86_64__)
#define LIB_DIR         "/system/x86_64/lib/"
#define R_ABS64         R_X86_64_64
#define R_COPY          R_X86_64_COPY
#define R_GLOB_DAT      R_X86_64_GLOB_DAT
#define R_JUMP_SLOT     R_X86_64_JUMP_SLOT
#define R_RELATIVE      R_X86_64_RELATIVE
#define R_DTPMOD        R_X86_64_DTPMOD64
#define R_DTPOFF        R_X86_64_DTPOFF64
#define R_TPOFF         R_X86_64_TPOFF64
#else
#define LIB_DIR         "/system/aarch64/lib/"
#define R_ABS64         R_AARCH64_ABS64
#define R_COPY          R_AARCH64_COPY
#define R_GLOB_DAT      R_AARCH64_GLOB_DAT
#define R_JUMP_SLOT     R_AARCH64_JUMP_SLOT
#define R_RELATIVE      R_AARCH64_RELATIVE
#define R_DTPMOD        R_AARCH64_TLS_DTPMOD64
#define R_DTPOFF        R_AARCH64_TLS_DTPREL64
#define R_TPOFF         R_AARCH64_TLS_TPREL64
#endif

#define MAX_OBJECTS     16              /* the program and its libraries */
#define MAX_NEEDED      16              /* libraries one object names */
#define MAX_SEGMENTS    16
#define PAGE            4096ULL
#define PAGE_UP(x)      (((x) + PAGE - 1) & ~(PAGE - 1))
#define PAGE_DOWN(x)    ((x) & ~(PAGE - 1))

struct object {                         /* the program, or a library */
    char name[64];
    uint64_t base;                      /* added to its addresses (0: the program) */
    const Elf64_Dyn *dynamic;
    const char *strtab;
    const Elf64_Sym *symtab;
    const uint32_t *hash;               /* nbucket, nchain, buckets[], chains[] */
    const Elf64_Rela *rela, *jmprel;
    uint64_t relasz, pltrelsz;
    uint64_t init_array, init_arraysz;
    uint64_t relro_start, relro_end;    /* read-only once relocated (absolute; 0: none) */
    uint64_t needed[MAX_NEEDED];        /* DT_NEEDED: name offsets in strtab */
    int needed_count;
    uint64_t tls_image, tls_filesz, tls_memsz, tls_align;  /* its PT_TLS (22i; memsz 0: none) */
    int tls_module;                     /* its number among the modules with per-thread data */
    long tls_tpoff;                     /* its block's offset from the thread pointer */
    uint64_t span, span_size;           /* where it is mapped (a library) */
    int refs;                           /* dlopen calls on it (22k) */
};

static struct object objects[MAX_OBJECTS];
static int count;
static const char *program = "?";      /* the program's name, for messages */

/* dlopen (22k): a failure goes back to the call (ldso.S) with the message,
 * instead of ending the program. */
int ldso_catch(uint64_t *buf);
__attribute__((noreturn)) void ldso_throw(uint64_t *buf);
static uint64_t catch_buf[24];
static bool catching;
static char dl_message[256];

static __attribute__((noreturn, format(printf, 1, 2))) void fail(const char *format, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (catching) {
        snprintf(dl_message, sizeof(dl_message), "%s", text);
        ldso_throw(catch_buf);
    }
    dprintf(STDERR_FILENO, "ld.so: %s: %s\n", program, text);
    exit(127);
}

/* What the dynamic section says (addresses made absolute). */
static void parse(struct object *o)
{
    for (const Elf64_Dyn *d = o->dynamic; d->d_tag != DT_NULL; d++) {
        uint64_t address = o->base + d->d_val;
        switch (d->d_tag) {
        case DT_NEEDED:
            if (o->needed_count == MAX_NEEDED)
                fail("%s needs more than %d libraries", o->name, MAX_NEEDED);
            o->needed[o->needed_count++] = d->d_val;
            break;
        case DT_HASH:         o->hash = (const uint32_t *)address; break;
        case DT_STRTAB:       o->strtab = (const char *)address; break;
        case DT_SYMTAB:       o->symtab = (const Elf64_Sym *)address; break;
        case DT_RELA:         o->rela = (const Elf64_Rela *)address; break;
        case DT_RELASZ:       o->relasz = d->d_val; break;
        case DT_JMPREL:       o->jmprel = (const Elf64_Rela *)address; break;
        case DT_PLTRELSZ:     o->pltrelsz = d->d_val; break;
        case DT_INIT_ARRAY:   o->init_array = address; break;
        case DT_INIT_ARRAYSZ: o->init_arraysz = d->d_val; break;
        case DT_PLTREL:
            if (d->d_val != DT_RELA)
                fail("%s: its PLT relocations are not RELA", o->name);
            break;
        case DT_TEXTREL:
            fail("%s: relocations in its code (was it built with -fPIC?)", o->name);
        }
    }
    if (!o->strtab || !o->symtab)
        fail("%s: no dynamic symbol table", o->name);
}

/* The System V ELF hash of a symbol name. */
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

/* `name`'s symbol in object o, if o defines it. */
static const Elf64_Sym *find_in(const struct object *o, const char *name, uint32_t h)
{
    if (!o->hash || o->hash[0] == 0)
        return 0;                       /* (a program exports nothing) */
    uint32_t nbucket = o->hash[0];
    const uint32_t *buckets = o->hash + 2, *chains = buckets + nbucket;
    for (uint32_t k = buckets[h % nbucket]; k; k = chains[k]) {
        const Elf64_Sym *s = &o->symtab[k];
        unsigned bind = s->st_info >> 4;
        if (s->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK) &&
            strcmp(o->strtab + s->st_name, name) == 0)
            return s;
    }
    return 0;
}

/* The first object that defines `name` (the program first, then the
 * libraries in the order they were loaded) and its symbol there. */
static bool find(const char *name, const struct object **where, const Elf64_Sym **symbol)
{
    uint32_t h = elf_hash(name);
    for (int i = 0; i < count; i++)
        if ((*symbol = find_in(&objects[i], name, h))) {
            *where = &objects[i];
            return true;
        }
    return false;
}

/* The address of `name` in the first object that defines it. */
static bool lookup(const char *name, uint64_t *value)
{
    const struct object *o;
    const Elf64_Sym *s;
    if (!find(name, &o, &s))
        return false;
    *value = o->base + s->st_value;
    return true;
}

#if defined(__aarch64__)
uint64_t ldso_tlsdesc_static(void);     /* ldso.S: a descriptor's function, x0 = [x0 + 8] */
#endif

/* Apply n bytes of relocations of object o. Libraries export functions only:
 * a copy relocation (data a program would copy out of a library) is refused. */
static void relocate(const struct object *o, const Elf64_Rela *r, uint64_t size)
{
    for (uint64_t n = r ? size / sizeof(Elf64_Rela) : 0; n--; r++) {
        uint32_t type = (uint32_t)r->r_info, index = (uint32_t)(r->r_info >> 32);
        uint64_t *where = (uint64_t *)(o->base + r->r_offset);
        if (type == R_RELATIVE) {
            *where = o->base + (uint64_t)r->r_addend;
            continue;
        }
        if (type == R_COPY)
            fail("%s: a copy relocation (libraries export functions, not data)", o->name);
        bool tls = type == R_DTPMOD || type == R_DTPOFF || type == R_TPOFF;
#if defined(__aarch64__)
        tls = tls || type == R_AARCH64_TLSDESC;
#endif
        if (tls) {                      /* per-thread data (22i): the module, offsets */
            const struct object *def = o;
            uint64_t offset = 0;
            if (index) {                /* a symbol (0: the object's own, by the addend) */
                const Elf64_Sym *s = &o->symtab[index];
                const char *name = o->strtab + s->st_name;
                if ((s->st_info >> 4) == STB_LOCAL)
                    offset = s->st_value;
                else if (find(name, &def, &s))
                    offset = s->st_value;
                else
                    fail("%s needs %s, which none of its libraries has", o->name, name);
            }
            if (!def->tls_memsz)
                fail("%s: per-thread data in %s, which has none", o->name, def->name);
            offset += (uint64_t)r->r_addend;
            if (type == R_DTPMOD)
                *where = (uint64_t)def->tls_module;
            else if (type == R_DTPOFF)
                *where = offset;
            else if (type == R_TPOFF)
                *where = (uint64_t)(def->tls_tpoff + (long)offset);
#if defined(__aarch64__)
            else {                      /* TLSDESC: a function and its argument */
                where[0] = (uint64_t)ldso_tlsdesc_static;
                where[1] = (uint64_t)(def->tls_tpoff + (long)offset);
            }
#endif
            continue;
        }
        if (type != R_ABS64 && type != R_GLOB_DAT && type != R_JUMP_SLOT)
            fail("%s: relocation type %u is not supported", o->name, type);
        const Elf64_Sym *s = &o->symtab[index];
        const char *name = o->strtab + s->st_name;
        unsigned bind = s->st_info >> 4;
        uint64_t value = 0;
        if (bind == STB_LOCAL)
            value = o->base + s->st_value;
        else if (!lookup(name, &value) && bind != STB_WEAK)
            fail("%s needs %s, which none of its libraries has", o->name, name);
        *where = value + (uint64_t)r->r_addend;
    }
}

/* Map the library `name` from LIB_DIR: its whole span reserved first, then
 * each segment put in its place in it. */
/* What a library being loaded holds until it is one of the objects: undone
 * if dlopen fails half way (22k). */
static int pending_fd = -1;
static void *pending_span;
static uint64_t pending_size;

static void load_library(const char *file)
{
    if (count == MAX_OBJECTS)
        fail("more than %d libraries", MAX_OBJECTS - 1);
    const char *name = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;   /* (a path: dlopen) */
    if (!name[0] || strlen(name) >= sizeof(objects[0].name))
        fail("%s is not a library name", file);
    char path[256];
    snprintf(path, sizeof(path), strchr(file, '/') ? "%s" : LIB_DIR "%s", file);
    int fd = open(path, O_RDONLY);
    if (fd < 0 && strchr(file, '/'))
        fail("%s: %s", file, strerror(errno));
    if (fd < 0)
        fail("%s is not in " LIB_DIR " (%s)", name, strerror(errno));
    pending_fd = fd;
    Elf64_Ehdr eh;
    Elf64_Phdr ph[MAX_SEGMENTS];
    if (read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh) || eh.e_magic != ELF_MAGIC ||
        eh.e_class != ELFCLASS64 || eh.e_type != ET_DYN || eh.e_machine != ELF_MACHINE_NATIVE ||
        eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 || eh.e_phnum > MAX_SEGMENTS)
        fail("%s is not a library for this CPU", name);
    ssize_t want = (ssize_t)(eh.e_phnum * sizeof(Elf64_Phdr));
    if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) < 0 || read(fd, ph, (size_t)want) != want)
        fail("%s: its program headers cannot be read", name);
    uint64_t lo = UINT64_MAX, hi = 0, dynamic = 0, relro = 0, relro_size = 0;
    const Elf64_Phdr *tls = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC)
            dynamic = ph[i].p_vaddr;
        if (ph[i].p_type == PT_GNU_RELRO)
            relro = ph[i].p_vaddr, relro_size = ph[i].p_memsz;
        if (ph[i].p_type == PT_TLS)
            tls = &ph[i];
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
            continue;
        if (PAGE_DOWN(ph[i].p_vaddr) < lo)
            lo = PAGE_DOWN(ph[i].p_vaddr);
        if (PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi)
            hi = PAGE_UP(ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo >= hi || !dynamic)
        fail("%s has nothing to load", name);
    uint8_t *span = mmap(0, hi - lo, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (span == MAP_FAILED)
        fail("%s: no room for it (%s)", name, strerror(errno));
    pending_span = span;
    pending_size = hi - lo;
    uint64_t base = (uint64_t)span - lo;
    for (int i = 0; i < eh.e_phnum; i++) {
        const Elf64_Phdr *s = &ph[i];
        if (s->p_type != PT_LOAD || s->p_memsz == 0)
            continue;
        int prot = (s->p_flags & PF_R ? PROT_READ : 0) | (s->p_flags & PF_W ? PROT_WRITE : 0) |
                   (s->p_flags & PF_X ? PROT_EXEC : 0);
        uint64_t start = PAGE_DOWN(s->p_vaddr), file_end = s->p_vaddr + s->p_filesz;
        uint64_t mem_end = s->p_vaddr + s->p_memsz, mapped_end = start;
        if (s->p_filesz) {
            mapped_end = PAGE_UP(file_end);
            if (mmap((void *)(base + start), mapped_end - start, prot, MAP_PRIVATE | MAP_FIXED, fd,
                     (off_t)PAGE_DOWN(s->p_offset)) == MAP_FAILED)
                fail("%s cannot be mapped (%s)", name, strerror(errno));
        }
        if (mem_end > file_end) {       /* .bss: zeros after the file's bytes */
            if (!(prot & PROT_WRITE))
                fail("%s: a read-only segment longer in memory than in the file", name);
            if (mapped_end > file_end)
                memset((void *)(base + file_end), 0, mapped_end - file_end);
            if (PAGE_UP(mem_end) > mapped_end &&
                mmap((void *)(base + mapped_end), PAGE_UP(mem_end) - mapped_end, prot,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
                fail("%s: no memory for its data (%s)", name, strerror(errno));
        }
    }
    close(fd);
    pending_fd = -1;
    struct object *o = &objects[count++];
    memset(o, 0, sizeof(*o));
    snprintf(o->name, sizeof(o->name), "%s", name);
    o->base = base;
    o->span = (uint64_t)span;
    o->span_size = hi - lo;
    pending_span = 0;
    o->dynamic = (const Elf64_Dyn *)(base + dynamic);
    if (tls && tls->p_memsz) {
        o->tls_image = base + tls->p_vaddr;
        o->tls_filesz = tls->p_filesz;
        o->tls_memsz = tls->p_memsz;
        o->tls_align = tls->p_align ? tls->p_align : 1;
    }
    if (relro_size) {                   /* whole pages only: the last one may hold other data */
        o->relro_start = PAGE_DOWN(base + relro);
        o->relro_end = PAGE_DOWN(base + relro + relro_size);
    }
    parse(o);
}

/* Called by ldso_start (user/arch/<arch>/ldso.S) with the first stack:
 * argc, argv[], 0, the environment, 0, the auxiliary vector. Returns the
 * program's entry point. */
/* ld.so's errno: a plain one (user/lib/errno.c's is per thread). ld.so runs
 * on the program's threads too (dlopen, 22k), where a per-thread errno of its
 * own would be written into the program's per-thread data. */
int *__errno(void)
{
    static int error;
    return &error;
}

/* The modules with per-thread data, the program's first (its own offsets
 * were fixed when it was linked: the block nearest the thread pointer), in
 * each architecture's layout: x86-64 below the pointer, AArch64 above its
 * 16-byte header. Then the program's C library sets up the main thread. */
static struct ritteros_tls_module tls_list[MAX_OBJECTS];
static int tls_count;
#if defined(__aarch64__)
static uint64_t at = 16;                /* where the next block goes */
#else
static uint64_t at;
#endif

static void assign_tls(int first)
{
    for (int i = first; i < count; i++) {
        struct object *o = &objects[i];
        if (!o->tls_memsz)
            continue;
        uint64_t a = o->tls_align;
#if defined(__x86_64__)
        at = (at + o->tls_memsz + a - 1) & ~(a - 1);
        o->tls_tpoff = -(long)at;
#else
        at = (at + a - 1) & ~(a - 1);
        o->tls_tpoff = (long)at;
        at += o->tls_memsz;
#endif
        o->tls_module = ++tls_count;
        struct ritteros_tls_module *m = &tls_list[tls_count - 1];
        m->image = (const void *)o->tls_image;
        m->filesz = o->tls_filesz;
        m->memsz = o->tls_memsz;
        m->align = a;
        m->tpoff = o->tls_tpoff;
    }
}

/* --- dlopen, dlsym, dlclose (22k): the C library's dlfcn.h calls these, one
 * at a time (it holds a lock), on the program's threads. --- */

static int (*tls_add)(const struct ritteros_tls_module *);     /* libritteros's */

static uintptr_t thread_pointer(void)
{
    uintptr_t tp;
#if defined(__x86_64__)
    __asm__ volatile ("movq %%fs:0, %0" : "=r"(tp));
#else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r"(tp));
#endif
    return tp;
}

static bool loaded(const char *name)
{
    for (int j = 1; j < count; j++)
        if (strcmp(objects[j].name, name) == 0)
            return true;
    return false;
}

static void *dl_open(const char *file, int flags, char *error, size_t size)
{
    (void)flags;                        /* every symbol now, and visible to all */
    if (!file)
        return &objects[0];             /* the program itself */
    const char *name = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    for (int i = 1; i < count; i++)
        if (strcmp(objects[i].name, name) == 0) {
            objects[i].refs++;
            return &objects[i];
        }
    static int before, tls_before;
    static uint64_t at_before;
    before = count;
    tls_before = tls_count;
    at_before = at;
    if (ldso_catch(catch_buf)) {        /* it failed: undo what it loaded */
        catching = false;
        for (int i = count - 1; i >= before; i--)
            munmap((void *)objects[i].span, objects[i].span_size);
        if (pending_fd >= 0)
            close(pending_fd);
        if (pending_span)
            munmap(pending_span, pending_size);
        pending_fd = -1;
        pending_span = 0;
        count = before;
        tls_count = tls_before;
        at = at_before;
        snprintf(error, size, "%s", dl_message);
        return 0;
    }
    catching = true;
    load_library(file);
    for (int i = before; i < count; i++)        /* what it needs, each once */
        for (int k = 0; k < objects[i].needed_count; k++) {
            const char *need = objects[i].strtab + objects[i].needed[k];
            if (!loaded(need))
                load_library(need);
        }
    assign_tls(before);
    for (int i = count - 1; i >= before; i--) {
        relocate(&objects[i], objects[i].rela, objects[i].relasz);
        relocate(&objects[i], objects[i].jmprel, objects[i].pltrelsz);
    }
    for (int i = before; i < count; i++)
        if (objects[i].relro_end > objects[i].relro_start &&
            mprotect((void *)objects[i].relro_start, objects[i].relro_end - objects[i].relro_start,
                     PROT_READ) != 0)
            fail("%s: its relocated data cannot be made read-only", objects[i].name);
    for (int i = before; i < count; i++)
        if (objects[i].tls_module && (!tls_add || tls_add(&tls_list[objects[i].tls_module - 1]) < 0))
            fail("%s: no room for its per-thread data", objects[i].name);
    catching = false;
    for (int i = count - 1; i >= before; i--) {   /* their initializers */
        void (**init)(void) = (void (**)(void))objects[i].init_array;
        for (uint64_t k = 0; init && k < objects[i].init_arraysz / sizeof(*init); k++)
            init[k]();
    }
    objects[before].refs = 1;
    return &objects[before];
}

static bool handle_valid(const void *handle)
{
    for (int i = 0; i < count; i++)
        if (handle == &objects[i])
            return true;
    return false;
}

static void *dl_sym(void *handle, const char *name, char *error, size_t size)
{
    const struct object *def = 0;
    const Elf64_Sym *s = 0;
    if (handle && !handle_valid(handle)) {
        snprintf(error, size, "%p is not a library dlopen gave", handle);
        return 0;
    }
    if (handle && (s = find_in(handle, name, elf_hash(name))))
        def = handle;                   /* the library itself first, */
    else if (!find(name, &def, &s)) {   /* then everything loaded */
        snprintf(error, size, "%s: no such symbol", name);
        return 0;
    }
    if ((s->st_info & 0xF) == STT_TLS)  /* per-thread data: the calling thread's */
        return (void *)(thread_pointer() + def->tls_tpoff + s->st_value);
    return (void *)(def->base + s->st_value);
}

static int dl_close(void *handle)
{
    if (!handle_valid(handle))
        return -1;
    struct object *o = handle;
    if (o->refs > 0)
        o->refs--;                      /* it stays loaded (its symbols may be bound elsewhere) */
    return 0;
}

static const struct ritteros_dl_loader dl_loader = { dl_open, dl_sym, dl_close };

/* ld.so is position-independent (22l): the kernel put it at a random place,
 * so first of all it applies its own relocations (only relative ones: it is
 * static). Nothing before this may use an address stored in its data; the
 * two symbols are reached relative to the code. */
extern const Elf64_Dyn _DYNAMIC[] __attribute__((visibility("hidden")));
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static void relocate_self(void)
{
    uint64_t base = (uint64_t)__ehdr_start;     /* (linked at 0) */
    const Elf64_Rela *r = 0;
    uint64_t size = 0;
    for (const Elf64_Dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA)
            r = (const Elf64_Rela *)(base + d->d_val);
        else if (d->d_tag == DT_RELASZ)
            size = d->d_val;
    }
    for (uint64_t n = r ? size / sizeof(*r) : 0; n--; r++)
        if ((uint32_t)r->r_info == R_RELATIVE)
            *(uint64_t *)(base + r->r_offset) = base + (uint64_t)r->r_addend;
}

uint64_t ldso_main(uint64_t *sp)
{
    relocate_self();
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    if (argc > 0 && argv[0]) {
        const char *slash = strrchr(argv[0], '/');
        program = slash ? slash + 1 : argv[0];
    }
    uint64_t *p = (uint64_t *)(argv + argc + 1);
    while (*p)                          /* the environment */
        p++;
    const Elf64_Phdr *phdr = 0;
    uint64_t phnum = 0, entry = 0, base = 0;
    for (p++; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)
            phdr = (const Elf64_Phdr *)p[1];
        else if (p[0] == AT_PHNUM)
            phnum = p[1];
        else if (p[0] == AT_ENTRY)
            entry = p[1];
        else if (p[0] == AT_BASE)
            base = p[1];
    }
    if (!phdr || !entry || !base) {     /* (no AT_BASE: started as a program of its own) */
        dprintf(STDERR_FILENO, "ld.so: the dynamic loader; the system starts it for programs "
                               "that use shared libraries\n");
        exit(1);
    }
    struct object *o = &objects[count++];
    snprintf(o->name, sizeof(o->name), "%s", program);
    /* A position-independent program (22l) is somewhere at random: its
     * headers in memory (PT_PHDR) tell how far from where it was linked. */
    uint64_t pbase = 0;
    for (uint64_t i = 0; i < phnum; i++)
        if (phdr[i].p_type == PT_PHDR)
            pbase = (uint64_t)phdr - phdr[i].p_vaddr;
    o->base = pbase;
    for (uint64_t i = 0; i < phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC)
            o->dynamic = (const Elf64_Dyn *)(pbase + phdr[i].p_vaddr);
        if (phdr[i].p_type == PT_GNU_RELRO && phdr[i].p_memsz) {   /* (programs too, 22g) */
            o->relro_start = PAGE_DOWN(pbase + phdr[i].p_vaddr);
            o->relro_end = PAGE_DOWN(pbase + phdr[i].p_vaddr + phdr[i].p_memsz);
        }
        if (phdr[i].p_type == PT_TLS && phdr[i].p_memsz) {
            o->tls_image = pbase + phdr[i].p_vaddr;
            o->tls_filesz = phdr[i].p_filesz;
            o->tls_memsz = phdr[i].p_memsz;
            o->tls_align = phdr[i].p_align ? phdr[i].p_align : 1;
        }
    }
    if (!o->dynamic)
        fail("no dynamic section");
    parse(o);
    for (int i = 0; i < count; i++)     /* every library needed, breadth first, each once */
        for (int k = 0; k < objects[i].needed_count; k++) {
            const char *name = objects[i].strtab + objects[i].needed[k];
            bool have = false;
            for (int j = 1; j < count && !have; j++)
                have = strcmp(objects[j].name, name) == 0;
            if (!have)
                load_library(name);
        }
    assign_tls(0);
    for (int i = count - 1; i >= 0; i--) {
        relocate(&objects[i], objects[i].rela, objects[i].relasz);
        relocate(&objects[i], objects[i].jmprel, objects[i].pltrelsz);
    }
    for (int i = 0; i < count; i++)             /* relocated: their GOTs read-only */
        if (objects[i].relro_end > objects[i].relro_start &&
            mprotect((void *)objects[i].relro_start, objects[i].relro_end - objects[i].relro_start,
                     PROT_READ) != 0)
            fail("%s: its relocated data cannot be made read-only (%s)", objects[i].name,
                 strerror(errno));
    /* The program's per-thread data, by its C library: the main thread's area
     * and thread pointer replace ld.so's own, so from here ld.so touches
     * nothing of its own C library that could set errno. */
    int (*setup)(const struct ritteros_tls_module *, int) = 0;
    uint64_t setup_at;
    if (lookup("__ritteros_tls_setup", &setup_at))
        setup = (int (*)(const struct ritteros_tls_module *, int))setup_at;
    if (setup && setup(tls_list, tls_count) < 0)
        fail("no memory for the program's per-thread data");
    /* dlopen (22k): the C library calls back into ld.so, which stays. */
    uint64_t at_add, at_dl;
    if (lookup("__ritteros_tls_add", &at_add))
        tls_add = (int (*)(const struct ritteros_tls_module *))at_add;
    if (lookup("__ritteros_dl_setup", &at_dl))
        ((int (*)(const struct ritteros_dl_loader *))at_dl)(&dl_loader);
    for (int i = count - 1; i >= 1; i--) {      /* the libraries' initializers, if any */
        void (**init)(void) = (void (**)(void))objects[i].init_array;
        for (uint64_t k = 0; init && k < objects[i].init_arraysz / sizeof(*init); k++)
            init[k]();
    }
    return entry;
}
