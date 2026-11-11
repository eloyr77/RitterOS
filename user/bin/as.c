/*
 * The assembler's common part (60e): statements, directives, symbols and
 * expressions, the layout (relaxing x86-64's jumps), and the ELF object.
 * The instructions are the architecture's (as_x86.c, as_aarch64.c).
 *
 * as [-target ARCH] [-o FILE] FILE
 */
#include <stdarg.h>
#include "as/as.h"

/* (built on the Mac too, for the tests: there without RitterOS's header) */
#if __has_include("ritteros.h")
#include "ritteros.h"
PROGRAM_HELP("as [-target ARCH] [-o FILE] FILE",
             "assemble");
#endif

struct as_arch *arch;
struct as_section *current_section;
int as_line;
const char *as_file;

static struct as_section *sections, *sections_tail;
static struct as_section *previous_section;
static struct as_symbol *symbols[4096];
static struct as_symbol **symbol_list;          /* in the order they were made */
static int nsymbols, capsymbols;
static int temp_count;

void as_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s:%d: error: ", as_file, as_line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *as_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "as: out of memory\n");
        exit(1);
    }
    return p;
}

char *as_strdup(const char *s, size_t n)
{
    char *p = as_alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* --- symbols ------------------------------------------------------------- */

static unsigned hash(const char *s, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h & 4095;
}

struct as_symbol *symbol(const char *name, size_t len)
{
    unsigned h = hash(name, len);
    for (struct as_symbol *s = symbols[h]; s; s = s->next)
        if (strlen(s->name) == len && memcmp(s->name, name, len) == 0)
            return s;
    struct as_symbol *s = as_alloc(sizeof(*s));
    s->name = as_strdup(name, len);
    s->next = symbols[h];
    symbols[h] = s;
    if (nsymbols == capsymbols) {
        capsymbols = capsymbols ? capsymbols * 2 : 256;
        symbol_list = realloc(symbol_list, sizeof(*symbol_list) * (size_t)capsymbols);
    }
    symbol_list[nsymbols++] = s;
    return s;
}

/* --- sections and fragments ---------------------------------------------- */

static struct as_section *find_section(const char *name, int type, uint64_t flags, bool explicit_flags)
{
    for (struct as_section *s = sections; s; s = s->next)
        if (strcmp(s->name, name) == 0) {
            if (explicit_flags) {
                s->flags |= flags;
                if (type)
                    s->type = type;
            }
            return s;
        }
    struct as_section *s = as_alloc(sizeof(*s));
    s->name = as_strdup(name, strlen(name));
    /* the usual kinds by name, when no flags were given */
    if (!explicit_flags) {
        type = SHT_PROGBITS;
        flags = 0;
        if (strcmp(name, ".text") == 0 || strncmp(name, ".text.", 6) == 0)
            flags = SHF_ALLOC | SHF_EXECINSTR;
        else if (strcmp(name, ".data") == 0 || strncmp(name, ".data.", 6) == 0)
            flags = SHF_ALLOC | SHF_WRITE;
        else if (strcmp(name, ".bss") == 0 || strncmp(name, ".bss.", 5) == 0)
            flags = SHF_ALLOC | SHF_WRITE, type = SHT_NOBITS;
        else if (strcmp(name, ".rodata") == 0 || strncmp(name, ".rodata.", 8) == 0)
            flags = SHF_ALLOC;
        else if (strcmp(name, ".tdata") == 0 || strncmp(name, ".tdata.", 7) == 0)
            flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
        else if (strcmp(name, ".tbss") == 0 || strncmp(name, ".tbss.", 6) == 0)
            flags = SHF_ALLOC | SHF_WRITE | SHF_TLS, type = SHT_NOBITS;
        else if (strcmp(name, ".init_array") == 0)
            flags = SHF_ALLOC | SHF_WRITE, type = SHT_INIT_ARRAY;
        else if (strcmp(name, ".fini_array") == 0)
            flags = SHF_ALLOC | SHF_WRITE, type = SHT_FINI_ARRAY;
        else if (strncmp(name, ".note", 5) == 0)
            type = strcmp(name, ".note.GNU-stack") == 0 ? SHT_PROGBITS : SHT_NOTE;
    }
    s->type = type ? type : SHT_PROGBITS;
    s->flags = flags;
    s->align = 1;
    s->symbol = as_alloc(sizeof(struct as_symbol));
    s->symbol->name = "";
    s->symbol->section = s;
    s->symbol->defined = true;
    s->symbol->type = STT_SECTION;
    if (sections_tail)
        sections_tail->next = s;
    else
        sections = s;
    sections_tail = s;
    return s;
}

static void switch_section(struct as_section *s)
{
    if (s != current_section)
        previous_section = current_section;
    current_section = s;
}

static struct as_fragment *new_fragment(enum frag_kind kind)
{
    struct as_fragment *f = as_alloc(sizeof(*f));
    f->kind = kind;
    struct as_section *s = current_section;
    if (s->last)
        s->last->next = f;
    else
        s->first = f;
    s->last = f;
    return f;
}

static struct as_fragment *data_fragment(void)
{
    struct as_section *s = current_section;
    if (s->last && s->last->kind == FRAG_DATA)
        return s->last;
    return new_fragment(FRAG_DATA);
}

void emit_bytes(const void *p, int n)
{
    struct as_fragment *f = data_fragment();
    if (current_section->type == SHT_NOBITS) {
        for (int i = 0; i < n; i++)
            if (((const unsigned char *)p)[i])
                as_error("non-zero data in a section without contents (%s)", current_section->name);
    }
    if (f->len + n > f->cap) {
        f->cap = f->cap ? f->cap * 2 : 256;
        while (f->cap < f->len + n)
            f->cap *= 2;
        f->data = realloc(f->data, (size_t)f->cap);
    }
    memcpy(f->data + f->len, p, (size_t)n);
    f->len += n;
}

void emit_byte(int b)
{
    unsigned char c = (unsigned char)b;
    emit_bytes(&c, 1);
}

void emit_le(uint64_t v, int n)
{
    unsigned char b[8];
    for (int i = 0; i < n; i++)
        b[i] = (unsigned char)(v >> (8 * i));
    emit_bytes(b, n);
}

int64_t here(void)
{
    return data_fragment()->len;
}

/* A fixup for the next size bytes (emitted by the caller afterwards, as
 * zeros or the instruction's own bits); PC-relative ones count from
 * pc_base_from_here bytes after the field's start. */
void emit_fixup(struct as_expr *e, int kind, int size, bool pcrel, int64_t pc_base_from_here)
{
    struct as_fragment *f = data_fragment();
    struct as_fixup *x = as_alloc(sizeof(*x));
    x->offset = f->len;
    x->expr = *e;
    x->kind = kind;
    x->size = size;
    x->pcrel = pcrel;
    x->pc_base = f->len + pc_base_from_here;
    x->next = f->fixups;
    f->fixups = x;
}

void emit_jump(struct as_expr *target, int op)
{
    struct as_fragment *f = new_fragment(FRAG_JUMP);
    f->target = *target;
    f->jump_op = op;
    /* only a jump to a label of this section may be short */
    struct as_symbol *s = target->sym;
    f->is_long = op == -2 || !s || s->is_global || s->is_weak || target->minus ||
                 (s->defined && s->section != current_section);
    new_fragment(FRAG_DATA);
}

static void define_label(const char *name, size_t len)
{
    struct as_symbol *s = symbol(name, len);
    if (s->defined)
        as_error("symbol '%s' is already defined", s->name);
    struct as_fragment *f = data_fragment();
    s->defined = true;
    s->section = current_section;
    s->frag = f;
    s->frag_offset = f->len;
}

static struct as_symbol *temp_here(void)
{
    char name[32];
    snprintf(name, sizeof(name), ".L.as.here.%d", temp_count++);
    define_label(name, strlen(name));
    return symbol(name, strlen(name));
}

/* numeric local labels (62e): instance k of label N is ".L.as.N.k" */
static struct {
    uint64_t label;
    int count;                                  /* instances defined so far */
} local_labels[256];
static int nlocal_labels;

static int local_label(uint64_t label)
{
    for (int i = 0; i < nlocal_labels; i++)
        if (local_labels[i].label == label)
            return i;
    if (nlocal_labels == 256)
        as_error("too many numeric labels");
    local_labels[nlocal_labels].label = label;
    return nlocal_labels++;
}

/* the name of the instance `delta` after the next one of `label` */
static void local_label_name(char *name, size_t size, uint64_t label, int delta)
{
    snprintf(name, size, ".L.as.%lu.%d", (unsigned long)label, local_labels[local_label(label)].count + delta);
}

/* --- expressions --------------------------------------------------------- */

static bool name_char(char c, bool first)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$' ||
           (!first && c >= '0' && c <= '9');
}

