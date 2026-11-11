/*
 * The AML interpreter (37b, roadmap item 37): ACPI's machine language, the
 * code in the DSDT and the SSDTs, run as the ACPI specification (6.5,
 * chapters 19 and 20) describes it. Our own code, from the specification;
 * Intel's ACPICA (acpiexec) is only the tests' oracle, on the Mac.
 *
 * Loading a table runs it: named objects are made as their definitions come,
 * and code outside methods (an If around a Name, an OperationRegion whose
 * offset is an expression) runs then, as ACPICA does. A method runs when it
 * is evaluated. The namespace is a tree of nodes under the root \.
 *
 * Hardware is reached only through struct aml_os (operation regions, sleep,
 * the time): until 37b-4 the kernel gives a simulation, every region memory
 * of its own starting at zero, as acpiexec has; the same code builds on the
 * Mac (scripts/amlhost.c) to compare its namespace with acpiexec's.
 *
 * Firmware AML is often wrong: every While, call depth and wait has a limit,
 * and an error ends the method (or, while loading, the block around it)
 * with a line in the log, never the machine.
 */
#ifndef AML_H
#define AML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Object types as ObjectType gives them (ACPI 6.5 19.6.97). */
enum {
    AML_UNINIT = 0, AML_INTEGER = 1, AML_STRING = 2, AML_BUFFER = 3, AML_PACKAGE = 4,
    AML_FIELD = 5, AML_DEVICE = 6, AML_EVENT = 7, AML_METHOD = 8, AML_MUTEX = 9,
    AML_REGION = 10, AML_POWER = 11, AML_PROCESSOR = 12, AML_THERMAL = 13,
    AML_BUFFER_FIELD = 14, AML_DEBUG = 16,
    /* (not ObjectType's: the interpreter's own) */
    AML_SCOPE = 20, AML_ALIAS = 21, AML_REFERENCE = 22,
};

/* A field unit's kind (AML_FIELD nodes). */
enum { AML_REGION_FIELD = 0, AML_INDEX_FIELD = 1, AML_BANK_FIELD = 2 };

/* A string's, buffer's or package's storage: shared, counted. */
struct aml_heap;

/* A value: an integer, a string, a buffer or a package (shared storage), a
 * reference (RefOf, Index, a named object), or nothing. */
struct aml_value {
    uint8_t type;                       /* AML_INTEGER ... AML_REFERENCE */
    uint8_t ref;                        /* AML_REFERENCE's kind (aml.c) */
    uint8_t container;                  /* an index reference's container's type */
    uint32_t index;                     /* an index reference's element */
    union {
        uint64_t integer;
        struct aml_heap *heap;          /* string, buffer, package; index's container */
        struct aml_node *node;          /* a named object */
        struct aml_value *slot;         /* a Local's or an Arg's place */
    };
};

struct aml_node {
    char name[4];
    struct aml_node *parent, *child, *next;
    uint8_t type;                       /* AML_* (AML_SCOPE: \_SB_ and other scopes) */
    bool temporary;                     /* made by a running method: gone at its end */
    struct aml_value value;             /* a Name's object */
    union {
        struct { const uint8_t *code; uint32_t length; uint8_t args, flags;
                 uint8_t revision;
                 /* the interpreter's own methods (\_OSI) */
                 bool (*native)(const struct aml_value *args, struct aml_value *out); } method;
        struct { uint8_t space; bool connected, pci_ready; uint64_t offset, length, pci; } region;
        struct { uint8_t kind, flags, attrib; uint32_t bit_offset, bit_length;
                 struct aml_node *region, *index, *data, *bank; uint64_t bank_value; } field;
        struct { struct aml_heap *buffer; uint32_t bit_offset, bit_length; } buffer_field;
        struct { uint8_t sync; uint32_t depth; } mutex;
        struct { uint8_t id; uint32_t block; uint8_t block_length; } processor;
        struct { uint8_t level; uint16_t order; } power;
        struct { uint64_t signals; } event;
        struct aml_node *alias;
    };
};

/* The machine, as the interpreter reaches it. region_read/region_write
 * give or take width bits (8, 16, 32, 64) at address in an address space
 * (0 SystemMemory, 1 SystemIO, 2 PCI configuration, 3 EmbeddedControl...;
 * PCI's address AML_PCI_ADDRESS: segment, bus, device, function and the
 * register, found as ACPICA finds them: the root bridge's _SEG and _BBN,
 * the bridges' secondary buses down to the device's _ADR); false when the
 * space or the place is not there. now_ns: the time; sleep_ms and
 * stall_us: waits; notify: a Notify(node, value) for its handler. */
#define AML_PCI_ADDRESS(seg, bus, dev, fn, reg) \
    ((uint64_t)(seg) << 48 | (uint64_t)(bus) << 40 | (uint64_t)(dev) << 35 | \
     (uint64_t)(fn) << 32 | (uint64_t)(reg))
#define AML_PCI_SEGMENT(a)  ((uint16_t)((a) >> 48))
#define AML_PCI_BUS(a)      ((uint8_t)((a) >> 40))
#define AML_PCI_DEVICE(a)   ((uint8_t)(((a) >> 35) & 31))
#define AML_PCI_FUNCTION(a) ((uint8_t)(((a) >> 32) & 7))
#define AML_PCI_REGISTER(a) ((uint32_t)(a))
struct aml_os {
    bool (*region_read)(uint8_t space, uint64_t address, int width, uint64_t *value);
    bool (*region_write)(uint8_t space, uint64_t address, int width, uint64_t value);
    uint64_t (*now_ns)(void);
    void (*sleep_ms)(uint32_t ms);
    void (*stall_us)(uint32_t us);
    void (*notify)(struct aml_node *node, uint64_t value);
};

