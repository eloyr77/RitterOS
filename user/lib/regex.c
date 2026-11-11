/*
 * Regular expressions (58a, regex.h). regcomp parses the pattern (basic or
 * extended syntax) into a tree and compiles it into a small program for a
 * virtual machine: characters and sets to match, anchors, SPLIT (try one
 * way, else the other) and JMP, SAVE (a group's start or end), back
 * references. regexec runs the program in up to two passes:
 *
 *   1. Without back references, a Thompson simulation (every way at once,
 *      one step per character of the string) finds the leftmost-longest
 *      match in time proportional to the string times the program.
 *   2. The groups: a backtracking walk that must end exactly where pass 1
 *      ended, trying the greedy way first; a (place in the program,
 *      position) pair that failed once fails again, so it is remembered
 *      and the walk stays linear too (also for loops that match nothing).
 *
 * With back references (\1) the match depends on what groups took, so a
 * backtracking search tries every way from each start for the longest
 * one, with a step limit (REG_ESPACE beyond it).
 */
#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { I_CHAR, I_ANY, I_SET, I_BOL, I_EOL, I_SPLIT, I_JMP, I_SAVE, I_BREF, I_MARK, I_CHECK,
       I_MATCH };

struct inst {
    uint8_t op;
    int x, y;               /* CHAR: the byte; SET: the set; SPLIT: preferred, other; JMP: where;
                             * SAVE: the slot; BREF: the group; MARK, CHECK: the loop's mark,
                             * CHECK's y the loop's exit: a pass that took nothing ends the
                             * loop (as glibc and the BSDs: (a*)* takes "" once) */
};

struct program {
    struct inst *code;
    int length, room;
    uint8_t (*sets)[32];
    int set_count, set_room;
    int groups, marks;
    bool backrefs;
    int flags;
};

/* --- parsing ------------------------------------------------------------------ */

enum { N_EMPTY, N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_CAT, N_ALT, N_REP, N_GROUP, N_BREF };

struct node {
    int kind;
    int value;              /* CHAR: the byte; SET: the set; GROUP, BREF: the group number */
    int min, max;           /* REP: max -1 for no limit */
    struct node *a, *b;
};

struct parser {
    const char *p, *end;
    struct program *prog;
    bool ere;
    int error;
    int open_groups[64], depth;
};

#define CODE_MAX 200000             /* instructions, after {m,n} copies */

static struct node *node(struct parser *ps, int kind, struct node *a, struct node *b)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n) {
        ps->error = REG_ESPACE;
        return 0;
    }
    n->kind = kind;
    n->a = a;
    n->b = b;
    return n;
}

static void free_tree(struct node *n)
{
    if (!n)
        return;
    free_tree(n->a);
    free_tree(n->b);
    free(n);
}

static int new_set(struct parser *ps)
{
    struct program *g = ps->prog;
    if (g->set_count == g->set_room) {
        int room = g->set_room ? g->set_room * 2 : 8;
        uint8_t (*more)[32] = realloc(g->sets, (size_t)room * 32);
        if (!more) {
            ps->error = REG_ESPACE;
            return -1;
        }
        g->sets = more;
        g->set_room = room;
    }
    memset(g->sets[g->set_count], 0, 32);
    return g->set_count++;
}

static void set_add(struct parser *ps, int set, int c)
{
    uint8_t *s = ps->prog->sets[set];
    s[c / 8] |= (uint8_t)(1u << (c % 8));
    if (ps->prog->flags & REG_ICASE) {
        int l = tolower(c), u = toupper(c);
        s[l / 8] |= (uint8_t)(1u << (l % 8));
        s[u / 8] |= (uint8_t)(1u << (u % 8));
    }
}

static bool in_class(const char *name, size_t len, int c)
{
#define CLASS(n, test) if (len == sizeof(n) - 1 && !memcmp(name, n, len)) return test
    CLASS("alpha", isalpha(c));
    CLASS("digit", isdigit(c));
    CLASS("alnum", isalnum(c));
    CLASS("upper", isupper(c));
    CLASS("lower", islower(c));
    CLASS("space", isspace(c));
    CLASS("blank", c == ' ' || c == '\t');
    CLASS("punct", ispunct(c));
    CLASS("print", isprint(c));
    CLASS("graph", isgraph(c));
    CLASS("cntrl", iscntrl(c));
    CLASS("xdigit", isxdigit(c));
#undef CLASS
    return false;
}

