/*
 * The AML interpreter (37b): values, the namespace, conversions, field
 * units and buffer fields, the simulated operation regions, the names'
 * listing. Reading and running the code is aml_run.c. See aml.h.
 *
 * Values that own storage (strings, buffers, packages, index references)
 * hold a count on it: every struct aml_value a function fills is its
 * caller's, given back with aml_release.
 */
#include "aml_impl.h"

const struct aml_os *aml_machine;
struct aml_node *aml_root;
uint64_t aml_ones = ~(uint64_t)0;
struct aml_stats aml_counters;

/* Log lines: errors repeat (a method run every 2 s), so after the first 64
 * only the count grows. */
#define AML_ERROR_LINES 64

static void log_line(const char *fmt, va_list ap)
{
    char line[200];
    kvsnprintf(line, sizeof(line), fmt, ap);
#ifdef AML_HOST
    printf("aml: %s\n", line);
#else
    kprintf("aml: %s\n", line);
#endif
}

void aml_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_line(fmt, ap);
    va_end(ap);
}

void aml_fail(const char *fmt, ...)
{
    if (++aml_counters.errors > AML_ERROR_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    log_line(fmt, ap);
    va_end(ap);
}

uint64_t aml_now_ns(void)
{
    return aml_machine && aml_machine->now_ns ? aml_machine->now_ns() : 0;
}

/* ---- Values ---- */

struct aml_heap *aml_heap_new(uint8_t type, uint32_t length)
{
    if (length > (1u << 24)) {
        aml_fail("an object of %u bytes or elements refused", length);
        return 0;
    }
    struct aml_heap *h = kzalloc(sizeof(*h));
    if (!h)
        return 0;
    h->refs = 1;
    h->type = type;
    h->length = length;
    if (type == AML_PACKAGE)
        h->elements = length ? kzalloc(length * sizeof(struct aml_value)) : 0;
    else
        h->bytes = kzalloc(length + 1);
    if ((type == AML_PACKAGE && length && !h->elements) || (type != AML_PACKAGE && !h->bytes)) {
        kfree(h);
        return 0;
    }
    return h;
}

static void heap_drop(struct aml_heap *h)
{
    if (!h || --h->refs)
        return;
    if (h->type == AML_PACKAGE) {
        for (uint32_t i = 0; i < h->length; i++)
            aml_release(&h->elements[i]);
        kfree(h->elements);
    } else {
        kfree(h->bytes);
    }
    kfree(h);
}

static bool owns_heap(const struct aml_value *v)
{
    return v->type == AML_STRING || v->type == AML_BUFFER || v->type == AML_PACKAGE ||
           (v->type == AML_REFERENCE && (v->ref == REF_INDEX || v->ref == REF_NAME));
}

void aml_hold(const struct aml_value *v)
{
    if (owns_heap(v) && v->heap)
        v->heap->refs++;
}

void aml_release(struct aml_value *v)
{
    if (owns_heap(v))
        heap_drop(v->heap);
    memset(v, 0, sizeof(*v));
}

void aml_copy(struct aml_value *dst, const struct aml_value *src)
{
    struct aml_value tmp = *src;
    aml_hold(&tmp);
    aml_release(dst);
    *dst = tmp;
}

/* A copy of its own (Store's: a buffer or package stored is the target's
 * alone afterwards); references and packages' references stay shared. */
bool aml_duplicate(struct aml_value *dst, const struct aml_value *src)
{
    struct aml_value out = {0};
    switch (src->type) {
    case AML_STRING:
    case AML_BUFFER:
        out.type = src->type;
        out.heap = aml_heap_new(src->type, src->heap->length);
        if (!out.heap)
            return false;
        memcpy(out.heap->bytes, src->heap->bytes, src->heap->length);
        break;
    case AML_PACKAGE:
        out.type = AML_PACKAGE;
        out.heap = aml_heap_new(AML_PACKAGE, src->heap->length);
        if (!out.heap)
            return false;
        for (uint32_t i = 0; i < src->heap->length; i++)
            if (!aml_duplicate(&out.heap->elements[i], &src->heap->elements[i])) {
                aml_release(&out);
                return false;
            }
        break;
    default:
        out = *src;
        aml_hold(&out);
    }
    aml_release(dst);
    *dst = out;
    return true;
}

void aml_set_integer(struct aml_value *v, uint64_t n)
{
    aml_release(v);
    v->type = AML_INTEGER;
    v->integer = n & aml_ones;
}

static bool set_bytes(struct aml_value *v, uint8_t type, const void *bytes, uint32_t length)
{
    struct aml_heap *h = aml_heap_new(type, length);
    if (!h)
        return false;
    if (bytes)
        memcpy(h->bytes, bytes, length);
    aml_release(v);
    v->type = type;
    v->heap = h;
    return true;
}

bool aml_set_string(struct aml_value *v, const char *s, uint32_t length)
{
    return set_bytes(v, AML_STRING, s, length);
}

bool aml_set_buffer(struct aml_value *v, const uint8_t *bytes, uint32_t length)
{
    return set_bytes(v, AML_BUFFER, bytes, length);
}

bool aml_set_package(struct aml_value *v, uint32_t count)
{
    struct aml_heap *h = aml_heap_new(AML_PACKAGE, count);
    if (!h)
        return false;
    aml_release(v);
    v->type = AML_PACKAGE;
    v->heap = h;
    return true;
}

/* ---- Conversions (ACPI 6.5 19.3.5, with ACPICA's choices where the
 * specification leaves them open) ---- */

static int hex_digit(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* A string as an integer: implicitly hexadecimal digits (after a "0x" if
 * there is one, as ACPICA) up to the first other character; ToInteger's
 * decimal, or hexadecimal after "0x". */
static uint64_t string_integer(const uint8_t *s, uint32_t length, bool strict)
{
    uint32_t i = 0;
    while (i < length && (s[i] == ' ' || s[i] == '\t'))
        i++;
    int base = strict ? 10 : 16;
    if (i + 1 < length && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    uint64_t n = 0;
    for (; i < length; i++) {
        int d = hex_digit(s[i]);
        if (d < 0 || d >= base)
            break;
        n = n * (uint64_t)base + (uint64_t)d;
    }
    return n & aml_ones;
}

/* The value a reference names, for operators that take values. */
static bool referred(const struct aml_value *v, struct aml_value *out);

bool aml_to_integer(const struct aml_value *v, uint64_t *n, bool strict)
{
    switch (v->type) {
    case AML_UNINIT:
        *n = 0;                          /* as ACPICA's slack mode, Linux's */
        return true;
    case AML_INTEGER:
        *n = v->integer & aml_ones;
        return true;
    case AML_STRING:
        *n = string_integer(v->heap->bytes, v->heap->length, strict);
        return true;
    case AML_BUFFER: {
        uint32_t width = aml_ones == 0xFFFFFFFF ? 4 : 8;
        uint64_t x = 0;
        for (uint32_t i = 0; i < v->heap->length && i < width; i++)
            x |= (uint64_t)v->heap->bytes[i] << (8 * i);
        *n = x;
        return true;
    }
    case AML_REFERENCE: {
        struct aml_value target = {0};
        if (!referred(v, &target))
            return false;
        bool ok = target.type != AML_REFERENCE && aml_to_integer(&target, n, strict);
        aml_release(&target);
        return ok;
    }
    }
    aml_fail("an object of type %u used as an integer", (unsigned)v->type);
    return false;
}

bool aml_to_buffer(const struct aml_value *v, struct aml_value *out)
{
    struct aml_value r = {0};
    bool ok = false;
    switch (v->type) {
    case AML_INTEGER: {
        uint8_t b[8];
        uint32_t width = aml_ones == 0xFFFFFFFF ? 4 : 8;
        for (uint32_t i = 0; i < width; i++)
            b[i] = (uint8_t)(v->integer >> (8 * i));
        ok = aml_set_buffer(&r, b, width);
        break;
    }
    case AML_STRING:
        /* the string with its 0 (ACPI 19.3.5.7: an empty string, none) */
        ok = aml_set_buffer(&r, v->heap->bytes, v->heap->length ? v->heap->length + 1 : 0);
        break;
    case AML_BUFFER:
        aml_copy(&r, v);
        ok = true;
        break;
    case AML_REFERENCE: {
        struct aml_value target = {0};
        if (referred(v, &target) && target.type != AML_REFERENCE)
            ok = aml_to_buffer(&target, &r);
        aml_release(&target);
        break;
    }
    default:
        aml_fail("an object of type %u used as a buffer", (unsigned)v->type);
    }
    if (ok) {
        aml_release(out);
        *out = r;
    }
    return ok;
}

static uint32_t put_number(char *s, uint64_t n, int base, int digits)
{
    char tmp[24];
    int k = 0;
    do {
        tmp[k++] = "0123456789ABCDEF"[n % (uint64_t)base];
        n /= (uint64_t)base;
    } while (n || k < digits);
    for (int i = 0; i < k; i++)
        s[i] = tmp[k - 1 - i];
    return (uint32_t)k;
}

bool aml_to_string(const struct aml_value *v, struct aml_value *out, int how)
{
    struct aml_value r = {0};
    bool ok = false;
    switch (v->type) {
    case AML_STRING:
        aml_copy(&r, v);
        ok = true;
        break;
    case AML_INTEGER: {
        char s[24];
        uint32_t n = 0;
        int width = aml_ones == 0xFFFFFFFF ? 8 : 16;
        if (how == STRING_DECIMAL) {
            n = put_number(s, v->integer, 10, 1);
        } else if (how == STRING_HEX) {
            s[0] = '0', s[1] = 'x';
            n = 2 + put_number(s + 2, v->integer, 16, 1);
        } else {
            n = put_number(s, v->integer, 16, width);
        }
        ok = aml_set_string(&r, s, n);
        break;
    }
    case AML_BUFFER: {
        /* bytes as "0x01 0x02" (implicit), "0x01,0x02" (ToHexString),
         * "1,2" (ToDecimalString) */
        uint32_t length = v->heap->length;
        if (!aml_set_string(&r, 0, length * 5)) /* room enough, cut below */
            break;
        char *s = (char *)r.heap->bytes;
        uint32_t n = 0;
        for (uint32_t i = 0; i < length; i++) {
            if (i)
                s[n++] = how == STRING_IMPLICIT ? ' ' : ',';
            if (how == STRING_DECIMAL) {
                n += put_number(s + n, v->heap->bytes[i], 10, 1);
            } else {
                s[n++] = '0', s[n++] = 'x';
                n += put_number(s + n, v->heap->bytes[i], 16, 2);
            }
        }
        s[n] = 0;
        r.heap->length = n;
        ok = true;
        break;
    }
    case AML_REFERENCE: {
        struct aml_value target = {0};
        if (referred(v, &target) && target.type != AML_REFERENCE)
            ok = aml_to_string(&target, &r, how);
        aml_release(&target);
        break;
    }
    default:
        aml_fail("an object of type %u used as a string", (unsigned)v->type);
    }
    if (ok) {
        aml_release(out);
        *out = r;
    }
    return ok;
}

/* ---- The namespace ---- */

struct aml_node *aml_child(struct aml_node *scope, const char *name)
{
    for (struct aml_node *n = scope ? scope->child : 0; n; n = n->next)
        if (!memcmp(n->name, name, 4))
            return n;
    return 0;
}

struct aml_node *aml_node_new(struct aml_node *scope, const char *name, uint8_t type)
{
    struct aml_node *n = kzalloc(sizeof(*n));
    if (!n)
        return 0;
    memcpy(n->name, name, 4);
    n->type = type;
    n->parent = scope;
    if (scope) {
        struct aml_node **at = &scope->child;
        while (*at)
            at = &(*at)->next;
        *at = n;
    }
    aml_counters.nodes++;
    return n;
}

static void node_contents_free(struct aml_node *n)
{
    aml_release(&n->value);
    if (n->type == AML_BUFFER_FIELD)
        heap_drop(n->buffer_field.buffer);
}

/* A node out of the tree and freed, its children first (a method's
 * temporary objects at its end). */
void aml_node_free(struct aml_node *node)
{
    while (node->child)
        aml_node_free(node->child);
    if (node->parent) {
        struct aml_node **at = &node->parent->child;
        while (*at && *at != node)
            at = &(*at)->next;
        if (*at)
            *at = node->next;
    }
    node_contents_free(node);
    kfree(node);
    aml_counters.nodes--;
}

struct aml_node *aml_alias_target(struct aml_node *node)
{
    for (int i = 0; node && node->type == AML_ALIAS && i < 16; i++)
        node = node->alias;
    return node && node->type == AML_ALIAS ? 0 : node;
}

struct aml_node *aml_lookup(struct aml_node *scope, const uint8_t *s, uint32_t length)
{
    char segs[16][4];
    int count = 0, parents = 0;
    bool root = false;
    uint32_t i = 0;
    if (i < length && s[i] == '\\')
        root = true, i++;
    while (i < length && s[i] == '^')
        parents++, i++;
    while (i < length) {
        if (count == 16)
            return 0;
        memset(segs[count], '_', 4);
        int k = 0;
        while (i < length && s[i] != '.') {
            if (k >= 4)
                return 0;
            segs[count][k++] = (char)s[i++];
        }
        if (k == 0)
            return 0;
        count++;
        if (i < length)
            i++;
    }
    struct aml_node *n = root ? aml_root : scope;
    if (!root && !parents && count == 1) {
        for (; n; n = n->parent) {
            struct aml_node *c = aml_child(n, segs[0]);
            if (c)
                return c;
        }
        return 0;
    }
    for (int k = 0; k < parents && n; k++)
        n = n->parent;
    for (int k = 0; k < count && n; k++)
        n = aml_child(aml_alias_target(n), segs[k]);
    return n;
}

static bool node_value(struct aml_node *n, struct aml_value *out);

/* An unresolved name's node now, or 0. */
static struct aml_node *name_now(const struct aml_value *v)
{
    const uint8_t *text = v->heap->bytes;
    uint32_t length = v->heap->length, split = 0;
    while (split < length && text[split] != ' ')
        split++;
    if (split >= length)
        return 0;
    struct aml_node *scope = aml_lookup(aml_root, text, split);
    if (split == 1 && text[0] == '\\')
        scope = aml_root;
    return scope ? aml_lookup(scope, text + split + 1, length - split - 1) : 0;
}

bool aml_package_element(struct aml_node *n, struct aml_value *out)
{
    n = aml_alias_target(n);
    if (!n)
        return false;
    switch (n->type) {
    case AML_DEVICE: case AML_THERMAL: case AML_METHOD: case AML_MUTEX: case AML_POWER:
    case AML_PROCESSOR: case AML_EVENT: case AML_REGION: case AML_SCOPE:
        aml_release(out);
        out->type = AML_REFERENCE;
        out->ref = REF_NODE;
        out->node = n;
        return true;
    }
    return node_value(n, out);
}

static void resolve_in(struct aml_value *v, int depth)
{
    if (depth > 16)
        return;
    if (v->type == AML_PACKAGE) {
        for (uint32_t i = 0; i < v->heap->length; i++)
            resolve_in(&v->heap->elements[i], depth + 1);
    } else if (v->type == AML_REFERENCE && v->ref == REF_NAME) {
        struct aml_node *n = name_now(v);
        struct aml_value e = {0};
        if (n && aml_package_element(n, &e)) {
            aml_release(v);
            *v = e;
        }
    }
}

static void resolve_tree(struct aml_node *scope, int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        if (n->type == AML_PACKAGE)
            resolve_in(&n->value, 0);
        resolve_tree(n, depth + 1);
    }
}

void aml_resolve_names(void)
{
    if (aml_root)
        resolve_tree(aml_root, 0);
}

void aml_loaded(void)
{
    aml_resolve_names();
}

bool aml_package_get(const struct aml_value *pkg, uint32_t i, struct aml_value *out)
{
    if (pkg->type != AML_PACKAGE || i >= pkg->heap->length)
        return false;
    aml_copy(out, &pkg->heap->elements[i]);
    if (out->type == AML_REFERENCE && out->ref != REF_DEBUG) {
        struct aml_value r = {0};
        if (!referred(out, &r))
            return false;
        aml_release(out);
        *out = r;
    }
    return true;
}

bool aml_to_integer_value(const struct aml_value *v, uint64_t *n)
{
    return aml_to_integer(v, n, false);
}

const uint8_t *aml_bytes(const struct aml_value *v)
{
    return (v->type == AML_STRING || v->type == AML_BUFFER) ? v->heap->bytes : 0;
}

uint32_t aml_length(const struct aml_value *v)
{
    return (v->type == AML_STRING || v->type == AML_BUFFER || v->type == AML_PACKAGE)
           ? v->heap->length : 0;
}

struct aml_node *aml_find_in(struct aml_node *scope, const char *name)
{
    char seg[4] = {'_', '_', '_', '_'};
    for (int i = 0; i < 4 && name[i]; i++)
        seg[i] = name[i];
    return scope ? aml_alias_target(aml_child(scope, seg)) : 0;
}

struct aml_node *aml_find(const char *path)
{
    struct aml_node *n = aml_root;
    if (!n || !path)
        return 0;
    if (*path == '\\')
        path++;
    while (*path) {
        char seg[4] = {'_', '_', '_', '_'};
        int k = 0;
        while (*path && *path != '.') {
            if (k < 4)
                seg[k] = *path;
            k++, path++;
        }
        if (k == 0 || k > 4)
            return 0;
        if (*path == '.')
            path++;
        n = aml_alias_target(aml_child(n, seg));
        if (!n)
            return 0;
    }
    return n;
}

void aml_path(const struct aml_node *node, char *buffer, size_t size)
{
    const struct aml_node *chain[64];
    int depth = 0;
    for (const struct aml_node *n = node; n && n != aml_root && depth < 64; n = n->parent)
        chain[depth++] = n;
    size_t at = 0;
    if (size)
        buffer[at++] = '\\';
    for (int i = depth - 1; i >= 0; i--) {
        if (i != depth - 1 && at + 1 < size)
            buffer[at++] = '.';
        for (int k = 0; k < 4 && at + 1 < size; k++)
            buffer[at++] = chain[i]->name[k];
    }
    if (size)
        buffer[at < size ? at : size - 1] = 0;
}

void aml_stats(struct aml_stats *s)
{
    *s = aml_counters;
}

/* ---- Simulated operation regions: 256-byte pages of the address spaces
 * that were written, found by a hash; everything else reads 0. ---- */

struct sim_page {
    struct sim_page *next;
    uint8_t space;
    uint64_t base;
    uint8_t bytes[256];
};
static struct sim_page *sim_pages[256];

static struct sim_page *sim_page(uint8_t space, uint64_t base, bool make)
{
    unsigned slot = (unsigned)((base >> 8) * 31 + space) & 255;
    for (struct sim_page *p = sim_pages[slot]; p; p = p->next)
        if (p->space == space && p->base == base)
            return p;
    if (!make)
        return 0;
    struct sim_page *p = kzalloc(sizeof(*p));
    if (!p)
        return 0;
    p->space = space;
    p->base = base;
    p->next = sim_pages[slot];
    sim_pages[slot] = p;
    return p;
}

bool aml_simulated_read(uint8_t space, uint64_t address, int width, uint64_t *value)
{
    uint64_t v = 0;
    for (int i = 0; i < width / 8; i++) {
        uint64_t a = address + (uint64_t)i;
        struct sim_page *p = sim_page(space, a & ~(uint64_t)255, false);
        if (p)
            v |= (uint64_t)p->bytes[a & 255] << (8 * i);
    }
    *value = v;
    return true;
}

bool aml_simulated_write(uint8_t space, uint64_t address, int width, uint64_t value)
{
    for (int i = 0; i < width / 8; i++) {
        uint64_t a = address + (uint64_t)i;
        struct sim_page *p = sim_page(space, a & ~(uint64_t)255, true);
        if (!p)
            return false;
        p->bytes[a & 255] = (uint8_t)(value >> (8 * i));
    }
    return true;
}

/* ---- Field units (ACPI 6.5 19.6.48, 19.6.64, 19.6.7) ---- */

static uint64_t get_bits(const uint8_t *bytes, uint32_t at, uint32_t count)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < count; i++, at++)
        if (bytes[at / 8] >> (at % 8) & 1)
            v |= (uint64_t)1 << i;
    return v;
}

