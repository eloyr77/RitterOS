/*
 * (43a) search.h: linear search, an AVL tree (tsearch's: every node keeps
 * its height; insertions and deletions rebalance on the way back up), an
 * open-addressing hash table (hsearch's, never shrinking: POSIX has no
 * removal), and insque/remque.
 */
#include <errno.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>

void *lfind(const void *key, const void *base, size_t *count, size_t size,
            int (*compare)(const void *a, const void *b))
{
    const char *p = base;
    for (size_t i = 0; i < *count; i++, p += size)
        if (compare(key, p) == 0)
            return (void *)p;
    return 0;
}

void *lsearch(const void *key, void *base, size_t *count, size_t size,
              int (*compare)(const void *a, const void *b))
{
    void *found = lfind(key, base, count, size, compare);
    if (found)
        return found;
    void *end = (char *)base + *count * size;
    memcpy(end, key, size);
    ++*count;
    return end;
}

/* The tree. A node's first member is the key (callers read *(void **)node). */
struct node {
    const void *key;
    struct node *left, *right;
    int height;
};

static int height(const struct node *n) { return n ? n->height : 0; }

static void fix_height(struct node *n)
{
    int l = height(n->left), r = height(n->right);
    n->height = (l > r ? l : r) + 1;
}

static struct node *rotate_right(struct node *n)
{
    struct node *l = n->left;
    n->left = l->right;
    l->right = n;
    fix_height(n);
    fix_height(l);
    return l;
}

static struct node *rotate_left(struct node *n)
{
    struct node *r = n->right;
    n->right = r->left;
    r->left = n;
    fix_height(n);
    fix_height(r);
    return r;
}

static struct node *balance(struct node *n)
{
    fix_height(n);
    int d = height(n->left) - height(n->right);
    if (d > 1) {
        if (height(n->left->left) < height(n->left->right))
            n->left = rotate_left(n->left);
        return rotate_right(n);
    }
    if (d < -1) {
        if (height(n->right->right) < height(n->right->left))
            n->right = rotate_right(n->right);
        return rotate_left(n);
    }
    return n;
}

/* key into the subtree n; *found the node holding it (new or old), 0 when
 * memory ran out. The subtree's new root. */
static struct node *insert(struct node *n, const void *key,
                           int (*compare)(const void *, const void *), struct node **found)
{
    if (!n) {
        n = malloc(sizeof(*n));
        if (n) {
            n->key = key;
            n->left = n->right = 0;
            n->height = 1;
        }
        *found = n;
        return n;
    }
    int c = compare(key, n->key);
    if (c == 0) {
        *found = n;
        return n;
    }
    if (c < 0)
        n->left = insert(n->left, key, compare, found);
    else
        n->right = insert(n->right, key, compare, found);
    return balance(n);
}

void *tsearch(const void *key, void **root, int (*compare)(const void *a, const void *b))
{
    if (!root)
        return 0;
    struct node *found = 0;
    *root = insert(*root, key, compare, &found);
    return found;
}

void *tfind(const void *key, void *const *root, int (*compare)(const void *a, const void *b))
{
    if (!root)
        return 0;
    struct node *n = *root;
    while (n) {
        int c = compare(key, n->key);
        if (c == 0)
            return n;
        n = c < 0 ? n->left : n->right;
    }
    return 0;
}

/* The smallest node of n taken out: its key into *key, the rest's root. */
static struct node *take_smallest(struct node *n, const void **key, struct node **taken)
{
    if (!n->left) {
        *key = n->key;
        *taken = n;
        return n->right;
    }
    n->left = take_smallest(n->left, key, taken);
    return balance(n);
}

/* key out of n: *parent_of gets the deleted node's parent (or the node
 * that took its place), as POSIX's tdelete returns. */
static struct node *delete(struct node *n, const void *key,
                           int (*compare)(const void *, const void *), struct node *parent,
                           struct node **result, int *gone)
{
    if (!n)
        return 0;
    int c = compare(key, n->key);
    if (c < 0) {
        n->left = delete(n->left, key, compare, n, result, gone);
    } else if (c > 0) {
        n->right = delete(n->right, key, compare, n, result, gone);
    } else {
        *gone = 1;
        *result = parent;
        if (!n->left || !n->right) {
            struct node *child = n->left ? n->left : n->right;
            free(n);
            return child;
        }
        struct node *taken;
        const void *next_key;
        n->right = take_smallest(n->right, &next_key, &taken);
        free(taken);
        n->key = next_key;                      /* (this node stays, with the next key) */
        if (!parent)
            *result = n;
    }
    return balance(n);
}

