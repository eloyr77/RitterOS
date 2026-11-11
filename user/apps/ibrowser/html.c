/*
 * HTML into a tree, forgiving as browsers are (a small part of the HTML
 * standard's parsing rules): tags and attributes in any case, values
 * quoted or not, comments and <!DOCTYPE> skipped, the contents of
 * <script>, <style>, <title> and <textarea> taken as text up to their end
 * tag. Elements that cannot hold anything (br, img, hr, meta, ...) never
 * open. An end tag closes up to its element if one is open, else it is
 * ignored; a new <p>, <li>, <dt>/<dd>, <tr>, <td>/<th> or <option> closes
 * the one before, and a block (div, h1, ul, table, ...) closes an open <p>.
 * Entities: the common named ones and numeric ones (&#38; &#x26;) become
 * UTF-8. The tree always has <html> at the top; a missing <body> is made.
 */
#include <stdlib.h>
#include <string.h>
#include "browser.h"
#include "image.h"

#define DEPTH_MAX 256                   /* deeper elements become siblings */

static const char *const VOID_TAGS[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
    "source", "track", "wbr", 0
};
static const char *const RAW_TAGS[] = { "script", "style", "title", "textarea", 0 };
/* Opening one of these closes an open <p>. */
static const char *const CLOSES_P[] = {
    "address", "article", "aside", "blockquote", "center", "div", "dl", "fieldset", "figure",
    "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol",
    "p", "pre", "section", "table", "ul", 0
};

static bool in_list(const char *const *list, const char *tag)
{
    for (; *list; list++)
        if (!strcmp(*list, tag))
            return true;
    return false;
}

static struct node *new_node(enum node_kind kind, const char *tag)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n)
        return 0;
    n->kind = kind;
    for (int i = 0; tag && tag[i] && i < (int)sizeof(n->tag) - 1; i++)
        n->tag[i] = tag[i];
    return n;
}

static void append(struct node *parent, struct node *child)
{
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
}

/* --- entities --- */

static const struct { const char *name; uint32_t code; } ENTITIES[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "laquo", 0xAB },
    { "raquo", 0xBB }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
    { "rdquo", 0x201D }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "deg", 0xB0 },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "euro", 0x20AC }, { "pound", 0xA3 },
    { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 }, { "para", 0xB6 }, { "iexcl", 0xA1 },
    { "iquest", 0xBF }, { "aacute", 0xE1 }, { "eacute", 0xE9 }, { "iacute", 0xED },
    { "oacute", 0xF3 }, { "uacute", 0xFA }, { "ntilde", 0xF1 }, { "uuml", 0xFC },
    { "ouml", 0xF6 }, { "auml", 0xE4 }, { "szlig", 0xDF }, { "Aacute", 0xC1 },
    { "Eacute", 0xC9 }, { "Iacute", 0xCD }, { "Oacute", 0xD3 }, { "Uacute", 0xDA },
    { "Ntilde", 0xD1 }, { "Uuml", 0xDC }, { "Ouml", 0xD6 }, { "Auml", 0xC4 },
    { "ccedil", 0xE7 }, { "agrave", 0xE0 }, { "egrave", 0xE8 }, { "larr", 0x2190 },
    { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "hearts", 0x2665 },
    { 0, 0 }
};

static size_t put_utf8(char *out, uint32_t c)
{
    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
        c = 0xFFFD;
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | c >> 6), out[1] = (char)(0x80 | (c & 63));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | c >> 12), out[1] = (char)(0x80 | ((c >> 6) & 63));
        out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | c >> 18), out[1] = (char)(0x80 | ((c >> 12) & 63));
    out[2] = (char)(0x80 | ((c >> 6) & 63)), out[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* text[0..n) with its entities replaced: malloc'ed. */
static char *decode(const char *text, size_t n)
{
    char *out = malloc(n + 1);              /* (an entity is never shorter than its UTF-8) */
    if (!out)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (text[i] != '&') {
            out[o++] = text[i++];
            continue;
        }
        size_t j = i + 1;
        uint32_t code = 0;
        bool found = false;
        if (j < n && text[j] == '#') {
            j++;
            bool hex = j < n && (text[j] == 'x' || text[j] == 'X');
            if (hex)
                j++;
            size_t digits = 0;
            while (j < n && digits < 8) {
                int c = lower((unsigned char)text[j]), d;
                if (c >= '0' && c <= '9')
                    d = c - '0';
                else if (hex && c >= 'a' && c <= 'f')
                    d = c - 'a' + 10;
                else
                    break;
                code = code * (hex ? 16 : 10) + (uint32_t)d;
                j++, digits++;
            }
            found = digits > 0;
        } else {
            size_t k = j;
            while (k < n && k - j < 10 && ((text[k] >= 'a' && text[k] <= 'z') ||
                                          (text[k] >= 'A' && text[k] <= 'Z') ||
                                          (text[k] >= '0' && text[k] <= '9')))
                k++;
            for (int e = 0; ENTITIES[e].name; e++)
                if (strlen(ENTITIES[e].name) == k - j && !memcmp(ENTITIES[e].name, text + j, k - j)) {
                    code = ENTITIES[e].code;
                    found = true;
                    j = k;
                    break;
                }
        }
        if (!found) {
            out[o++] = text[i++];
            continue;
        }
        if (j < n && text[j] == ';')
            j++;
        char utf8[4];
        size_t u = put_utf8(utf8, code);
        if (u > j - i)
            u = put_utf8(utf8, '?');        /* (only "&#1" short forms could be longer) */
        memcpy(out + o, utf8, u);
        o += u;
        i = j;
    }
    out[o] = 0;
    return out;
}

