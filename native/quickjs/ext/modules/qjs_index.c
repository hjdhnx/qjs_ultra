/*
 * qjs_index.c — Hash index acceleration layer implementation.
 *
 * Builds three hash tables on top of a lexbor document:
 *   id_index:    "main"       → [node]          (typically 1 node)
 *   class_index: "container"  → [node1, node2, ...]
 *   tag_index:   "div"        → [node1, node2, ...]
 *
 * Optimizations:
 *   - Arena allocator for entries/keys (eliminates hundreds of small mallocs)
 *   - Single-pass lowercase+hash in index building (no temp buffer)
 *   - Stack buffers in query path (no malloc/free per query)
 *   - lxb_tag_id_t integer comparison instead of string comparison
 *   - Borrowed pointer for no-filter queries (no malloc+copy)
 *
 * Fast path handles: #id, .class, tag, tag.class, #id.class, tag#id
 * Everything else falls through to lxb_selectors_find.
 */

#define _GNU_SOURCE
#include "qjs_index.h"
#include <lexbor/tag/tag.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ========================================================================
 * Arena allocator — eliminates per-entry malloc overhead during indexing.
 * Uses multiple fixed blocks (never realloc) so existing pointers stay valid.
 * ======================================================================== */

#define QJS_ARENA_BLOCK_SIZE 8192

typedef struct {
    char *blocks[32];
    int n_blocks;
    size_t cur_pos;    /* offset in current block */
} qjs_arena_t;

static void *qjs_arena_alloc(qjs_arena_t *a, size_t size)
{
    size_t aligned = (size + 7) & ~(size_t)7;  /* 8-byte align */

    /* If size > block size, allocate as its own block */
    if (aligned > QJS_ARENA_BLOCK_SIZE) {
        if (a->n_blocks >= 32) return NULL;
        char *block = (char *)malloc(aligned);
        if (!block) return NULL;
        /* Store oversized block — it will be freed in destroy */
        a->blocks[a->n_blocks++] = block;
        return block;
    }

    /* Check if current block has room */
    if (a->n_blocks == 0 || a->cur_pos + aligned > QJS_ARENA_BLOCK_SIZE) {
        if (a->n_blocks >= 32) return NULL;
        char *block = (char *)malloc(QJS_ARENA_BLOCK_SIZE);
        if (!block) return NULL;
        a->blocks[a->n_blocks++] = block;
        a->cur_pos = 0;
    }

    void *p = a->blocks[a->n_blocks - 1] + a->cur_pos;
    a->cur_pos += aligned;
    return p;
}

static void qjs_arena_destroy(qjs_arena_t *a)
{
    for (int i = 0; i < a->n_blocks; i++)
        free(a->blocks[i]);
    a->n_blocks = 0;
    a->cur_pos = 0;
}

/* ========================================================================
 * Hash table primitives
 * ======================================================================== */

/* djb2 hash */
static unsigned int qjs_hash_str(const char *str, size_t len)
{
    unsigned int h = 5381;
    for (size_t i = 0; i < len; i++)
        h = ((h << 5) + h) + (unsigned char)str[i];
    return h;
}

/* Dynamic node array — uses individual malloc (needs realloc) */
typedef struct {
    lxb_dom_node_t **nodes;
    size_t count;
    size_t cap;
} qjs_node_arr_t;

static void qjs_arr_push(qjs_node_arr_t *arr, lxb_dom_node_t *node)
{
    if (arr->count >= arr->cap) {
        size_t nc = (arr->cap + 8) * 2;
        lxb_dom_node_t **nn = (lxb_dom_node_t **)realloc(arr->nodes,
                            nc * sizeof(lxb_dom_node_t *));
        if (!nn) return;
        arr->nodes = nn;
        arr->cap = nc;
    }
    arr->nodes[arr->count++] = node;
}

/* Hash entry (chaining) — entries and keys allocated from arena */
typedef struct qjs_idx_entry {
    char *key;                      /* arena-allocated, lowercased */
    size_t keylen;                  /* cached key length */
    qjs_node_arr_t arr;             /* node array (individually malloc'd) */
    struct qjs_idx_entry *next;
} qjs_idx_entry_t;

/* Hash table */
typedef struct {
    qjs_idx_entry_t **buckets;
    size_t nbuckets;
    size_t count;
} qjs_htab_t;

#define QJS_INITIAL_BUCKETS 64

static bool qjs_htab_init(qjs_htab_t *ht)
{
    ht->buckets = (qjs_idx_entry_t **)calloc(QJS_INITIAL_BUCKETS, sizeof(qjs_idx_entry_t *));
    if (!ht->buckets) return false;
    ht->nbuckets = QJS_INITIAL_BUCKETS;
    ht->count = 0;
    return true;
}

static qjs_idx_entry_t *qjs_htab_lookup(qjs_htab_t *ht, const char *key, size_t keylen)
{
    unsigned int h = qjs_hash_str(key, keylen) % ht->nbuckets;
    for (qjs_idx_entry_t *e = ht->buckets[h]; e; e = e->next) {
        if (e->keylen == keylen && memcmp(e->key, key, keylen) == 0)
            return e;
    }
    return NULL;
}

