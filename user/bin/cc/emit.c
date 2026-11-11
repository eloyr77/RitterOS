/*
 * What both backends write the same way (60c): the data sections (GNU
 * assembler syntax, ELF), and the places of values in a frame.
 */
#include "ir.h"

bool symbol_is_local(struct obj *o)
{
    /* defined in this file and not replaceable: reached directly, without
     * the GOT (the programs are position-independent executables); (62d)
     * or hidden: in this module, whose address PC-relative code reaches
     * even before relocation (ld.so's _DYNAMIC) */
    if (o && opt_no_pic && !(!o->is_definition && o->is_weak))
        return true;                            /* (62f: one image, every address known when linking) */
    return o && ((o->is_definition && !o->is_weak) || o->is_hidden);
}

static bool all_zero(const char *data, int size)
{
    if (!data)
        return true;
    for (int i = 0; i < size; i++)
        if (data[i])
            return false;
    return true;
}

static int log2_of(int n)
{
    int k = 0;
    while ((1 << k) < n)
        k++;
    return k;
}

static bool is_read_only(struct obj *o)
{
    if (o->is_literal)
        return true;
    struct type *ty = o->type;
    while (ty->kind == TY_ARRAY)
        ty = ty->base;
    return (ty->qual & Q_CONST) && !(ty->qual & Q_VOLATILE) && !(is_struct_union(ty) && false);
}

void emit_data(FILE *out, struct obj *program, bool aarch64)
{
    /* (62d) constructors and destructors: their addresses, which the loader
     * (ld.so, or the start code of a static program) calls, as clang writes them */
    for (struct obj *o = program; o; o = o->next)
        if (o->is_function && o->is_definition && (o->is_constructor || o->is_destructor)) {
            const char *s = o->is_constructor ? "init_array" : "fini_array";
            fprintf(out, "\t.section .%s,\"aw\",%c%s\n", s, aarch64 ? '%' : '@', s);
            fprintf(out, "\t.p2align 3\n");
            fprintf(out, "\t.%s %s\n", aarch64 ? "xword" : "quad", o->name);
        }
    /* (62d) a weak declaration used but not defined here: a weak reference,
     * which pulls no archive member in and is 0 when nothing defines it */
    for (struct obj *o = program; o; o = o->next) {
        if (!o->is_definition && o->is_weak && o->referenced)
            fprintf(out, "\t.weak %s\n", o->name);
        if (o->is_hidden)
            fprintf(out, "\t.hidden %s\n", o->name);
    }
    for (struct obj *o = program; o; o = o->next) {
        if (o->is_function || !o->is_definition)
            continue;
        int size = o->type->size;
        if (size < 0)
            error_at(o->tok, "variable has incomplete type '%s'", type_name(o->type));
        int align = o->align > o->type->align ? o->align : o->type->align;
        if (align < 1)
            align = 1;
        bool zero = all_zero(o->init_data, size) && !o->relocs;
        if (o->is_tls) {
            /* (62b) per-thread data: the template each thread's copy starts as */
            if (zero)
                fprintf(out, "\t.section .tbss,\"awT\",@nobits\n");
            else
                fprintf(out, "\t.section .tdata,\"awT\",@progbits\n");
        } else if (o->section) {
            fprintf(out, "\t.section %s,\"%s\",@progbits\n", o->section, is_read_only(o) ? "a" : "aw");
        } else if (zero && !is_read_only(o))
            fprintf(out, "\t.bss\n");
        else if (is_read_only(o) && !o->relocs)
            fprintf(out, "\t.section .rodata\n");
        else if (is_read_only(o))
            fprintf(out, "\t.section .data.rel.ro,\"aw\",@progbits\n");
        else
            fprintf(out, "\t.data\n");
        if (!o->is_static)
            fprintf(out, "\t.globl %s\n", o->name);
        if (o->is_weak)
            fprintf(out, "\t.weak %s\n", o->name);
        fprintf(out, "\t.p2align %d\n", log2_of(align));
        fprintf(out, "\t.type %s, @%s\n", o->name, o->is_tls ? "tls_object" : "object");
        fprintf(out, "\t.size %s, %d\n", o->name, size);
        fprintf(out, "%s:\n", o->name);
        if (zero) {
            if (size)
                fprintf(out, "\t.zero %d\n", size);
            continue;
        }
        /* bytes, with an 8-byte address wherever a relocation is */
        int at = 0;
        while (at < size) {
            struct reloc *r = 0;
            for (struct reloc *x = o->relocs; x; x = x->next)
                if (x->offset == at)
                    r = x;
            if (r) {
                if (r->addend)
                    fprintf(out, "\t.quad %s%+lld\n", r->sym, (long long)r->addend);
                else
                    fprintf(out, "\t.quad %s\n", r->sym);
                at += 8;
                continue;
            }
            /* a run of plain bytes up to the next relocation */
            int end = size;
            for (struct reloc *x = o->relocs; x; x = x->next)
                if (x->offset > at && x->offset < end)
                    end = x->offset;
            while (at < end) {
                int n = end - at < 16 ? end - at : 16;
                fprintf(out, "\t.byte ");
                for (int i = 0; i < n; i++)
                    fprintf(out, "%s%d", i ? "," : "", (unsigned char)o->init_data[at + i]);
                fprintf(out, "\n");
                at += n;
            }
        }
    }
}

/* (61d) The frame's room for the locals that live in memory, from `top`
 * on: a local is placed at the lowest place free of every local whose
 * block overlaps its own (locals of separate blocks, such as the arms of a
 * switch, share), aligned. Down (x86-64): offsets below the frame pointer,
 * the local's end at -offset; else (AArch64) offsets upwards from the base.
 * Returns the new top. */
int place_locals(struct obj *fn, int top, bool down)
{
    struct placed {
        int lo, hi, block_lo, block_hi;
    } *placed = 0;
    int n = 0, cap = 0, end = top;
    for (struct obj *o = fn->locals; o; o = o->next) {
        if (o->ir_value)
            continue;                           /* (a promoted local is a value: no room here) */
        int size = o->type->kind == TY_VLA ? 8 : o->type->size;
        if (size < 0)
            size = 0;
        if (is_memory_value(o->type))
            size = (size + 7) & ~7;             /* (results arrive in eightbytes) */
        int a = o->type->kind == TY_VLA ? 8 : o->align;
        if (a < 1)
            a = 1;
        int blo = o->block_hi ? o->block_lo : 0, bhi = o->block_hi ? o->block_hi : 0x7FFFFFFF;
        int lo = top;
        for (;;) {
            lo = down ? (lo + size + a - 1) / a * a - size : (lo + a - 1) / a * a;
            int next = -1;
            for (int k = 0; k < n; k++)
                if (placed[k].block_lo <= bhi && blo <= placed[k].block_hi && lo < placed[k].hi &&
                    placed[k].lo < lo + size && placed[k].hi > next)
                    next = placed[k].hi;
            if (next < 0)
                break;
            lo = next;
        }
        o->offset = down ? -(lo + size) : lo;
        if (n == cap) {
            cap = cap ? 2 * cap : 16;
            placed = realloc(placed, sizeof(*placed) * (size_t)cap);
            if (!placed)
                fatal("out of memory");
        }
        placed[n++] = (struct placed){ lo, lo + size, blo, bhi };
        if (lo + size > end)
            end = lo + size;
    }
    free(placed);
    return end;
}