static bool known_class(const char *name, size_t len)
{
    static const char *const names[] = { "alpha", "digit", "alnum", "upper", "lower", "space",
                                         "blank", "punct", "print", "graph", "cntrl", "xdigit" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i]) == len && !memcmp(names[i], name, len))
            return true;
    return false;
}

/* One element of a bracket: [.c.], [=c=] or a byte; -1 after an error. */
static int bracket_char(struct parser *ps)
{
    if (ps->p + 1 < ps->end && ps->p[0] == '[' && (ps->p[1] == '.' || ps->p[1] == '=')) {
        char kind = ps->p[1];
        const char *s = ps->p + 2, *e = s;
        while (e + 1 < ps->end && !(e[0] == kind && e[1] == ']'))
            e++;
        if (e + 1 >= ps->end) {
            ps->error = REG_EBRACK;
            return -1;
        }
        if (e - s != 1) {
            ps->error = REG_ECOLLATE;           /* (the "C" locale: single bytes only) */
            return -1;
        }
        ps->p = e + 2;
        return (unsigned char)*s;
    }
    return (unsigned char)*ps->p++;
}

/* After '[': a set. */
static struct node *bracket(struct parser *ps)
{
    int set = new_set(ps);
    if (set < 0)
        return 0;
    bool negate = ps->p < ps->end && *ps->p == '^';
    if (negate)
        ps->p++;
    bool first = true;
    for (;;) {
        if (ps->p >= ps->end) {
            ps->error = REG_EBRACK;
            return 0;
        }
        if (*ps->p == ']' && !first) {
            ps->p++;
            break;
        }
        first = false;
        if (ps->p + 1 < ps->end && ps->p[0] == '[' && ps->p[1] == ':') {
            const char *s = ps->p + 2, *e = s;
            while (e + 1 < ps->end && !(e[0] == ':' && e[1] == ']'))
                e++;
            if (e + 1 >= ps->end) {
                ps->error = REG_EBRACK;
                return 0;
            }
            if (!known_class(s, (size_t)(e - s))) {
                ps->error = REG_ECTYPE;
                return 0;
            }
            for (int c = 1; c < 256; c++)
                if (in_class(s, (size_t)(e - s), c))
                    set_add(ps, set, c);
            ps->p = e + 2;
            continue;
        }
        bool equivalence = ps->p + 1 < ps->end && ps->p[0] == '[' && ps->p[1] == '=';
        int lo = bracket_char(ps);
        if (lo < 0)
            return 0;
        int hi = lo;
        if (!equivalence && ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] != ']') {
            ps->p++;
            if (ps->p + 1 < ps->end && ps->p[0] == '[' && ps->p[1] == ':') {
                ps->error = REG_ERANGE;
                return 0;
            }
            hi = bracket_char(ps);
            if (hi < 0)
                return 0;
            if (hi < lo) {
                ps->error = REG_ERANGE;
                return 0;
            }
        }
        for (int c = lo; c <= hi; c++)
            set_add(ps, set, c);
    }
    uint8_t *s = ps->prog->sets[set];
    if (negate) {
        for (int i = 0; i < 32; i++)
            s[i] = (uint8_t)~s[i];
        if (ps->prog->flags & REG_NEWLINE)
            s['\n' / 8] &= (uint8_t)~(1u << ('\n' % 8));
    }
    s[0] &= (uint8_t)~1u;                       /* (never the string's end, NUL) */
    struct node *n = node(ps, N_SET, 0, 0);
    if (n)
        n->value = set;
    return n;
}