/*
 * Insert or find; accepts pre-computed hash to avoid re-hashing.
 * Entries and keys are arena-allocated (no individual free needed).
 */
static qjs_idx_entry_t *qjs_htab_intern(qjs_htab_t *ht, qjs_arena_t *arena,
    const char *key, size_t keylen, unsigned int hash)
{
    /* Check if resize needed (load factor > 0.75) */
    if (ht->count * 4 >= ht->nbuckets * 3) {
        size_t new_nb = ht->nbuckets * 2;
        qjs_idx_entry_t **new_bk = (qjs_idx_entry_t **)calloc(new_nb, sizeof(qjs_idx_entry_t *));
        if (!new_bk) {
            /* Continue with current size, just less efficient */
        } else {
            for (size_t i = 0; i < ht->nbuckets; i++) {
                qjs_idx_entry_t *e = ht->buckets[i];
                while (e) {
                    qjs_idx_entry_t *next = e->next;
                    unsigned int h = qjs_hash_str(e->key, e->keylen) % new_nb;
                    e->next = new_bk[h];
                    new_bk[h] = e;
                    e = next;
                }
            }
            free(ht->buckets);
            ht->buckets = new_bk;
            ht->nbuckets = new_nb;
        }
    }

    unsigned int bucket = hash % ht->nbuckets;
    for (qjs_idx_entry_t *e = ht->buckets[bucket]; e; e = e->next) {
        if (e->keylen == keylen && memcmp(e->key, key, keylen) == 0)
            return e;
    }

    qjs_idx_entry_t *e = (qjs_idx_entry_t *)qjs_arena_alloc(arena, sizeof(*e));
    if (!e) return NULL;
    e->key = (char *)qjs_arena_alloc(arena, keylen + 1);
    if (!e->key) return NULL;
    memcpy(e->key, key, keylen);
    e->key[keylen] = '\0';
    e->keylen = keylen;
    e->arr.nodes = NULL; e->arr.count = 0; e->arr.cap = 0;
    e->next = ht->buckets[bucket];
    ht->buckets[bucket] = e;
    ht->count++;
    return e;
}

/* Destroy: free node arrays and bucket array. Entries/keys are in arena. */
static void qjs_htab_destroy(qjs_htab_t *ht)
{
    if (!ht->buckets) return;
    for (size_t i = 0; i < ht->nbuckets; i++) {
        qjs_idx_entry_t *e = ht->buckets[i];
        while (e) {
            qjs_idx_entry_t *next = e->next;
            free(e->arr.nodes);  /* only node arrays need individual free */
            e = next;
        }
    }
    free(ht->buckets);
    ht->buckets = NULL;
}

/* ========================================================================
 * Node metadata — cached per-element data for O(1) verification
 * (inspired by Nordstjernen's ns_node direct field access)
 * ======================================================================== */

typedef struct {
    lxb_dom_node_t *node;
    lxb_tag_id_t tag_id;            /* cached tag_id (LXB_TAG__UNDEF if unknown) */
    const lxb_char_t *id;           /* borrowed pointer to id attribute value */
    size_t id_len;                   /* 0 if no id */
    const lxb_char_t *cls;          /* borrowed pointer to class attribute value */
    size_t cls_len;                  /* 0 if no class */
} qjs_node_meta_t;

/* ========================================================================
 * Index structure
 * ======================================================================== */

struct qjs_idx {
    qjs_htab_t id_index;
    qjs_htab_t class_index;
    qjs_htab_t tag_index;
    qjs_arena_t arena;                    /* arena for entries and keys */
    lxb_html_document_t *document;        /* for lxb_tag_id_by_name */
    size_t total_elements;                /* total element nodes indexed */
    bool dirty;                           /* set true after DOM modifications */

    /* Per-node metadata cache (ns_node style: O(1) field access).
     * Populated during index building, used during verification.
     * Eliminates all lxb_dom_element_get_attribute calls in hot path. */
    qjs_node_meta_t *meta_buckets;        /* open-addressing hash table */
    size_t meta_nbuckets;                 /* power of 2 */
    size_t meta_count;
};

/* ========================================================================
 * Node metadata cache — ns_node inspired
 *
 * Maps lxb_dom_node_t* → {tag_id, id, class} with O(1) lookup.
 * All pointers are borrowed from lexbor (valid for document lifetime).
 * This replaces per-query lxb_dom_element_get_attribute calls (which
 * traverse a linked list) with direct field access.
 * ======================================================================== */

#define QJS_META_LOAD_FACTOR 0.6

static bool qjs_meta_init(qjs_idx_t *idx)
{
    /* Start with enough buckets for expected element count */
    size_t nb = 256;
    idx->meta_buckets = (qjs_node_meta_t *)calloc(nb, sizeof(qjs_node_meta_t));
    if (!idx->meta_buckets) return false;
    idx->meta_nbuckets = nb;
    idx->meta_count = 0;
    return true;
}