/* --- the tree builder --- */

struct builder {
    struct node *root, *body;
    struct node *open[DEPTH_MAX];
    int depth;                          /* open[depth - 1] is the current element */
    bool self_closing;                  /* (27j) the tag being opened ended in "/>" */
};

/* (27j) Inside SVG or MathML ("foreign" content) "<path/>" ends the element,
 * as in XML; in HTML a "/>" means nothing. */
static bool foreign(const struct builder *b, const struct node *e)
{
    if (!strcmp(e->tag, "svg") || !strcmp(e->tag, "math"))
        return true;
    for (int i = b->depth - 1; i > 0; i--)
        if (!strcmp(b->open[i]->tag, "svg") || !strcmp(b->open[i]->tag, "math"))
            return true;
    return false;
}

static struct node *current(struct builder *b)
{
    return b->open[b->depth - 1];
}

/* Close elements down to (and with) the innermost `tag`, if one is open
 * below a stop element (a table cell does not close a list outside it). */
static bool close_tag(struct builder *b, const char *tag, const char *const *stops)
{
    for (int i = b->depth - 1; i >= 1; i--) {
        if (!strcmp(b->open[i]->tag, tag)) {
            b->depth = i;
            return true;
        }
        if (stops && in_list(stops, b->open[i]->tag))
            return false;
        if (!strcmp(b->open[i]->tag, "body"))
            return false;
    }
    return false;
}

static struct node *body_of(struct builder *b)
{
    if (!b->body) {
        b->body = new_node(NODE_ELEMENT, "body");
        if (!b->body)
            return 0;
        append(b->root, b->body);
        b->open[1] = b->body;
        b->depth = 2;
    }
    return b->body;
}

static void add_text(struct builder *b, const char *text, size_t n, bool raw)
{
    if (!n)
        return;
    struct node *parent = current(b);
    if (parent == b->root) {                    /* text before <body>: whitespace is lost */
        size_t i = 0;
        while (i < n && is_space(text[i]))
            i++;
        if (i == n)
            return;
        if (!body_of(b))
            return;
        parent = current(b);
    }
    struct node *t = new_node(NODE_TEXT, 0);
    if (!t)
        return;
    t->text = raw ? copy_text(text, n) : decode(text, n);
    if (!t->text) {
        free(t);
        return;
    }
    append(parent, t);
}