static void put_bits(uint8_t *bytes, uint32_t at, uint64_t v, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++, at++) {
        if (v >> i & 1)
            bytes[at / 8] |= (uint8_t)(1 << (at % 8));
        else
            bytes[at / 8] &= (uint8_t)~(1 << (at % 8));
    }
}

/* The bytes a field reads at once: its access type's; AnyAcc and
 * BufferAcc a byte at a time (ACPICA's, so Linux's, choice). */
static uint32_t access_bytes(const struct aml_node *f)
{
    switch (f->field.flags & 0xF) {
    case 2: return 2;
    case 3: return 4;
    case 4: return 8;
    }
    return 1;
}

static bool unit_read(struct aml_node *f, uint32_t offset, uint32_t width, uint64_t *v);

/* ---- (37b-4) Where a PCI configuration region is (ACPICA's
 * AcpiEvPciConfigRegionSetup and AcpiHwDerivePciId) ---- */

static bool is_root_bridge(struct aml_node *dev);
static uint64_t integer_of(struct aml_node *scope, const char *name, uint64_t otherwise);

static struct aml_node *device_of(struct aml_node *n)
{
    while (n && n->type != AML_DEVICE)
        n = n->parent;
    return n;
}

/* dev's PCI address (register 0). */
static uint64_t pci_address(struct aml_node *dev)
{
    struct aml_node *root = dev;
    while (root && !is_root_bridge(root))
        root = device_of(root->parent);
    uint64_t seg = 0, bus = 0;
    if (root) {
        seg = integer_of(root, "_SEG", 0);
        bus = integer_of(root, "_BBN", 0);
    }
    /* the devices from below the root bridge down to dev: each
     * one's _ADR on the bus so far, a bridge's secondary bus for the next */
    struct aml_node *chain[16];
    int depth = 0;
    for (struct aml_node *n = dev; n && n != root && depth < 16; n = device_of(n->parent))
        chain[depth++] = n;
    uint64_t adr = 0;
    for (int i = depth - 1; i >= 0; i--) {
        adr = integer_of(chain[i], "_ADR", 0);
        if (i == 0)
            break;
        uint64_t header = 0, secondary = 0;
        uint64_t at = AML_PCI_ADDRESS(seg, bus, (adr >> 16) & 31, adr & 7, 0);
        const struct aml_os *m = aml_machine;
        if (m && m->region_read && m->region_read(2, at + 0x0E, 8, &header) &&
            ((header & 0x7F) == 1 || (header & 0x7F) == 2) &&
            m->region_read(2, at + 0x19, 8, &secondary))
            bus = secondary;
    }
    return AML_PCI_ADDRESS(seg, bus, (adr >> 16) & 31, adr & 7, 0);
}

