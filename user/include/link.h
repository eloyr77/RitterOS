/*
 * link.h (43a-4): dl_iterate_phdr: callback for each loaded object (the
 * program first, its name "", then its libraries in load order, then
 * dlopen's) with where it is (dlpi_addr: what its addresses are moved
 * by), its program headers in memory (an unwinder finds PT_GNU_EH_FRAME
 * there), its per-thread data's module and the calling thread's block; it
 * stops at the first nonzero result, which it returns. A static program
 * is the only object.
 */
#ifndef RITTEROS_LINK_H
#define RITTEROS_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <elf.h>
#include <stddef.h>

#define ElfW(type) Elf64_##type

struct dl_phdr_info {
    Elf64_Addr dlpi_addr;
    const char *dlpi_name;
    const Elf64_Phdr *dlpi_phdr;
    Elf64_Half dlpi_phnum;
    unsigned long long dlpi_adds;
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data), void *data);

#ifdef __cplusplus
}
#endif

#endif