/* Hash a node pointer → bucket index */
static inline size_t qjs_meta_hash(const lxb_dom_node_t *node, size_t nbuckets)
{
    /* Mix the pointer bits — lower bits are aligned, so shift */
    uintptr_t v = (uintptr_t)node;
    v = (v >> 4) ^ (v >> 12) ^ (v >> 20);
    return (size_t)(v & (nbuckets - 1));
}

/* Insert metadata for a node. Called once per element during index building. */
static void qjs_meta_put(qjs_idx_t *idx, lxb_dom_node_t *node,
                         lxb_tag_id_t tag_id,
                         const lxb_char_t *id, size_t id_len,
                         const lxb_char_t *cls, size_t cls_len)
{
    /* Resize if load factor exceeded */
    if (idx->meta_count * 10 >= idx->meta_nbuckets * 6) {
        size_t new_nb = idx->meta_nbuckets * 2;
        qjs_node_meta_t *new_bk = (qjs_node_meta_t *)calloc(new_nb, sizeof(qjs_node_meta_t));
        if (!new_bk) return;  /* continue with old table */
        for (size_t i = 0; i < idx->meta_nbuckets; i++) {
            if (idx->meta_buckets[i].node) {
                size_t h = qjs_meta_hash(idx->meta_buckets[i].node, new_nb);
                while (new_bk[h].node) h = (h + 1) & (new_nb - 1);
                new_bk[h] = idx->meta_buckets[i];
            }
        }
        free(idx->meta_buckets);
        idx->meta_buckets = new_bk;
        idx->meta_nbuckets = new_nb;
    }

    size_t h = qjs_meta_hash(node, idx->meta_nbuckets);
    while (idx->meta_buckets[h].node) {
        if (idx->meta_buckets[h].node == node) return;  /* already cached */
        h = (h + 1) & (idx->meta_nbuckets - 1);
    }
    idx->meta_buckets[h].node = node;
    idx->meta_buckets[h].tag_id = tag_id;
    idx->meta_buckets[h].id = id;
    idx->meta_buckets[h].id_len = id_len;
    idx->meta_buckets[h].cls = cls;
    idx->meta_buckets[h].cls_len = cls_len;
    idx->meta_count++;
}

/* Lookup metadata for a node. Returns NULL if not cached. */
static inline const qjs_node_meta_t *qjs_meta_get(const qjs_idx_t *idx,
                                                  const lxb_dom_node_t *node)
{
    if (!idx->meta_buckets) return NULL;
    size_t h = qjs_meta_hash(node, idx->meta_nbuckets);
    while (idx->meta_buckets[h].node) {
        if (idx->meta_buckets[h].node == node)
            return &idx->meta_buckets[h];
        h = (h + 1) & (idx->meta_nbuckets - 1);
    }
    return NULL;
}

/* ========================================================================
 * Index building — walk lexbor tree once, register all elements
 * Single-pass lowercase+hash, no temp buffer allocation
 * ======================================================================== */

static void qjs_register_element(qjs_idx_t *idx, lxb_dom_node_t *node)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    idx->total_elements++;

    /* Resolve tag_id once (for later O(1) integer comparison) */
    lxb_tag_id_t tag_id = lxb_dom_element_tag_id(el);

    /* Get id attribute (borrowed pointer — valid for document lifetime) */
    size_t id_len;
    const lxb_char_t *id = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"id", 2, &id_len);

    /* Get class attribute (borrowed pointer) */
    size_t cls_len;
    const lxb_char_t *cls = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"class", 5, &cls_len);

    /* Cache metadata for this node (ns_node style — O(1) lookup later) */
    qjs_meta_put(idx, node, tag_id,
                 id ? id : (const lxb_char_t *)"", id ? id_len : 0,
                 cls ? cls : (const lxb_char_t *)"", cls ? cls_len : 0);

    /* Register tag name — single pass: lowercase + hash into stack buffer */
    size_t tag_len;
    const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &tag_len);
    if (tag && tag_len > 0 && tag_len < 256) {
        char lower[256];
        unsigned int h = 5381;
        for (size_t i = 0; i < tag_len; i++) {
            lower[i] = (char)tolower((unsigned char)tag[i]);
            h = ((h << 5) + h) + (unsigned char)lower[i];
        }
        qjs_idx_entry_t *e = qjs_htab_intern(&idx->tag_index, &idx->arena,
                                              lower, tag_len, h);
        if (e) qjs_arr_push(&e->arr, node);
    }

    /* Register id attribute — use cached value from meta_put above */
    if (id && id_len > 0) {
        unsigned int h = qjs_hash_str((const char *)id, id_len);
        qjs_idx_entry_t *e = qjs_htab_intern(&idx->id_index, &idx->arena,
            (const char *)id, id_len, h);
        if (e) qjs_arr_push(&e->arr, node);
    }

    /* Register class attribute — use cached value from meta_put above */
    if (cls && cls_len > 0) {
        const char *p = (const char *)cls;
        const char *end = p + cls_len;
        while (p < end) {
            while (p < end && isspace((unsigned char)*p)) p++;
            if (p >= end) break;

            /* Lowercase + hash in one pass */
            char lower[256];
            unsigned int h = 5381;
            size_t clen = 0;
            while (p < end && !isspace((unsigned char)*p) && clen < 255) {
                char c = (char)tolower((unsigned char)*p);
                lower[clen++] = c;
                h = ((h << 5) + h) + (unsigned char)c;
                p++;
            }
            /* Skip rest of class name if >255 chars */
            while (p < end && !isspace((unsigned char)*p)) p++;

            if (clen > 0) {
                qjs_idx_entry_t *e = qjs_htab_intern(&idx->class_index,
                    &idx->arena, lower, clen, h);
                if (e) qjs_arr_push(&e->arr, node);
            }
        }
    }
}