static bool pci_place(struct aml_node *region)
{
    region->region.pci = pci_address(device_of(region->parent));
    region->region.pci_ready = true;
    return true;
}

/* (37e-2a) A device with an _ADR and a child name, at address. */
static struct aml_node *pci_child_in(struct aml_node *scope, uint64_t address, const char *name,
                                     int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        struct aml_node *c;
        if (n->type == AML_DEVICE && aml_child(n, "_ADR") && (c = aml_find_in(n, name)) &&
            pci_address(n) == address)
            return c;
        if ((c = pci_child_in(n, address, name, depth + 1)))
            return c;
    }
    return 0;
}

struct aml_node *aml_pci_child(uint64_t address, const char *name)
{
    return aml_root ? pci_child_in(aml_root, address, name, 0) : 0;
}
static bool unit_write(struct aml_node *f, uint32_t offset, uint32_t width, uint64_t v);

static bool region_access(struct aml_node *region, uint32_t offset, uint32_t width,
                          uint64_t *v, bool write)
{
    region = aml_alias_target(region);
    if (!region || region->type != AML_REGION) {
        aml_fail("a field's region is not a region");
        return false;
    }
    if ((uint64_t)offset + width > region->region.length) {
        char path[80];
        aml_path(region, path, sizeof(path));
        aml_fail("%s: access at %u past its length %lu", path, offset,
                 (unsigned long)region->region.length);
        return false;
    }
    uint64_t address = region->region.offset + offset;
    if (region->region.space == 2) {    /* PCI configuration: its device's */
        if (!region->region.pci_ready && !pci_place(region))
            return false;
        address = region->region.pci + region->region.offset + offset;
    }
    const struct aml_os *m = aml_machine;
    bool ok = write ? m && m->region_write && m->region_write(region->region.space, address,
                                                               (int)width * 8, *v)
                    : m && m->region_read && m->region_read(region->region.space, address,
                                                            (int)width * 8, v);
    if (!ok)
        aml_fail("address space %u at 0x%lX: no way there", (unsigned)region->region.space,
                 (unsigned long)address);
    return ok;
}

