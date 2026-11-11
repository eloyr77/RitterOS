/*
 * Forms (27c): what each control holds, and what a form sends. A control's
 * state lives in its node (node->control), made from its attributes the
 * first time it is laid out or looked at, so it survives a new layout (a
 * resized window) and is lost with the page. A control belongs to the form
 * its form="" attribute names, else to the <form> around it.
 *
 * Sending (the HTML standard's application/x-www-form-urlencoded): the
 * form's controls in document order that have a name and are not
 * disabled: text-like ones and textareas with their text (line breaks as
 * CR LF), checked checkboxes and radio buttons with their value ("on"
 * without one), a select's chosen option (its value="", else its text),
 * hidden inputs, and the button that sent it. Bytes are kept as they are
 * but for letters, digits and "*-._", which stay, a space, which becomes
 * '+', and the rest, which become %XX. GET puts that after the action's
 * '?', POST sends it as the body.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"

static enum control_kind kind_of(const struct node *e)
{
    if (!strcmp(e->tag, "textarea"))
        return CONTROL_TEXTAREA;
    if (!strcmp(e->tag, "select"))
        return CONTROL_SELECT;
    const char *type = node_attr(e, "type");
    if (!strcmp(e->tag, "button")) {            /* (submit unless it says otherwise) */
        if (type && same_word(type, "reset"))
            return CONTROL_RESET;
        if (type && same_word(type, "button"))
            return CONTROL_BUTTON;
        return CONTROL_SUBMIT;
    }
    if (!type)
        return CONTROL_TEXT;
    static const struct { const char *type; enum control_kind kind; } TYPES[] = {
        { "password", CONTROL_PASSWORD }, { "checkbox", CONTROL_CHECKBOX },
        { "radio", CONTROL_RADIO }, { "submit", CONTROL_SUBMIT }, { "image", CONTROL_SUBMIT },
        { "reset", CONTROL_RESET }, { "button", CONTROL_BUTTON }, { "hidden", CONTROL_HIDDEN },
        { "file", CONTROL_BUTTON },              /* (not sendable here: a plain button) */
    };
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++)
        if (same_word(type, TYPES[i].type))
            return TYPES[i].kind;
    return CONTROL_TEXT;                        /* text, search, email, number, ... */
}

bool is_control(const struct node *e)
{
    return e->kind == NODE_ELEMENT && (!strcmp(e->tag, "input") || !strcmp(e->tag, "button") ||
                                       !strcmp(e->tag, "select") || !strcmp(e->tag, "textarea"));
}

/* The text inside e, its white space collapsed (an option's or a button's
 * label); malloc'ed. */
char *node_text(const struct node *e)
{
    char *out = copy_text("", 0);
    bool space = false;
    for (const struct node *n = e->first; n && out; n = node_next((struct node *)n, (struct node *)e)) {
        if (n->kind != NODE_TEXT)
            continue;
        for (const char *p = n->text; *p; p++) {
            if (is_space(*p)) {
                space = out[0] != 0;
                continue;
            }
            char one[3] = { ' ', *p, 0 };
            out = join_text(out, space ? one : one + 1);
            space = false;
        }
    }
    return out;
}

int select_options(struct node *sel, struct node **options, int max)
{
    int n = 0;
    for (struct node *o = sel->first; o; o = node_next(o, sel))
        if (o->kind == NODE_ELEMENT && !strcmp(o->tag, "option")) {
            if (n < max)
                options[n] = o;
            n++;
        }
    return n;
}

const char *option_value(struct node *option, char **made)
{
    *made = 0;
    const char *v = node_attr(option, "value");
    if (v)
        return v;
    *made = node_text(option);
    return *made ? *made : "";
}

/* A control's first state: what its attributes say. */
static void set_defaults(struct node *e, struct control *c)
{
    free(c->value);
    c->value = 0;
    c->checked = node_attr(e, "checked") != 0;
    c->selected = 0;
    if (c->kind == CONTROL_TEXTAREA) {
        char *t = copy_text("", 0);             /* (raw text: no collapsing) */
        for (struct node *n = e->first; n && t; n = n->next)
            if (n->kind == NODE_TEXT)
                t = join_text(t, n->text);
        if (t && t[0] == '\n')                  /* (a first line break is not part of it) */
            memmove(t, t + 1, strlen(t));
        c->value = t;
    } else if (c->kind == CONTROL_SELECT) {
        struct node *options[256];
        int n = select_options(e, options, 256);
        for (int i = 0; i < n && i < 256; i++)
            if (node_attr(options[i], "selected"))
                c->selected = i;
    } else {
        const char *v = node_attr(e, "value");
        c->value = copy_text(v ? v : "", v ? strlen(v) : 0);
    }
}