/* {m,n} or \{m,n\} after its opening; false after an error. */
static bool interval(struct parser *ps, int *min, int *max)
{
    int m = -1, n;
    if (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
        m = 0;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
            m = m * 10 + (*ps->p++ - '0');
            if (m > RE_DUP_MAX)
                m = RE_DUP_MAX + 1;
        }
    }
    if (m < 0) {
        ps->error = ps->p >= ps->end ? REG_EBRACE : REG_BADBR;
        return false;
    }
    n = m;
    if (ps->p < ps->end && *ps->p == ',') {
        ps->p++;
        n = -1;
        if (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
            n = 0;
            while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
                n = n * 10 + (*ps->p++ - '0');
                if (n > RE_DUP_MAX)
                    n = RE_DUP_MAX + 1;
            }
        }
    }
    bool closed = ps->ere ? ps->p < ps->end && *ps->p == '}'
                          : ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == '}';
    if (!closed) {
        ps->error = ps->p >= ps->end ? REG_EBRACE : REG_BADBR;
        return false;
    }
    ps->p += ps->ere ? 1 : 2;
    if (m > RE_DUP_MAX || n > RE_DUP_MAX || (n >= 0 && n < m)) {
        ps->error = REG_BADBR;
        return false;
    }
    *min = m;
    *max = n;
    return true;
}

static struct node *repeat(struct parser *ps, struct node *atom, int min, int max)
{
    struct node *n = node(ps, N_REP, atom, 0);
    if (!n) {
        free_tree(atom);
        return 0;
    }
    n->min = min;
    n->max = max;
    return n;
}

static struct node *cat(struct node *a, struct node *b, struct parser *ps)
{
    if (!a)
        return b;
    if (!b)
        return a;
    struct node *n = node(ps, N_CAT, a, b);
    if (!n) {
        free_tree(a);
        free_tree(b);
    }
    return n;
}

static struct node *ere_alternation(struct parser *ps);

/* An ERE atom with its repetitions; 0 for none (or an error). */
static struct node *ere_piece(struct parser *ps)
{
    struct node *atom = 0;
    char c = *ps->p;
    if (c == '*' || c == '+' || c == '?' || c == '{') {
        ps->error = REG_BADRPT;
        return 0;
    }
    ps->p++;
    if (c == '(') {
        int group = ++ps->prog->groups;
        struct node *inner = ere_alternation(ps);
        if (ps->error) {
            free_tree(inner);
            return 0;
        }
        if (ps->p >= ps->end || *ps->p != ')') {
            free_tree(inner);
            ps->error = REG_EPAREN;
            return 0;
        }
        ps->p++;
        atom = node(ps, N_GROUP, inner, 0);
        if (atom)
            atom->value = group;
    } else if (c == '.') {
        atom = node(ps, N_ANY, 0, 0);
    } else if (c == '[') {
        atom = bracket(ps);
    } else if (c == '^') {
        atom = node(ps, N_BOL, 0, 0);
    } else if (c == '$') {
        atom = node(ps, N_EOL, 0, 0);
    } else if (c == '\\') {
        if (ps->p >= ps->end) {
            ps->error = REG_EESCAPE;
            return 0;
        }
        char e = *ps->p++;
        if (e >= '1' && e <= '9') {
            if (e - '0' > ps->prog->groups) {
                ps->error = REG_ESUBREG;
                return 0;
            }
            atom = node(ps, N_BREF, 0, 0);
            if (atom)
                atom->value = e - '0';
            ps->prog->backrefs = true;
        } else {
            atom = node(ps, N_CHAR, 0, 0);
            if (atom)
                atom->value = (unsigned char)e;
        }
    } else {
        atom = node(ps, N_CHAR, 0, 0);
        if (atom)
            atom->value = (unsigned char)c;
    }
    if (!atom)
        return 0;
    while (ps->p < ps->end) {
        char r = *ps->p;
        int min, max;
        if (r == '*')
            min = 0, max = -1;
        else if (r == '+')
            min = 1, max = -1;
        else if (r == '?')
            min = 0, max = 1;
        else if (r == '{') {
            ps->p++;
            if (!interval(ps, &min, &max)) {
                free_tree(atom);
                return 0;
            }
            if (!(atom = repeat(ps, atom, min, max)))
                return 0;
            continue;
        } else
            break;
        ps->p++;
        if (!(atom = repeat(ps, atom, min, max)))
            return 0;
    }
    return atom;
}

static struct node *ere_branch(struct parser *ps)
{
    struct node *all = node(ps, N_EMPTY, 0, 0);
    while (all && !ps->error && ps->p < ps->end && *ps->p != '|' && *ps->p != ')') {
        struct node *piece = ere_piece(ps);
        if (ps->error) {
            free_tree(piece);
            break;
        }
        all = cat(all, piece, ps);
    }
    return all;
}