bool aml_region_bytes(struct aml_node *region, uint32_t offset, uint8_t *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint64_t v;
        if (!region_access(region, offset + i, 1, &v, false))
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static bool select_bank(struct aml_node *f)
{
    struct aml_value bank = {0};
    aml_set_integer(&bank, f->field.bank_value);
    return aml_field_write(f->field.bank, &bank);
}

static bool unit_read(struct aml_node *f, uint32_t offset, uint32_t width, uint64_t *v)
{
    if (f->field.kind == AML_INDEX_FIELD) {
        struct aml_value at = {0}, data = {0};
        aml_set_integer(&at, offset);
        if (!aml_field_write(f->field.index, &at) || !aml_field_read(f->field.data, &data))
            return false;
        bool ok = aml_to_integer(&data, v, false);
        aml_release(&data);
        return ok;
    }
    if (f->field.kind == AML_BANK_FIELD && !select_bank(f))
        return false;
    return region_access(f->field.region, offset, width, v, false);
}

static bool unit_write(struct aml_node *f, uint32_t offset, uint32_t width, uint64_t v)
{
    if (f->field.kind == AML_INDEX_FIELD) {
        struct aml_value at = {0}, data = {0};
        aml_set_integer(&at, offset);
        aml_set_integer(&data, v);
        return aml_field_write(f->field.index, &at) && aml_field_write(f->field.data, &data);
    }
    if (f->field.kind == AML_BANK_FIELD && !select_bank(f))
        return false;
    return region_access(f->field.region, offset, width, &v, true);
}

/* A value of bits: an integer when it fits in one, else a buffer. */
static bool bits_value(const uint8_t *bytes, uint32_t bits, struct aml_value *out)
{
    uint32_t int_bits = aml_ones == 0xFFFFFFFF ? 32 : 64;
    if (bits <= int_bits) {
        aml_set_integer(out, get_bits(bytes, 0, bits));
        return true;
    }
    return aml_set_buffer(out, bytes, (bits + 7) / 8);
}

/* A value as the bits a field takes (integers little-endian, buffers and
 * strings by their bytes), zero beyond the value. */
static bool value_bits(const struct aml_value *v, uint8_t *bytes, uint32_t bits)
{
    uint32_t length = (bits + 7) / 8;
    memset(bytes, 0, length);
    if (v->type == AML_BUFFER || v->type == AML_STRING) {
        uint32_t n = v->heap->length < length ? v->heap->length : length;
        memcpy(bytes, v->heap->bytes, n);
        return true;
    }
    uint64_t n;
    if (!aml_to_integer(v, &n, false))
        return false;
    for (uint32_t i = 0; i < length && i < 8; i++)
        bytes[i] = (uint8_t)(n >> (8 * i));
    return true;
}

#define FIELD_LARGEST (1u << 21)        /* bits: the old laptop's video BIOS copy is 1 << 18 */

bool aml_field_read(struct aml_node *f, struct aml_value *out)
{
    if (!f || f->type != AML_FIELD || f->field.bit_length == 0 ||
        f->field.bit_length > FIELD_LARGEST)
        return false;
    uint32_t width = access_bytes(f), unit_bits = 8 * width;
    uint32_t start = f->field.bit_offset, end = start + f->field.bit_length;
    uint8_t *bytes = kzalloc((f->field.bit_length + 7) / 8 + 1);
    if (!bytes)
        return false;
    bool ok = true;
    for (uint32_t u = start / unit_bits; ok && u * unit_bits < end; u++) {
        uint64_t datum;
        ok = unit_read(f, u * width, width, &datum);
        uint32_t lo = start > u * unit_bits ? start : u * unit_bits;
        uint32_t hi = end < (u + 1) * unit_bits ? end : (u + 1) * unit_bits;
        if (ok)
            put_bits(bytes, lo - start, datum >> (lo - u * unit_bits), hi - lo);
    }
    ok = ok && bits_value(bytes, f->field.bit_length, out);
    kfree(bytes);
    return ok;
}

bool aml_field_write(struct aml_node *f, const struct aml_value *v)
{
    if (!f || f->type != AML_FIELD || f->field.bit_length == 0 ||
        f->field.bit_length > FIELD_LARGEST)
        return false;
    uint32_t width = access_bytes(f), unit_bits = 8 * width;
    uint32_t start = f->field.bit_offset, end = start + f->field.bit_length;
    uint8_t *bytes = kzalloc((f->field.bit_length + 7) / 8 + 1);
    if (!bytes)
        return false;
    bool ok = value_bits(v, bytes, f->field.bit_length);
    int update = f->field.flags >> 5 & 3;
    for (uint32_t u = start / unit_bits; ok && u * unit_bits < end; u++) {
        uint32_t lo = start > u * unit_bits ? start : u * unit_bits;
        uint32_t hi = end < (u + 1) * unit_bits ? end : (u + 1) * unit_bits;
        uint32_t shift = lo - u * unit_bits, count = hi - lo;
        uint64_t mask = (count == 64 ? ~(uint64_t)0 : (((uint64_t)1 << count) - 1)) << shift;
        uint64_t whole = unit_bits == 64 ? ~(uint64_t)0 : ((uint64_t)1 << unit_bits) - 1;
        uint64_t datum = 0;
        if (mask != whole) {
            if (update == 0)            /* Preserve: the other bits as they are */
                ok = unit_read(f, u * width, width, &datum);
            else if (update == 1)       /* WriteAsOnes */
                datum = whole;
        }
        datum = (datum & ~mask) | ((get_bits(bytes, lo - start, count) << shift) & mask);
        ok = ok && unit_write(f, u * width, width, datum);
    }
    kfree(bytes);
    return ok;
}

bool aml_buffer_field_read(struct aml_node *f, struct aml_value *out)
{
    struct aml_heap *h = f->buffer_field.buffer;
    uint32_t at = f->buffer_field.bit_offset, bits = f->buffer_field.bit_length;
    if (!h || (uint64_t)at + bits > (uint64_t)h->length * 8 || bits > FIELD_LARGEST) {
        aml_fail("a buffer field past its buffer's end");
        return false;
    }
    uint8_t *bytes = kzalloc((bits + 7) / 8 + 1);
    if (!bytes)
        return false;
    for (uint32_t i = 0; i < bits; i += 64) {
        uint32_t n = bits - i < 64 ? bits - i : 64;
        put_bits(bytes, i, get_bits(h->bytes, at + i, n), n);
    }
    bool ok = bits_value(bytes, bits, out);
    kfree(bytes);
    return ok;
}

bool aml_buffer_field_write(struct aml_node *f, const struct aml_value *v)
{
    struct aml_heap *h = f->buffer_field.buffer;
    uint32_t at = f->buffer_field.bit_offset, bits = f->buffer_field.bit_length;
    if (!h || (uint64_t)at + bits > (uint64_t)h->length * 8 || bits > FIELD_LARGEST) {
        aml_fail("a buffer field past its buffer's end");
        return false;
    }
    uint8_t *bytes = kzalloc((bits + 7) / 8 + 1);
    if (!bytes)
        return false;
    bool ok = value_bits(v, bytes, bits);
    for (uint32_t i = 0; ok && i < bits; i += 64) {
        uint32_t n = bits - i < 64 ? bits - i : 64;
        put_bits(h->bytes, at + i, get_bits(bytes, i, n), n);
    }
    kfree(bytes);
    return ok;
}

/* ---- A reference's target ---- */

static bool node_value(struct aml_node *n, struct aml_value *out)
{
    n = aml_alias_target(n);
    if (!n)
        return false;
    switch (n->type) {
    case AML_UNINIT:
    case AML_INTEGER:
    case AML_STRING:
    case AML_BUFFER:
    case AML_PACKAGE:
    case AML_REFERENCE:
        aml_copy(out, &n->value);
        return true;
    case AML_FIELD:
        return aml_field_read(n, out);
    case AML_BUFFER_FIELD:
        return aml_buffer_field_read(n, out);
    case AML_METHOD:
        return aml_run_method(n, 0, 0, out, 0);
    }
    /* devices, mutexes...: the object itself */
    aml_release(out);
    out->type = AML_REFERENCE;
    out->ref = REF_NODE;
    out->node = n;
    return true;
}

static bool referred(const struct aml_value *v, struct aml_value *out)
{
    switch (v->ref) {
    case REF_NODE:
        return node_value(v->node, out);
    case REF_NAME: {
        struct aml_node *n = name_now(v);
        if (!n) {
            aml_fail("%s: a name in a package that is not there",
                     (const char *)v->heap->bytes);
            return false;
        }
        return node_value(n, out);
    }
    case REF_SLOT:
        aml_copy(out, v->slot);
        return true;
    case REF_INDEX: {
        struct aml_heap *h = v->heap;
        if (v->index >= h->length) {
            aml_fail("index %u past the end (%u)", v->index, h->length);
            return false;
        }
        if (h->type == AML_PACKAGE)
            aml_copy(out, &h->elements[v->index]);
        else
            aml_set_integer(out, h->bytes[v->index]);
        return true;
    }
    }
    aml_fail("a reference to nothing used");
    return false;
}

bool aml_value_of(struct aml_node *n, struct aml_value *out)
{
    return node_value(n, out);
}

bool aml_deref(const struct aml_value *v, struct aml_value *out)
{
    return referred(v, out);
}

/* ---- Evaluation from outside ---- */

bool aml_evaluate(struct aml_node *node, const struct aml_value *args, int nargs,
                  struct aml_value *out)
{
    node = aml_alias_target(node);
    if (!node)
        return false;
    if (node->type == AML_METHOD)
        return aml_run_method(node, args, nargs, out, 0);
    return node_value(node, out);
}

/* ---- _REG, devices by ID ---- */

static void run_reg(struct aml_node *reg, uint8_t space)
{
    struct aml_value args[2] = {{0}, {0}}, out = {0};
    aml_set_integer(&args[0], space);
    aml_set_integer(&args[1], 1);
    aml_run_method(reg, args, 2, &out, 0);
    aml_release(&out);
}

static void regs_in(struct aml_node *scope, uint8_t space, int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        if (n->type == AML_REGION && n->region.space == space && !n->region.connected) {
            n->region.connected = true;
            struct aml_node *reg = aml_alias_target(aml_child(n->parent, "_REG"));
            if (reg && reg->type == AML_METHOD)
                run_reg(reg, space);
        }
        regs_in(n, space, depth + 1);
    }
}

