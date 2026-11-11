/*
 * iBrowser's parts (user/apps/ibrowser/): what the program ibrowser.c joins.
 *   url.c     web addresses: parsing, and a link's address from the page's
 *   http.c    getting a page or picture: http://, https:// (libtls), file
 *             paths; redirections, lengths, chunks
 *   html.c    HTML into a tree of elements and texts (a forgiving parser:
 *             tags closed where a browser closes them, entities)
 *   css.c     style sheets (the browser's own, <style>, <link>, style="")
 *             into each element's computed style
 *   layout.c  the styled tree into what is drawn: rectangles, words,
 *             pictures, form controls, and where the links are
 *   form.c    form controls' state, and what a form sends (27c)
 * All of it is the program's own: nothing here is a library interface.
 */
#ifndef IBROWSER_BROWSER_H
#define IBROWSER_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "media.h"

/* --- small text helpers (the C library has no ctype or strcasecmp) --- */
int lower(int c);
int hex_digit(int c);                                    /* 0-15, or -1 */
bool is_space(int c);
bool same_word(const char *a, const char *b);            /* equal, ignoring case */
bool starts_word(const char *s, const char *prefix);     /* prefix, ignoring case */
char *copy_text(const char *s, size_t n);                /* malloc'ed, NUL-ended */
char *join_text(char *base, const char *more);           /* base (freed) + more */

/* --- url.c --- */
#define URL_MAX 1024

struct url {
    char scheme[8];             /* "http", "https", "file", "about" */
    char host[256];
    unsigned port;
    char path[URL_MAX];         /* with the query, without the fragment */
};

bool url_parse(const char *text, struct url *u);
void url_text(const struct url *u, char *out, size_t size);
/* The absolute address of `ref` (a link's href) on the page at `base`. */
bool url_resolve(const char *base, const char *ref, char *out, size_t size);

/* --- http.c --- */
#define BODY_MAX (16L * 1024 * 1024)

struct response {
    int status;                 /* 200, 404, ...; 0 for a local file */
    char type[64];              /* Content-Type, lower case, without parameters */
    char url[URL_MAX];          /* the address it finally came from */
    char *body;                 /* malloc'ed, a NUL after it */
    long length;
    char encoding[16];          /* (27b) "gzip" or "deflate" if it came packed, else "" */
    long packed;                /* then the length as it came */
    char error[160];            /* why there is none */
    char *headers;              /* (27r) its header lines as they came ("name: value\n"...), or 0 */
    char status_text[64];       /* (27r) the status line's text ("OK") */
    bool redirected;            /* (27r) it came from another address than asked */
};

/* (27r) Any request (fetch, XMLHttpRequest): a method (0: GET, or POST with
 * a body), header lines ("Name: value\r\n"...) besides Host and the
 * browser's own, a body; no_cookies: none sent and none kept (a cross-origin
 * request without credentials); no_files: a path or file: address refused. */
struct http_request {
    const char *url;
    const char *method;
    const char *headers;
    const char *body;
    size_t body_n;
    bool no_cookies, no_files;
    bool one_hop;               /* a redirection is the answer (its status, its Location header) */
};
int http_request(const struct http_request *q, struct response *r);

/* Get url (following redirections): 1 with *r filled (free r->body), or 0
 * with r->error. */
int http_get(const char *url, struct response *r);
/* (27c) POST n bytes of a form's data (urlencoded) to url; redirections
 * 301-303 then GET the new address (as browsers do), 307 and 308 post again. */
int http_post(const char *url, const char *body, size_t n, struct response *r);
void response_free(struct response *r);
/* (27q) Cookies (RFC 6265), kept in memory while iBrowser runs, nothing on
 * the disk: a Set-Cookie header's value (or document.cookie's) for the
 * page at url, and the cookies url gets ("a=1; b=2", malloc'ed; a script
 * does not see the HttpOnly ones). */
void cookie_set(const char *url, const char *text, bool from_script);
char *cookie_get(const char *url, bool for_script);