static struct node *ere_alternation(struct parser *ps)
{
    struct node *all = ere_branch(ps);
    while (!ps->error && ps->p < ps->end && *ps->p == '|') {
        ps->p++;
        struct node *other = ere_branch(ps);
        if (ps->error) {
            free_tree(other);
            break;
        }
        struct node *alt = node(ps, N_ALT, all, other);
        if (!alt) {
            free_tree(other);
            break;
        }
        all = alt;
    }
    return all;
}

/* A BRE up to its end or \); first: at the start of the RE or a group. */
static struct node *bre_sequence(struct parser *ps)
{
    struct node *all = node(ps, N_EMPTY, 0, 0);
    bool first = true;
    while (all && !ps->error && ps->p < ps->end) {
        if (ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == ')')
            break;
        struct node *atom = 0;
        char c = *ps->p++;
        if (c == '*' && first) {
            atom = node(ps, N_CHAR, 0, 0);          /* (a * at the start is itself) */
            if (atom)
                atom->value = '*';
        } else if (c == '^' && first) {
            atom = node(ps, N_BOL, 0, 0);
            all = cat(all, atom, ps);
            continue;                               /* (still the start for a following *) */
        } else if (c == '$' && (ps->p >= ps->end ||
                                (ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == ')'))) {
            atom = node(ps, N_EOL, 0, 0);
        } else if (c == '.') {
            atom = node(ps, N_ANY, 0, 0);
        } else if (c == '[') {
            atom = bracket(ps);
        } else if (c == '\\') {
            if (ps->p >= ps->end) {
                ps->error = REG_EESCAPE;
                break;
            }
            char e = *ps->p++;
            if (e == '(') {
                int group = ++ps->prog->groups;
                struct node *inner = bre_sequence(ps);
                if (ps->error) {
                    free_tree(inner);
                    break;
                }
                if (!(ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == ')')) {
                    free_tree(inner);
                    ps->error = REG_EPAREN;
                    break;
                }
                ps->p += 2;
                atom = node(ps, N_GROUP, inner, 0);
                if (atom)
                    atom->value = group;
            } else if (e == '{' && first) {
                ps->error = REG_BADRPT;
                break;
            } else if (e == '}' || e == ')') {
                ps->error = e == ')' ? REG_EPAREN : REG_EBRACE;
                break;
            } else if (e >= '1' && e <= '9') {
                if (e - '0' > ps->prog->groups) {
                    ps->error = REG_ESUBREG;
                    break;
                }
                atom = node(ps, N_BREF, 0, 0);
                if (atom)
                    atom->value = e - '0';
                ps->prog->backrefs = true;
            } else {
                atom = node(ps, N_CHAR, 0, 0);
                if (atom)
                    atom->value = (unsigned char)e;
            }
        } else {
            atom = node(ps, N_CHAR, 0, 0);
            if (atom)
                atom->value = (unsigned char)c;
        }
        if (!atom || ps->error) {
            free_tree(atom);
            break;
        }
        first = false;
        for (;;) {
            int min, max;
            if (ps->p < ps->end && *ps->p == '*') {
                ps->p++;
                min = 0, max = -1;
            } else if (ps->p + 1 < ps->end && ps->p[0] == '\\' && ps->p[1] == '{') {
                ps->p += 2;
                if (!interval(ps, &min, &max)) {
                    free_tree(atom);
                    atom = 0;
                    break;
                }
            } else {
                break;
            }
            if (!(atom = repeat(ps, atom, min, max)))
                break;
        }
        if (!atom)
            break;
        all = cat(all, atom, ps);
    }
    return all;
}

/* --- compiling ------------------------------------------------------------------ */

static int emit(struct parser *ps, int op, int x, int y)
{
    struct program *g = ps->prog;
    if (ps->error)
        return -1;
    if (g->length == g->room) {
        if (g->room >= CODE_MAX) {
            ps->error = REG_ESPACE;
            return -1;
        }
        int room = g->room ? g->room * 2 : 64;
        struct inst *more = realloc(g->code, (size_t)room * sizeof(*more));
        if (!more) {
            ps->error = REG_ESPACE;
            return -1;
        }
        g->code = more;
        g->room = room;
    }
    g->code[g->length] = (struct inst){ (uint8_t)op, x, y };
    return g->length++;
}