void aml_run_regs(struct aml_node *scope, uint8_t space)
{
    if (!scope || space == 0 || space == 1 || space == 0x7E)
        return;
    regs_in(scope, space, 0);
    if (space != 3 && space != 8)
        return;
    struct aml_node *reg = aml_alias_target(aml_child(scope, "_REG"));
    if (!reg || reg->type != AML_METHOD)
        return;
    for (struct aml_node *n = scope->child; n; n = n->next)
        if (n->type == AML_REGION && n->region.space == space)
            return;
    run_reg(reg, space);
}

/* An ID as text: a string, or an EISA ID (compressed letters, then four
 * hex digits, its bytes in the other order). */
static bool id_text(const struct aml_value *v, char *text)
{
    if (v->type == AML_STRING) {
        uint32_t n = v->heap->length < 15 ? v->heap->length : 15;
        memcpy(text, v->heap->bytes, n);
        text[n] = 0;
        return true;
    }
    if (v->type != AML_INTEGER)
        return false;
    uint32_t x = (uint32_t)v->integer;
    x = (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
    text[0] = (char)(((x >> 26) & 0x1F) + 0x40);
    text[1] = (char)(((x >> 21) & 0x1F) + 0x40);
    text[2] = (char)(((x >> 16) & 0x1F) + 0x40);
    for (int i = 0; i < 4; i++)
        text[3 + i] = "0123456789ABCDEF"[(x >> (12 - 4 * i)) & 0xF];
    text[7] = 0;
    return true;
}

static bool has_id(struct aml_node *dev, const char *name, const char *id)
{
    struct aml_node *n = aml_child(dev, name);
    struct aml_value v = {0};
    if (!n || !aml_evaluate(n, 0, 0, &v))
        return false;
    char text[16];
    bool yes = false;
    if (v.type == AML_PACKAGE) {
        for (uint32_t i = 0; i < v.heap->length && !yes; i++)
            yes = id_text(&v.heap->elements[i], text) && !strcmp(text, id);
    } else {
        yes = id_text(&v, text) && !strcmp(text, id);
    }
    aml_release(&v);
    return yes;
}

static uint64_t integer_of(struct aml_node *scope, const char *name, uint64_t otherwise)
{
    struct aml_node *n = aml_child(scope, name);
    struct aml_value v = {0};
    uint64_t x = otherwise;
    if (n && (!aml_evaluate(n, 0, 0, &v) || !aml_to_integer(&v, &x, false)))
        x = otherwise;
    aml_release(&v);
    return x;
}

static bool is_root_bridge(struct aml_node *dev)
{
    return has_id(dev, "_HID", "PNP0A03") || has_id(dev, "_HID", "PNP0A08") ||
           has_id(dev, "_CID", "PNP0A03") || has_id(dev, "_CID", "PNP0A08");
}

/* _STA's bits 0 (present) or 3 (working); no _STA: present. */
static bool present(struct aml_node *dev)
{
    struct aml_node *n = aml_child(dev, "_STA");
    struct aml_value v = {0};
    uint64_t sta = 0xF;
    if (n && aml_evaluate(n, 0, 0, &v) && !aml_to_integer(&v, &sta, false))
        sta = 0;
    aml_release(&v);
    return sta & 9;
}

static int devices_in(struct aml_node *scope, const char *id, struct aml_node **out, int max,
                      int found, int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        if (n->type == AML_DEVICE && (!id || has_id(n, "_HID", id) || has_id(n, "_CID", id))) {
            if (!present(n))
                continue;               /* (nor its children) */
            if (found < max)
                out[found] = n;
            found++;
        }
        found = devices_in(n, id, out, max, found, depth + 1);
    }
    return found;
}