/* Iterative DFS to avoid stack overflow on deep documents */
static void qjs_walk_tree(qjs_idx_t *idx, lxb_dom_node_t *root)
{
    lxb_dom_node_t **stack = (lxb_dom_node_t **)malloc(256 * sizeof(lxb_dom_node_t *));
    if (!stack) return;
    size_t sp = 0, cap = 256;

    stack[sp++] = root;
    while (sp > 0) {
        lxb_dom_node_t *node = stack[--sp];
        qjs_register_element(idx, node);

        lxb_dom_node_t *child = node->last_child;
        while (child) {
            if (sp >= cap) {
                cap *= 2;
                lxb_dom_node_t **ns = (lxb_dom_node_t **)realloc(stack,
                    cap * sizeof(lxb_dom_node_t *));
                if (!ns) { free(stack); return; }
                stack = ns;
            }
            stack[sp++] = child;
            child = child->prev;
        }
    }
    free(stack);
}

qjs_idx_t *qjs_index_build(lxb_html_document_t *doc)
{
    if (!doc) return NULL;

    qjs_idx_t *idx = (qjs_idx_t *)calloc(1, sizeof(qjs_idx_t));
    if (!idx) return NULL;
    idx->document = doc;

    if (!qjs_htab_init(&idx->id_index) ||
        !qjs_htab_init(&idx->class_index) ||
        !qjs_htab_init(&idx->tag_index) ||
        !qjs_meta_init(idx)) {
        qjs_index_destroy(idx);
        return NULL;
    }

    lxb_dom_node_t *root = lxb_dom_interface_node(&doc->dom_document);
    qjs_walk_tree(idx, root);

    return idx;
}

void qjs_index_destroy(qjs_idx_t *idx)
{
    if (!idx) return;
    qjs_htab_destroy(&idx->id_index);
    qjs_htab_destroy(&idx->class_index);
    qjs_htab_destroy(&idx->tag_index);
    qjs_arena_destroy(&idx->arena);
    free(idx->meta_buckets);
    free(idx);
}

void qjs_index_mark_dirty(qjs_idx_t *idx)
{
    if (idx) idx->dirty = true;
}

/* ========================================================================
 * Fast path query — parse simple selectors and hit indexes
 * ======================================================================== */

static bool qjs_is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_';
}

/* Check if a node is a descendant of root (or root itself) */
static bool qjs_node_in_subtree(lxb_dom_node_t *node, lxb_dom_node_t *root)
{
    if (!root) return true;
    while (node) {
        if (node == root) return true;
        node = node->parent;
    }
    return false;
}

/*
 * Tag matching using lxb_tag_id_t — integer comparison instead of string.
 * Falls back to string comparison for custom/unknown tags.
 */
static bool qjs_node_tag_id_matches(lxb_dom_node_t *node, lxb_tag_id_t tag_id)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    return lxb_dom_element_tag_id(lxb_dom_interface_element(node)) == tag_id;
}

/* Fallback: string-based tag matching for custom tags */
static bool qjs_node_tag_matches_str(lxb_dom_node_t *node,
    const char *tag_lower, size_t tag_len)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t nlen;
    const lxb_char_t *name = lxb_dom_element_qualified_name(el, &nlen);
    if (!name || nlen != tag_len) return false;
    for (size_t i = 0; i < tag_len; i++) {
        if (tolower((unsigned char)name[i]) != (unsigned char)tag_lower[i])
            return false;
    }
    return true;
}