struct value {
    struct as_symbol *sym, *minus;
    int64_t n;
};

static bool parse_value(const char **pp, struct value *v);

static bool parse_primary(const char **pp, struct value *v)
{
    const char *p = skip_space(*pp);
    memset(v, 0, sizeof(*v));
    if (*p == '(') {
        p++;
        if (!parse_value(&p, v))
            return false;
        p = skip_space(p);
        if (*p != ')')
            return false;
        *pp = p + 1;
        return true;
    }
    if (*p == '-' || *p == '~' || *p == '+') {
        char op = *p++;
        if (!parse_primary(&p, v))
            return false;
        if (op == '+') {
            *pp = p;
            return true;
        }
        if (v->sym)
            return false;
        v->n = op == '-' ? (int64_t)(0 - (uint64_t)v->n) : ~v->n;   /* (-INT64_MIN wraps to itself) */
        *pp = p;
        return true;
    }
    if (*p >= '0' && *p <= '9') {
        char *end;
        uint64_t n;
        const char *d = p;
        while (*d >= '0' && *d <= '9')
            d++;
        if ((*d == 'f' || *d == 'b') && !name_char(d[1], false)) {
            /* a numeric local label (62e): the last one before (1b) or the next (1f) */
            char name[48];
            uint64_t label = strtoull(p, 0, 10);
            if (*d == 'b' && local_labels[local_label(label)].count == 0)
                as_error("no local label %lu before here", (unsigned long)label);
            local_label_name(name, sizeof(name), label, *d == 'b' ? -1 : 0);
            v->sym = symbol(name, strlen(name));
            *pp = d + 1;
            return true;
        }
        if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B'))
            n = strtoull(p + 2, &end, 2);
        else
            n = strtoull(p, &end, 0);
        v->n = (int64_t)n;
        *pp = end;
        return true;
    }
    if (*p == '\'') {
        v->n = (unsigned char)p[1];
        *pp = p + (p[2] == '\'' ? 3 : 2);
        return true;
    }
    if (*p == '.' && !name_char(p[1], false)) {
        v->sym = temp_here();
        *pp = p + 1;
        return true;
    }
    if (name_char(*p, true)) {
        const char *start = p;
        while (name_char(*p, false))
            p++;
        v->sym = symbol(start, (size_t)(p - start));
        *pp = p;
        return true;
    }
    return false;
}

static bool combine(struct value *a, struct value *b, char op)
{
    if (op == '+') {
        if (a->sym && b->sym)
            return false;
        if (!a->sym) {
            a->sym = b->sym;
            a->minus = a->minus ? a->minus : b->minus;
        }
        a->n += b->n;
        return true;
    }
    if (op == '-') {
        if (b->sym) {
            if (b->minus || a->minus)
                return false;
            if (!a->sym)
                return false;
            a->minus = b->sym;
        }
        a->n -= b->n;
        return true;
    }
    if (a->sym || b->sym)
        return false;
    switch (op) {
    case '*': a->n *= b->n; break;
    case '/': if (!b->n) return false; a->n /= b->n; break;
    case '%': if (!b->n) return false; a->n %= b->n; break;
    case '<': a->n = (int64_t)((uint64_t)a->n << b->n); break;
    case '>': a->n >>= b->n; break;
    case '&': a->n &= b->n; break;
    case '|': a->n |= b->n; break;
    case '^': a->n ^= b->n; break;
    }
    return true;
}

static int binary_precedence(const char *p, char *op, int *len)
{
    *len = 1;
    switch (p[0]) {
    case '*': case '/': case '%': *op = p[0]; return 5;
    case '<': if (p[1] == '<') { *op = '<'; *len = 2; return 4; } return 0;
    case '>': if (p[1] == '>') { *op = '>'; *len = 2; return 4; } return 0;
    case '+': case '-': *op = p[0]; return 3;
    case '&': *op = '&'; return 2;
    case '^': *op = '^'; return 1;
    case '|': *op = '|'; return 1;
    }
    return 0;
}

static bool parse_binary(const char **pp, struct value *v, int min)
{
    if (!parse_primary(pp, v))
        return false;
    for (;;) {
        const char *p = skip_space(*pp);
        char op;
        int len;
        int prec = binary_precedence(p, &op, &len);
        if (!prec || prec < min)
            return true;
        p += len;
        struct value b;
        if (!parse_binary(&p, &b, prec + 1))
            return false;
        if (!combine(v, &b, op))
            return false;
        *pp = p;
    }
}