int aml_find_devices(const char *id, struct aml_node **out, int max)
{
    if (!aml_root)
        return 0;
    int n = devices_in(aml_root, id, out, max, 0, 0);
    return n < max ? n : max;
}

/* (37d) Processor objects and processor devices (ACPI0007), present. */
static int processors_in(struct aml_node *scope, struct aml_node **out, int max, int found,
                         int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        bool processor = n->type == AML_PROCESSOR ||
                         (n->type == AML_DEVICE && has_id(n, "_HID", "ACPI0007"));
        if ((n->type == AML_PROCESSOR || n->type == AML_DEVICE) && !present(n))
            continue;                   /* (nor its children) */
        if (processor) {
            if (found < max)
                out[found] = n;
            found++;
        }
        found = processors_in(n, out, max, found, depth + 1);
    }
    return found;
}

int aml_find_processors(struct aml_node **out, int max)
{
    if (!aml_root)
        return 0;
    int n = processors_in(aml_root, out, max, 0, 0);
    return n < max ? n : max;
}

/* ---- (37b-4) The boot's _INI ---- */

/* Does scope's subtree have an _INI (ACPICA walks only those)? */
static bool has_ini(struct aml_node *scope, int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next)
        if (!memcmp(n->name, "_INI", 4) || has_ini(n, depth + 1))
            return true;
    return false;
}

