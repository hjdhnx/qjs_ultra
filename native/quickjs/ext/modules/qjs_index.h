/*
 * qjs_index.h — Hash index acceleration layer for lexbor DOM queries.
 *
 * Builds id/class/tag inverted indexes on top of a lexbor document tree,
 * enabling O(1) lookup for simple selectors instead of O(n) tree traversal.
 *
 * Inspired by Nordstjernen's ns_node index design (dom.c), but operates
 * directly on lxb_dom_node_t without tree conversion — zero double memory.
 *
 * Usage:
 *   qjs_idx_t *idx = qjs_index_build(document);
 *   // ... later in query path ...
 *   if (qjs_index_try_fast(idx, root, selector, sel_len, &nodes, &count))
 *       return nodes;  // fast path hit
 *   // else fall through to lxb_selectors_find
 *   qjs_index_destroy(idx);
 */
#ifndef QJS_INDEX_H
#define QJS_INDEX_H

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque index handle */
typedef struct qjs_idx qjs_idx_t;

/*
 * Build id/class/tag indexes from a lexbor HTML document.
 * Walks the tree once (O(n)) and registers every element's id, class, and tag.
 * Returns NULL on allocation failure.
 * Caller must free with qjs_index_destroy().
 */
qjs_idx_t *qjs_index_build(lxb_html_document_t *doc);

/* Free all index memory. Safe to call with NULL. */
void qjs_index_destroy(qjs_idx_t *idx);

/* Mark the index as dirty (DOM modified, fast path will be skipped). */
void qjs_index_mark_dirty(qjs_idx_t *idx);

/*
 * Try the fast path for a CSS selector.
 *
 * Handles simple selectors: #id, .class, tag, tag.class, #id.class, tag#id
 * For complex selectors (descendant, child, attribute, pseudo), returns false.
 *
 * On success (returns true):
 *   - *out_nodes is an array of matching nodes
 *   - *out_count is the number of matches
 *   - *out_borrowed: if true, array is borrowed (do NOT free); if false, free with free()
 *   - For querySelector (first_only=true), array has at most 1 element
 *
 * On miss (returns false):
 *   - Caller should fall through to lxb_selectors_find
 *
 * root limits the search scope; if NULL, uses the document root.
 */
bool qjs_index_try_fast(const qjs_idx_t *idx,
                        lxb_dom_node_t *root,
                        const char *selector, size_t sel_len,
                        bool first_only,
                        lxb_dom_node_t ***out_nodes,
                        size_t *out_count,
                        bool *out_borrowed);

#ifdef __cplusplus
}
#endif

#endif /* QJS_INDEX_H */