static bool parse_value(const char **pp, struct value *v)
{
    return parse_binary(pp, v, 1);
}

bool parse_expr(const char **p, struct as_expr *e)
{
    struct value v;
    memset(e, 0, sizeof(*e));
    if (!parse_value(p, &v))
        return false;
    e->sym = v.sym;
    e->minus = v.minus;
    e->addend = v.n;
    return true;
}

bool expr_constant(struct as_expr *e, int64_t *v)
{
    if (e->sym && e->sym->is_absolute && !e->minus) {
        *v = e->sym->value + e->addend;
        return true;
    }
    if (e->sym)
        return false;
    *v = e->addend;
    return true;
}

/* --- directives ---------------------------------------------------------- */

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

/* the next comma-separated item of a list (outside quotes) */
static char *next_item(char **pp)
{
    char *p = *pp, *start = p;
    if (!*p)
        return 0;
    bool quoted = false;
    for (; *p; p++) {
        if (*p == '"' && (p == start || p[-1] != '\\'))
            quoted = !quoted;
        if (*p == ',' && !quoted)
            break;
    }
    if (*p)
        *p++ = 0;
    *pp = p;
    return trim(start);
}

static int64_t constant_operand(const char *text)
{
    struct as_expr e;
    const char *p = text;
    int64_t v;
    if (!parse_expr(&p, &e) || !expr_constant(&e, &v) || *skip_space(p))
        as_error("a constant expression is required: '%s'", text);
    return v;
}

static void data_items(char *args, int size)
{
    char *item;
    while ((item = next_item(&args))) {
        struct as_expr e;
        const char *p = item;
        if (!parse_expr(&p, &e) || *skip_space(p)) {
            /* sym@modifier in data: the architecture knows them */
            as_error("bad expression in data: '%s'", item);
        }
        int64_t v;
        if (expr_constant(&e, &v)) {
            emit_le((uint64_t)v, size);
            continue;
        }
        emit_fixup(&e, 0, size, false, 0);      /* kind 0: a plain address */
        emit_le(0, size);
    }
}

static void string_items(char *args, bool zero)
{
    char *item;
    while ((item = next_item(&args))) {
        if (*item != '"')
            as_error("expected a string");
        for (char *p = item + 1; *p && *p != '"'; p++) {
            int c = (unsigned char)*p;
            if (c == '\\') {
                p++;
                switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case 'x': {
                    c = 0;
                    while ((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'a' && p[1] <= 'f') || (p[1] >= 'A' && p[1] <= 'F')) {
                        p++;
                        c = c * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
                    }
                    break;
                }
                default:
                    if (*p >= '0' && *p <= '7') {
                        c = *p - '0';
                        for (int i = 0; i < 2 && p[1] >= '0' && p[1] <= '7'; i++)
                            c = c * 8 + *++p - '0';
                    } else
                        c = (unsigned char)*p;
                }
            }
            emit_byte(c);
        }
        if (zero)
            emit_byte(0);
    }
}

static void align_to(int64_t align, bool code)
{
    if (align <= 1)
        return;
    if (align > current_section->align)
        current_section->align = (int)align;
    struct as_fragment *f = new_fragment(FRAG_ALIGN);
    f->align = (int)align;
    f->code = code && (current_section->flags & SHF_EXECINSTR);
    new_fragment(FRAG_DATA);
}

static void section_directive(char *args)
{
    char *name = next_item(&args);
    if (!name)
        as_error(".section needs a name");
    if (*name == '"')
        name = as_strdup(name + 1, strlen(name) - 2);
    char *flags = next_item(&args);
    char *type = next_item(&args);
    if (!flags) {
        switch_section(find_section(name, 0, 0, false));
        return;
    }
    uint64_t f = 0;
    int entsize = 0;
    if (*flags == '"')
        for (char *p = flags + 1; *p && *p != '"'; p++)
            switch (*p) {
            case 'a': f |= SHF_ALLOC; break;
            case 'w': f |= SHF_WRITE; break;
            case 'x': f |= SHF_EXECINSTR; break;
            case 'M': f |= SHF_MERGE; break;
            case 'S': f |= SHF_STRINGS; break;
            case 'T': f |= SHF_TLS; break;
            }
    int t = SHT_PROGBITS;
    if (type) {
        type++;                                 /* (@ or %) */
        if (strcmp(type, "nobits") == 0)
            t = SHT_NOBITS;
        else if (strcmp(type, "note") == 0)
            t = SHT_NOTE;
        else if (strcmp(type, "init_array") == 0)
            t = SHT_INIT_ARRAY;
        else if (strcmp(type, "fini_array") == 0)
            t = SHT_FINI_ARRAY;
        char *es = next_item(&args);
        if (es)
            entsize = (int)constant_operand(es);
    }
    struct as_section *s = find_section(name, t, f, true);
    if (entsize)
        s->entsize = entsize;
    switch_section(s);
}

static void symbol_directive(const char *dir, char *args)
{
    char *item;
    while ((item = next_item(&args))) {
        struct as_symbol *s = symbol(item, strlen(item));
        if (strcmp(dir, ".globl") == 0 || strcmp(dir, ".global") == 0)
            s->is_global = true;
        else if (strcmp(dir, ".weak") == 0)
            s->is_weak = true;
        else if (strcmp(dir, ".hidden") == 0)
            s->is_hidden = true;
        else if (strcmp(dir, ".local") == 0)
            s->is_global = false;
    }
}