static void run_ini(struct aml_node *scope, struct aml_init *c)
{
    struct aml_node *ini = aml_alias_target(aml_child(scope, "_INI"));
    if (!ini || ini->type != AML_METHOD)
        return;
    struct aml_value out = {0};
    c->ini++;
    if (!aml_run_method(ini, 0, 0, &out, 0))
        c->failed++;
    aml_release(&out);
}

static void init_tree(struct aml_node *scope, struct aml_init *c, int depth)
{
    for (struct aml_node *n = scope->child; n && depth < 60; n = n->next) {
        if (n->type != AML_DEVICE && n->type != AML_PROCESSOR && n->type != AML_THERMAL) {
            init_tree(n, c, depth + 1);
            continue;
        }
        if (!has_ini(n, 0))
            continue;
        uint64_t sta = 0xF;
        struct aml_node *s = aml_alias_target(aml_child(n, "_STA"));
        if (s) {
            struct aml_value v = {0};
            c->sta++;
            bool ok = aml_evaluate(s, 0, 0, &v) && aml_to_integer(&v, &sta, false);
            aml_release(&v);
            if (!ok) {                  /* (ACPICA: the error ignored, its children looked at) */
                c->failed++;
                init_tree(n, c, depth + 1);
                continue;
            }
        }
        if (!(sta & 1)) {
            c->absent++;
            if (sta & 8)                /* not present but working: its children */
                init_tree(n, c, depth + 1);
            continue;
        }
        if (!(n->parent == aml_root && !memcmp(n->name, "_SB_", 4)))
            run_ini(n, c);
        init_tree(n, c, depth + 1);
    }
}

void aml_initialize(struct aml_init *c)
{
    memset(c, 0, sizeof(*c));
    if (!aml_root)
        return;
    run_ini(aml_root, c);
    struct aml_node *sb = aml_child(aml_root, "_SB_");
    if (sb)
        run_ini(sb, c);
    aml_run_regs(aml_root, 2);
    init_tree(aml_root, c, 0);
}

/* ---- \_OSI and the predefined names ---- */

static const char *const osi_names[] = {
    "Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001.1",
    "Windows 2001 SP2", "Windows 2001.1 SP1", "Windows 2006", "Windows 2006.1",
    "Windows 2006 SP1", "Windows 2006 SP2", "Windows 2009", "Windows 2012",
    "Windows 2013", "Windows 2015", "Windows 2016", "Windows 2017", "Windows 2017.2",
    "Windows 2018", "Windows 2018.2", "Windows 2019", "Windows 2020", "Windows 2021",
    "Windows 2022",
    "Extended Address Space Descriptor", "Module Device", "Processor Device",
    "3.0 Thermal Model", "3.0 _SCP Extensions", "Processor Aggregator Device",
};

#define OSI_COUNT (sizeof(osi_names) / sizeof(osi_names[0]))
static bool osi_dropped[OSI_COUNT];

void aml_osi_drop(const char *name)
{
    for (size_t i = 0; i < OSI_COUNT; i++)
        if (!strcmp(name, osi_names[i]))
            osi_dropped[i] = true;
}

static bool osi(const struct aml_value *args, struct aml_value *out)
{
    bool yes = false;
    if (args[0].type == AML_STRING)
        for (size_t i = 0; i < OSI_COUNT; i++)
            if (!osi_dropped[i] && !strcmp((const char *)args[0].heap->bytes, osi_names[i]))
                yes = true;
    aml_set_integer(out, yes ? aml_ones : 0);
    return true;
}

void aml_init(const struct aml_os *os)
{
    aml_machine = os;
    if (aml_root)
        return;
    aml_root = kzalloc(sizeof(*aml_root));
    if (!aml_root)
        return;
    memcpy(aml_root->name, "\\___", 4);
    aml_root->type = AML_SCOPE;
    aml_node_new(aml_root, "_GPE", AML_SCOPE);
    aml_node_new(aml_root, "_PR_", AML_SCOPE);
    aml_node_new(aml_root, "_SB_", AML_DEVICE);
    aml_node_new(aml_root, "_SI_", AML_SCOPE);
    aml_node_new(aml_root, "_TZ_", AML_DEVICE);
    struct aml_node *n = aml_node_new(aml_root, "_REV", AML_INTEGER);
    if (n)
        aml_set_integer(&n->value, 2);
    n = aml_node_new(aml_root, "_OS_", AML_STRING);
    if (n)
        aml_set_string(&n->value, "Microsoft Windows NT", 20);
    aml_node_new(aml_root, "_GL_", AML_MUTEX);
    n = aml_node_new(aml_root, "_OSI", AML_METHOD);
    if (n) {
        n->method.args = 1;
        n->method.native = osi;
    }
}

/* ---- The names' listing ---- */

static const char *type_name(const struct aml_node *n)
{
    switch (n->type) {
    case AML_UNINIT: return "Untyped";
    case AML_INTEGER: return "Integer";
    case AML_STRING: return "String";
    case AML_BUFFER: return "Buffer";
    case AML_PACKAGE: return "Package";
    case AML_FIELD:
        return n->field.kind == AML_INDEX_FIELD ? "IndexField"
             : n->field.kind == AML_BANK_FIELD ? "BankField" : "RegionField";
    case AML_DEVICE: return "Device";
    case AML_EVENT: return "Event";
    case AML_METHOD: return "Method";
    case AML_MUTEX: return "Mutex";
    case AML_REGION: return "Region";
    case AML_POWER: return "Power";
    case AML_PROCESSOR: return "Processor";
    case AML_THERMAL: return "Thermal";
    case AML_BUFFER_FIELD: return "BufferField";
    case AML_SCOPE: return "Scope";
    case AML_ALIAS: return "Alias";
    case AML_REFERENCE: return "Reference";
    }
    return "?";
}