static void compile(struct parser *ps, const struct node *n)
{
    if (!n || ps->error)
        return;
    struct program *g = ps->prog;
    switch (n->kind) {
    case N_EMPTY:
        break;
    case N_CHAR:
        if ((g->flags & REG_ICASE) && isalpha(n->value)) {
            int set = new_set(ps);
            if (set >= 0) {
                set_add(ps, set, n->value);
                emit(ps, I_SET, set, 0);
            }
        } else {
            emit(ps, I_CHAR, n->value, 0);
        }
        break;
    case N_ANY:
        emit(ps, I_ANY, 0, 0);
        break;
    case N_SET:
        emit(ps, I_SET, n->value, 0);
        break;
    case N_BOL:
        emit(ps, I_BOL, 0, 0);
        break;
    case N_EOL:
        emit(ps, I_EOL, 0, 0);
        break;
    case N_CAT:
        compile(ps, n->a);
        compile(ps, n->b);
        break;
    case N_ALT: {
        int split = emit(ps, I_SPLIT, 0, 0);
        compile(ps, n->a);
        int jump = emit(ps, I_JMP, 0, 0);
        if (ps->error)
            return;
        g->code[split].x = split + 1;
        g->code[split].y = g->length;
        compile(ps, n->b);
        if (!ps->error)
            g->code[jump].x = g->length;
        break;
    }
    case N_GROUP:
        emit(ps, I_SAVE, 2 * n->value, 0);
        compile(ps, n->a);
        emit(ps, I_SAVE, 2 * n->value + 1, 0);
        break;
    case N_BREF:
        emit(ps, I_BREF, n->value, 0);
        break;
    case N_REP: {
        for (int i = 0; i < n->min; i++)
            compile(ps, n->a);
        if (n->max < 0) {                           /* a loop: L: SPLIT body, out */
            int mark = g->marks++;
            int loop = emit(ps, I_SPLIT, 0, 0);
            emit(ps, I_MARK, mark, 0);
            compile(ps, n->a);
            int check = emit(ps, I_CHECK, mark, 0);
            emit(ps, I_JMP, loop, 0);
            if (!ps->error) {
                g->code[loop].x = loop + 1;
                g->code[loop].y = g->length;
                g->code[check].y = g->length;       /* (a pass that took nothing: out) */
            }
        } else {                                    /* max - min copies, each one optional */
            int count = n->max - n->min, *splits = count ? malloc(sizeof(int) * (size_t)count) : 0;
            if (count && !splits) {
                ps->error = REG_ESPACE;
                return;
            }
            for (int i = 0; i < count; i++) {
                splits[i] = emit(ps, I_SPLIT, 0, 0);
                compile(ps, n->a);
            }
            for (int i = 0; i < count && !ps->error; i++) {
                g->code[splits[i]].x = splits[i] + 1;
                g->code[splits[i]].y = g->length;
            }
            free(splits);
        }
        break;
    }
    }
}

int regcomp(regex_t *re, const char *pattern, int flags)
{
    struct program *g = calloc(1, sizeof(*g));
    if (!g)
        return REG_ESPACE;
    g->flags = flags;
    struct parser ps = { pattern, pattern + strlen(pattern), g, (flags & REG_EXTENDED) != 0, 0,
                         { 0 }, 0 };
    struct node *tree = ps.ere ? ere_alternation(&ps) : bre_sequence(&ps);
    if (!ps.error && ps.p < ps.end)
        ps.error = REG_EPAREN;                      /* (a ) or \) with no opening) */
    if (!ps.error && !tree)
        ps.error = REG_ESPACE;
    compile(&ps, tree);
    emit(&ps, I_MATCH, 0, 0);
    free_tree(tree);
    if (ps.error) {
        free(g->code);
        free(g->sets);
        free(g);
        return ps.error;
    }
    re->re_nsub = (size_t)g->groups;
    re->re_program = g;
    return 0;
}

void regfree(regex_t *re)
{
    struct program *g = re->re_program;
    if (g) {
        free(g->code);
        free(g->sets);
        free(g);
    }
    re->re_program = 0;
}

/* --- matching ------------------------------------------------------------------- */