/* --- html.c --- */
enum node_kind { NODE_ELEMENT, NODE_TEXT };

struct attr {
    char *name, *value;
};

struct style;
struct control;
struct css_var;
struct gradient;

struct node {
    enum node_kind kind;
    char tag[16];               /* lower case; "" for text */
    char *text;                 /* NODE_TEXT */
    struct attr *attrs;
    int attr_count;
    struct node *parent, *first, *last, *next, *prev;
    struct style *style;        /* computed by css.c */
    struct image *picture;      /* <img>: the decoded picture, 0 until loaded */
    int picture_failed;
    struct image *background;   /* (27h) its background-image's picture, 0 until loaded */
    int background_failed;
    struct control *control;    /* (27c) a form control's state (form.c), 0 until used */
    int box[4];                 /* (27d) its border box as laid out: x, y, w, h (blocks) */
    bool svg_drawn;             /* (27j) an inline <svg>: picture is its drawing (made by layout) */
    int memo[3][2];             /* (27g) measured in this layout (layout.c's measure): the max- and
                                   min-content widths and a height, each with its key (0: none) */
    uint64_t js;                /* (27p) its object for scripts (dom.c), 0 until asked for */
    uint8_t script_state;       /* (27p) <script>: 1 run (or never to run) */
};

struct node *html_parse(const char *text, size_t n);
void html_free(struct node *root);
const char *node_attr(const struct node *e, const char *name);
struct node *node_find(struct node *root, const char *tag);     /* first in document order */
struct node *node_next(struct node *n, struct node *root);       /* document order */

/* --- form.c --- */
enum control_kind { CONTROL_TEXT, CONTROL_PASSWORD, CONTROL_CHECKBOX, CONTROL_RADIO,
                    CONTROL_SUBMIT, CONTROL_RESET, CONTROL_BUTTON, CONTROL_SELECT,
                    CONTROL_TEXTAREA, CONTROL_HIDDEN };

struct control {
    enum control_kind kind;
    char *value;                /* text kinds and textareas: the text (malloc'ed) */
    bool checked;               /* checkboxes and radio buttons */
    int selected;               /* a select: its chosen option's index */
};

bool is_control(const struct node *e);          /* input, button, select, textarea */
struct control *control_of(struct node *e);     /* its state (made at the first look), or 0 */
void control_free(struct control *c);
void control_set_text(struct control *c, const char *text);
char *node_text(const struct node *e);          /* the text inside, collapsed: malloc'ed */
char *control_label(struct node *e);            /* a button's label: malloc'ed */
int select_options(struct node *select, struct node **options, int max);    /* how many */
const char *option_value(struct node *option, char **made);    /* (free *made after) */
struct node *form_of(struct node *e, struct node *root);       /* its <form>, or 0 */
void radio_check(struct node *e, struct node *root);           /* and the others of its group off */
void form_reset(struct node *form, struct node *root);
/* What form sends (sent by submitter, or 0): the address to go to (GET:
 * with the data as its query) and, for POST, the data in *body (malloc'ed). */
bool form_submission(struct node *form, struct node *root, struct node *submitter,
                     const char *page_url, char *url, size_t size, char **body);
/* For -t: "[text: q=VALUE]", "[checkbox: NAME=VALUE on]", "[submit: LABEL]", ... */
void control_describe(struct node *e, char *out, size_t size);

/* --- css.c --- */
enum display { DISPLAY_INLINE, DISPLAY_BLOCK, DISPLAY_LIST_ITEM, DISPLAY_NONE, DISPLAY_TABLE_ROW,
               DISPLAY_TABLE_CELL, DISPLAY_INLINE_BLOCK,
               DISPLAY_CONTENTS };      /* (no box: its children in its place) */
enum float_side { FLOAT_NONE, FLOAT_LEFT, FLOAT_RIGHT };
enum position { POSITION_STATIC, POSITION_RELATIVE, POSITION_ABSOLUTE, POSITION_FIXED };
#define CLEAR_LEFT  1
#define CLEAR_RIGHT 2