struct node *form_of(struct node *e, struct node *root)
{
    const char *id = node_attr(e, "form");
    if (id && *id) {
        for (struct node *n = root; n; n = node_next(n, root))
            if (n->kind == NODE_ELEMENT && !strcmp(n->tag, "form") && node_attr(n, "id") &&
                !strcmp(node_attr(n, "id"), id))
                return n;
        return 0;
    }
    for (struct node *p = e->parent; p; p = p->parent)
        if (!strcmp(p->tag, "form"))
            return p;
    return 0;
}

struct control *control_of(struct node *e)
{
    if (!is_control(e))
        return 0;
    if (!e->control) {
        struct control *c = calloc(1, sizeof(*c));
        if (!c)
            return 0;
        c->kind = kind_of(e);
        set_defaults(e, c);
        e->control = c;
    }
    return e->control;
}

void control_free(struct control *c)
{
    if (c) {
        free(c->value);
        free(c);
    }
}

void control_set_text(struct control *c, const char *text)
{
    char *t = copy_text(text, strlen(text));
    if (t) {
        free(c->value);
        c->value = t;
    }
}

/* The controls of form (or, for form 0, those of no form) in document order. */
static struct node *next_control(struct node *n, struct node *root, struct node *form)
{
    for (n = n ? node_next(n, root) : root; n; n = node_next(n, root))
        if (is_control(n) && form_of(n, root) == form)
            return n;
    return 0;
}

void radio_check(struct node *e, struct node *root)
{
    const char *name = node_attr(e, "name");
    struct node *form = form_of(e, root);
    for (struct node *n = next_control(0, root, form); n; n = next_control(n, root, form)) {
        struct control *c = control_of(n);
        if (c && c->kind == CONTROL_RADIO && n != e && name && node_attr(n, "name") &&
            !strcmp(node_attr(n, "name"), name))
            c->checked = false;
    }
    struct control *mine = control_of(e);
    if (mine)
        mine->checked = true;
}

void form_reset(struct node *form, struct node *root)
{
    for (struct node *n = next_control(0, root, form); n; n = next_control(n, root, form)) {
        struct control *c = control_of(n);
        if (c)
            set_defaults(n, c);
    }
}

/* --- sending --- */

struct data {
    char *text;
    size_t len, room;
    bool ok;
};

static void put(struct data *d, const char *s, size_t n)
{
    if (!d->ok)
        return;
    if (d->len + n + 1 > d->room) {
        size_t room = (d->len + n + 1) * 2;
        char *t = realloc(d->text, room);
        if (!t) {
            d->ok = false;
            return;
        }
        d->text = t, d->room = room;
    }
    memcpy(d->text + d->len, s, n);
    d->len += n;
    d->text[d->len] = 0;
}

static void put_encoded(struct data *d, const char *s, bool line_breaks)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char out[6];
        size_t n = 0;
        if (line_breaks && (*p == '\n' || *p == '\r')) {        /* (every one as CR LF) */
            if (*p == '\r' && p[1] == '\n')
                p++;
            put(d, "%0D%0A", 6);
            continue;
        }
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '*' || *p == '-' || *p == '.' || *p == '_')
            out[n++] = (char)*p;
        else if (*p == ' ')
            out[n++] = '+';
        else
            out[n++] = '%', out[n++] = HEX[*p >> 4], out[n++] = HEX[*p & 15];
        put(d, out, n);
    }
}

static void put_pair(struct data *d, const char *name, const char *value, bool line_breaks)
{
    if (d->len)
        put(d, "&", 1);
    put_encoded(d, name, false);
    put(d, "=", 1);
    put_encoded(d, value, line_breaks);
}