struct run {
    const struct program *g;
    const unsigned char *s;
    regoff_t n;
    int eflags;
    regoff_t *caps;                 /* 2 per group and the whole match (group 0) */
    regoff_t *marks;
    regoff_t end;                   /* pass 2: where the match must end */
    uint8_t *visited;               /* pass 2: (instruction, position) pairs that failed */
    regoff_t best_end;              /* pass 3 */
    regoff_t *best_caps;
    long steps;
    bool overflow;
};

#define STEPS_MAX 20000000L         /* pass 3's budget */

static bool at_bol(const struct run *r, regoff_t pos)
{
    if (pos == 0)
        return !(r->eflags & REG_NOTBOL);
    return (r->g->flags & REG_NEWLINE) && r->s[pos - 1] == '\n';
}

static bool at_eol(const struct run *r, regoff_t pos)
{
    if (pos == r->n)
        return !(r->eflags & REG_NOTEOL);
    return (r->g->flags & REG_NEWLINE) && r->s[pos] == '\n';
}

/* Whether the instruction at pc takes the byte c. */
static bool takes(const struct run *r, const struct inst *in, unsigned c)
{
    switch (in->op) {
    case I_CHAR:
        return c == (unsigned)in->x;
    case I_ANY:
        return c != 0 && !(c == '\n' && (r->g->flags & REG_NEWLINE));
    case I_SET:
        return (r->g->sets[in->x][c / 8] >> (c % 8)) & 1;
    }
    return false;
}

/* Pass 1: the leftmost-longest match's start and end, or false. */
struct thread {
    int pc;
    regoff_t start;
};

static void add_thread(const struct run *r, struct thread *list, int *count, int *seen, int gen,
                       int *stack, int pc, regoff_t pos, regoff_t start)
{
    int top = 0;
    stack[top++] = pc;
    while (top) {
        pc = stack[--top];
        if (seen[pc] == gen)
            continue;
        seen[pc] = gen;
        const struct inst *in = &r->g->code[pc];
        switch (in->op) {
        case I_JMP:
            stack[top++] = in->x;
            break;
        case I_SPLIT:
            stack[top++] = in->y;                   /* (x first: it is popped first) */
            stack[top++] = in->x;
            break;
        case I_SAVE:
        case I_MARK:
        case I_CHECK:
            stack[top++] = pc + 1;
            break;
        case I_BOL:
            if (at_bol(r, pos))
                stack[top++] = pc + 1;
            break;
        case I_EOL:
            if (at_eol(r, pos))
                stack[top++] = pc + 1;
            break;
        default:
            list[(*count)++] = (struct thread){ pc, start };
        }
    }
}

static int longest(struct run *r, regoff_t *match_start, regoff_t *match_end)
{
    int len = r->g->length;
    struct thread *now = malloc(sizeof(*now) * (size_t)len), *next = malloc(sizeof(*next) * (size_t)len);
    int *seen = malloc(sizeof(int) * (size_t)len), *stack = malloc(sizeof(int) * (size_t)len * 2);
    if (!now || !next || !seen || !stack) {
        free(now), free(next), free(seen), free(stack);
        return REG_ESPACE;
    }
    for (int i = 0; i < len; i++)
        seen[i] = -1;
    int now_count = 0, gen = 0;
    regoff_t best_start = -1, best_end = -1;
    for (regoff_t pos = 0; pos <= r->n; pos++) {
        if (best_start < 0)                         /* (a new start, after the older ones) */
            add_thread(r, now, &now_count, seen, gen, stack, 0, pos, pos);
        if (!now_count && best_start >= 0)
            break;                                  /* (else a later start may still match) */
        int next_count = 0;
        gen++;
        unsigned c = pos < r->n ? r->s[pos] : 0;
        for (int i = 0; i < now_count; i++) {
            struct thread t = now[i];
            if (best_start >= 0 && t.start > best_start)
                continue;
            const struct inst *in = &r->g->code[t.pc];
            if (in->op == I_MATCH) {
                if (best_start < 0 || t.start < best_start ||
                    (t.start == best_start && pos > best_end)) {
                    best_start = t.start;
                    best_end = pos;
                }
                continue;
            }
            if (pos < r->n && takes(r, in, c))
                add_thread(r, next, &next_count, seen, gen, stack, t.pc + 1, pos + 1, t.start);
        }
        struct thread *swap = now;
        now = next;
        next = swap;
        now_count = next_count;
        gen++;
        /* (seen marks belong to the step that made them: the next list's
         *  pcs were marked with the old gen, the new start uses the new) */
        for (int i = 0; i < now_count; i++)
            seen[now[i].pc] = gen;
    }
    free(now), free(next), free(seen), free(stack);
    if (best_start < 0)
        return REG_NOMATCH;
    *match_start = best_start;
    *match_end = best_end;
    return 0;
}

