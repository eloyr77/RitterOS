/*
 * search.h (43a): POSIX's searching tables.
 * - lfind/lsearch: a linear search of *count elements of size bytes
 *   (lsearch adds key at the end when it is not there);
 * - tsearch/tfind/tdelete/twalk (and GNU's tdestroy): a balanced binary
 *   tree (AVL) of keys, rooted at *root; tsearch adds key when it is not
 *   there; the result points to the node, whose first member is the key
 *   pointer; twalk calls action for each node in preorder, postorder,
 *   endorder (and leaf for leaves) with the depth;
 * - hcreate/hsearch/hdestroy: one hash table of at least count entries,
 *   ENTER or FIND by a string key (and the _r forms with the caller's
 *   table);
 * - insque/remque: a doubly linked queue of elements whose first two
 *   members are the next and previous pointers.
 */
#ifndef RITTEROS_SEARCH_H
#define RITTEROS_SEARCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void *lfind(const void *key, const void *base, size_t *count, size_t size,
            int (*compare)(const void *a, const void *b));
void *lsearch(const void *key, void *base, size_t *count, size_t size,
              int (*compare)(const void *a, const void *b));

typedef enum { preorder, postorder, endorder, leaf } VISIT;
void *tsearch(const void *key, void **root, int (*compare)(const void *a, const void *b));
void *tfind(const void *key, void *const *root, int (*compare)(const void *a, const void *b));
void *tdelete(const void *key, void **root, int (*compare)(const void *a, const void *b));
void twalk(const void *root, void (*action)(const void *node, VISIT visit, int depth));
void tdestroy(void *root, void (*free_key)(void *key));

typedef struct entry {
    char *key;
    void *data;
} ENTRY;
typedef enum { FIND, ENTER } ACTION;
struct hsearch_data {
    struct ritteros_htab *table;
    size_t size, filled;
};
int hcreate(size_t count);
void hdestroy(void);
ENTRY *hsearch(ENTRY item, ACTION action);
int hcreate_r(size_t count, struct hsearch_data *table);
void hdestroy_r(struct hsearch_data *table);
int hsearch_r(ENTRY item, ACTION action, ENTRY **found, struct hsearch_data *table);

void insque(void *element, void *previous);
void remque(void *element);

#ifdef __cplusplus
}
#endif

#endif