static void open_element(struct builder *b, struct node *e)
{
    const char *tag = e->tag;
    static const char *const CELL_STOPS[] = { "table", 0 };
    static const char *const LIST_STOPS[] = { "ul", "ol", "table", 0 };
    if (!strcmp(tag, "html")) {                 /* (its attributes are not needed) */
        html_free(e);
        return;
    }
    if (!strcmp(tag, "head") || !strcmp(tag, "body")) {
        if (!strcmp(tag, "body") && !b->body) {
            b->body = e;
            append(b->root, e);
            b->open[1] = e;
            b->depth = 2;
            return;
        }
        if (!strcmp(tag, "head") && !b->body && b->depth == 1) {
            append(b->root, e);
            b->open[b->depth++] = e;
            return;
        }
        html_free(e);                           /* a second one: ignored */
        return;
    }
    bool head_kind = !strcmp(tag, "title") || !strcmp(tag, "meta") || !strcmp(tag, "link") ||
                     !strcmp(tag, "style") || !strcmp(tag, "base") || !strcmp(tag, "script");
    if (!head_kind || b->body) {
        if (b->depth >= 2 && !strcmp(b->open[1]->tag, "head") && !head_kind)
            b->depth = 1;                       /* the head ends */
        if (!body_of(b) && !head_kind) {
            html_free(e);
            return;
        }
    }
    if (in_list(CLOSES_P, tag))
        close_tag(b, "p", LIST_STOPS);
    if (!strcmp(tag, "li"))
        close_tag(b, "li", LIST_STOPS);
    if (!strcmp(tag, "dt") || !strcmp(tag, "dd")) {
        close_tag(b, "dt", LIST_STOPS);
        close_tag(b, "dd", LIST_STOPS);
    }
    if (!strcmp(tag, "tr")) {
        close_tag(b, "td", CELL_STOPS);
        close_tag(b, "th", CELL_STOPS);
        close_tag(b, "tr", CELL_STOPS);
    }
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) {
        close_tag(b, "td", CELL_STOPS);
        close_tag(b, "th", CELL_STOPS);
    }
    if (!strcmp(tag, "option"))
        close_tag(b, "option", 0);
    if (tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6' && !tag[2]) {
        const char *const heads[] = { "h1", "h2", "h3", "h4", "h5", "h6" };
        for (int i = 0; i < 6; i++)             /* (a heading inside a heading closes it) */
            if (!strcmp(current(b)->tag, heads[i]))
                b->depth--;
    }
    append(current(b), e);
    if (!in_list(VOID_TAGS, tag) && !(b->self_closing && foreign(b, e)) && b->depth < DEPTH_MAX)
        b->open[b->depth++] = e;
}

static void end_tag(struct builder *b, const char *tag)
{
    if (!strcmp(tag, "html") || !strcmp(tag, "body"))
        return;                                 /* (text after them still belongs to body) */
    if (!strcmp(tag, "head")) {
        close_tag(b, "head", 0);
        return;
    }
    if (!strcmp(tag, "p") && !close_tag(b, "p", 0)) {
        struct node *p = new_node(NODE_ELEMENT, "p");   /* "</p>" alone: an empty paragraph */
        if (p && body_of(b))
            append(current(b), p);
        else
            free(p);
        return;
    }
    if (!strcmp(tag, "br")) {                   /* "</br>": a <br>, as browsers take it */
        struct node *br = new_node(NODE_ELEMENT, "br");
        if (br && body_of(b))
            append(current(b), br);
        else
            free(br);
        return;
    }
    close_tag(b, tag, 0);
}

/* Parse a tag at text[i] ('<' there); its end. */
static size_t parse_tag(struct builder *b, const char *text, size_t n, size_t i, bool *raw_open,
                        char *raw_tag)
{
    size_t j = i + 1;
    bool closing = j < n && text[j] == '/';
    if (closing)
        j++;
    char tag[16];
    size_t t = 0;
    while (j < n && !is_space(text[j]) && text[j] != '>' && text[j] != '/') {
        if (t + 1 < sizeof(tag))
            tag[t++] = (char)lower((unsigned char)text[j]);
        j++;
    }
    tag[t] = 0;
    struct node *e = closing ? 0 : new_node(NODE_ELEMENT, tag);
    /* Attributes: name, name=value, name="value", name='value'. */
    for (;;) {
        while (j < n && (is_space(text[j]) || text[j] == '/'))
            j++;
        if (j >= n || text[j] == '>')
            break;
        size_t ns = j;
        while (j < n && !is_space(text[j]) && text[j] != '=' && text[j] != '>' &&
               !(text[j] == '/' && j + 1 < n && text[j + 1] == '>'))
            j++;
        size_t ne = j;
        while (j < n && is_space(text[j]))
            j++;
        size_t vs = 0, ve = 0;
        bool has_value = j < n && text[j] == '=';
        if (has_value) {
            j++;
            while (j < n && is_space(text[j]))
                j++;
            if (j < n && (text[j] == '"' || text[j] == '\'')) {
                char q = text[j++];
                vs = j;
                while (j < n && text[j] != q)
                    j++;
                ve = j;
                if (j < n)
                    j++;
            } else {
                vs = j;
                while (j < n && !is_space(text[j]) && text[j] != '>')
                    j++;
                ve = j;
            }
        }
        if (e && ne > ns && e->attr_count < 32) {
            struct attr *a = realloc(e->attrs, sizeof(*a) * (size_t)(e->attr_count + 1));
            if (a) {
                e->attrs = a;
                a[e->attr_count].name = copy_text(text + ns, ne - ns);
                a[e->attr_count].value = has_value ? decode(text + vs, ve - vs) : copy_text("", 0);
                if (a[e->attr_count].name && a[e->attr_count].value) {
                    for (char *c = a[e->attr_count].name; *c; c++)
                        *c = (char)lower((unsigned char)*c);
                    e->attr_count++;
                } else {
                    free(a[e->attr_count].name);
                    free(a[e->attr_count].value);
                }
            }
        }
    }
    b->self_closing = j > 0 && j < n && text[j - 1] == '/';
    if (j < n)
        j++;                                    /* '>' */
    if (closing) {
        end_tag(b, tag);
    } else if (e) {
        open_element(b, e);
        if (in_list(RAW_TAGS, tag) && current(b) == e) {     /* (not a self-closed <title/> in SVG) */
            *raw_open = true;
            strcpy(raw_tag, tag);
        }
    }
    return j;
}