#define LENGTH_AUTO (-100000)   /* (a margin or an offset not given) */
#define OFFSET_PERCENT(p) (-200000 - (p))       /* (lengths stay within +-100000) */
enum align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
enum list_style { LIST_DISC, LIST_DECIMAL, LIST_NONE, LIST_CIRCLE, LIST_SQUARE };

enum flex_direction { FLEX_ROW, FLEX_ROW_REVERSE, FLEX_COLUMN, FLEX_COLUMN_REVERSE };
enum flex_wrap { FLEX_NOWRAP, FLEX_WRAP, FLEX_WRAP_REVERSE };
enum flex_align { FLEX_NORMAL, FLEX_START, FLEX_END, FLEX_CENTER, FLEX_STRETCH, FLEX_BASELINE,
                  FLEX_BETWEEN, FLEX_AROUND, FLEX_EVENLY, FLEX_AUTO };

enum font_family { FAMILY_SERIF, FAMILY_SANS, FAMILY_MONO,      /* (27k) Times, Helvetica, Courier, */
                   FAMILY_FIXED };                              /*   the X11 misc-fixed faces */
enum text_transform { TRANSFORM_NONE, TRANSFORM_UPPER, TRANSFORM_LOWER, TRANSFORM_CAPITALIZE };
enum vertical_align { VALIGN_BASELINE, VALIGN_SUB, VALIGN_SUPER, VALIGN_TOP, VALIGN_MIDDLE,
                      VALIGN_BOTTOM, VALIGN_LENGTH };
enum bg_repeat { REPEAT_BOTH, REPEAT_NONE, REPEAT_X, REPEAT_Y };
#define BG_SIZE_AUTO    0
#define BG_SIZE_COVER   1
#define BG_SIZE_CONTAIN 2
#define BG_SIZE_GIVEN   3

#define COLOR_NONE 0xFF000000u  /* (transparent: a background not set) */
#define COLOR_CLEAR 0xFE000000u /* (27h) a border that takes its room and is not drawn (transparent) */