/* Pass 2: the groups of a match from pos that ends at r->end. */
static bool groups_from(struct run *r, int pc, regoff_t pos)
{
    for (;;) {
        const struct inst *in = &r->g->code[pc];
        if (in->op == I_CHECK) {                    /* (depends on the mark: not remembered) */
            pc = r->marks[in->x] == pos ? in->y : pc + 1;
            continue;
        }
        size_t key = (size_t)pc * (size_t)(r->n + 1) + (size_t)pos;
        if (r->visited[key / 8] & (1u << (key % 8)))
            return false;                           /* (failed before) */
        r->visited[key / 8] |= (uint8_t)(1u << (key % 8));
        switch (in->op) {
        case I_MATCH:
            return pos == r->end;
        case I_CHAR:
        case I_ANY:
        case I_SET:
            if (pos >= r->end || !takes(r, in, r->s[pos]))
                return false;
            pos++;
            pc++;
            break;
        case I_BOL:
            if (!at_bol(r, pos))
                return false;
            pc++;
            break;
        case I_EOL:
            if (!at_eol(r, pos))
                return false;
            pc++;
            break;
        case I_JMP:
            pc = in->x;
            break;
        case I_SPLIT:
            if (groups_from(r, in->x, pos))
                return true;
            pc = in->y;
            break;
        case I_SAVE: {
            regoff_t old = r->caps[in->x];
            r->caps[in->x] = pos;
            if (groups_from(r, pc + 1, pos))
                return true;
            r->caps[in->x] = old;
            return false;
        }
        case I_MARK: {
            regoff_t old = r->marks[in->x];
            r->marks[in->x] = pos;
            if (groups_from(r, pc + 1, pos))
                return true;
            r->marks[in->x] = old;
            return false;
        }
        default:
            pc++;
        }
    }
}

/* Pass 3 (back references): every way from pos; the longest end is kept. */
static bool search_from(struct run *r, int pc, regoff_t pos)
{
    for (;;) {
        if (++r->steps > STEPS_MAX) {
            r->overflow = true;
            return true;                            /* (stop) */
        }
        const struct inst *in = &r->g->code[pc];
        switch (in->op) {
        case I_MATCH:
            if (pos > r->best_end) {
                r->best_end = pos;
                memcpy(r->best_caps, r->caps, sizeof(regoff_t) * 2 * (size_t)(r->g->groups + 1));
            }
            return pos == r->n;                     /* (nothing can be longer) */
        case I_CHAR:
        case I_ANY:
        case I_SET:
            if (pos >= r->n || !takes(r, in, r->s[pos]))
                return false;
            pos++;
            pc++;
            break;
        case I_BOL:
            if (!at_bol(r, pos))
                return false;
            pc++;
            break;
        case I_EOL:
            if (!at_eol(r, pos))
                return false;
            pc++;
            break;
        case I_JMP:
            pc = in->x;
            break;
        case I_SPLIT:
            if (search_from(r, in->x, pos))
                return true;
            pc = in->y;
            break;
        case I_SAVE: {
            regoff_t old = r->caps[in->x];
            r->caps[in->x] = pos;
            bool done = search_from(r, pc + 1, pos);
            r->caps[in->x] = old;
            return done;
        }
        case I_MARK: {
            regoff_t old = r->marks[in->x];
            r->marks[in->x] = pos;
            bool done = search_from(r, pc + 1, pos);
            r->marks[in->x] = old;
            return done;
        }
        case I_CHECK:
            pc = r->marks[in->x] == pos ? in->y : pc + 1;   /* (a pass that took nothing) */
            break;
        case I_BREF: {
            regoff_t a = r->caps[2 * in->x], b = r->caps[2 * in->x + 1];
            if (a < 0 || b < 0)
                return false;
            regoff_t length = b - a;
            if (pos + length > r->n)
                return false;
            for (regoff_t i = 0; i < length; i++) {
                unsigned x = r->s[a + i], y = r->s[pos + i];
                if ((r->g->flags & REG_ICASE) ? tolower((int)x) != tolower((int)y) : x != y)
                    return false;
            }
            pos += length;
            pc++;
            break;
        }
        }
    }
}