struct node *html_parse(const char *text, size_t n)
{
    struct builder b = { 0 };
    b.root = new_node(NODE_ELEMENT, "html");
    if (!b.root)
        return 0;
    b.open[0] = b.root;
    b.depth = 1;
    size_t i = 0, start = 0;
    bool raw = false;
    char raw_tag[16] = "";
    while (i < n) {
        if (raw) {                              /* up to </tag> */
            size_t len = strlen(raw_tag);
            size_t j = i;
            while (j < n && !(text[j] == '<' && j + 1 < n && text[j + 1] == '/' &&
                              j + 2 + len <= n && starts_word(text + j + 2, raw_tag)))
                j++;
            struct node *e = current(&b);
            bool keep_raw = !strcmp(raw_tag, "script") || !strcmp(raw_tag, "style");
            if (j > i && !strcmp(e->tag, raw_tag))
                add_text(&b, text + i, j - i, keep_raw);
            raw = false;
            if (j >= n) {
                close_tag(&b, raw_tag, 0);
                break;
            }
            i = start = j;
            continue;
        }
        if (text[i] != '<') {
            i++;
            continue;
        }
        size_t at = i;
        if (i + 3 < n && !memcmp(text + i, "<!--", 4)) {
            add_text(&b, text + start, at - start, false);
            const char *end = 0;
            for (size_t k = i + 4; k + 2 < n; k++)
                if (!memcmp(text + k, "-->", 3)) {
                    end = text + k;
                    break;
                }
            i = end ? (size_t)(end - text) + 3 : n;
            start = i;
            continue;
        }
        if (i + 1 < n && (text[i + 1] == '!' || text[i + 1] == '?')) {   /* <!DOCTYPE>, <?xml> */
            add_text(&b, text + start, at - start, false);
            while (i < n && text[i] != '>')
                i++;
            i = start = i < n ? i + 1 : n;
            continue;
        }
        bool tag_like = i + 1 < n && ((text[i + 1] >= 'a' && text[i + 1] <= 'z') ||
                                      (text[i + 1] >= 'A' && text[i + 1] <= 'Z') ||
                                      (text[i + 1] == '/' && i + 2 < n &&
                                       ((text[i + 2] >= 'a' && text[i + 2] <= 'z') ||
                                        (text[i + 2] >= 'A' && text[i + 2] <= 'Z'))));
        if (!tag_like) {                        /* "a < b": text */
            i++;
            continue;
        }
        add_text(&b, text + start, at - start, false);
        i = start = parse_tag(&b, text, n, i, &raw, raw_tag);
    }
    if (!raw)
        add_text(&b, text + start, i > n ? 0 : i - start, false);
    body_of(&b);
    return b.root;
}

void html_free(struct node *n)
{
    if (!n)
        return;
    for (struct node *c = n->first; c;) {
        struct node *after = c->next;
        html_free(c);
        c = after;
    }
    for (int i = 0; i < n->attr_count; i++) {
        free(n->attrs[i].name);
        free(n->attrs[i].value);
    }
    free(n->attrs);
    free(n->text);
    css_style_free(n->style);
    control_free(n->control);
    if (n->picture) {
        image_free(n->picture);
        free(n->picture);
    }
    if (n->background) {
        image_free(n->background);
        free(n->background);
    }
    free(n);
}

const char *node_attr(const struct node *e, const char *name)
{
    for (int i = 0; i < e->attr_count; i++)
        if (!strcmp(e->attrs[i].name, name))
            return e->attrs[i].value;
    return 0;
}

struct node *node_next(struct node *n, struct node *root)
{
    if (n->first)
        return n->first;
    while (n && n != root) {
        if (n->next)
            return n->next;
        n = n->parent;
    }
    return 0;
}

struct node *node_find(struct node *root, const char *tag)
{
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->kind == NODE_ELEMENT && !strcmp(n->tag, tag))
            return n;
    return 0;
}