struct style {
    enum display display;
    uint32_t color;             /* 0xRRGGBB */
    uint32_t background;        /* 0xRRGGBB or COLOR_NONE */
    int font_size;              /* pixels */
    bool bold, italic, underline, pre, strike;
    enum align align;
    enum list_style list;
    int margin[4], padding[4];  /* top, right, bottom, left, in pixels */
    int border[4];              /* (27h) widths drawn: top, right, bottom, left (0: none) */
    uint32_t border_colors[4];  /* COLOR_NONE: the text's colour */
    int width, height;          /* pixels, or -1 (auto) */
    int max_width;              /* -1: none */
    /* (27d) floats and positioning; none of them inherited */
    enum float_side floating;
    int clear;                  /* CLEAR_LEFT | CLEAR_RIGHT */
    enum position position;
    int offset[4];              /* top, right, bottom, left: pixels, LENGTH_AUTO, or
                                   OFFSET_PERCENT(p) of the containing width */
    int z_index;                /* (z_auto: none given) */
    bool z_auto;
    bool holds_floats;          /* overflow other than visible, display: flow-root */
    bool clips;                 /* (27h) overflow other than visible: its contents cut at its edges */
    /* (27f) */
    int width_add, max_width_add;       /* pixels added to a percentage: calc(100% - 20px) */
    struct css_var *vars;       /* custom properties set on it (--name), inherited by lookup */
    char *content;              /* ::before and ::after: the content value */
    /* (27g) flexible boxes; box-sizing, min-width */
    bool flex;                  /* display: flex (DISPLAY_BLOCK) or inline-flex (DISPLAY_INLINE_BLOCK) */
    enum flex_direction direction;
    enum flex_wrap wrap;
    enum flex_align justify;    /* justify-content */
    enum flex_align align_items, align_self, align_content;     /* (align_self FLEX_AUTO: the container's) */
    float grow, shrink;         /* flex-grow, flex-shrink (1) */
    int basis, basis_add;       /* flex-basis as a width (-1: auto, its width or contents) */
    int order;
    int row_gap, column_gap;    /* pixels */
    int min_width, min_width_add;       /* as a width (-1: auto) */
    bool border_box;            /* box-sizing: border-box (widths and heights include them) */
    /* (27h) the look */
    int border_set[4];          /* a side's width as given (medium: 3), drawn only with a style */
    bool border_style[4];       /* a border-style other than none, hidden */
    int radius[4];              /* corners top-left, top-right, bottom-right, bottom-left: px, or
                                   -p for p% of the box's smaller side */
    bool shadow;                /* box-shadow (the first one that is not inset) */
    int shadow_x, shadow_y, shadow_blur, shadow_spread;
    uint32_t shadow_color;
    int shadow_alpha;           /* 0-255 */
    int background_alpha, color_alpha;  /* 0-255 (rgba(), hsla(), #rrggbbaa) */
    int opacity;                /* 0-255 */
    bool hidden;                /* visibility: hidden (inherited) */
    enum text_transform transform;      /* (inherited) */
    float line_factor;          /* line-height as a number (inherited as one), or 0 */
    int line_px;                /* as a length, or 0: normal */
    enum vertical_align valign;
    int raise;                  /* vertical-align as a length: pixels up */
    char *background_image;     /* url(ADDRESS) or a gradient, as written (malloc'ed), or 0 */
    enum bg_repeat bg_repeat;
    int bg_size;                /* BG_SIZE_* */
    int bg_w, bg_h;             /* BG_SIZE_GIVEN: px or percentages, or LENGTH_AUTO */
    bool bg_w_pct, bg_h_pct;
    int bg_x, bg_y;             /* background-position: px or percentages of the room left */
    bool bg_x_pct, bg_y_pct;
    /* (27i) tables */
    bool table;                 /* display: table (DISPLAY_BLOCK) or inline-table (DISPLAY_INLINE_BLOCK) */
    int spacing_x, spacing_y;   /* border-spacing (inherited) */
    bool collapse;              /* border-collapse: collapse (inherited) */
    /* (27j) SVG painting, all inherited */
    uint32_t fill, stroke;      /* colours */
    int fill_alpha, stroke_alpha;       /* 0-255 (0: none) */
    float stroke_width;
    bool evenodd;               /* fill-rule: evenodd */
    bool fill_current, stroke_current;  /* currentColor: the element's own color */
    int font_px;                /* (SVG text's font-size as given, px) */
    enum font_family family;    /* (27k, inherited) */
    /* (27m) */
    int height_pct, height_add; /* height: p% (+ pixels) of the containing block's height, or 0 */
    bool translated;            /* transform: translate(...) given */
    int tx, ty;                 /* its pixels */
    int tx_pct, ty_pct;         /* and percentages of the element's own box */
    int min_height, min_height_pct;     /* px (0: none), or p% of the containing block's height */
    int max_height, max_height_pct;     /* px (-1: none), or p% */
    /* (27n) grids: the container's templates as written (malloc'ed, or 0) */
    bool grid;                  /* display: grid (DISPLAY_BLOCK) or inline-grid (DISPLAY_INLINE_BLOCK) */
    char *grid_cols, *grid_rows, *grid_areas, *grid_auto_cols, *grid_auto_rows;
    bool grid_by_column;        /* grid-auto-flow: column */
    enum flex_align justify_items, justify_self;
    /* an item's place: lines (1 the first, negative from the end, 0 auto) and spans */
    int col_start, col_end, col_span, row_start, row_end, row_span;
    char *grid_area;            /* grid-area: NAME (malloc'ed), or 0 */
    bool nowrap;                /* (27n) white-space: nowrap or pre: lines break only where the text does
                                   (inherited) */
};

struct sheet;

struct sheet *css_parse(const char *text, struct sheet *append);
void css_free(struct sheet *s);
/* Give every element of the tree its style: the browser's own sheet, then
 * `author`'s rules (in order), then style="" attributes, for a window of
 * width x height pixels (@media, vw, vh); ::before and ::after become
 * children tagged "::before" and "::after". Again after a resize. */