/* Check if node has a specific class (case-insensitive) */
static bool qjs_node_has_class(lxb_dom_node_t *node, const char *cls_lower, size_t cls_len)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t attr_len;
    const lxb_char_t *attr = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"class", 5, &attr_len);
    if (!attr || attr_len == 0) return false;

    const char *p = (const char *)attr;
    const char *end = p + attr_len;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *start = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - start) == cls_len) {
            bool match = true;
            for (size_t i = 0; i < cls_len; i++) {
                if (tolower((unsigned char)start[i]) != (unsigned char)cls_lower[i]) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

/* Check if node has a specific id (case-sensitive) */
static bool qjs_node_has_id(lxb_dom_node_t *node, const char *id, size_t id_len)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t attr_len;
    const lxb_char_t *attr = lxb_dom_element_get_attribute(el,
        (const lxb_char_t *)"id", 2, &attr_len);
    if (!attr || attr_len != id_len) return false;
    return memcmp(attr, id, id_len) == 0;
}

/* ========================================================================
 * Key-index: complex selector optimization (inspired by Nordstjernen)
 *
 * For complex selectors like "div.list dd a", instead of full tree traversal:
 *   1. Parse selector into compounds + combinators
 *   2. Take rightmost compound's key (id > class > tag)
 *   3. Query index for candidate set
 *   4. For each candidate, walk up tree verifying the compound chain
 *
 * Supported combinators: descendant (space), child (>)
 * Unsupported (falls back to lxb): +, ~, pseudo, attribute, multi-selector
 * ======================================================================== */

#define QJS_MAX_COMPOUNDS 8

typedef struct {
    /* Pointers into the original selector string */
    const char *tag;     size_t tag_len;
    const char *id;      size_t id_len;
    const char *cls;     size_t cls_len;
    bool has_unsupported;  /* contains pseudo/attr/etc */
} qjs_compound_t;

typedef enum {
    QJS_COMB_DESCENDANT = 0,  /* space */
    QJS_COMB_CHILD = 1,       /* > */
} qjs_comb_t;

/*
 * Parse a compound selector fragment (e.g., "div.list#id" → tag=div, cls=list, id=id).
 * Returns false if the compound contains unsupported features (pseudo, attr, *).
 */
static bool qjs_parse_compound(const char *start, size_t len, qjs_compound_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = start;
    const char *end = start + len;

    while (p < end) {
        if (*p == '#') {
            p++;
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (p == s) return false;
            if (out->id) return false;  /* duplicate */
            out->id = s; out->id_len = p - s;
        } else if (*p == '.') {
            p++;
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (p == s) return false;
            if (out->cls) return false;  /* only first class */
            out->cls = s; out->cls_len = p - s;
        } else if (isalpha((unsigned char)*p)) {
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (out->tag) return false;
            out->tag = s; out->tag_len = p - s;
        } else if (*p == '*') {
            out->has_unsupported = true;
            p++;
        } else {
            /* Contains :pseudo, [attr], or other unsupported syntax */
            out->has_unsupported = true;
            return false;
        }
    }
    return true;
}

/*
 * Check if a node matches a compound (tag + id + class).
 * Uses tag_id for O(1) tag comparison when available.
 */
/*
 * Check if a node matches a compound (tag + id + class).
 * Uses metadata cache for O(1) field access (ns_node style).
 * Falls back to lexbor API only if node is not in cache (shouldn't happen
 * for nodes within an indexed document).
 */
static bool qjs_match_compound(lxb_dom_node_t *node, const qjs_compound_t *c,
                               const qjs_idx_t *idx)
{
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;

    /* Try metadata cache first (O(1) — no lexbor API calls) */
    const qjs_node_meta_t *meta = idx ? qjs_meta_get(idx, node) : NULL;

    if (meta) {
        /* Tag check: integer comparison via cached tag_id */
        if (c->tag && c->tag_len > 0) {
            if (meta->tag_id != LXB_TAG__UNDEF) {
                /* Resolve expected tag_id from document */
                lxb_tag_id_t expected = LXB_TAG__UNDEF;
                if (idx && idx->document) {
                    const lxb_tag_data_t *td = lxb_tag_data_by_name(
                        idx->document->dom_document.tags,
                        (const lxb_char_t *)c->tag, c->tag_len);
                    if (td) expected = td->tag_id;
                }
                if (expected != LXB_TAG__UNDEF) {
                    if (meta->tag_id != expected) return false;
                } else {
                    /* Custom tag: fall through to string comparison */
                    char lower[256];
                    if (c->tag_len >= 256) return false;
                    for (size_t i = 0; i < c->tag_len; i++)
                        lower[i] = (char)tolower((unsigned char)c->tag[i]);
                    if (!qjs_node_tag_matches_str(node, lower, c->tag_len)) return false;
                }
            } else {
                char lower[256];
                if (c->tag_len >= 256) return false;
                for (size_t i = 0; i < c->tag_len; i++)
                    lower[i] = (char)tolower((unsigned char)c->tag[i]);
                if (!qjs_node_tag_matches_str(node, lower, c->tag_len)) return false;
            }
        }

        /* ID check (case-sensitive, direct pointer comparison) */
        if (c->id && c->id_len > 0) {
            if (meta->id_len != c->id_len) return false;
            if (memcmp(meta->id, c->id, c->id_len) != 0) return false;
        }

        /* Class check (case-insensitive, scan cached class string) */
        if (c->cls && c->cls_len > 0) {
            char lower[256];
            if (c->cls_len >= 256) return false;
            for (size_t i = 0; i < c->cls_len; i++)
                lower[i] = (char)tolower((unsigned char)c->cls[i]);
            /* Scan meta->cls for matching token */
            const char *p = (const char *)meta->cls;
            const char *end = p + meta->cls_len;
            bool found = false;
            while (p < end) {
                while (p < end && isspace((unsigned char)*p)) p++;
                const char *tok = p;
                while (p < end && !isspace((unsigned char)*p)) p++;
                if ((size_t)(p - tok) == c->cls_len) {
                    bool match = true;
                    for (size_t i = 0; i < c->cls_len; i++) {
                        if (tolower((unsigned char)tok[i]) != (unsigned char)lower[i]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) { found = true; break; }
                }
            }
            if (!found) return false;
        }

        return true;
    }

    /* Fallback: use lexbor API (for nodes not in cache) */
    /* Tag check */
    if (c->tag && c->tag_len > 0) {
        char lower[256];
        if (c->tag_len >= 256) return false;
        for (size_t i = 0; i < c->tag_len; i++)
            lower[i] = (char)tolower((unsigned char)c->tag[i]);
        if (idx && idx->document) {
            const lxb_tag_data_t *td = lxb_tag_data_by_name(
                idx->document->dom_document.tags, (const lxb_char_t *)c->tag, c->tag_len);
            if (td) {
                lxb_tag_id_t tid = lxb_dom_element_tag_id(lxb_dom_interface_element(node));
                if (tid != td->tag_id) return false;
            } else {
                if (!qjs_node_tag_matches_str(node, lower, c->tag_len)) return false;
            }
        } else {
            if (!qjs_node_tag_matches_str(node, lower, c->tag_len)) return false;
        }
    }

    /* ID check (case-sensitive) */
    if (c->id && c->id_len > 0) {
        if (!qjs_node_has_id(node, c->id, c->id_len)) return false;
    }

    /* Class check (case-insensitive) */
    if (c->cls && c->cls_len > 0) {
        char lower[256];
        if (c->cls_len >= 256) return false;
        for (size_t i = 0; i < c->cls_len; i++)
            lower[i] = (char)tolower((unsigned char)c->cls[i]);
        if (!qjs_node_has_class(node, lower, c->cls_len)) return false;
    }

    return true;
}

/*
 * Verify a candidate node against the full compound chain.
 * Walks from the rightmost compound (already matched) leftward,
 * following combinators (descendant/child) up the tree.
 *
 * compounds[n-1] is already matched to `node`.
 * We need to match compounds[0..n-2] against ancestors.
 * combinators[i] connects compounds[i] → compounds[i+1].
 */
static bool qjs_verify_chain(lxb_dom_node_t *node,
                             qjs_compound_t *compounds, int n,
                             qjs_comb_t *combinators,
                             const qjs_idx_t *idx)
{
    lxb_dom_node_t *cur = node;
    for (int i = n - 2; i >= 0; i--) {
        if (combinators[i] == QJS_COMB_CHILD) {
            /* Child: parent must match */
            cur = cur->parent;
            if (!cur || !qjs_match_compound(cur, &compounds[i], idx))
                return false;
        } else {
            /* Descendant: walk up ancestors until match found */
            cur = cur->parent;
            bool found = false;
            while (cur) {
                if (qjs_match_compound(cur, &compounds[i], idx)) {
                    found = true;
                    break;
                }
                cur = cur->parent;
            }
            if (!found) return false;
        }
    }
    return true;
}

/*
 * Try key-index for complex selectors.
 * Returns true if handled (including empty results).
 * Returns false if selector is not suitable for key-index (caller falls back to lxb).
 */
static bool qjs_index_try_key_index(const qjs_idx_t *idx,
                                    lxb_dom_node_t *root,
                                    const char *selector, size_t sel_len,
                                    bool first_only,
                                    lxb_dom_node_t ***out_nodes,
                                    size_t *out_count,
                                    bool *out_borrowed)
{
    *out_nodes = NULL;
    *out_count = 0;
    if (out_borrowed) *out_borrowed = false;

    /* Parse selector into compounds + combinators */
    qjs_compound_t compounds[QJS_MAX_COMPOUNDS];
    qjs_comb_t combs[QJS_MAX_COMPOUNDS];
    int n_compounds = 0;

    const char *p = selector;
    const char *end = selector + sel_len;
    const char *comp_start = p;

    while (p <= end) {
        /* End of compound: space, '>', or end of string */
        if (p == end || *p == ' ' || *p == '>') {
            size_t comp_len = p - comp_start;
            if (comp_len > 0) {
                if (n_compounds >= QJS_MAX_COMPOUNDS) return false;
                if (!qjs_parse_compound(comp_start, comp_len, &compounds[n_compounds]))
                    return false;
                n_compounds++;
            }

            /* Parse combinator */
            if (p < end) {
                if (n_compounds == 0) return false;  /* leading combinator */

                if (*p == '>') {
                    combs[n_compounds - 1] = QJS_COMB_CHILD;
                    p++;
                    /* Skip spaces after > */
                    while (p < end && *p == ' ') p++;
                } else {
                    combs[n_compounds - 1] = QJS_COMB_DESCENDANT;
                    p++;
                    /* Skip extra spaces */
                    while (p < end && *p == ' ') p++;
                }
                comp_start = p;
            } else {
                break;  /* end of string */
            }
        } else {
            p++;
        }
    }

    /* Need at least 2 compounds for key-index (1 compound = simple, handled elsewhere) */
    if (n_compounds < 2) return false;

    /* Check no compound has unsupported features */
    for (int i = 0; i < n_compounds; i++) {
        if (compounds[i].has_unsupported) return false;
    }

    /* Take rightmost compound as key */
    qjs_compound_t *key = &compounds[n_compounds - 1];

    /* Get candidate set from index (priority: id > class > tag) */
    qjs_idx_entry_t *e = NULL;
    char key_lower[256];
    const char *lookup_key = NULL;
    size_t lookup_len = 0;

    if (key->id && key->id_len > 0) {
        e = qjs_htab_lookup((qjs_htab_t *)&idx->id_index, key->id, key->id_len);
    } else if (key->cls && key->cls_len > 0) {
        if (key->cls_len >= 256) return false;
        for (size_t i = 0; i < key->cls_len; i++)
            key_lower[i] = (char)tolower((unsigned char)key->cls[i]);
        e = qjs_htab_lookup((qjs_htab_t *)&idx->class_index, key_lower, key->cls_len);
    } else if (key->tag && key->tag_len > 0) {
        if (key->tag_len >= 256) return false;
        for (size_t i = 0; i < key->tag_len; i++)
            key_lower[i] = (char)tolower((unsigned char)key->tag[i]);
        e = qjs_htab_lookup((qjs_htab_t *)&idx->tag_index, key_lower, key->tag_len);
    } else {
        return false;  /* no usable key */
    }

    if (!e) {
        /* Key not in index → no results */
        return true;
    }

    /* Selectivity check: if candidates are >25% of total elements, key-index
     * verification overhead exceeds tree traversal cost. Fall back to lxb.
     * (Inspired by Nordstjernen's approach where class/tag key-index is only
     * used when root==doc; our heuristic is more general.) */
    if (e->arr.count > idx->total_elements / 4 && idx->total_elements > 100) {
        return false;
    }

    /* Verify each candidate against the full chain */
    lxb_dom_node_t **results = NULL;
    size_t rcount = 0, rcap = 0;

    for (size_t i = 0; i < e->arr.count; i++) {
        lxb_dom_node_t *node = e->arr.nodes[i];

        /* Check subtree */
        if (root && !qjs_node_in_subtree(node, root)) continue;

        /* Verify the full compound chain */
        if (!qjs_verify_chain(node, compounds, n_compounds, combs, idx))
            continue;

        /* Collect */
        if (rcount >= rcap) {
            rcap = (rcap + 8) * 2;
            lxb_dom_node_t **nr = (lxb_dom_node_t **)realloc(results,
                rcap * sizeof(lxb_dom_node_t *));
            if (!nr) { free(results); *out_nodes = NULL; *out_count = 0; return true; }
            results = nr;
        }
        results[rcount++] = node;
        if (first_only) break;
    }

    *out_nodes = results;
    *out_count = rcount;
    return true;
}

/*
 * Collect results from an index entry.
 * Uses tag_id for O(1) tag comparison when available.
 * When no filtering needed and root is NULL, returns borrowed pointer.
 */
static void qjs_collect_results(qjs_idx_entry_t *e,
                                lxb_dom_node_t *root,
                                lxb_tag_id_t tag_id,
                                const char *tag_lower, size_t tag_len,
                                const char *cls_lower, size_t cls_len,
                                bool first_only,
                                lxb_dom_node_t ***out_nodes,
                                size_t *out_count,
                                bool *out_borrowed)
{
    *out_borrowed = false;

#ifdef QJS_NO_INDEX_OPT
    /* Baseline: always malloc+copy, always string comparison */
    (void)tag_id;
    {
#else
    /* Fastest path: no filtering, no subtree check, not first_only */
    if (!root && tag_id == LXB_TAG__UNDEF && !tag_lower && !cls_lower && !first_only) {
        *out_nodes = e->arr.nodes;
        *out_count = e->arr.count;
        *out_borrowed = true;  /* caller must NOT free */
        return;
    }
#endif

    lxb_dom_node_t **results = NULL;
    size_t rcount = 0, rcap = 0;

    for (size_t i = 0; i < e->arr.count; i++) {
        lxb_dom_node_t *node = e->arr.nodes[i];
        if (root && !qjs_node_in_subtree(node, root)) continue;

        /* Tag filter: use tag_id (O(1)) when available, else string comparison */
        if (tag_id != LXB_TAG__UNDEF) {
            if (!qjs_node_tag_id_matches(node, tag_id)) continue;
        } else if (tag_lower) {
            if (!qjs_node_tag_matches_str(node, tag_lower, tag_len)) continue;
        }

        if (cls_lower && !qjs_node_has_class(node, cls_lower, cls_len))
            continue;

        if (rcount >= rcap) {
            rcap = (rcap + 8) * 2;
            lxb_dom_node_t **nr = (lxb_dom_node_t **)realloc(results,
                rcap * sizeof(lxb_dom_node_t *));
            if (!nr) { free(results); *out_nodes = NULL; *out_count = 0; return; }
            results = nr;
        }
        results[rcount++] = node;
        if (first_only) break;
    }

    *out_nodes = results;
    *out_count = rcount;

#ifdef QJS_NO_INDEX_OPT
    }
#endif
}

bool qjs_index_try_fast(const qjs_idx_t *idx,
                        lxb_dom_node_t *root,
                        const char *selector, size_t sel_len,
                        bool first_only,
                        lxb_dom_node_t ***out_nodes,
                        size_t *out_count,
                        bool *out_borrowed)
{
    *out_nodes = NULL;
    *out_count = 0;
    if (out_borrowed) *out_borrowed = false;
    if (!idx || !selector || sel_len == 0) return false;
    /* If DOM has been modified since index was built, skip fast path */
    if (idx->dirty) return false;

    /* Detect complex selectors (contain space or >) → route to key-index */
    bool has_combinator = false;
    for (size_t i = 0; i < sel_len; i++) {
        if (selector[i] == ' ' || selector[i] == '>') {
            has_combinator = true;
            break;
        }
    }
    if (has_combinator) {
        return qjs_index_try_key_index(idx, root, selector, sel_len,
                                       first_only, out_nodes, out_count,
                                       out_borrowed);
    }

    const char *p = selector;
    const char *end = selector + sel_len;

    /* Parsed components */
    const char *tag_start = NULL; size_t tag_len = 0;
    const char *id_start = NULL;  size_t id_len = 0;
    const char *cls_start = NULL; size_t cls_len = 0;
    int component_count = 0;

    /* Parse: [tag][#id][.class] or [#id][.class] or [.class] etc. */
    while (p < end) {
        if (*p == '#') {
            p++;
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (p == s) return false;
            if (id_start) return false;
            id_start = s; id_len = p - s;
            component_count++;
        } else if (*p == '.') {
            p++;
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (p == s) return false;
            if (cls_start) return false;
            cls_start = s; cls_len = p - s;
            component_count++;
        } else if (isalpha((unsigned char)*p)) {
            const char *s = p;
            while (p < end && qjs_is_ident_char(*p)) p++;
            if (tag_start) return false;
            tag_start = s; tag_len = p - s;
            component_count++;
        } else {
            return false;
        }
    }

    if (component_count == 0 || component_count > 2) return false;

    /* Stack buffers for lowercasing — no malloc/free per query */
    char tag_lower_buf[256];
    char cls_lower_buf[256];
    const char *tag_lower = NULL;
    const char *cls_lower = NULL;

    /* Resolve tag to lxb_tag_id_t for O(1) integer comparison */
    lxb_tag_id_t tag_id = LXB_TAG__UNDEF;
    if (tag_start) {
        if (tag_len >= 256) return false;
        for (size_t i = 0; i < tag_len; i++)
            tag_lower_buf[i] = (char)tolower((unsigned char)tag_start[i]);
        tag_lower = tag_lower_buf;

        /* Try to resolve to lxb_tag_id_t (case-insensitive lookup) */
        if (idx->document) {
            const lxb_tag_data_t *td = lxb_tag_data_by_name(
                idx->document->dom_document.tags,
                (const lxb_char_t *)tag_start, tag_len);
            if (td) tag_id = td->tag_id;
        }
    }

    if (cls_start) {
        if (cls_len >= 256) return false;
        for (size_t i = 0; i < cls_len; i++)
            cls_lower_buf[i] = (char)tolower((unsigned char)cls_start[i]);
        cls_lower = cls_lower_buf;
    }

    /* Choose the best index to use */
    if (id_start) {
        qjs_idx_entry_t *e = qjs_htab_lookup((qjs_htab_t *)&idx->id_index,
            id_start, id_len);
        if (!e) {
            /* Index miss: DOM may have changed, fall back to full traversal */
            return false;
        }
        qjs_collect_results(e, root, tag_id, tag_lower, tag_len,
                           cls_lower, cls_len,
                           first_only, out_nodes, out_count, out_borrowed);
    } else if (cls_start) {
        qjs_idx_entry_t *e = qjs_htab_lookup((qjs_htab_t *)&idx->class_index,
            cls_lower, cls_len);
        if (!e) {
            /* Index miss: DOM may have changed, fall back to full traversal */
            return false;
        }
        qjs_collect_results(e, root, tag_id, tag_lower, tag_len,
                           NULL, 0,
                           first_only, out_nodes, out_count, out_borrowed);
    } else if (tag_start) {
        qjs_idx_entry_t *e = qjs_htab_lookup((qjs_htab_t *)&idx->tag_index,
            tag_lower, tag_len);
        if (!e) {
            /* Index miss: DOM may have changed, fall back to full traversal */
            return false;
        }
        qjs_collect_results(e, root, LXB_TAG__UNDEF, NULL, 0,
                           NULL, 0,
                           first_only, out_nodes, out_count, out_borrowed);
    } else {
        /* Complex selector: try key-index before falling back to lxb */
        return qjs_index_try_key_index(idx, root, selector, sel_len,
                                       first_only, out_nodes, out_count,
                                       out_borrowed);
    }

    return true;
}