char *form_data(struct node *form, struct node *root, struct node *submitter)
{
    struct data d = { .ok = true };
    put(&d, "", 0);
    for (struct node *n = next_control(0, root, form); n; n = next_control(n, root, form)) {
        struct control *c = control_of(n);
        const char *name = node_attr(n, "name");
        if (!c || !name || !*name || node_attr(n, "disabled"))
            continue;
        switch (c->kind) {
        case CONTROL_TEXT:
        case CONTROL_PASSWORD:
        case CONTROL_HIDDEN:
            put_pair(&d, name, c->value ? c->value : "", false);
            break;
        case CONTROL_TEXTAREA:
            put_pair(&d, name, c->value ? c->value : "", true);
            break;
        case CONTROL_CHECKBOX:
        case CONTROL_RADIO:
            if (c->checked)
                put_pair(&d, name, node_attr(n, "value") ? node_attr(n, "value") : "on", false);
            break;
        case CONTROL_SELECT: {
            struct node *options[256];
            int count = select_options(n, options, 256);
            if (c->selected < count && c->selected < 256) {
                char *made;
                const char *v = option_value(options[c->selected], &made);
                put_pair(&d, name, v, false);
                free(made);
            }
            break;
        }
        case CONTROL_SUBMIT:
            if (n == submitter)
                put_pair(&d, name, node_attr(n, "value") ? node_attr(n, "value") : "", false);
            break;
        case CONTROL_RESET:
        case CONTROL_BUTTON:
            break;
        }
    }
    if (!d.ok) {
        free(d.text);
        return 0;
    }
    return d.text;
}

bool form_submission(struct node *form, struct node *root, struct node *submitter,
                     const char *page_url, char *url, size_t size, char **body)
{
    *body = 0;
    const char *action = submitter && node_attr(submitter, "formaction")
                       ? node_attr(submitter, "formaction") : form ? node_attr(form, "action") : 0;
    const char *method = submitter && node_attr(submitter, "formmethod")
                       ? node_attr(submitter, "formmethod") : form ? node_attr(form, "method") : 0;
    char target[URL_MAX];
    if (!action || !*action)
        snprintf(target, sizeof(target), "%s", page_url);   /* (no action: the page itself) */
    else if (!url_resolve(page_url, action, target, sizeof(target)))
        return false;
    target[strcspn(target, "#")] = 0;
    char *data = form_data(form, root, submitter);
    if (!data)
        return false;
    if (method && same_word(method, "post")) {
        snprintf(url, size, "%s", target);
        *body = data;
        return true;
    }
    size_t keep = strcspn(target, "?");         /* GET: the data replaces the query */
    int n = snprintf(url, size, "%.*s?%s", (int)keep, target, data);
    free(data);
    return n > 0 && (size_t)n < size;
}

void control_describe(struct node *e, char *out, size_t size)
{
    struct control *c = control_of(e);
    if (!c) {
        snprintf(out, size, "[?]");
        return;
    }
    const char *name = node_attr(e, "name") ? node_attr(e, "name") : "";
    static const char *const NAMES[] = { "text", "password", "checkbox", "radio", "submit",
                                         "reset", "button", "select", "textarea", "hidden" };
    const char *kind = NAMES[c->kind];
    char *label = 0;
    switch (c->kind) {
    case CONTROL_PASSWORD: {
        size_t n = c->value ? strlen(c->value) : 0;
        snprintf(out, size, "[%s: %s=%zu characters]", kind, name, n);
        break;
    }
    case CONTROL_CHECKBOX:
    case CONTROL_RADIO:
        snprintf(out, size, "[%s: %s=%s %s]", kind, name,
                 node_attr(e, "value") ? node_attr(e, "value") : "on", c->checked ? "on" : "off");
        break;
    case CONTROL_SUBMIT:
    case CONTROL_RESET:
    case CONTROL_BUTTON:
        label = control_label(e);
        snprintf(out, size, "[%s: %s]", kind, label ? label : "");
        break;
    case CONTROL_SELECT: {
        struct node *options[256];
        int count = select_options(e, options, 256);
        label = c->selected < count && c->selected < 256 ? node_text(options[c->selected]) : 0;
        snprintf(out, size, "[%s: %s=%s]", kind, name, label ? label : "");
        break;
    }
    default: {
        int n = snprintf(out, size, "[%s: %s=", kind, name);
        for (const char *p = c->value ? c->value : ""; *p && n > 0 && (size_t)n + 4 < size; p++) {
            if (*p == '\n')                     /* (one line for -t: a line break as \n) */
                out[n++] = '\\', out[n++] = 'n';
            else if (*p != '\r')
                out[n++] = *p;
        }
        if (n > 0 && (size_t)n + 2 <= size)
            out[n++] = ']', out[n] = 0;
        break;
    }
    }
    free(label);
}

char *control_label(struct node *e)
{
    struct control *c = control_of(e);
    if (!strcmp(e->tag, "button"))
        return node_text(e);
    const char *v = node_attr(e, "value");
    if (v)
        return copy_text(v, strlen(v));
    const char *fixed = c && c->kind == CONTROL_RESET ? "Reset" : c && c->kind == CONTROL_SUBMIT
                      ? "Submit" : "";
    return copy_text(fixed, strlen(fixed));
}