void css_apply(struct node *root, const struct sheet *author, int width, int height);
void css_style_free(struct style *s);
extern bool css_scripting;                      /* (27q) the page's scripts run: <noscript> is not shown */
/* "red", "#f00", "rgb(255,0,0)": 0xRRGGBB, or false. */
bool css_color(const char *text, uint32_t *out);
/* (27h) the same with its alpha (0-255: transparent is 0, *out then 0). */
bool css_color_alpha(const char *text, uint32_t *out, int *alpha);
/* (27h) a style sheet's url(...)s made absolute against its own address:
 * malloc'ed. */
char *css_absolute_urls(const char *text, const char *base);
/* (27h) linear-gradient(...) (radial and repeating ones roughly) into g,
 * its colours mixed over `under`: false if it cannot be read. */
bool css_gradient(const char *text, struct gradient *g, uint32_t under);
/* (27h) the address in a background-image's url(...): malloc'ed, or 0. */
char *css_url(const char *value);

/* (27p) Selectors for scripts (querySelector, matches, closest): 0 if the
 * text is not a valid selector list. */
struct selector_list;
struct selector_list *css_selectors(const char *text);
bool css_selectors_match(const struct selector_list *l, const struct node *e);
void css_selectors_free(struct selector_list *l);
/* (27p) matchMedia's query for a window of width x height. */
bool css_media_matches(const char *query, int width, int height);

/* (27n) A length in CSS text ("12px", "2em", "50%" of percent_of,
 * calc(...)): false if it is none. */
bool css_length_px(const char *text, int em, int percent_of, int *out);

/* --- dom.c (27p): scripts and the document's objects --- */
/* An address in the outermost frame of the calling thread (a local of
 * main, of a thread's function): every thread that calls dom_* sets its
 * own first (27q). */
void dom_set_stack_base(void *p);
/* The page `tree` at url (its window width x height); with scripting, a
 * script world for it (pages of the browser's own have none). */
void dom_start(struct node *tree, const char *url, int width, int height, bool scripting);
void dom_run_scripts(void);                     /* the page's <script>s, in document order */
void dom_end(void);                             /* before the tree is freed */
extern void (*dom_layout_hook)(void);           /* styles and lays the page out now (for geometry) */
extern bool dom_changed;                        /* a script changed the tree or an attribute */
extern bool dom_print;                          /* script errors on standard output (ibrowser -t) */
/* (27q) Script files: the window program's, so it never waits for the
 * network with the page's lock: 1 with *r filled (a copy of its own), 0
 * failed (r->error), -1 on its way (the script waits; dom_run_waiting runs
 * it once the file is there). Without a hook: http_get. */
extern int (*dom_fetch_hook)(const char *url, struct response *r);
/* (27r) A script's request (fetch, XMLHttpRequest): dom.c checks and keeps
 * it; the window program makes it on a thread of its own: dom_perform
 * there (the network: no script world, no lock), then dom_request_done
 * with the page's lock (the script's callback; it frees the request).
 * The hook returns 0 if it could not start one. */
struct dom_request;
extern int (*dom_request_hook)(struct dom_request *q);
void dom_perform(struct dom_request *q);
void dom_request_done(struct dom_request *q);
extern const char dom_prelude[];                 /* (prelude.c) */
void dom_run_waiting(void);
bool dom_script_runnable(const struct node *s);  /* a type that runs (not a module, not JSON) */
extern char dom_navigate_to[URL_MAX];           /* where a script sent the page (location), or "" */