int regexec(const regex_t *re, const char *string, size_t nmatch, regmatch_t pmatch[], int flags)
{
    const struct program *g = re->re_program;
    if (!g)
        return REG_BADPAT;
    struct run r = { g, (const unsigned char *)string, (regoff_t)strlen(string), flags, 0, 0, 0, 0,
                     -1, 0, 0, false };
    size_t slots = 2 * (size_t)(g->groups + 1);
    r.caps = malloc(sizeof(regoff_t) * slots);
    r.marks = malloc(sizeof(regoff_t) * (size_t)(g->marks + 1));
    if (!r.caps || !r.marks) {
        free(r.caps), free(r.marks);
        return REG_ESPACE;
    }
    for (size_t i = 0; i < slots; i++)
        r.caps[i] = -1;
    for (int i = 0; i <= g->marks; i++)
        r.marks[i] = -1;
    bool want_groups = !(g->flags & REG_NOSUB) && nmatch > 1 && g->groups > 0;
    int result = 0;
    regoff_t start = -1, end = -1;
    if (!g->backrefs) {
        result = longest(&r, &start, &end);
        if (!result && want_groups) {
            size_t bits = (size_t)g->length * (size_t)(r.n + 1);
            r.visited = calloc(bits / 8 + 1, 1);
            r.end = end;
            if (!r.visited || !groups_from(&r, 0, start))
                result = REG_ESPACE;                /* (out of memory; it matched in pass 1) */
            free(r.visited);
        }
    } else {
        r.best_caps = malloc(sizeof(regoff_t) * slots);
        result = r.best_caps ? REG_NOMATCH : REG_ESPACE;
        for (regoff_t from = 0; r.best_caps && from <= r.n && result == REG_NOMATCH; from++) {
            r.best_end = -1;
            search_from(&r, 0, from);
            if (r.overflow)
                result = REG_ESPACE;
            else if (r.best_end >= 0) {
                result = 0;
                start = from;
                end = r.best_end;
                memcpy(r.caps, r.best_caps, sizeof(regoff_t) * slots);
            }
        }
        free(r.best_caps);
    }
    if (!result && !(g->flags & REG_NOSUB) && nmatch > 0) {
        pmatch[0].rm_so = start;
        pmatch[0].rm_eo = end;
        for (size_t i = 1; i < nmatch; i++) {
            bool have = (int)i <= g->groups && want_groups;
            pmatch[i].rm_so = have ? r.caps[2 * i] : -1;
            pmatch[i].rm_eo = have && r.caps[2 * i] >= 0 ? r.caps[2 * i + 1] : -1;
            if (pmatch[i].rm_eo < 0)
                pmatch[i].rm_so = -1;
        }
    }
    free(r.caps), free(r.marks);
    return result;
}

size_t regerror(int error, const regex_t *re, char *buffer, size_t size)
{
    (void)re;
    static const char *const texts[] = {
        [0] = "success",
        [REG_NOMATCH] = "regexec() failed to match",
        [REG_BADPAT] = "invalid regular expression",
        [REG_ECOLLATE] = "invalid collating element",
        [REG_ECTYPE] = "invalid character class",
        [REG_EESCAPE] = "trailing backslash (\\)",
        [REG_ESUBREG] = "invalid backreference number",
        [REG_EBRACK] = "brackets ([ ]) not balanced",
        [REG_EPAREN] = "parentheses not balanced",
        [REG_EBRACE] = "braces not balanced",
        [REG_BADBR] = "invalid repetition count(s)",
        [REG_ERANGE] = "invalid character range",
        [REG_ESPACE] = "out of memory",
        [REG_BADRPT] = "repetition-operator operand invalid",
    };
    const char *t = error >= 0 && error < (int)(sizeof(texts) / sizeof(texts[0])) && texts[error]
                    ? texts[error] : "unknown error";
    size_t n = strlen(t) + 1;
    if (size) {
        size_t k = n < size ? n : size;
        memcpy(buffer, t, k - 1);
        buffer[k - 1] = 0;
    }
    return n;
}