static bool directive(char *name, char *args)
{
    if (strcmp(name, ".text") == 0 || strcmp(name, ".data") == 0 || strcmp(name, ".bss") == 0) {
        switch_section(find_section(name, 0, 0, false));
        return true;
    }
    if (strcmp(name, ".section") == 0) {
        section_directive(args);
        return true;
    }
    if (strcmp(name, ".previous") == 0) {
        if (previous_section)
            switch_section(previous_section);
        return true;
    }
    if (strcmp(name, ".globl") == 0 || strcmp(name, ".global") == 0 || strcmp(name, ".weak") == 0 ||
        strcmp(name, ".hidden") == 0 || strcmp(name, ".local") == 0) {
        symbol_directive(name, args);
        return true;
    }
    if (strcmp(name, ".type") == 0) {
        char *sym = next_item(&args), *type = next_item(&args);
        if (!sym || !type)
            as_error(".type needs a symbol and a type");
        struct as_symbol *s = symbol(sym, strlen(sym));
        type++;
        if (strcmp(type, "function") == 0)
            s->type = STT_FUNC;
        else if (strcmp(type, "object") == 0)
            s->type = STT_OBJECT;
        else if (strcmp(type, "tls_object") == 0)
            s->type = STT_TLS;
        else if (strcmp(type, "notype") == 0)
            s->type = STT_NOTYPE;
        return true;
    }
    if (strcmp(name, ".size") == 0) {
        char *sym = next_item(&args);
        if (!sym || !*args)
            as_error(".size needs a symbol and a size");
        struct as_symbol *s = symbol(sym, strlen(sym));
        /* (evaluated at the end: ".-f" needs the final layout) */
        const char *p = args;
        struct as_expr e;
        if (!parse_expr(&p, &e))
            as_error("bad .size expression");
        s->size_expr = (char *)as_alloc(sizeof(struct as_expr));
        memcpy(s->size_expr, &e, sizeof(e));
        return true;
    }
    if (strcmp(name, ".p2align") == 0 || strcmp(name, ".balign") == 0 || strcmp(name, ".align") == 0) {
        char *n = next_item(&args);
        int64_t v = n ? constant_operand(n) : 0;
        bool power = strcmp(name, ".p2align") == 0 || (strcmp(name, ".align") == 0 && arch == &arch_aarch64);
        align_to(power ? (int64_t)1 << v : v, true);
        return true;
    }
    if (strcmp(name, ".zero") == 0 || strcmp(name, ".skip") == 0 || strcmp(name, ".space") == 0) {
        char *n = next_item(&args), *fill = next_item(&args);
        int64_t count = constant_operand(n), value = fill ? constant_operand(fill) : 0;
        for (int64_t i = 0; i < count; i++)
            emit_byte((int)value);
        return true;
    }
    if (strcmp(name, ".byte") == 0) {
        data_items(args, 1);
        return true;
    }
    if (strcmp(name, ".short") == 0 || strcmp(name, ".hword") == 0 || strcmp(name, ".2byte") == 0 ||
        (strcmp(name, ".word") == 0 && arch == &arch_x86_64) || strcmp(name, ".value") == 0) {
        data_items(args, 2);
        return true;
    }
    if (strcmp(name, ".long") == 0 || strcmp(name, ".int") == 0 || strcmp(name, ".4byte") == 0 ||
        strcmp(name, ".word") == 0) {
        data_items(args, 4);
        return true;
    }
    if (strcmp(name, ".quad") == 0 || strcmp(name, ".8byte") == 0 || strcmp(name, ".xword") == 0 ||
        strcmp(name, ".dword") == 0) {
        data_items(args, 8);
        return true;
    }
    if (strcmp(name, ".ascii") == 0) {
        string_items(args, false);
        return true;
    }
    if (strcmp(name, ".asciz") == 0 || strcmp(name, ".string") == 0) {
        string_items(args, true);
        return true;
    }
    if (strcmp(name, ".set") == 0 || strcmp(name, ".equ") == 0) {
        char *sym = next_item(&args);
        struct as_symbol *s = symbol(sym, strlen(sym));
        const char *p = args;
        struct as_expr e;
        int64_t v;
        if (!parse_expr(&p, &e))
            as_error("bad expression");
        if (expr_constant(&e, &v)) {
            s->defined = true;
            s->is_absolute = true;
            s->value = v;
        } else if (e.sym && e.sym->defined && !e.minus) {
            /* another name for a label (plus an offset) */
            s->defined = true;
            s->section = e.sym->section;
            s->frag = e.sym->frag;
            s->frag_offset = e.sym->frag_offset + e.addend;
        } else
            as_error(".set of an expression that is not known yet");
        return true;
    }
    if (strcmp(name, ".file") == 0 || strcmp(name, ".ident") == 0 || strncmp(name, ".cfi_", 5) == 0 ||
        strcmp(name, ".loc") == 0 || strcmp(name, ".addrsig") == 0 || strcmp(name, ".addrsig_sym") == 0 ||
        strcmp(name, ".type") == 0 || strcmp(name, ".arch") == 0 || strcmp(name, ".code64") == 0 ||
        strcmp(name, ".intel_syntax") == 0 || strcmp(name, ".att_syntax") == 0) {
        if (strcmp(name, ".intel_syntax") == 0)
            as_error("Intel syntax is not supported");
        return true;
    }
    return false;
}

/* --- statements ---------------------------------------------------------- */

static void statement(char *s)
{
    s = trim(s);
    if (!*s)
        return;
    /* labels: "name:" first */
    for (;;) {
        char *p = s;
        if (*p == '"')
            break;
        if (*p >= '0' && *p <= '9') {
            /* a numeric local label (62e): "1:", referred to as 1b and 1f */
            while (*p >= '0' && *p <= '9')
                p++;
            if (*skip_space(p) != ':')
                break;
            char name[48];
            local_label_name(name, sizeof(name), strtoull(s, 0, 10), 0);
            define_label(name, strlen(name));
            local_labels[local_label(strtoull(s, 0, 10))].count++;
            s = trim((char *)skip_space(p) + 1);
            if (!*s)
                return;
            continue;
        }
        while (name_char(*p, p == s) || (p > s && *p >= '0' && *p <= '9'))
            p++;
        if (p > s && *skip_space(p) == ':' && skip_space(p)[1] != ':') {
            define_label(s, (size_t)(p - s));
            s = trim((char *)skip_space(p) + 1);
            if (!*s)
                return;
            continue;
        }
        break;
    }
    char *p = s;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    char *mnemonic = as_strdup(s, (size_t)(p - s));
    char *args = trim(p);
    if (mnemonic[0] == '.') {
        if (strcmp(mnemonic, ".tlsdesccall") == 0)
            arch->instruction(mnemonic, args);  /* (62b: the architecture's own) */
        else if (!directive(mnemonic, args))
            as_error("unknown directive '%s'", mnemonic);
        return;
    }
    for (char *c = mnemonic; *c; c++)
        if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    arch->instruction(mnemonic, args);
}

static void assemble_line(char *line)
{
    /* comments, then statements separated by ';' (outside strings) */
    bool quoted = false;
    char *start = line;
    for (char *p = line;; p++) {
        if (*p == '"' && (p == line || p[-1] != '\\'))
            quoted = !quoted;
        bool comment = !quoted && ((arch->comment && *p == arch->comment) || (p[0] == '/' && p[1] == '/'));
        if (!*p || comment || (*p == ';' && !quoted)) {
            char end = *p;
            *p = 0;
            statement(start);
            if (!end || comment)
                return;
            start = p + 1;
        }
    }
}

/* --- macros and repetitions (62e) ----------------------------------------- */

/* .macro NAME PARAMS ... .endm, called as NAME ARGS; .rept COUNT, .irp NAME,
 * VALUES and .irpc NAME, CHARS ... .endr. Bodies are kept as lines and fed
 * back through source_line with \PARAM, \() and \@ replaced. */