/* (27q) Events. What one from the user carries (0s when nothing): */
struct dom_event {
    int x, y;                                   /* the pointer in the page's view (clientX, clientY) */
    int button, buttons;                        /* MouseEvent's: 0 left, 1 middle, 2 right; held 1, 4, 2 */
    int mods;                                   /* DOM_CTRL | DOM_SHIFT | DOM_ALT | DOM_META */
    int detail;                                 /* clicks in a row */
    const char *key, *code;                     /* KeyboardEvent's ("a", "KeyA") */
    int key_code, char_code;
    bool repeat;
    struct node *related;                       /* mouseover's and the like's other element */
    int delta_y;                                /* WheelEvent's, in pixels */
    const char *data, *input_type;              /* InputEvent's */
    struct node *submitter;                     /* SubmitEvent's */
};
enum { DOM_CTRL = 1, DOM_SHIFT = 2, DOM_ALT = 4, DOM_META = 8 };
/* Fire the browser's event `type` (kind, bubbles and cancelable as the
 * web's) at target (an element, dom_document(), or 0: the window): false
 * if a listener canceled it. Cheap when nothing listens to the type. */
bool dom_fire(struct node *target, const char *type, const struct dom_event *e);
bool dom_wants(const char *type);               /* something on the page listens to type */
struct node *dom_document(void);
/* A click on e (the user's: d, or 0 for a script's): the click event, with
 * a check box or radio button changed first (back if canceled), then what
 * it activates: a link sets dom_navigate_to (dom_navigate_link), a submit
 * button its form's submit event and dom_submit_*, a reset button resets,
 * a label clicks its control. False if canceled. */
bool dom_click(struct node *e, const struct dom_event *d);
/* The form's submit event, then (unless canceled) dom_submit_* set. */
void dom_submit(struct node *form, struct node *submitter);
void dom_loaded(int stage);                     /* 1: DOMContentLoaded (parsed), 2: load (all in) */
void dom_resize(int width, int height);         /* the window's new size, and its resize event */
void dom_set_url(const char *url);              /* the page's address now (a #part changed) */
long dom_next_timer(void);                      /* ms until a timer is due (0: now), or -1 for none */
void dom_run_timers(void);                      /* those due now */
extern bool dom_navigate_link;                  /* dom_navigate_to is a link followed */
extern int dom_scroll_y;                        /* the page's scroll (the window program keeps it) */
extern int dom_scroll_request;                  /* where a script asked to scroll to, or -1 */
extern struct node *dom_focused;                /* the control with the keyboard (the program's) */
extern void (*dom_focus_hook)(struct node *e);  /* a script's focus() (e) or blur() (0) */
extern void (*dom_control_hook)(struct node *e);/* a control changed by a click (or a form reset) */
extern void (*dom_timers_hook)(void);           /* a timer was set (wake the thread running them) */
extern bool dom_submit_pending;                 /* a form to send: dom_submit_form, by dom_submitter (or 0) */
extern struct node *dom_submit_form, *dom_submitter;

/* --- svg.c (27j) --- */
/* An <svg> element's size as it asks (width/height, else its viewBox, else
 * 300 x 150; percentages: of nothing, so the viewBox's). */
void svg_size(const struct node *svg, int *w, int *h);
/* Draw the styled <svg> tree at w x h: a picture (malloc'ed, image_free and
 * free it), or 0. */
struct image *svg_draw(const struct node *svg, int w, int h);
/* An SVG file's text drawn at its own size (or w x h when given, > 0). */
struct image *svg_load(const char *text, size_t n, int w, int h);
bool svg_looks_like(const char *text, size_t n, const char *type);

/* --- layout.c --- */
enum item_kind { ITEM_FILL, ITEM_TEXT, ITEM_PICTURE, ITEM_CONTROL,
                 ITEM_BOX };            /* (27d) an inline-block's place on its line: not drawn */