static void print_node(const struct aml_node *n, void (*print)(const char *line))
{
    char line[200];
    char path[120];
    aml_path(n, path, sizeof(path));
    int at = ksnprintf(line, sizeof(line), "%s %s", path, type_name(n));
    if (at < 0 || at >= (int)sizeof(line))
        at = (int)sizeof(line) - 1;
    char *rest = line + at;
    size_t room = sizeof(line) - (size_t)at;
    switch (n->type) {
    case AML_INTEGER:
        ksnprintf(rest, room, " 0x%lX", (unsigned long)n->value.integer);
        break;
    case AML_STRING: {
        char text[48];
        uint32_t k = 0;
        for (uint32_t i = 0; i < n->value.heap->length && k < sizeof(text) - 1; i++) {
            uint8_t c = n->value.heap->bytes[i];
            text[k++] = c >= 32 && c < 127 && c != '"' ? (char)c : '.';
        }
        text[k] = 0;
        ksnprintf(rest, room, " length %u \"%s\"", n->value.heap->length, text);
        break;
    }
    case AML_BUFFER:
        ksnprintf(rest, room, " length %u", n->value.heap->length);
        break;
    case AML_PACKAGE:
        ksnprintf(rest, room, " elements %u", n->value.heap->length);
        break;
    case AML_METHOD:
        ksnprintf(rest, room, " args %u%s", (unsigned)n->method.args,
                  n->method.flags & 8 ? " serialized" : "");
        break;
    case AML_REGION:
        ksnprintf(rest, room, " space %u offset 0x%lX length 0x%lX", (unsigned)n->region.space,
                  (unsigned long)n->region.offset, (unsigned long)n->region.length);
        break;
    case AML_FIELD:
        ksnprintf(rest, room, " bits %u+%u access %u", n->field.bit_offset, n->field.bit_length,
                  (unsigned)(n->field.flags & 0xF));
        break;
    case AML_BUFFER_FIELD:
        ksnprintf(rest, room, " bits %u+%u", n->buffer_field.bit_offset,
                  n->buffer_field.bit_length);
        break;
    case AML_PROCESSOR:
        ksnprintf(rest, room, " id %u", (unsigned)n->processor.id);
        break;
    case AML_POWER:
        ksnprintf(rest, room, " level %u order %u", (unsigned)n->power.level,
                  (unsigned)n->power.order);
        break;
    case AML_MUTEX:
        ksnprintf(rest, room, " sync %u", (unsigned)n->mutex.sync);
        break;
    case AML_ALIAS: {
        char target[120];
        if (n->alias)
            aml_path(n->alias, target, sizeof(target));
        else
            target[0] = 0;
        ksnprintf(rest, room, " %s", target);
        break;
    }
    }
    print(line);
}

static void print_tree(const struct aml_node *scope, void (*print)(const char *line), int depth)
{
    if (depth > 60)
        return;
    for (const struct aml_node *n = scope->child; n; n = n->next) {
        print_node(n, print);
        print_tree(n, print, depth + 1);
    }
}

void aml_print_names(void (*print)(const char *line))
{
    if (aml_root)
        print_tree(aml_root, print, 0);
}

/* ---- (37b-3) Values as text ---- */

#define VALUE_DEPTH 8                   /* packages in packages printed */

static void value_lines(const struct aml_value *v, int depth, const char *first,
                        void (*print)(const char *line))
{
    char line[200];
    int at = ksnprintf(line, sizeof(line), "%*s%s", 2 * depth, "", first);
    if (at < 0 || at >= (int)sizeof(line))
        at = (int)sizeof(line) - 1;
    char *rest = line + at;
    size_t room = sizeof(line) - (size_t)at;
    switch (v->type) {
    case AML_UNINIT:
        ksnprintf(rest, room, "Uninitialized");
        print(line);
        return;
    case AML_INTEGER:
        ksnprintf(rest, room, "Integer 0x%lX", (unsigned long)v->integer);
        print(line);
        return;
    case AML_STRING: {
        char text[120];
        uint32_t k = 0;
        for (uint32_t i = 0; i < v->heap->length && k < sizeof(text) - 1; i++) {
            uint8_t c = v->heap->bytes[i];
            text[k++] = c >= 32 && c < 127 && c != '"' ? (char)c : '.';
        }
        text[k] = 0;
        ksnprintf(rest, room, "String length %u \"%s\"", v->heap->length, text);
        print(line);
        return;
    }
    case AML_BUFFER: {
        ksnprintf(rest, room, "Buffer %u bytes", v->heap->length);
        print(line);
        for (uint32_t i = 0; i < v->heap->length; i += 16) {
            int n = ksnprintf(line, sizeof(line), "%*s%04X:", 2 * depth + 2, "", i);
            for (uint32_t k = i; k < i + 16 && k < v->heap->length && n < (int)sizeof(line) - 4; k++)
                n += ksnprintf(line + n, sizeof(line) - (size_t)n, " %02X",
                               (unsigned)v->heap->bytes[k]);
            print(line);
        }
        return;
    }
    case AML_PACKAGE:
        ksnprintf(rest, room, "Package %u elements", v->heap->length);
        print(line);
        if (depth >= VALUE_DEPTH)
            return;
        for (uint32_t i = 0; i < v->heap->length; i++)
            value_lines(&v->heap->elements[i], depth + 1, "", print);
        return;
    case AML_REFERENCE:
        if (v->ref == REF_NODE) {
            char path[120];
            aml_path(v->node, path, sizeof(path));
            ksnprintf(rest, room, "Reference %s %s", path, type_name(v->node));
        } else if (v->ref == REF_INDEX) {
            ksnprintf(rest, room, "Reference to element %u", v->index);
        } else if (v->ref == REF_NAME) {
            ksnprintf(rest, room, "Reference to %s (not there)", (const char *)v->heap->bytes);
        } else if (v->ref == REF_SLOT) {
            ksnprintf(rest, room, "Reference to a local or argument");
        } else {
            ksnprintf(rest, room, "Reference to Debug");
        }
        print(line);
        return;
    }
    ksnprintf(rest, room, "type %u", (unsigned)v->type);
    print(line);
}

void aml_print_value(const struct aml_value *v, const char *first,
                     void (*print)(const char *line))
{
    value_lines(v, 0, first, print);
}