void *tdelete(const void *key, void **root, int (*compare)(const void *a, const void *b))
{
    if (!root || !*root)
        return 0;
    struct node *result = 0;
    int gone = 0;
    static struct node sentinel;                /* (the root deleted: a non-null answer) */
    *root = delete(*root, key, compare, 0, &result, &gone);
    if (!gone)
        return 0;
    return result ? (void *)result : *root ? *root : (void *)&sentinel;
}

static void walk(const struct node *n, void (*action)(const void *, VISIT, int), int depth)
{
    if (!n->left && !n->right) {
        action(n, leaf, depth);
        return;
    }
    action(n, preorder, depth);
    if (n->left)
        walk(n->left, action, depth + 1);
    action(n, postorder, depth);
    if (n->right)
        walk(n->right, action, depth + 1);
    action(n, endorder, depth);
}

void twalk(const void *root, void (*action)(const void *node, VISIT visit, int depth))
{
    if (root && action)
        walk(root, action, 0);
}

void tdestroy(void *root, void (*free_key)(void *key))
{
    struct node *n = root;
    if (!n)
        return;
    tdestroy(n->left, free_key);
    tdestroy(n->right, free_key);
    if (free_key)
        free_key((void *)n->key);
    free(n);
}

/* The hash table: entries by the key's hash, the next free place on a
 * collision; at most 3/4 full (hcreate rounds the size up for that). */
struct ritteros_htab {
    ENTRY entry;
    int used;
};

static size_t hash_of(const char *s)
{
    size_t h = 5381;
    while (*s)
        h = h * 33 + (unsigned char)*s++;
    return h;
}

int hcreate_r(size_t count, struct hsearch_data *table)
{
    size_t size = 16;
    while (size < count + count / 3 + 1)
        size *= 2;
    table->table = calloc(size, sizeof(struct ritteros_htab));
    if (!table->table) {
        errno = ENOMEM;
        return 0;
    }
    table->size = size;
    table->filled = 0;
    return 1;
}

void hdestroy_r(struct hsearch_data *table)
{
    free(table->table);
    table->table = 0;
    table->size = table->filled = 0;
}

int hsearch_r(ENTRY item, ACTION action, ENTRY **found, struct hsearch_data *table)
{
    if (!table->table) {
        errno = ESRCH;
        *found = 0;
        return 0;
    }
    size_t mask = table->size - 1, i = hash_of(item.key) & mask;
    for (;;) {
        struct ritteros_htab *e = &table->table[i];
        if (!e->used)
            break;
        if (strcmp(e->entry.key, item.key) == 0) {
            *found = &e->entry;
            return 1;
        }
        i = (i + 1) & mask;
    }
    if (action == FIND || table->filled * 4 >= table->size * 3) {
        errno = action == FIND ? ESRCH : ENOMEM;
        *found = 0;
        return 0;
    }
    table->table[i].entry = item;
    table->table[i].used = 1;
    table->filled++;
    *found = &table->table[i].entry;
    return 1;
}

static struct hsearch_data global_table;

int hcreate(size_t count)
{
    return hcreate_r(count, &global_table);
}

void hdestroy(void)
{
    hdestroy_r(&global_table);
}

ENTRY *hsearch(ENTRY item, ACTION action)
{
    ENTRY *found;
    return hsearch_r(item, action, &found, &global_table) ? found : 0;
}

struct queue_element {
    struct queue_element *next, *previous;
};

void insque(void *element, void *previous)
{
    struct queue_element *e = element, *p = previous;
    if (!p) {
        e->next = e->previous = 0;
        return;
    }
    e->next = p->next;
    e->previous = p;
    p->next = e;
    if (e->next)
        e->next->previous = e;
}

void remque(void *element)
{
    struct queue_element *e = element;
    if (e->next)
        e->next->previous = e->previous;
    if (e->previous)
        e->previous->next = e->next;
}