struct item {
    enum item_kind kind;
    int x, y, w, h;             /* in the page (y from its top) */
    uint32_t color;             /* fill and text */
    const struct font *font;
    char *text;                 /* ITEM_TEXT */
    bool underline, strike;
    const struct image *picture;
    int link;                   /* the index of its link, or -1 */
    int scale;                  /* text drawn 1 or 2 times its font's size */
    bool fake_bold;             /* drawn twice a pixel apart (a font without a bold face) */
    int line;                   /* the line it is on (in reading order) */
    bool decoration;            /* a background or border (not content) */
    struct node *node;          /* ITEM_CONTROL: the control (its state: node->control) */
    /* (27d) */
    int band;                   /* its painting layer as laid out (0: in the flow) */
    bool fixed;                 /* position: fixed: y is in the window, it does not scroll */
    bool rider;                 /* inside an inline-block: y = its box's y + ry */
    int leader, ry;
    /* (27h) */
    int lead;                   /* text: half its line-height's leading (end_line places it) */
    int raise;                  /* vertical-align: pixels up from the baseline */
    enum vertical_align valign; /* pictures, controls, boxes: top, middle, bottom of the line */
    bool clipped;               /* drawn only inside clip (overflow other than visible) */
    int clip[4];                /* x0, y0, x1, y1 in the page */
    bool hidden;                /* visibility: hidden, opacity: 0 (laid out, not drawn) */
    int radius[4];              /* ITEM_FILL: rounded corners */
    int ring;                   /* ITEM_FILL: only an outline this thick (a rounded border) */
    int soft;                   /* ITEM_FILL: a shadow's blur: drawn grown by it in color2 */
    uint32_t color2;
    struct gradient *gradient;  /* ITEM_FILL: drawn as this gradient (the page's to free) */
    bool tiled;                 /* ITEM_PICTURE: a background: the picture tile_w x tile_h, */
    int tile_w, tile_h;         /*   from (tile_x, tile_y), repeated as `repeat` says */
    int tile_x, tile_y;
    enum bg_repeat repeat;
    int group;                  /* the inline box it is in (words merge only within one) */
    struct node *owner;         /* (27q) the element it is part of (what a click there is on) */
};

#define GRADIENT_STOPS 8
struct gradient {               /* (27h) a linear gradient, its colours blended already */
    bool across;                /* left to right (else top to bottom) */
    bool reverse;               /* (to top, to left) */
    int count;
    uint32_t colors[GRADIENT_STOPS];
    int at[GRADIENT_STOPS];     /* 0-1000 along it */
};

struct link {
    char *href;                 /* absolute */
};

struct anchor {                 /* (27b) where an id="" or <a name=""> is */
    char *name;
    int y;
};

struct page {
    struct item *items;
    int count, room;
    struct link *links;
    int link_count, link_room;
    struct anchor *anchors;
    int anchor_count, anchor_room;
    int height;                 /* in pixels */
    int width;
    int *order;                 /* (27d) the items in painting order (CSS 2.1 appendix E) */
    uint32_t background;
    char title[200];
};

/* The font for a style: the misc-fixed face nearest its size, drawn
 * at *scale (2 for the largest), *fake_bold if it has no bold face. */
const struct font *style_font(const struct style *s, int *scale, bool *fake_bold);

/* Lay the styled tree out at `width` pixels, in a view `view_h` high (what
 * position: fixed is placed in); base: the page's address (links become
 * absolute). */
void layout(struct node *root, int width, int view_h, const char *base, struct page *out);
void page_free(struct page *p);
/* The y of the element a "#part" names (its id, else an <a name>), or -1. */
int page_anchor(const struct page *p, const char *name);
/* A form control's box, w x h pixels (27c). */
void control_box(struct node *e, int *w, int *h);
/* The link at (x, y) of the page scrolled by `scroll` (the topmost one
 * painted there), or -1. */
int page_link_at(const struct page *p, int x, int y, int scroll);
/* (27q) The element at (x, y) of the page scrolled by `scroll` (what a
 * click there is on), or 0 for none (the caller's: the body). */
struct node *page_node_at(const struct page *p, struct node *root, int x, int y, int scroll);
/* Item k's top as the window shows it with the page scrolled by `scroll`. */
int item_top(const struct item *it, int scroll);
/* The page's text as lines (what a reader sees, top to bottom), one per
 * printed line with prefix: for the tests and `-t`. */
void page_print_text(const struct page *p, const char *prefix);

#endif
