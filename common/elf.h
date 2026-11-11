/* ELF64 definitions (System V ABI), just enough to load an executable, and
 * (step 22b) for the dynamic loader ld.so: shared objects, their dynamic
 * section, symbols and relocations, and the auxiliary vector. */
#ifndef RITTEROS_ELF_H
#define RITTEROS_ELF_H

#include <stdint.h>

#define ELF_MAGIC       0x464C457FU   /* "\x7F" "ELF" read as little-endian u32 */
#define ELFCLASS64      2
#define ELFDATA2LSB     1             /* little endian */
#define ET_EXEC         2             /* executable at fixed addresses */
#define ET_DYN          3             /* a shared object (a library), loaded anywhere */
#define ET_REL          1             /* (67a) a relocatable object: a kernel module */
#define EM_X86_64       62
#define EM_AARCH64      183

/* The machine type this code is compiled for. */
#if defined(__x86_64__)
#define ELF_MACHINE_NATIVE EM_X86_64
#elif defined(__aarch64__)
#define ELF_MACHINE_NATIVE EM_AARCH64
#endif
#define PT_LOAD         1             /* segment to be loaded into memory */
#define PT_DYNAMIC      2             /* the dynamic section (Elf64_Dyn entries) */
#define PT_INTERP       3             /* the path of the program's loader (ld.so) */
#define PT_GNU_RELRO    0x6474E552    /* data written only by relocation: read-only after (22f) */
#define PT_PHDR         6             /* the program headers themselves, in memory */
#define PT_TLS          7             /* the template of per-thread data (22i) */
#define STT_TLS         6             /* a symbol of per-thread data (its offset in the block) */
#define PF_X            1             /* segment permissions: execute */
#define PF_W            2             /*                      write */
#define PF_R            4             /*                      read */

typedef struct {
    uint32_t e_magic;
    uint8_t  e_class;
    uint8_t  e_data;
    uint8_t  e_version_ident;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;      /* entry point address */
    uint64_t e_phoff;      /* program header table offset in the file */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;  /* size of one program header */
    uint16_t e_phnum;      /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;     /* segment data offset in the file */
    uint64_t p_vaddr;
    uint64_t p_paddr;      /* physical load address */
    uint64_t p_filesz;     /* bytes present in the file */
    uint64_t p_memsz;      /* bytes in memory; the rest (e.g. .bss) is zeroed */
    uint64_t p_align;
} Elf64_Phdr;

/* The dynamic section: tag and value, up to DT_NULL. */
typedef struct {
    int64_t d_tag;
    uint64_t d_val;        /* a number, or an address (relative to the object's base) */
} Elf64_Dyn;

#define DT_NULL         0
#define DT_NEEDED       1      /* a library it needs: offset of its name in DT_STRTAB */
#define DT_PLTRELSZ     2
#define DT_HASH         4      /* the System V symbol hash table */
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_SONAME       14
#define DT_PLTREL       20
#define DT_TEXTREL      22     /* relocations in read-only code: not supported */
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27

typedef struct {
    uint32_t st_name;      /* offset in the string table */
    uint8_t  st_info;      /* binding << 4 | type */
    uint8_t  st_other;
    uint16_t st_shndx;     /* SHN_UNDEF: defined elsewhere */
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* (67a) Sections, for the kernel's module loader (a module is one
 * relocatable object, sections and relocations as the compiler left them). */
typedef struct {
    uint32_t sh_name;      /* offset in the section names' string table */
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;    /* its data in the file */
    uint64_t sh_size;
    uint32_t sh_link;      /* a symbol table's strings; a relocation table's symbols */
    uint32_t sh_info;      /* a relocation table's section */
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_NOBITS      8             /* .bss: zeroed, nothing in the file */
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2           /* in memory when running */
#define SHF_EXECINSTR   0x4
#define SHN_ABS         0xFFF1
#define SHN_COMMON      0xFFF2
#define SHN_LORESERVE   0xFF00

#define SHN_UNDEF       0
#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2

typedef struct {
    uint64_t r_offset;     /* where (relative to the object's base) */
    uint64_t r_info;       /* symbol index << 32 | type */
    int64_t  r_addend;
} Elf64_Rela;

/* The relocations ld.so applies (with -z now, no lazy binding). */
#define R_X86_64_64             1
#define R_X86_64_COPY           5
#define R_X86_64_GLOB_DAT       6
#define R_X86_64_JUMP_SLOT      7
#define R_X86_64_RELATIVE       8
#define R_X86_64_DTPMOD64       16      /* per-thread data (22i): the module's number */
#define R_X86_64_DTPOFF64       17      /* ... a variable's offset in its module's block */
#define R_X86_64_TPOFF64        18      /* ... its offset from the thread pointer */
#define R_X86_64_PC32           2       /* (67a) a module's: S + A - P, 32 bits */
#define R_X86_64_PLT32          4       /* ... a call (no PLT in the kernel: as PC32) */
#define R_X86_64_32             10      /* ... S + A, unsigned 32 bits */
#define R_X86_64_32S            11      /* ... S + A, signed 32 bits (the top 2 GiB) */
#define R_X86_64_PC64           24
#define R_AARCH64_ABS64         257
#define R_AARCH64_ABS32         258     /* (67a) a module's, as AAELF64 names them */
#define R_AARCH64_PREL64        260
#define R_AARCH64_PREL32        261
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC 278
#define R_AARCH64_TSTBR14       279
#define R_AARCH64_CONDBR19      280
#define R_AARCH64_JUMP26        282
#define R_AARCH64_CALL26        283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_LDST128_ABS_LO12_NC 299
#define R_AARCH64_COPY          1024
#define R_AARCH64_GLOB_DAT      1025
#define R_AARCH64_JUMP_SLOT     1026
#define R_AARCH64_RELATIVE      1027
#define R_AARCH64_TLS_DTPMOD64  1028    /* per-thread data (22i), as on x86 */
#define R_AARCH64_TLS_DTPREL64  1029
#define R_AARCH64_TLS_TPREL64   1030
#define R_AARCH64_TLSDESC       1031    /* a descriptor: a function returning the offset */

/* The auxiliary vector after a dynamic program's environment on its first
 * stack: what ld.so needs to find the program (type, value pairs). */
#define AT_NULL         0
#define AT_PHDR         3      /* the program's program headers (a copy) */
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7      /* where ld.so is */
#define AT_ENTRY        9      /* the program's entry point */

#endif
