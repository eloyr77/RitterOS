/*
 * The AML interpreter's inside (37b), shared by aml.c (values, names,
 * conversions, fields) and aml_run.c (reading and running the code). The
 * same files build on the Mac with AML_HOST for scripts/amlhost.c.
 */
#ifndef AML_IMPL_H
#define AML_IMPL_H

#include "aml.h"

#ifdef AML_HOST
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define kmalloc malloc
#define kfree free
#define kvsnprintf vsnprintf
#define ksnprintf snprintf
static inline void *kzalloc(size_t n) { return calloc(1, n); }
#else
#include <stdarg.h>
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#endif

/* A string's, buffer's or package's storage, counted: values share it, and
 * the last one to let go frees it. A string's bytes end with a 0 not
 * counted in length. */
struct aml_heap {
    uint32_t refs;
    uint8_t type;                       /* AML_STRING, AML_BUFFER, AML_PACKAGE */
    uint32_t length;                    /* bytes, or a package's elements */
    union {
        uint8_t *bytes;
        struct aml_value *elements;
    };
};

/* struct aml_value's ref for AML_REFERENCE: a named object (RefOf(NAME),
 * a name in a package), an element (Index: heap + index; container says
 * whether a string's, a buffer's or a package's), a Local or an Arg
 * (slot, RefOf(Local0)), the Debug object, a name in a package not there
 * yet when the package was made (heap: the string "SCOPE NAME", resolved
 * when its table has loaded, as ACPICA does: QEMU's _PRT names link
 * devices defined after it). */
enum { REF_NODE = 1, REF_INDEX = 2, REF_SLOT = 3, REF_DEBUG = 4, REF_NAME = 5 };

/* A running method's (or a table load's) state. */
struct aml_frame {
    struct aml_value locals[8], args[7];
    struct aml_node *method;            /* 0 while a table loads */
    struct aml_node **made;             /* the method's named objects (temporary) */
    uint32_t made_count, made_size;
    struct aml_frame *caller;
    int depth;
    struct aml_value implicit;          /* the last result: returned without a Return */
};

extern const struct aml_os *aml_machine;
extern struct aml_node *aml_root;
extern uint64_t aml_ones;               /* the integers' width: 0xFFFFFFFF below revision 2 */
extern struct aml_stats aml_counters;

/* The log: aml_note a line ("aml: ..."), aml_fail an error (counted; the
 * first lines only, firmware's errors repeat). */
void aml_note(const char *fmt, ...);
void aml_fail(const char *fmt, ...);

/* Values. */
struct aml_heap *aml_heap_new(uint8_t type, uint32_t length);
void aml_hold(const struct aml_value *v);
void aml_copy(struct aml_value *dst, const struct aml_value *src);     /* shares, held */
bool aml_duplicate(struct aml_value *dst, const struct aml_value *src); /* a new copy */
bool aml_set_string(struct aml_value *v, const char *s, uint32_t length);
bool aml_set_package(struct aml_value *v, uint32_t count);

/* Conversions as ACPI 6.5 19.3.5 has them (implicit) and the To* operators
 * (explicit); false when the type cannot convert. */
bool aml_to_integer(const struct aml_value *v, uint64_t *n, bool strict);
bool aml_to_buffer(const struct aml_value *v, struct aml_value *out);
enum { STRING_IMPLICIT, STRING_HEX, STRING_DECIMAL };
bool aml_to_string(const struct aml_value *v, struct aml_value *out, int how);

/* Names: 4-character segments, the tree, the search rules (19.3.3). */
struct aml_node *aml_child(struct aml_node *scope, const char *name);
struct aml_node *aml_node_new(struct aml_node *scope, const char *name, uint8_t type);
void aml_node_free(struct aml_node *node);
struct aml_node *aml_alias_target(struct aml_node *node);
/* A name's text ("\\_SB.PCI0", "^FOO", "LNKA") from scope: one segment by
 * the search rules (scope, then each one above), anything else exactly. */
struct aml_node *aml_lookup(struct aml_node *scope, const uint8_t *text, uint32_t length);
/* A name in a package as ACPICA makes it: a data object's value (fields
 * read), else (devices, methods, mutexes, processors...) a reference. */
bool aml_package_element(struct aml_node *node, struct aml_value *out);
/* Every unresolved name in the namespace's packages that can be resolved
 * now, resolved (after each table). */
void aml_resolve_names(void);

/* Field units and buffer fields: read into a value, write from one. */
bool aml_field_read(struct aml_node *field, struct aml_value *out);
bool aml_field_write(struct aml_node *field, const struct aml_value *v);
bool aml_buffer_field_read(struct aml_node *field, struct aml_value *out);
bool aml_buffer_field_write(struct aml_node *field, const struct aml_value *v);

/* A node's value (a method run without arguments, a field read, else a
 * reference to the object), and the value a reference names. */
bool aml_value_of(struct aml_node *node, struct aml_value *out);
bool aml_deref(const struct aml_value *ref, struct aml_value *out);

/* count bytes of a region from offset (Load's table). */
bool aml_region_bytes(struct aml_node *region, uint32_t offset, uint8_t *out, uint32_t count);

/* The machine's waits and time, safe without hooks (the host). */
uint64_t aml_now_ns(void);

/* aml_run.c: run a method (args already values) in frame order. */
bool aml_run_method(struct aml_node *method, const struct aml_value *args, int nargs,
                    struct aml_value *out, struct aml_frame *caller);

#endif