#define MACRO_PARAMS 16

struct macro {
    char *name;
    int nparams;
    char *params[MACRO_PARAMS], *defaults[MACRO_PARAMS];
    char **lines;
    int nlines;
    struct macro *next;
};

static struct macro *macros;
static int macro_calls;                         /* \@: the number of expansions so far */

/* the body being collected */
enum { BODY_NONE, BODY_MACRO, BODY_REPT, BODY_IRP, BODY_IRPC };
static struct {
    int kind, depth;
    char **lines;
    int nlines, cap;
    struct macro *macro;
    int64_t count;
    char *var, *values;
} body;

static void source_line(char *line);

/* the first word of a line and the rest after it */
static char *first_word(const char *line, const char **rest)
{
    const char *p = skip_space(line), *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != ',')
        p++;
    *rest = p;
    return as_strdup(start, (size_t)(p - start));
}

/* items separated by commas, or by blanks when there is no comma */
static int split_items(char *s, char **items, int max)
{
    int n = 0;
    bool commas = strchr(s, ',') != 0;
    s = trim(s);
    while (*s) {
        char *e = s;
        if (commas)
            while (*e && *e != ',')
                e++;
        else
            while (*e && *e != ' ' && *e != '\t')
                e++;
        char end = *e;
        *e = 0;
        if (n == max)
            as_error("too many items");
        items[n++] = trim(s);
        s = end ? trim(e + 1) : e;
    }
    return n;
}

static void add_body_line(const char *line)
{
    if (body.nlines == body.cap) {
        body.cap = body.cap ? body.cap * 2 : 16;
        body.lines = realloc(body.lines, (size_t)body.cap * sizeof(char *));
        if (!body.lines)
            as_error("out of memory");
    }
    body.lines[body.nlines++] = as_strdup(line, strlen(line));
}

static bool param_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$';
}

/* line with \NAME replaced by values[i] where names[i] is NAME */
static char *substitute(const char *line, char **names, char **values, int n, int call)
{
    size_t cap = strlen(line) + 64, len = 0;
    char *out = as_alloc(cap);
    for (const char *p = line; *p;) {
        const char *with = 0;
        size_t skip = 1;
        char number[16];
        if (*p == '\\') {
            if (p[1] == '(' && p[2] == ')') {
                with = "";
                skip = 3;
            } else if (p[1] == '@') {
                snprintf(number, sizeof(number), "%d", call);
                with = number;
                skip = 2;
            } else {
                size_t k = 0;
                while (param_char(p[1 + k]))
                    k++;
                for (int i = 0; i < n; i++)
                    if (strlen(names[i]) == k && strncmp(p + 1, names[i], k) == 0) {
                        with = values[i];
                        skip = 1 + k;
                    }
            }
        }
        size_t add = with ? strlen(with) : 1;
        while (len + add + 1 > cap) {
            cap *= 2;
            out = realloc(out, cap);
            if (!out)
                as_error("out of memory");
        }
        if (with)
            memcpy(out + len, with, add);
        else
            out[len] = *p;
        len += add;
        p += with ? skip : 1;
    }
    out[len] = 0;
    return out;
}

static void run_lines(char **lines, int nlines, char **names, char **values, int n)
{
    int call = macro_calls++;
    for (int i = 0; i < nlines; i++)
        source_line(substitute(lines[i], names, values, n, call));
}

static void call_macro(struct macro *m, char *args)
{
    char *items[MACRO_PARAMS], *values[MACRO_PARAMS];
    int n = split_items(args, items, MACRO_PARAMS);
    if (n > m->nparams)
        as_error("too many arguments for macro '%s'", m->name);
    for (int i = 0; i < m->nparams; i++)
        values[i] = i < n && *items[i] ? items[i] : m->defaults[i] ? m->defaults[i] : "";
    run_lines(m->lines, m->nlines, m->params, values, m->nparams);
}

/* the end of a collected body: define the macro, or repeat the lines */
static void end_body(void)
{
    /* (taken out first: a repetition inside the lines collects into body again) */
    int kind = body.kind;
    char **lines = body.lines;
    int nlines = body.nlines;
    int64_t count = body.count;
    char *var = body.var, *values = body.values;
    body.kind = BODY_NONE;
    body.lines = 0;
    body.nlines = body.cap = 0;
    if (kind == BODY_MACRO) {
        body.macro->lines = lines;
        body.macro->nlines = nlines;
        body.macro->next = macros;
        macros = body.macro;
        return;
    }
    if (kind == BODY_REPT) {
        for (int64_t i = 0; i < count; i++)
            run_lines(lines, nlines, 0, 0, 0);
        return;
    }
    if (kind == BODY_IRP) {
        char *items[256];
        int n = split_items(values, items, 256);
        for (int i = 0; i < n; i++)
            run_lines(lines, nlines, &var, &items[i], 1);
    } else {
        for (const char *c = trim(values); *c; c++) {
            char one[2] = { *c, 0 };
            char *value = one;
            run_lines(lines, nlines, &var, &value, 1);
        }
    }
}

static void source_line(char *line)
{
    const char *rest;
    char *word = first_word(line, &rest);
    if (body.kind != BODY_NONE) {
        bool opens = strcmp(word, ".rept") == 0 || strcmp(word, ".irp") == 0 || strcmp(word, ".irpc") == 0;
        bool closes = strcmp(word, ".endr") == 0;
        if (body.kind == BODY_MACRO) {
            opens = strcmp(word, ".macro") == 0;
            closes = strcmp(word, ".endm") == 0;
        }
        if (closes && body.depth == 0) {
            end_body();
            return;
        }
        body.depth += opens ? 1 : closes ? -1 : 0;
        add_body_line(line);
        return;
    }
    char *args = as_strdup(rest, strlen(rest));
    if (strcmp(word, ".macro") == 0) {
        char *items[MACRO_PARAMS + 1];
        const char *after;
        struct macro *m = as_alloc(sizeof(*m));
        memset(m, 0, sizeof(*m));
        m->name = first_word(args, &after);
        if (!*m->name)
            as_error(".macro needs a name");
        char *params = as_strdup(after, strlen(after));
        if (*trim(params) == ',')
            params = trim(params) + 1;
        int n = split_items(params, items, MACRO_PARAMS + 1);
        if (n > MACRO_PARAMS)
            as_error("too many macro parameters");
        for (int i = 0; i < n; i++) {
            char *eq = strchr(items[i], '='), *colon = strchr(items[i], ':');
            if (colon)
                *colon = 0;                     /* (":req", ":vararg": not checked) */
            if (eq) {
                *eq = 0;
                m->defaults[i] = trim(eq + 1);
            }
            m->params[i] = trim(items[i]);
        }
        m->nparams = n;
        body.kind = BODY_MACRO;
        body.depth = 0;
        body.macro = m;
        return;
    }
    if (strcmp(word, ".rept") == 0) {
        const char *p = args;
        struct as_expr e;
        if (!parse_expr(&p, &e) || !expr_constant(&e, &body.count))
            as_error(".rept needs a constant count");
        body.kind = BODY_REPT;
        body.depth = 0;
        return;
    }
    if (strcmp(word, ".irp") == 0 || strcmp(word, ".irpc") == 0) {
        char *s = trim(args), *comma = s;
        while (*comma && *comma != ',' && *comma != ' ' && *comma != '\t')
            comma++;
        body.values = trim(*comma ? comma + 1 : comma);
        if (*body.values == ',')
            body.values = trim(body.values + 1);
        *comma = 0;
        body.var = s;
        body.kind = strcmp(word, ".irp") == 0 ? BODY_IRP : BODY_IRPC;
        body.depth = 0;
        return;
    }
    if (strcmp(word, ".endm") == 0 || strcmp(word, ".endr") == 0)
        as_error("'%s' without a beginning", word);
    for (struct macro *m = macros; m; m = m->next)
        if (strcmp(m->name, word) == 0) {
            call_macro(m, args);
            return;
        }
    assemble_line(line);
}