/* Operation regions simulated in memory, as acpiexec has them: every byte
 * of every space 0 until written (the hooks for struct aml_os until 37b-4,
 * and on the Mac). */
bool aml_simulated_read(uint8_t space, uint64_t address, int width, uint64_t *value);
bool aml_simulated_write(uint8_t space, uint64_t address, int width, uint64_t value);

/* The interpreter's start: the root and the predefined scopes (\_GPE, \_PR_,
 * \_SB_, \_SI_, \_TZ_), \_OS_, \_REV, \_OSI (answered as Linux's ACPICA
 * does: the Windows versions up to 2022), os's ways to the machine. */
void aml_init(const struct aml_os *os);

/* A DSDT or SSDT run into the namespace (its header's revision sets the
 * integers' width: 32 bits below 2). How many objects it made, -1 when
 * its block did not read. Names in its packages wait for aml_loaded. */
int aml_load(const void *table, uint32_t length);
/* After the last table: names in packages resolved (ACPICA resolves them
 * once every table is loaded: a later table's object may be the one a
 * name finds). */
void aml_loaded(void);

/* (37b-4) What the boot runs after the last table, in ACPICA's order (as
 * Linux): \_INI, \_SB._INI, _REG for PCI configuration, then _INI of every
 * device, processor and thermal zone in a subtree that has one, as its
 * _STA allows (not present and not working: nor its children). */
struct aml_init { uint32_t ini, sta, absent, failed; };
void aml_initialize(struct aml_init *counts);

/* (37b-3) _REG(space, 1) for every region of space below scope whose own
 * scope has a _REG, in the namespace's order, once per region (ACPICA's way
 * when a space's handler is installed there): the firmware learns it may
 * use the space. For the embedded controller and GPIO also scope's own
 * _REG when no region of the space is directly in it (ACPICA's "orphan"
 * _REG); none for memory and I/O ports, always there (ACPICA since 2018). */
void aml_run_regs(struct aml_node *scope, uint8_t space);

/* (37b-3) The devices whose _HID or _CID is id ("PNP0C09"; EISA IDs
 * decoded), present by their _STA (or without one), in the namespace's
 * order, as ACPICA's AcpiGetDevices finds them (an absent device's
 * children are not looked at): at most max into out, how many. */
int aml_find_devices(const char *id, struct aml_node **out, int max);
/* (id 0: every present device, each one's _STA run.) */

/* (37d) The processors: Processor objects and ACPI0007 devices, present
 * by their _STA, in the namespace's order: at most max into out, how many. */
int aml_find_processors(struct aml_node **out, int max);

/* \_OSI no longer answers yes for name (as ACPICA's AcpiRemoveInterface;
 * the Mac's harness imitates acpiexec, which drops "Windows 2006"). */
void aml_osi_drop(const char *name);

/* (37b-5) A string's or buffer's bytes and length (a package's elements),
 * (37c-1) a package's element i (a copy to release) and a value as an
 * integer (implicit conversion), for drivers reading values; scope's child name ("_CRS"), aliases
 * followed, or 0. */
bool aml_package_get(const struct aml_value *pkg, uint32_t i, struct aml_value *out);
bool aml_to_integer_value(const struct aml_value *v, uint64_t *n);
const uint8_t *aml_bytes(const struct aml_value *v);
uint32_t aml_length(const struct aml_value *v);
struct aml_node *aml_find_in(struct aml_node *scope, const char *name);

/* The node at path ("\_SB_.PCI0", "_SB.PCI0.LPCB", relative to the root),
 * or 0. */
struct aml_node *aml_find(const char *path);

/* (37e-2a) The child name ("ATIF") of the device whose PCI address is
 * address (AML_PCI_ADDRESS, register 0: its _ADR down the bridges from
 * its root bridge, as a PCI configuration region in it is placed), the
 * first in the namespace's order, or 0. */
struct aml_node *aml_pci_child(uint64_t address, const char *name);

/* (37e-2a) Values made for a method's arguments (release them after). */
void aml_set_integer(struct aml_value *v, uint64_t n);
bool aml_set_buffer(struct aml_value *v, const uint8_t *bytes, uint32_t length);

/* Evaluate node (a method with args, or any object's value) into *out:
 * false when it failed (the reason in the log). Release *out after. */
bool aml_evaluate(struct aml_node *node, const struct aml_value *args, int nargs,
                  struct aml_value *out);
void aml_release(struct aml_value *v);

/* A node's path ("\_SB_.PCI0.LPCB") into buffer. */
void aml_path(const struct aml_node *node, char *buffer, size_t size);

/* (37b-3) A value as lines (power eval): "Integer 0x1F", "String \"...\"",
 * "Buffer N bytes" and its bytes 16 a line, "Package N elements" and each
 * one indented, "Reference \PATH Type"; first is the first line's start
 * (the object's path), every line through print. */
void aml_print_value(const struct aml_value *v, const char *first,
                     void (*print)(const char *line));

/* Every node as a line "\PATH Type details" (power names), through print. */
void aml_print_names(void (*print)(const char *line));

/* Counters for the report: objects, methods run, errors, tables loaded,
 * the deepest the interpreter's own stack went (bytes). */
struct aml_stats { uint32_t nodes, methods_run, errors, loads, stack_deepest; };
void aml_stats(struct aml_stats *s);

#endif