/* --- layout -------------------------------------------------------------- */

static int64_t frag_size(struct as_fragment *f, int64_t address)
{
    switch (f->kind) {
    case FRAG_DATA:
        return f->len;
    case FRAG_JUMP:
        return arch->jump_size(f, f->is_long);
    case FRAG_ALIGN: {
        int64_t pad = (f->align - address % f->align) % f->align;
        return pad;
    }
    }
    return 0;
}

static void place_fragments(struct as_section *s)
{
    int64_t a = 0;
    for (struct as_fragment *f = s->first; f; f = f->next) {
        f->address = a;
        a += frag_size(f, a);
    }
    s->size = a;
}

static int64_t symbol_address(struct as_symbol *sym)
{
    if (sym->is_absolute)
        return sym->value;
    return sym->frag ? sym->frag->address + sym->frag_offset : 0;
}

static void layout(void)
{
    for (struct as_section *s = sections; s; s = s->next) {
        for (;;) {
            place_fragments(s);
            bool changed = false;
            for (struct as_fragment *f = s->first; f; f = f->next) {
                if (f->kind != FRAG_JUMP || f->is_long)
                    continue;
                struct as_symbol *t = f->target.sym;
                if (!t->defined || t->section != s || t->is_global || t->is_weak) {
                    /* (a label made global after the jump, too: it takes a relocation) */
                    f->is_long = true;
                    changed = true;
                    continue;
                }
                int64_t disp = symbol_address(t) + f->target.addend - (f->address + arch->jump_size(f, false));
                if (disp < -128 || disp > 127) {
                    f->is_long = true;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
    }
    for (int i = 0; i < nsymbols; i++) {
        struct as_symbol *sym = symbol_list[i];
        if (sym->defined && !sym->is_absolute)
            sym->value = symbol_address(sym);
    }
}

/* --- fixups and relocations ---------------------------------------------- */

static void add_reloc(struct as_section *s, int64_t offset, struct as_symbol *sym, int type, int64_t addend)
{
    struct as_reloc *r = as_alloc(sizeof(*r));
    r->offset = offset;
    r->type = type;
    /* a local label: the section's symbol and its offset (62b: not a
     * per-thread variable, whose TLS relocations name it, as LLVM's do) */
    if (sym && sym->defined && !sym->is_global && !sym->is_weak && !sym->is_absolute && sym->type != STT_TLS) {
        addend += sym->value;
        sym = sym->section->symbol;
    }
    if (sym)
        sym->used_in_reloc = true;
    r->sym = sym;
    r->addend = addend;
    r->next = s->relocs;
    s->relocs = r;
}

static void resolve(struct as_section *s, unsigned char *out)
{
    for (struct as_fragment *f = s->first; f; f = f->next) {
        unsigned char *at = out + f->address;
        if (f->kind == FRAG_DATA) {
            if (out)
                memcpy(at, f->data, (size_t)f->len);
            for (struct as_fixup *x = f->fixups; x; x = x->next) {
                struct as_expr e = x->expr;
                if (e.minus) {
                    /* a difference: both in one section */
                    if (!e.sym->defined || !e.minus->defined || e.sym->section != e.minus->section)
                        as_error("a difference of symbols in different sections");
                    arch->apply(x, f, at, e.sym->value - e.minus->value + e.addend, true);
                    continue;
                }
                if (!e.sym || e.sym->is_absolute) {
                    arch->apply(x, f, at, e.addend + (e.sym ? e.sym->value : 0), true);
                    continue;
                }
                if (x->pcrel && e.sym->defined && e.sym->section == s && !e.sym->is_global && !e.sym->is_weak) {
                    arch->apply(x, f, at, e.sym->value + e.addend - (f->address + x->pc_base), true);
                    continue;
                }
                int type;
                int64_t addend = 0;
                if (!arch->relocate(x, &type, &addend))
                    as_error("no relocation for a reference to '%s'", e.sym->name);
                add_reloc(s, f->address + x->offset, e.sym, type, e.addend + addend);
                arch->apply(x, f, at, 0, false);
            }
        } else if (f->kind == FRAG_JUMP) {
            struct as_symbol *t = f->target.sym;
            int size = arch->jump_size(f, f->is_long);
            if (out)
                arch->write_jump(f, at);
            /* the displacement: the last four bytes (or one) */
            struct as_fixup x = { 0 };
            x.expr = f->target;
            x.size = f->is_long ? 4 : 1;
            x.offset = size - x.size;
            x.pc_base = size;
            x.pcrel = true;
            x.kind = 2;                         /* (x86-64: PLT32 for every jump and call, as LLVM's) */
            if (t->defined && t->section == s && !t->is_global && !t->is_weak) {
                int64_t disp = t->value + f->target.addend - (f->address + size);
                if (out)
                    arch->apply(&x, f, at, disp, true);
            } else {
                int type;
                int64_t addend = 0;
                if (!arch->relocate(&x, &type, &addend))
                    as_error("no relocation for a jump to '%s'", t->name);
                if (out)
                    add_reloc(s, f->address + x.offset, t, type, f->target.addend + addend);
            }
        } else if (f->kind == FRAG_ALIGN && out) {
            int64_t n = frag_size(f, f->address);
            if (f->code)
                arch->nops(at, n);
            else
                memset(at, 0, (size_t)n);
        }
    }
}

/* --- ELF ----------------------------------------------------------------- */

struct strtab {
    char *data;
    size_t len, cap;
};

static uint32_t str_add(struct strtab *t, const char *s)
{
    size_t n = strlen(s) + 1;
    if (t->len + n > t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        while (t->cap < t->len + n)
            t->cap *= 2;
        t->data = realloc(t->data, t->cap);
    }
    memcpy(t->data + t->len, s, n);
    t->len += n;
    return (uint32_t)(t->len - n);
}

static void put(unsigned char *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void write_object(const char *path)
{
    /* contents of every section */
    unsigned char **contents = as_alloc(sizeof(unsigned char *) * 256);
    int nsec = 0;
    for (struct as_section *s = sections; s; s = s->next) {
        unsigned char *buf = as_alloc((size_t)s->size + 1);
        resolve(s, buf);
        contents[nsec++] = buf;
        if (nsec >= 250)
            as_error("too many sections");
    }
    for (int i = 0; i < nsymbols; i++) {
        struct as_symbol *sym = symbol_list[i];
        if (sym->size_expr) {
            struct as_expr *e = (struct as_expr *)sym->size_expr;
            if (e->minus)
                sym->size = e->sym->value - e->minus->value + e->addend;
            else if (!e->sym || e->sym->is_absolute)
                sym->size = e->addend + (e->sym ? e->sym->value : 0);
            else
                as_error(".size of '%s' is not a constant", sym->name);
        }
    }
    /* symbols: the null one, sections, locals, then globals */
    struct strtab strtab = { 0 }, shstrtab = { 0 };
    str_add(&strtab, "");
    str_add(&shstrtab, "");
    int nsyms = 1;
    struct as_symbol **table = as_alloc(sizeof(struct as_symbol *) * (size_t)(nsymbols + 300));
    int index = 1;
    for (struct as_section *s = sections; s; s = s->next) {
        s->index = index++;
        s->symbol->index = nsyms;
        table[nsyms++] = s->symbol;
    }
    for (int i = 0; i < nsymbols; i++) {
        struct as_symbol *sym = symbol_list[i];
        if (sym->is_global || sym->is_weak || !sym->defined)
            continue;
        if (strncmp(sym->name, ".L", 2) == 0)
            continue;                           /* (temporary labels stay out) */
        sym->index = nsyms;
        table[nsyms++] = sym;
    }
    int first_global = nsyms;
    for (int i = 0; i < nsymbols; i++) {
        struct as_symbol *sym = symbol_list[i];
        bool external = sym->is_global || sym->is_weak || (!sym->defined && sym->used_in_reloc);
        if (!external)
            continue;
        if (!sym->defined && !sym->used_in_reloc && !sym->is_global && !sym->is_weak)
            continue;
        if (strncmp(sym->name, ".L", 2) == 0 && !sym->defined)
            as_error("undefined temporary label '%s'", sym->name);
        sym->index = nsyms;
        table[nsyms++] = sym;
    }
    /* section headers: null, the sections, their .rela, .symtab, .strtab, .shstrtab */
    int nrela = 0;
    for (struct as_section *s = sections; s; s = s->next)
        if (s->relocs)
            s->rela_index = index + nrela++;
    int symtab_index = index + nrela, strtab_index = symtab_index + 1, shstrtab_index = strtab_index + 1;
    int nheaders = shstrtab_index + 1;
    /* the file: header, contents, rela, symtab, strtab, shstrtab, then headers */
    size_t cap = 64;
    for (struct as_section *s = sections; s; s = s->next)
        cap += (size_t)s->size + 16;
    unsigned char *file = 0;
    size_t len = 64;
    uint64_t *offsets = as_alloc(sizeof(uint64_t) * (size_t)nheaders);
    uint64_t *sizes = as_alloc(sizeof(uint64_t) * (size_t)nheaders);
    /* (sizes first, then one buffer) */
    struct as_section **by_index = as_alloc(sizeof(struct as_section *) * (size_t)nheaders);
    for (struct as_section *s = sections; s; s = s->next)
        by_index[s->index] = s;
    int nrel_entries[256] = { 0 };
    for (struct as_section *s = sections; s; s = s->next)
        for (struct as_reloc *r = s->relocs; r; r = r->next)
            nrel_entries[s->index]++;
    uint32_t *sec_names = as_alloc(sizeof(uint32_t) * (size_t)nheaders);
    for (struct as_section *s = sections; s; s = s->next)
        sec_names[s->index] = str_add(&shstrtab, s->name);
    for (struct as_section *s = sections; s; s = s->next)
        if (s->relocs) {
            char *n = as_alloc(strlen(s->name) + 6);
            snprintf(n, strlen(s->name) + 6, ".rela%s", s->name);
            sec_names[s->rela_index] = str_add(&shstrtab, n);
        }
    sec_names[symtab_index] = str_add(&shstrtab, ".symtab");
    sec_names[strtab_index] = str_add(&shstrtab, ".strtab");
    sec_names[shstrtab_index] = str_add(&shstrtab, ".shstrtab");
    uint32_t *sym_names = as_alloc(sizeof(uint32_t) * (size_t)nsyms);
    for (int i = 1; i < nsyms; i++)
        sym_names[i] = table[i]->type == STT_SECTION ? 0 : str_add(&strtab, table[i]->name);
    for (struct as_section *s = sections; s; s = s->next) {
        len = (len + 15) & ~(size_t)15;
        offsets[s->index] = len;
        sizes[s->index] = (uint64_t)s->size;
        if (s->type != SHT_NOBITS)
            len += (size_t)s->size;
    }
    for (struct as_section *s = sections; s; s = s->next)
        if (s->relocs) {
            len = (len + 7) & ~(size_t)7;
            offsets[s->rela_index] = len;
            sizes[s->rela_index] = (uint64_t)nrel_entries[s->index] * 24;
            len += sizes[s->rela_index];
        }
    len = (len + 7) & ~(size_t)7;
    offsets[symtab_index] = len;
    sizes[symtab_index] = (uint64_t)nsyms * 24;
    len += sizes[symtab_index];
    offsets[strtab_index] = len;
    sizes[strtab_index] = strtab.len;
    len += strtab.len;
    offsets[shstrtab_index] = len;
    sizes[shstrtab_index] = shstrtab.len;
    len += shstrtab.len;
    len = (len + 7) & ~(size_t)7;
    size_t shoff = len;
    len += (size_t)nheaders * 64;
    file = as_alloc(len);
    (void)cap;
    /* the ELF header */
    memcpy(file, "\177ELF\2\1\1", 7);
    put(file + 16, 1, 2);                       /* ET_REL */
    put(file + 18, (uint64_t)arch->machine, 2);
    put(file + 20, 1, 4);
    put(file + 40, shoff, 8);
    put(file + 52, 64, 2);
    put(file + 58, 64, 2);
    put(file + 60, (uint64_t)nheaders, 2);
    put(file + 62, (uint64_t)shstrtab_index, 2);
    int k = 0;
    for (struct as_section *s = sections; s; s = s->next, k++)
        if (s->type != SHT_NOBITS)
            memcpy(file + offsets[s->index], contents[k], (size_t)s->size);
    for (struct as_section *s = sections; s; s = s->next) {
        if (!s->relocs)
            continue;
        /* in the order of their offsets */
        int n = nrel_entries[s->index], j = 0;
        struct as_reloc **list = as_alloc(sizeof(struct as_reloc *) * (size_t)n);
        for (struct as_reloc *r = s->relocs; r; r = r->next)
            list[j++] = r;
        for (int a = 1; a < n; a++)
            for (int b = a; b > 0 && list[b - 1]->offset > list[b]->offset; b--) {
                struct as_reloc *t = list[b - 1];
                list[b - 1] = list[b];
                list[b] = t;
            }
        unsigned char *p = file + offsets[s->rela_index];
        for (j = 0; j < n; j++, p += 24) {
            put(p, (uint64_t)list[j]->offset, 8);
            put(p + 8, ((uint64_t)(list[j]->sym ? list[j]->sym->index : 0) << 32) | (uint32_t)list[j]->type, 8);
            put(p + 16, (uint64_t)list[j]->addend, 8);
        }
    }
    unsigned char *p = file + offsets[symtab_index] + 24;
    for (int i = 1; i < nsyms; i++, p += 24) {
        struct as_symbol *sym = table[i];
        int bind = i < first_global ? 0 : sym->is_weak ? 2 : 1;
        put(p, sym_names[i], 4);
        p[4] = (unsigned char)((bind << 4) | (sym->type & 15));
        p[5] = sym->is_hidden ? 2 : 0;
        int shndx = sym->is_absolute ? 0xFFF1 : sym->defined ? sym->section->index : 0;
        put(p + 6, (uint64_t)shndx, 2);
        put(p + 8, sym->type == STT_SECTION ? 0 : (uint64_t)sym->value, 8);
        put(p + 16, (uint64_t)sym->size, 8);
    }
    memcpy(file + offsets[strtab_index], strtab.data, strtab.len);
    memcpy(file + offsets[shstrtab_index], shstrtab.data, shstrtab.len);
    /* section headers */
    for (int i = 1; i < nheaders; i++) {
        unsigned char *h = file + shoff + 64 * (size_t)i;
        put(h, sec_names[i], 4);
        struct as_section *s = i < index ? by_index[i] : 0;
        if (s) {
            put(h + 4, (uint64_t)s->type, 4);
            put(h + 8, s->flags, 8);
            put(h + 24, offsets[i], 8);
            put(h + 32, sizes[i], 8);
            put(h + 48, (uint64_t)s->align, 8);
            put(h + 56, (uint64_t)s->entsize, 8);
        } else if (i == symtab_index) {
            put(h + 4, SHT_SYMTAB, 4);
            put(h + 24, offsets[i], 8);
            put(h + 32, sizes[i], 8);
            put(h + 40, (uint64_t)strtab_index, 4);
            put(h + 44, (uint64_t)first_global, 4);
            put(h + 48, 8, 8);
            put(h + 56, 24, 8);
        } else if (i == strtab_index || i == shstrtab_index) {
            put(h + 4, SHT_STRTAB, 4);
            put(h + 24, offsets[i], 8);
            put(h + 32, sizes[i], 8);
            put(h + 48, 1, 8);
        } else {
            /* a .rela section: which one? */
            for (struct as_section *x = sections; x; x = x->next)
                if (x->relocs && x->rela_index == i) {
                    put(h + 4, SHT_RELA, 4);
                    put(h + 8, SHF_INFO_LINK, 8);
                    put(h + 24, offsets[i], 8);
                    put(h + 32, sizes[i], 8);
                    put(h + 40, (uint64_t)symtab_index, 4);
                    put(h + 44, (uint64_t)x->index, 4);
                    put(h + 48, 8, 8);
                    put(h + 56, 24, 8);
                }
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(file, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "as: cannot write '%s'\n", path);
        exit(1);
    }
}

/* --- the program --------------------------------------------------------- */

int main(int argc, char **argv)
{
#if defined(__aarch64__) || defined(__arm64__)
    arch = &arch_aarch64;
#else
    arch = &arch_x86_64;
#endif
    const char *input = 0, *output = "a.out";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (strcmp(argv[i], "-target") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "x86_64") == 0 || strncmp(argv[i], "x86_64-", 7) == 0)
                arch = &arch_x86_64;
            else if (strcmp(argv[i], "aarch64") == 0 || strncmp(argv[i], "aarch64-", 8) == 0)
                arch = &arch_aarch64;
            else {
                fprintf(stderr, "as: unknown target '%s'\n", argv[i]);
                return 2;
            }
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "as: unknown option '%s'\n", argv[i]);
            return 2;
        } else
            input = argv[i];
    }
    if (!input) {
        fprintf(stderr, "usage: as [-target ARCH] [-o FILE] FILE\n");
        return 2;
    }
    as_file = input;
    FILE *f = strcmp(input, "-") == 0 ? stdin : fopen(input, "r");
    if (!f) {
        fprintf(stderr, "as: cannot read '%s'\n", input);
        return 1;
    }
    /* .text, .data and .bss always exist, in that order (as GNU's) */
    find_section(".text", 0, 0, false);
    find_section(".data", 0, 0, false);
    find_section(".bss", 0, 0, false);
    current_section = sections;
    /* lines of any length (no getline in RitterOS's C library) */
    size_t cap = 4096, len = 0;
    char *line = as_alloc(cap);
    int c;
    do {
        c = getc(f);
        if (c == '\n' || c == EOF) {
            if (c == EOF && len == 0)
                break;
            line[len] = 0;
            as_line++;
            source_line(line);
            len = 0;
            continue;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            line = realloc(line, cap);
            if (!line)
                as_error("out of memory");
        }
        line[len++] = (char)c;
    } while (c != EOF);
    free(line);
    if (body.kind != BODY_NONE)
        as_error("the file ends inside a %s", body.kind == BODY_MACRO ? ".macro" : "repetition");
    if (f != stdin)
        fclose(f);
    for (int i = 0; i < nsymbols; i++) {
        struct as_symbol *s = symbol_list[i];
        if (s->is_weak)
            s->is_global = true;
    }
    layout();
    write_object(output);
    return 0;
}
