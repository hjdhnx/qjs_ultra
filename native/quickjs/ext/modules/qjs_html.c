/*
 * qjs_html.c - cheerio-like HTML parsing interface
 *
 * Uses the Lexbor HTML parser and CSS selectors engine to provide a
 * cheerio-compatible API for parsing and manipulating HTML documents.
 */

/*
 * qjs_html.c - HTML 解析模块（cheerio 兼容接口）（中文说明）
 *
 * 基于 Lexbor HTML 解析器和 CSS 选择器引擎，
提供 cheerio 兼容的 HTML 解析与操作 API。
 */

#include "qjs_native.h"
#include "qjs_index.h"
#include <lexbor/html/html.h>
#include <lexbor/css/css.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/selectors/selectors.h>
#include <lexbor/dom/dom.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Document context */
typedef struct {
    lxb_html_document_t   *document;
    lxb_css_parser_t      *css_parser;
    lxb_selectors_t       *selectors;
    qjs_idx_t             *idx;       /* id/class/tag hash index for fast queries */
} qjs_cheeriodoc_t;


typedef struct {
    qjs_cheeriodoc_t *doc;
    lxb_dom_node_t  **nodes;
    size_t            count;
    JSValue           prev;   /* previous selection for .end() */
} qjs_cheerio_t;

static JSValue qjs_cheerio_wrap(JSContext *ctx, qjs_cheeriodoc_t *doc,
                                lxb_dom_node_t **nodes, size_t count);

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} qjs_sb_t;

/*
 * qjs_found_ctx_t - 选择器匹配回调上下文
 * 用于 .is() / .filter() / .not() / .closest() 等方法，
 * 记录是否找到匹配节点。
 */
typedef struct {
    bool    found;    /* 是否找到匹配 */
} qjs_found_ctx_t;

/*
 * qjs_selectors_found_cb - lxb_selectors 回调函数（文件作用域，符合 C11 标准）
 *
 * 当选择器匹配到节点时被调用，设置 found=true 并返回 LXB_STATUS_STOP
 * 以终止后续匹配。
 *
 * 参数签名严格匹配 lxb_selectors_cb_f：
 *   node - 匹配到的 DOM 节点
 *   spec - CSS 选择器特异性（未使用）
 *   ctx  - qjs_found_ctx_t 上下文指针
 */
static lxb_status_t
qjs_selectors_found_cb(lxb_dom_node_t *node,
                        lxb_css_selector_specificity_t spec, void *ctx)
{
    (void)node;
    (void)spec;
    ((qjs_found_ctx_t *)ctx)->found = true;
    return LXB_STATUS_STOP;
}

/* Initialize string buffer with reasonable pre-allocation */
static void qjs_sb_init(qjs_sb_t *sb)
{
    sb->buf = (char *)malloc(256);
    sb->cap = sb->buf ? 256 : 0;
    sb->len = 0;
}

static lxb_status_t qjs_ser_cb(const lxb_char_t *data, size_t len, void *ctx)
{
    qjs_sb_t *sb = (qjs_sb_t *)ctx;
    if (sb->len + len > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 256;
        while (nc < sb->len + len) nc *= 2;
        char *nb = (char *)realloc(sb->buf, nc);
        if (!nb) return LXB_STATUS_ERROR;
        sb->buf = nb;
        sb->cap = nc;
    }
    memcpy(sb->buf + sb->len, data, len);
    sb->len += len;
    /* No per-callback null termination: JS_NewStringLen uses explicit length */
    return LXB_STATUS_OK;
}

static JSValue qjs_sb_to_js(JSContext *ctx, qjs_sb_t *sb)
{
    JSValue ret;
    if (!sb->buf) return JS_NewString(ctx, "");
    ret = JS_NewStringLen(ctx, sb->buf, sb->len);
    free(sb->buf);
    sb->buf = NULL;
    return ret;
}

/* Get a valid context element for lxb_html_document_parse_fragment.
 * The API requires a non-NULL element to read local_name and ns.
 * Uses body element if available, otherwise creates a temporary div. */
static lxb_dom_element_t *qjs_get_frag_ctx(lxb_html_document_t *document) {
    if (!document) return NULL;
    /* Try body element */
    lxb_html_body_element_t *body = lxb_html_document_body_element(document);
    if (body) return &body->element.element;
    /* Try html root element */
    lxb_dom_node_t *root = (lxb_dom_node_t *)
        lxb_dom_document_root(&document->dom_document);
    if (root && root->type == LXB_DOM_NODE_TYPE_ELEMENT)
        return (lxb_dom_element_t *)root;
    /* Last resort: create a temporary div */
    lxb_dom_element_t *el = lxb_dom_document_create_element(
        &document->dom_document, (const lxb_char_t *)"div", 3, NULL);
    return el;
}

/* Find all nodes matching a CSS selector under root */
typedef struct {
    lxb_dom_node_t **nodes;
    size_t count;
    size_t cap;
} qjs_col_t;

static lxb_status_t qjs_find_cb(lxb_dom_node_t *node, unsigned int idx, void *ctx)
{
    qjs_col_t *c = (qjs_col_t *)ctx;
    if (c->count >= c->cap) {
        size_t nc = (c->cap + 8) * 2;
        lxb_dom_node_t **nn = (lxb_dom_node_t **)realloc(c->nodes,
                            nc * sizeof(lxb_dom_node_t *));
        if (!nn) return LXB_STATUS_ERROR;
        c->nodes = nn;
        c->cap = nc;
    }
    c->nodes[c->count++] = node;
    return LXB_STATUS_OK;
}

/*
 * qjs_col_dedup - 按节点指针去重，保留首次出现的顺序。
 * 用于 find() 在多个父节点上调用时去除重复结果（cheerio 行为）。
 */
static void qjs_col_dedup(qjs_col_t *col)
{
    if (col->count < 2) return;
    size_t w = 0;
    for (size_t r = 0; r < col->count; r++) {
        bool dup = false;
        for (size_t j = 0; j < w; j++) {
            if (col->nodes[j] == col->nodes[r]) { dup = true; break; }
        }
        if (!dup) col->nodes[w++] = col->nodes[r];
    }
    col->count = w;
}

static lxb_dom_node_t **qjs_find_nodes(JSContext *ctx,
                                        qjs_cheeriodoc_t *doc,
                                        lxb_dom_node_t *root,
                                        const char *selector,
                                        size_t sel_len,
                                        size_t *out_count,
                                        bool *out_borrowed)
{
    *out_count = 0;
    if (out_borrowed) *out_borrowed = false;
    if (sel_len == 0) return NULL;

    /* Fast path: try hash index for simple selectors (#id, .class, tag, tag.class, etc.) */
    if (doc->idx) {
        lxb_dom_node_t **fast_nodes = NULL;
        size_t fast_count = 0;
        bool borrowed = false;
        if (qjs_index_try_fast(doc->idx, root, selector, sel_len, false,
                               &fast_nodes, &fast_count, &borrowed)) {
            *out_count = fast_count;
            if (out_borrowed) *out_borrowed = borrowed;
            return fast_nodes;
        }
        /* fast path miss → fall through to lxb_selectors_find */
    }

    /* Parse selector (result lives in css_parser's memory pool, freed on doc destroy) */
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_relative_list(
        doc->css_parser, (const lxb_char_t *)selector, sel_len);
    if (!list) return NULL;

    qjs_col_t col = { NULL, 0, 0 };
    lxb_status_t status = lxb_selectors_find(doc->selectors, root, list,
            qjs_find_cb, &col);

    /* list lives in css_parser's memory pool, freed on doc destroy */

    if (status != LXB_STATUS_OK) {
        free(col.nodes);
        return NULL;
    }

    *out_count = col.count;
    return col.nodes;
}

/* jQuery position pseudo-class filter types */
typedef enum {
    QJS_POS_NONE = 0,
    QJS_POS_FIRST,
    QJS_POS_LAST,
    QJS_POS_EQ,
    QJS_POS_EVEN,
    QJS_POS_ODD
} qjs_pos_filter_t;

/* Forward declarations */
static char *qjs_preprocess_selector(const char *sel, size_t len,
                                      qjs_pos_filter_t *filter, int *param,
                                      char **contains_text);
static void qjs_apply_pos_filter(qjs_col_t *col, qjs_pos_filter_t filter, int param);

static JSValue qjs_cheerio_from_selector(JSContext *ctx,
                                          qjs_cheeriodoc_t *doc,
                                          const char *selector,
                                          size_t sel_len,
                                          lxb_dom_node_t *base)
{
    /* Preprocess: extract jQuery pseudo-classes, translate [attr!=val] */
    qjs_pos_filter_t pos_filter = QJS_POS_NONE;
    int pos_param = 0;
    char *contains_text = NULL;
    char *clean_sel = qjs_preprocess_selector(selector, sel_len, &pos_filter, &pos_param, &contains_text);
    if (!clean_sel) return JS_ThrowInternalError(ctx, "selector preprocessing failed");
    size_t clean_len = strlen(clean_sel);

    size_t count = 0;
    bool borrowed = false;
    lxb_dom_node_t **nodes = qjs_find_nodes(ctx, doc, base, clean_sel,
                                             clean_len, &count, &borrowed);
    free(clean_sel);

    /* Apply :contains() filter */
    if (contains_text && count > 0) {
        size_t w = 0;
        for (size_t r = 0; r < count; r++) {
            lxb_char_t *txt = lxb_dom_node_text_content(nodes[r], NULL);
            if (txt && strstr((const char *)txt, contains_text)) {
                nodes[w++] = nodes[r];
            }
            if (txt) lxb_dom_document_destroy_text(nodes[r]->owner_document, txt);
        }
        count = w;
    }

    /* Apply position filter (jQuery :first, :last, :eq, :even, :odd) */
    if (pos_filter != QJS_POS_NONE && count > 0) {
        qjs_col_t col = { nodes, count, count };
        qjs_apply_pos_filter(&col, pos_filter, pos_param);
        count = col.count;
    }

    free(contains_text);

    JSValue ret = qjs_cheerio_wrap(ctx, doc, nodes, count);
    if (nodes && !borrowed) free(nodes);
    return ret;
}

/* ------------------------------------------------------------------ */
/* CheerioDoc                                                          */
/* ------------------------------------------------------------------ */

static void qjs_doc_finalizer(JSRuntime *rt, JSValue val)
{
    qjs_cheeriodoc_t *doc = JS_GetOpaque(val, QJS_CORE_CLASS_ID_CHEERIO_DOC);
    if (!doc) return;
    if (doc->idx) qjs_index_destroy(doc->idx);
    if (doc->selectors) lxb_selectors_destroy(doc->selectors, true);
    if (doc->css_parser) {
        /* Destroy CSS memory pool (all parsed selector lists live here). */
        lxb_css_memory_t *mem = lxb_css_parser_memory(doc->css_parser);
        if (mem) lxb_css_memory_destroy(mem, true);
        lxb_css_parser_destroy(doc->css_parser, true);
    }
    if (doc->document) lxb_html_document_destroy(doc->document);
    js_free_rt(rt, doc);
}

/* The $ function body: $(selector) or $(html) or $(cheerio)
 * The CheerioDoc object is passed in func_data[0]. */
/* Preprocess selector: extract jQuery pseudo-classes and translate [attr!=val].
 * Returns a malloc'd cleaned selector string. Sets filter type and param.
 * If :contains("text") is found, *contains_text is set to a malloc'd string.
 * Caller must free the returned string and *contains_text (if non-NULL). */
static char *qjs_preprocess_selector(const char *sel, size_t len,
                                      qjs_pos_filter_t *filter, int *param,
                                      char **contains_text)
{
    *filter = QJS_POS_NONE;
    *param = 0;
    *contains_text = NULL;
    char *buf = (char *)malloc(len + 32); /* extra room for :not() expansion */
    if (!buf) return NULL;
    size_t bi = 0;
    size_t i = 0;

    while (i < len) {
        /* Check for :first, :last, :eq(), :even, :odd.
         * Use a word-boundary check: the char after the pseudo-class name
         * must NOT be '-', ':' or alphanumeric (to avoid matching :first-child,
         * :first-of-type, :last-child, etc. which are standard CSS). */
        if (sel[i] == ':' && i + 1 < len) {
            /* :first — but not :first-child, :first-of-type, etc. */
            if (i + 6 <= len && memcmp(sel + i, ":first", 6) == 0 &&
                (i + 6 == len || sel[i+6] == 0 ||
                 (sel[i+6] != '-' && sel[i+6] != ':' && !isalnum(sel[i+6])))) {
                *filter = QJS_POS_FIRST;
                i += 6;
                continue;
            }
            /* :last — but not :last-child, :last-of-type, etc. */
            if (i + 5 <= len && memcmp(sel + i, ":last", 5) == 0 &&
                (i + 5 == len || sel[i+5] == 0 ||
                 (sel[i+5] != '-' && sel[i+5] != ':' && !isalnum(sel[i+5])))) {
                *filter = QJS_POS_LAST;
                i += 5;
                continue;
            }
            /* :even — no standard CSS conflict, but check word boundary */
            if (i + 5 <= len && memcmp(sel + i, ":even", 5) == 0 &&
                (i + 5 == len || sel[i+5] == 0 ||
                 (sel[i+5] != '-' && sel[i+5] != ':' && !isalnum(sel[i+5])))) {
                *filter = QJS_POS_EVEN;
                i += 5;
                continue;
            }
            /* :odd — no standard CSS conflict, but check word boundary */
            if (i + 4 <= len && memcmp(sel + i, ":odd", 4) == 0 &&
                (i + 4 == len || sel[i+4] == 0 ||
                 (sel[i+4] != '-' && sel[i+4] != ':' && !isalnum(sel[i+4])))) {
                *filter = QJS_POS_ODD;
                i += 4;
                continue;
            }
            if (i + 3 < len && memcmp(sel + i, ":eq", 3) == 0 && sel[i+3] == '(') {
                /* Parse :eq(n) */
                size_t j = i + 4;
                int val = 0;
                bool neg = false;
                if (j < len && sel[j] == '-') { neg = true; j++; }
                while (j < len && isdigit(sel[j])) { val = val * 10 + (sel[j] - '0'); j++; }
                if (j < len && sel[j] == ')') {
                    *filter = QJS_POS_EQ;
                    *param = neg ? -val : val;
                    i = j + 1;
                    continue;
                }
            }
            /* :contains("text") or :contains(text) — extract text, strip from selector */
            if (i + 9 < len && memcmp(sel + i, ":contains(", 10) == 0) {
                size_t j = i + 10;
                /* Skip optional quotes */
                char quote = 0;
                if (j < len && (sel[j] == '"' || sel[j] == '\'')) {
                    quote = sel[j];
                    j++;
                }
                size_t text_start = j;
                if (quote) {
                    while (j < len && sel[j] != quote) j++;
                } else {
                    while (j < len && sel[j] != ')') j++;
                }
                size_t text_end = j;
                /* Skip closing quote */
                if (quote && j < len && sel[j] == quote) j++;
                /* Expect closing ) */
                if (j < len && sel[j] == ')') {
                    size_t tlen = text_end - text_start;
                    *contains_text = (char *)malloc(tlen + 1);
                    if (*contains_text) {
                        memcpy(*contains_text, sel + text_start, tlen);
                        (*contains_text)[tlen] = '\0';
                    }
                    i = j + 1;
                    continue;
                }
            }
        }
        if (sel[i] == '[' && i + 1 < len) {
            /* Find the matching ] */
            size_t j = i + 1;
            while (j < len && sel[j] != ']') j++;
            if (j < len) {
                /* Check for != inside the brackets */
                size_t k = i + 1;
                while (k < j) {
                    if (sel[k] == '!' && k + 1 < j && sel[k+1] == '=') {
                        /* Found [attr!=val] — translate to :not([attr=val]) */
                        buf[bi++] = ':';
                        buf[bi++] = 'n';
                        buf[bi++] = 'o';
                        buf[bi++] = 't';
                        buf[bi++] = '(';
                        buf[bi++] = '[';
                        /* Copy attr name (before !=) */
                        size_t m = i + 1;
                        while (m < k) buf[bi++] = sel[m++];
                        /* Skip !=, add = */
                        buf[bi++] = '=';
                        m = k + 2;
                        while (m <= j) buf[bi++] = sel[m++];
                        buf[bi++] = ')';
                        i = j + 1;
                        goto next_char;
                    }
                    k++;
                }
            }
        }
        buf[bi++] = sel[i];
        next_char:
        i++;
    }
    buf[bi] = '\0';
    return buf;
}

/* Apply position filter to a collection */
static void qjs_apply_pos_filter(qjs_col_t *col, qjs_pos_filter_t filter, int param)
{
    if (filter == QJS_POS_NONE || col->count == 0) return;
    switch (filter) {
        case QJS_POS_FIRST:
            if (col->count > 0) { col->count = 1; }
            break;
        case QJS_POS_LAST:
            if (col->count > 0) {
                col->nodes[0] = col->nodes[col->count - 1];
                col->count = 1;
            }
            break;
        case QJS_POS_EQ: {
            int idx = param;
            if (idx < 0) idx += (int)col->count;
            if (idx >= 0 && idx < (int)col->count) {
                col->nodes[0] = col->nodes[idx];
                col->count = 1;
            } else {
                col->count = 0;
            }
            break;
        }
        case QJS_POS_EVEN: {
            size_t w = 0;
            for (size_t r = 0; r < col->count; r += 2)
                col->nodes[w++] = col->nodes[r];
            col->count = w;
            break;
        }
        case QJS_POS_ODD: {
            size_t w = 0;
            for (size_t r = 1; r < col->count; r += 2)
                col->nodes[w++] = col->nodes[r];
            col->count = w;
            break;
        }
        default: break;
    }
}

static JSValue qjs_doc_call_data(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv,
                                  int magic, JSValueConst *func_data)
{
    qjs_cheeriodoc_t *doc = JS_GetOpaque(func_data[0], QJS_CORE_CLASS_ID_CHEERIO_DOC);
    if (!doc)
        return JS_ThrowInternalError(ctx, "cheerio document not initialized");

    if (argc < 1)
        return qjs_cheerio_wrap(ctx, doc, NULL, 0);

    JSValueConst arg = argv[0];

    /* If it's already a Cheerio object, re-wrap its nodes */
    qjs_cheerio_t *existing = JS_GetOpaque(arg, QJS_CORE_CLASS_ID_CHEERIO);
    if (existing)
        return qjs_cheerio_wrap(ctx, doc, existing->nodes, existing->count);

    const char *str;
    size_t len;
    str = JS_ToCStringLen(ctx, &len, arg);
    if (!str) return JS_EXCEPTION;

    /* If string starts with '<', treat as HTML fragment */
    if (len > 0 && str[0] == '<') {
        lxb_dom_element_t *fctx = qjs_get_frag_ctx(doc->document);
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            doc->document, fctx, (const lxb_char_t *)str, len);
        JS_FreeCString(ctx, str);
        if (!frag)
            return JS_ThrowInternalError(ctx, "failed to parse HTML fragment");

        size_t cnt = 0;
        for (lxb_dom_node_t *c = frag->first_child; c; c = c->next) cnt++;
        if (cnt == 0)
            return qjs_cheerio_wrap(ctx, doc, NULL, 0);

        lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
            cnt * sizeof(lxb_dom_node_t *));
        if (!nodes) return JS_EXCEPTION;
        size_t i = 0;
        for (lxb_dom_node_t *c = frag->first_child; c; c = c->next)
            nodes[i++] = c;
        JSValue ret = qjs_cheerio_wrap(ctx, doc, nodes, cnt);
        js_free(ctx, nodes);
        return ret;
    }

    /* Otherwise, treat as CSS selector against the whole document */
    lxb_dom_node_t *root = (lxb_dom_node_t *)
        lxb_dom_document_root(&doc->document->dom_document);
    JSValue ret = qjs_cheerio_from_selector(ctx, doc, str, len, root);
    JS_FreeCString(ctx, str);
    return ret;
}

/* $.html() - return the full document HTML */
static JSValue qjs_doc_html_data(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv,
                                  int magic, JSValueConst *func_data)
{
    qjs_cheeriodoc_t *doc = JS_GetOpaque(func_data[0], QJS_CORE_CLASS_ID_CHEERIO_DOC);
    if (!doc)
        return JS_ThrowInternalError(ctx, "cheerio document not initialized");

    qjs_sb_t sb; qjs_sb_init(&sb);
    { lxb_dom_node_t *root = lxb_dom_document_root(&doc->document->dom_document); if (root) lxb_html_serialize_tree_cb(root, qjs_ser_cb, &sb); }
    return qjs_sb_to_js(ctx, &sb);
}

/* $.root() - return a Cheerio object wrapping the document root */
static JSValue qjs_doc_root_data(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv,
                                  int magic, JSValueConst *func_data)
{
    qjs_cheeriodoc_t *doc = JS_GetOpaque(func_data[0], QJS_CORE_CLASS_ID_CHEERIO_DOC);
    if (!doc)
        return JS_ThrowInternalError(ctx, "cheerio document not initialized");

    lxb_dom_node_t *root = (lxb_dom_node_t *)
        lxb_dom_document_root(&doc->document->dom_document);
    return qjs_cheerio_wrap(ctx, doc, &root, 1);
}

/* cheerio.load(html) -> callable $ function */
static JSValue qjs_cheerio_load(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    const char *html = "";
    size_t html_len = 0;
    int allocated = 0;

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        html = JS_ToCStringLen(ctx, &html_len, argv[0]);
        if (!html) return JS_EXCEPTION;
        allocated = 1;
    }

    qjs_cheeriodoc_t *doc = (qjs_cheeriodoc_t *)js_mallocz(ctx, sizeof(*doc));
    if (!doc) {
        if (allocated) JS_FreeCString(ctx, html);
        return JS_EXCEPTION;
    }

    doc->document = lxb_html_document_create();
    if (!doc->document) {
        js_free(ctx, doc);
        if (allocated) JS_FreeCString(ctx, html);
        return JS_ThrowInternalError(ctx, "failed to create HTML document");
    }

    lxb_status_t status = lxb_html_document_parse(doc->document,
        (const lxb_char_t *)html, html_len);
    if (allocated) JS_FreeCString(ctx, html);

    if (status != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc->document);
        js_free(ctx, doc);
        return JS_ThrowInternalError(ctx, "failed to parse HTML");
    }

    doc->css_parser = lxb_css_parser_create();
    if (!doc->css_parser ||
        lxb_css_parser_init(doc->css_parser, NULL) != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc->document);
        js_free(ctx, doc);
        return JS_ThrowInternalError(ctx, "failed to create CSS parser");
    }

    doc->selectors = lxb_selectors_create();
    if (!doc->selectors ||
        lxb_selectors_init(doc->selectors) != LXB_STATUS_OK) {
        lxb_css_parser_destroy(doc->css_parser, true);
        lxb_html_document_destroy(doc->document);
        js_free(ctx, doc);
        return JS_ThrowInternalError(ctx, "failed to create selectors");
    }

    /* Build id/class/tag hash index for O(1) fast-path queries */
    doc->idx = qjs_index_build(doc->document);

    /* Create the internal CheerioDoc object to hold the document */
    JSValue doc_obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_CHEERIO_DOC);
    if (JS_IsException(doc_obj)) {
        if (doc->idx) qjs_index_destroy(doc->idx);
        lxb_selectors_destroy(doc->selectors, true);
        lxb_css_parser_destroy(doc->css_parser, true);
        lxb_html_document_destroy(doc->document);
        js_free(ctx, doc);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(doc_obj, doc);

    /* Create the callable $ function, capturing doc_obj as data */
    JSValue data[1] = { doc_obj };
    JSValue fn = JS_NewCFunctionData(ctx, qjs_doc_call_data, 1, 0, 1, data);
    JS_FreeValue(ctx, doc_obj); /* fn holds a reference */

    if (JS_IsException(fn))
        return JS_EXCEPTION;

    /* Attach .html() and .root() methods to the function */
    JSValue html_fn = JS_NewCFunctionData(ctx, qjs_doc_html_data, 0, 0, 1, data);
    JS_SetPropertyStr(ctx, fn, "html", html_fn);

    JSValue root_fn = JS_NewCFunctionData(ctx, qjs_doc_root_data, 0, 0, 1, data);
    JS_SetPropertyStr(ctx, fn, "root", root_fn);

    return fn;
}

/* ------------------------------------------------------------------ */
/* Cheerio object                                                      */
/* ------------------------------------------------------------------ */

static void qjs_cheerio_finalizer(JSRuntime *rt, JSValue val)
{
    qjs_cheerio_t *ch = JS_GetOpaque(val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return;
    JS_FreeValueRT(rt, ch->prev);
    if (ch->nodes) js_free_rt(rt, ch->nodes);
    js_free_rt(rt, ch);
}

static JSValue qjs_cheerio_wrap(JSContext *ctx, qjs_cheeriodoc_t *doc,
                                lxb_dom_node_t **nodes, size_t count)
{
    qjs_cheerio_t *ch = (qjs_cheerio_t *)js_mallocz(ctx, sizeof(*ch));
    if (!ch) return JS_EXCEPTION;

    ch->doc = doc;
    ch->count = count;
    ch->prev = JS_NULL;
    if (count > 0) {
        ch->nodes = (lxb_dom_node_t **)js_malloc(ctx,
            count * sizeof(lxb_dom_node_t *));
        if (!ch->nodes) { js_free(ctx, ch); return JS_EXCEPTION; }
        memcpy(ch->nodes, nodes, count * sizeof(lxb_dom_node_t *));
    }

    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_CHEERIO);
    if (JS_IsException(obj)) {
        if (ch->nodes) js_free(ctx, ch->nodes);
        js_free(ctx, ch);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, ch);
    /* Set length as a regular property */
    JS_SetPropertyStr(ctx, obj, "length", JS_NewInt32(ctx, (int32_t)count));
    return obj;
}

/* Set prevObject on a newly created Cheerio (for .end() support) */
static void qjs_cheerio_set_prev(JSContext *ctx, JSValue obj, JSValueConst prev)
{
    qjs_cheerio_t *ch = JS_GetOpaque(obj, QJS_CORE_CLASS_ID_CHEERIO);
    if (ch) {
        ch->prev = JS_DupValue(ctx, prev);
    }
}

/* .find(selector) — optimized: parse selector once, direct lxb_selectors_find */
static JSValue qjs_ch_find(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "find() requires a selector");

    const char *sel; size_t sel_len;
    sel = JS_ToCStringLen(ctx, &sel_len, argv[0]);
    if (!sel) return JS_EXCEPTION;

    /* Preprocess: extract jQuery pseudo-classes, translate [attr!=val] */
    qjs_pos_filter_t pos_filter = QJS_POS_NONE;
    int pos_param = 0;
    char *contains_text = NULL;
    char *clean_sel = qjs_preprocess_selector(sel, sel_len, &pos_filter, &pos_param, &contains_text);
    JS_FreeCString(ctx, sel);
    if (!clean_sel) return JS_ThrowInternalError(ctx, "selector preprocessing failed");
    sel_len = strlen(clean_sel);

    qjs_col_t col = { NULL, 0, 0 };

    /* Try fast path (hash index) for each node */
    if (ch->doc->idx) {
        bool fast_hit = true;
        for (size_t i = 0; i < ch->count; i++) {
            size_t sc = 0;
            lxb_dom_node_t **sub = NULL;
            bool sub_borrowed = false;
            if (qjs_index_try_fast(ch->doc->idx, ch->nodes[i], clean_sel, sel_len,
                                   false, &sub, &sc, &sub_borrowed)) {
                if (sub) {
                    for (size_t j = 0; j < sc; j++) {
                        if (col.count >= col.cap) {
                            size_t nc = (col.cap + 8) * 2;
                            lxb_dom_node_t **nn = (lxb_dom_node_t **)realloc(col.nodes,
                                                nc * sizeof(lxb_dom_node_t *));
                            if (!nn) { if (!sub_borrowed) free(sub); free(clean_sel); free(col.nodes); return JS_EXCEPTION; }
                            col.nodes = nn; col.cap = nc;
                        }
                        col.nodes[col.count++] = sub[j];
                    }
                    if (!sub_borrowed) free(sub);
                }
            } else {
                fast_hit = false;
                break;
            }
        }
        if (fast_hit) {
            free(clean_sel);
            /* Deduplicate by node pointer (cheerio find on multiple parents) */
            qjs_col_dedup(&col);
            /* Apply :contains() filter */
            if (contains_text && col.count > 0) {
                size_t w = 0;
                for (size_t r = 0; r < col.count; r++) {
                    lxb_char_t *txt = lxb_dom_node_text_content(col.nodes[r], NULL);
                    if (txt && strstr((const char *)txt, contains_text))
                        col.nodes[w++] = col.nodes[r];
                    if (txt) lxb_dom_document_destroy_text(col.nodes[r]->owner_document, txt);
                }
                col.count = w;
            }
            /* Apply position filter */
            qjs_apply_pos_filter(&col, pos_filter, pos_param);
            free(contains_text);
            JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, col.nodes, col.count);
            free(col.nodes);
            qjs_cheerio_set_prev(ctx, ret, this_val);
            return ret;
        }
        /* fast path missed for some node, reset col and use slow path */
        free(col.nodes);
        col.nodes = NULL; col.count = 0; col.cap = 0;
    }

    /* Slow path: parse selector, then lxb_selectors_find per node */
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_relative_list(
        ch->doc->css_parser, (const lxb_char_t *)clean_sel, sel_len);
    free(clean_sel);
    if (!list) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);

    for (size_t i = 0; i < ch->count; i++) {
        qjs_col_t sub = { NULL, 0, 0 };
        lxb_selectors_find(ch->doc->selectors, ch->nodes[i], list,
                           qjs_find_cb, &sub);
        for (size_t j = 0; j < sub.count; j++) {
            if (col.count >= col.cap) {
                size_t nc = (col.cap + 8) * 2;
                lxb_dom_node_t **nn = (lxb_dom_node_t **)realloc(col.nodes,
                                    nc * sizeof(lxb_dom_node_t *));
                if (!nn) { free(sub.nodes); free(col.nodes); return JS_EXCEPTION; }
                col.nodes = nn; col.cap = nc;
            }
            col.nodes[col.count++] = sub.nodes[j];
        }
        free(sub.nodes);
    }

    /* Deduplicate by node pointer (cheerio find on multiple parents) */
    qjs_col_dedup(&col);

    /* Apply :contains() filter */
    if (contains_text && col.count > 0) {
        size_t w = 0;
        for (size_t r = 0; r < col.count; r++) {
            lxb_char_t *txt = lxb_dom_node_text_content(col.nodes[r], NULL);
            if (txt && strstr((const char *)txt, contains_text))
                col.nodes[w++] = col.nodes[r];
            if (txt) lxb_dom_document_destroy_text(col.nodes[r]->owner_document, txt);
        }
        col.count = w;
    }

    /* Apply position filter */
    qjs_apply_pos_filter(&col, pos_filter, pos_param);
    free(contains_text);

    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, col.nodes, col.count);
    free(col.nodes);
    qjs_cheerio_set_prev(ctx, ret, this_val);
    return ret;
}

/* .text() */
static JSValue qjs_ch_text(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    qjs_sb_t sb; qjs_sb_init(&sb);
    for (size_t i = 0; i < ch->count; i++) {
        size_t len;
        const lxb_char_t *t = lxb_dom_node_text_content(ch->nodes[i], &len);
        if (t && len > 0) qjs_ser_cb(t, len, &sb);
    }
    return qjs_sb_to_js(ctx, &sb);
}

/* .html() - inner HTML */
static JSValue qjs_ch_html(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* cheerio: .html() on empty set returns null */
    if (ch->count == 0) return JS_NULL;

    /* cheerio: .html() getter only operates on the first element */
    qjs_sb_t sb; qjs_sb_init(&sb);
    lxb_dom_node_t *child = ch->nodes[0]->first_child;
    while (child) {
        lxb_html_serialize_tree_cb(child, qjs_ser_cb, &sb);
        child = child->next;
    }
    return qjs_sb_to_js(ctx, &sb);
}

/* .outerHTML() / .toString() */
static JSValue qjs_ch_outer(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    qjs_sb_t sb; qjs_sb_init(&sb);
    for (size_t i = 0; i < ch->count; i++)
        lxb_html_serialize_tree_cb(ch->nodes[i], qjs_ser_cb, &sb);
    return qjs_sb_to_js(ctx, &sb);
}

/* .attr(name) or .attr(name, value) or .attr({k:v,...}) or .attr(name, fn) */
static JSValue qjs_ch_attr(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "attr() requires a name");

    /* attr({key: val, ...}) — batch set attributes from object */
    if (JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0]) &&
        !JS_IsArray(ctx, argv[0])) {
        JSPropertyEnum *props = NULL;
        uint32_t n_props = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &n_props, argv[0],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t p = 0; p < n_props; p++) {
                JSAtom atom = props[p].atom;
                const char *kname = JS_AtomToCString(ctx, atom);
                if (kname) {
                    JSValue kv = JS_GetProperty(ctx, argv[0], atom);
                    size_t klen = strlen(kname);
                    if (JS_IsNull(kv)) {
                        for (size_t i = 0; i < ch->count; i++)
                            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                                lxb_dom_element_remove_attribute(
                                    (lxb_dom_element_t *)ch->nodes[i],
                                    (const lxb_char_t *)kname, klen);
                    } else if (!JS_IsUndefined(kv)) {
                        const char *vstr; size_t vlen;
                        vstr = JS_ToCStringLen(ctx, &vlen, kv);
                        if (vstr) {
                            for (size_t i = 0; i < ch->count; i++)
                                if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                                    lxb_dom_element_set_attribute(
                                        (lxb_dom_element_t *)ch->nodes[i],
                                        (const lxb_char_t *)kname, klen,
                                        (const lxb_char_t *)vstr, vlen);
                            JS_FreeCString(ctx, vstr);
                        }
                    }
                    JS_FreeValue(ctx, kv);
                    JS_FreeCString(ctx, kname);
                }
                JS_FreeAtom(ctx, atom);
            }
            js_free(ctx, props);
        }
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    const char *name; size_t nlen;
    name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!name) return JS_EXCEPTION;

    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        if (JS_IsNull(argv[1])) {
            /* attr(name, null) → remove attribute (cheerio behavior) */
            for (size_t i = 0; i < ch->count; i++) {
                if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    lxb_dom_element_remove_attribute(
                        (lxb_dom_element_t *)ch->nodes[i],
                        (const lxb_char_t *)name, nlen);
                }
            }
            JS_FreeCString(ctx, name);
            qjs_index_mark_dirty(ch->doc->idx);
            return JS_DupValue(ctx, this_val);
        }
        /* attr(name, function(i, oldVal)) — function callback for value */
        if (JS_IsFunction(ctx, argv[1])) {
            for (size_t i = 0; i < ch->count; i++) {
                if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
                JSValue oldv = JS_UNDEFINED;
                size_t vlen;
                const lxb_char_t *ov = lxb_dom_element_get_attribute(el,
                    (const lxb_char_t *)name, nlen, &vlen);
                if (ov) oldv = JS_NewStringLen(ctx, (const char *)ov, vlen);
                JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
                JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), oldv };
                JSValue ret = JS_Call(ctx, argv[1], node, 2, args);
                JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
                JS_FreeValue(ctx, node);
                if (JS_IsException(ret)) { JS_FreeCString(ctx, name); return ret; }
                if (JS_IsNull(ret)) {
                    lxb_dom_element_remove_attribute(el,
                        (const lxb_char_t *)name, nlen);
                } else {
                    const char *rv; size_t rlen;
                    rv = JS_ToCStringLen(ctx, &rlen, ret);
                    if (rv) {
                        lxb_dom_element_set_attribute(el,
                            (const lxb_char_t *)name, nlen,
                            (const lxb_char_t *)rv, rlen);
                        JS_FreeCString(ctx, rv);
                    }
                }
                JS_FreeValue(ctx, ret);
            }
            JS_FreeCString(ctx, name);
            qjs_index_mark_dirty(ch->doc->idx);
            return JS_DupValue(ctx, this_val);
        }
        const char *val; size_t vlen;
        val = JS_ToCStringLen(ctx, &vlen, argv[1]);
        if (!val) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
        for (size_t i = 0; i < ch->count; i++) {
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                lxb_dom_element_set_attribute(
                    (lxb_dom_element_t *)ch->nodes[i],
                    (const lxb_char_t *)name, nlen,
                    (const lxb_char_t *)val, vlen);
            }
        }
        JS_FreeCString(ctx, val);
        JS_FreeCString(ctx, name);
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    JSValue ret = JS_UNDEFINED;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t vlen;
            const lxb_char_t *v = lxb_dom_element_get_attribute(
                (lxb_dom_element_t *)ch->nodes[i],
                (const lxb_char_t *)name, nlen, &vlen);
            if (v) ret = JS_NewStringLen(ctx, (const char *)v, vlen);
            break;
        }
    }
    JS_FreeCString(ctx, name);
    return ret;
}

/* .removeAttr(name) - supports space-separated multiple attributes */
static int qjs_split_cls(const char *s, size_t len, char cls[][256], int max);
static JSValue qjs_ch_removeAttr(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "removeAttr() requires a name");

    const char *name; size_t nlen;
    name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Split on whitespace to support multiple attributes */
    char attrs[32][256]; int na = qjs_split_cls(name, nlen, attrs, 32);
    JS_FreeCString(ctx, name);

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
        for (int a = 0; a < na; a++)
            lxb_dom_element_remove_attribute(el,
                (const lxb_char_t *)attrs[a], strlen(attrs[a]));
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* Class helpers */
static int qjs_split_cls(const char *s, size_t len, char cls[][256], int max)
{
    int n = 0; size_t i = 0;
    while (i < len && n < max) {
        while (i < len && (s[i]==' '||s[i]=='\t'||s[i]=='\n')) i++;
        if (i >= len) break;
        size_t st = i;
        while (i < len && s[i]!=' '&&s[i]!='\t'&&s[i]!='\n') i++;
        size_t cl = i - st; if (cl > 255) cl = 255;
        memcpy(cls[n], s+st, cl); cls[n][cl] = '\0'; n++;
    }
    return n;
}

static JSValue qjs_ch_addClass(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);

    bool is_fn = JS_IsFunction(ctx, argv[0]);
    /* Pre-split static class string */
    char add_static[32][256]; int na_static = 0;
    if (!is_fn) {
        const char *cs; size_t cl;
        cs = JS_ToCStringLen(ctx, &cl, argv[0]);
        if (!cs) return JS_EXCEPTION;
        na_static = qjs_split_cls(cs, cl, add_static, 32);
        JS_FreeCString(ctx, cs);
    }

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];

        char add[32][256]; int na;
        if (is_fn) {
            /* addClass(function(i, currentClass)) — call function per element */
            size_t cur_len0;
            const lxb_char_t *cur0 = lxb_dom_element_class(el, &cur_len0);
            JSValue curj = (cur0 && cur_len0 > 0)
                ? JS_NewStringLen(ctx, (const char *)cur0, cur_len0)
                : JS_NewString(ctx, "");
            JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
            JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), curj };
            JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
            JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, node);
            if (JS_IsException(ret)) return ret;
            const char *cs; size_t cl;
            cs = JS_ToCStringLen(ctx, &cl, ret);
            JS_FreeValue(ctx, ret);
            na = 0;
            if (cs) { na = qjs_split_cls(cs, cl, add, 32); JS_FreeCString(ctx, cs); }
        } else {
            memcpy(add, add_static, sizeof(add_static));
            na = na_static;
        }

        size_t cur_len;
        const lxb_char_t *cur = lxb_dom_element_class(el, &cur_len);
        char nc[4096]; size_t pos = 0;
        if (cur && cur_len > 0) { memcpy(nc, cur, cur_len); pos = cur_len; }
        for (int c = 0; c < na; c++) {
            char ex[32][256]; int ne = qjs_split_cls(nc, pos, ex, 32);
            bool found = false;
            for (int e = 0; e < ne; e++) if (strcmp(ex[e], add[c])==0) { found=true; break; }
            if (!found) {
                if (pos > 0 && pos < sizeof(nc)) nc[pos++] = ' ';
                size_t al = strlen(add[c]);
                if (pos + al < sizeof(nc)) { memcpy(nc+pos, add[c], al); pos += al; }
            }
        }
        nc[pos] = '\0';
        lxb_dom_element_set_attribute(el, (const lxb_char_t*)"class", 5,
            (const lxb_char_t*)nc, pos);
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

static JSValue qjs_ch_removeClass(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* removeClass() with no args → remove ALL classes (set class to empty) */
    if (argc < 1) {
        for (size_t i = 0; i < ch->count; i++) {
            if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
            lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
            size_t cur_len;
            const lxb_char_t *cur = lxb_dom_element_class(el, &cur_len);
            if (cur && cur_len > 0)
                lxb_dom_element_set_attribute(el,
                    (const lxb_char_t*)"class", 5,
                    (const lxb_char_t*)"", 0);
        }
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    const char *cs; size_t cl;
    cs = JS_ToCStringLen(ctx, &cl, argv[0]);
    if (!cs) return JS_EXCEPTION;
    char rm[32][256]; int nr = qjs_split_cls(cs, cl, rm, 32);
    JS_FreeCString(ctx, cs);

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
        size_t cur_len;
        const lxb_char_t *cur = lxb_dom_element_class(el, &cur_len);
        if (!cur || cur_len == 0) continue;
        char ex[32][256]; int ne = qjs_split_cls((const char*)cur, cur_len, ex, 32);
        char nc[4096]; size_t pos = 0;
        for (int e = 0; e < ne; e++) {
            bool rem = false;
            for (int r = 0; r < nr; r++) if (strcmp(ex[e], rm[r])==0) { rem=true; break; }
            if (!rem) {
                if (pos > 0) nc[pos++] = ' ';
                size_t el2 = strlen(ex[e]); memcpy(nc+pos, ex[e], el2); pos += el2;
            }
        }
        nc[pos] = '\0';
        lxb_dom_element_set_attribute(el, (const lxb_char_t*)"class", 5,
            (const lxb_char_t*)nc, pos);
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .toggleClass(className) - toggle classes on/off; supports space-separated */
static JSValue qjs_ch_toggleClass(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);

    const char *cs; size_t cl;
    cs = JS_ToCStringLen(ctx, &cl, argv[0]);
    if (!cs) return JS_EXCEPTION;
    char tg[32][256]; int nt = qjs_split_cls(cs, cl, tg, 32);
    JS_FreeCString(ctx, cs);

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
        size_t cur_len;
        const lxb_char_t *cur = lxb_dom_element_class(el, &cur_len);
        char nc[4096]; size_t pos = 0;
        if (cur && cur_len > 0) { memcpy(nc, cur, cur_len); pos = cur_len; }
        for (int t = 0; t < nt; t++) {
            char ex[32][256]; int ne = qjs_split_cls(nc, pos, ex, 32);
            bool found = false; int found_idx = -1;
            for (int e = 0; e < ne; e++)
                if (strcmp(ex[e], tg[t])==0) { found=true; found_idx=e; break; }
            if (found) {
                /* remove this class: rebuild without found_idx */
                char nc2[4096]; size_t pos2 = 0;
                for (int e = 0; e < ne; e++) {
                    if (e == found_idx) continue;
                    if (pos2 > 0) nc2[pos2++] = ' ';
                    size_t el2 = strlen(ex[e]);
                    memcpy(nc2+pos2, ex[e], el2); pos2 += el2;
                }
                memcpy(nc, nc2, pos2); pos = pos2;
            } else {
                /* add this class */
                if (pos > 0 && pos < sizeof(nc)) nc[pos++] = ' ';
                size_t al = strlen(tg[t]);
                if (pos + al < sizeof(nc)) { memcpy(nc+pos, tg[t], al); pos += al; }
            }
        }
        nc[pos] = '\0';
        lxb_dom_element_set_attribute(el, (const lxb_char_t*)"class", 5,
            (const lxb_char_t*)nc, pos);
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

static JSValue qjs_ch_hasClass(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_FALSE;

    const char *cs; size_t cl;
    cs = JS_ToCStringLen(ctx, &cl, argv[0]);
    if (!cs) return JS_EXCEPTION;

    JSValue ret = JS_FALSE;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
        size_t cur_len;
        const lxb_char_t *cur = lxb_dom_element_class(el, &cur_len);
        if (!cur || cur_len == 0) continue;
        char ex[32][256]; int ne = qjs_split_cls((const char*)cur, cur_len, ex, 32);
        for (int e = 0; e < ne; e++) if (strcmp(ex[e], cs)==0) { ret = JS_TRUE; break; }
        if (JS_ToBool(ctx, ret)) break;
    }
    JS_FreeCString(ctx, cs);
    return ret;
}

/* .parent() */
static JSValue qjs_ch_parent(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *p = ch->nodes[i]->parent;
        if (p && p->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            bool dup = false;
            for (size_t j = 0; j < cnt; j++) if (nodes[j]==p) { dup=true; break; }
            if (!dup) nodes[cnt++] = p;
        }
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .children() */
static JSValue qjs_ch_children(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* Parse optional selector filter */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
        }
    }

    /* cheerio children() only returns element nodes */
    size_t total = 0;
    for (size_t i = 0; i < ch->count; i++)
        for (lxb_dom_node_t *c = ch->nodes[i]->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) total++;
    if (total == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        total * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++)
        for (lxb_dom_node_t *c = ch->nodes[i]->first_child; c; c = c->next)
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                if (filter_list) {
                    /* Apply selector filter */
                    qjs_found_ctx_t fctx = { false };
                    lxb_selectors_match_node(ch->doc->selectors, c, filter_list,
                        qjs_selectors_found_cb, &fctx);
                    if (!fctx.found) continue;
                }
                nodes[cnt++] = c;
            }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .next() / .prev() */
static JSValue qjs_ch_next(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++)
        if (ch->nodes[i]->next) nodes[cnt++] = ch->nodes[i]->next;
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

static JSValue qjs_ch_prev(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++)
        if (ch->nodes[i]->prev) nodes[cnt++] = ch->nodes[i]->prev;
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .siblings() */
static JSValue qjs_ch_siblings(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    size_t cap = ch->count * 4;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *p = ch->nodes[i]->parent;
        if (!p) continue;
        for (lxb_dom_node_t *c = p->first_child; c; c = c->next) {
            if (c == ch->nodes[i]) continue;
            bool dup = false;
            for (size_t j = 0; j < cnt; j++) if (nodes[j]==c) { dup=true; break; }
            if (!dup) {
                if (cnt >= cap) {
                    cap = cap * 2 + 8;
                    lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                    if (!nn) { js_free(ctx, nodes); return JS_EXCEPTION; }
                    nodes = nn;
                }
                nodes[cnt++] = c;
            }
        }
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .first() / .last() / .eq(i) */
static JSValue qjs_ch_first(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (ch->count == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    return qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[0], 1);
}

static JSValue qjs_ch_last(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (ch->count == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    return qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[ch->count-1], 1);
}

static JSValue qjs_ch_eq(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    int32_t idx;
    if (JS_ToInt32(ctx, &idx, argv[0])) return JS_EXCEPTION;
    if (idx < 0) idx += (int32_t)ch->count;
    if (idx < 0 || (size_t)idx >= ch->count)
        return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    return qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[idx], 1);
}

/* .get(i) */
static JSValue qjs_ch_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) {
        JSValue arr = JS_NewArray(ctx);
        for (size_t i = 0; i < ch->count; i++)
            JS_SetPropertyUint32(ctx, arr, i,
                qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1));
        return arr;
    }
    int32_t idx;
    if (JS_ToInt32(ctx, &idx, argv[0])) return JS_EXCEPTION;
    if (idx < 0) idx += (int32_t)ch->count;
    if (idx < 0 || (size_t)idx >= ch->count) return JS_NULL;
    return qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[idx], 1);
}

/* .each(fn) */
static JSValue qjs_ch_each(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "each() requires a function");
    for (size_t i = 0; i < ch->count; i++) {
        JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
        /* cheerio each(fn) callback signature: function(index, element)
         * element is a cheerio object wrapping the DOM node */
        JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), node };
        JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
        if (JS_IsException(ret)) return ret;
        /* return false breaks the loop (jQuery/cheerio convention) */
        bool should_break = false;
        if (JS_IsBool(ret)) {
            should_break = !JS_ToBool(ctx, ret);
        }
        JS_FreeValue(ctx, ret);
        if (should_break) break;
    }
    return JS_DupValue(ctx, this_val);
}

/* .map(fn) */
static JSValue qjs_ch_map(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "map() requires a function");
    JSValue arr = JS_NewArray(ctx);
    uint32_t oi = 0;
    for (size_t i = 0; i < ch->count; i++) {
        JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
        JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), node };
        /* In cheerio, 'this' inside map() is the element (like each) */
        JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
        JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
        if (JS_IsException(ret)) { JS_FreeValue(ctx, arr); return ret; }
        if (!JS_IsNull(ret) && !JS_IsUndefined(ret))
            JS_SetPropertyUint32(ctx, arr, oi++, ret);
        else JS_FreeValue(ctx, ret);
    }
    return arr;
}

/* .remove([selector]) - remove elements; with selector, only matching ones */
static JSValue qjs_ch_remove(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    lxb_css_selector_list_t *list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
        }
    }

    for (size_t i = 0; i < ch->count; i++) {
        if (list) {
            /* only remove nodes that match the selector */
            qjs_found_ctx_t fctx = { false };
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                lxb_selectors_match_node(ch->doc->selectors, ch->nodes[i], list,
                    qjs_selectors_found_cb, &fctx);
            if (!fctx.found) continue;
        }
        lxb_dom_node_remove(ch->nodes[i]);
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .empty() */
static JSValue qjs_ch_empty(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *c = ch->nodes[i]->first_child;
        while (c) { lxb_dom_node_t *n = c->next; lxb_dom_node_remove(c); c = n; }
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .val() */
static JSValue qjs_ch_val(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        /* val(function(i, oldVal)) — function callback */
        if (JS_IsFunction(ctx, argv[0])) {
            for (size_t i = 0; i < ch->count; i++) {
                if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
                JSValue oldv = JS_UNDEFINED;
                size_t vlen;
                const lxb_char_t *ov = lxb_dom_element_get_attribute(el,
                    (const lxb_char_t*)"value", 5, &vlen);
                if (ov) oldv = JS_NewStringLen(ctx, (const char *)ov, vlen);
                JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
                JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), oldv };
                JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
                JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
                JS_FreeValue(ctx, node);
                if (JS_IsException(ret)) return ret;
                const char *v; size_t vl;
                v = JS_ToCStringLen(ctx, &vl, ret);
                JS_FreeValue(ctx, ret);
                if (v) {
                    lxb_dom_element_set_attribute(el,
                        (const lxb_char_t*)"value", 5,
                        (const lxb_char_t*)v, vl);
                    JS_FreeCString(ctx, v);
                }
            }
            return JS_DupValue(ctx, this_val);
        }
        const char *v; size_t vl;
        v = JS_ToCStringLen(ctx, &vl, argv[0]);
        if (!v) return JS_EXCEPTION;
        for (size_t i = 0; i < ch->count; i++)
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                lxb_dom_element_set_attribute(
                    (lxb_dom_element_t *)ch->nodes[i],
                    (const lxb_char_t*)"value", 5,
                    (const lxb_char_t*)v, vl);
        JS_FreeCString(ctx, v);
        return JS_DupValue(ctx, this_val);
    }
    /* Getter: for <select>, return value of selected <option> */
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_element_t *el = (lxb_dom_element_t *)ch->nodes[i];
        size_t tl;
        const lxb_char_t *tag = lxb_dom_element_qualified_name(el, &tl);
        if (tag && tl == 6 && memcmp(tag, "select", 6) == 0) {
            /* Find selected option */
            lxb_dom_node_t *opt = ch->nodes[i]->first_child;
            while (opt) {
                if (opt->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    lxb_dom_element_t *oe = (lxb_dom_element_t *)opt;
                    /* Check if option has 'selected' attribute */
                    size_t ol;
                    const lxb_char_t *otag = lxb_dom_element_qualified_name(oe, &ol);
                    if (otag && ol == 6 && memcmp(otag, "option", 6) == 0) {
                        bool is_selected = lxb_dom_element_has_attribute(oe,
                            (const lxb_char_t*)"selected", 8);
                        if (is_selected) {
                            size_t vl;
                            const lxb_char_t *v = lxb_dom_element_get_attribute(oe,
                                (const lxb_char_t*)"value", 5, &vl);
                            if (v) return JS_NewStringLen(ctx, (const char*)v, vl);
                            /* No value attr → use text content */
                            size_t tlen;
                            lxb_char_t *tc = lxb_dom_node_text_content(opt, &tlen);
                            if (tc) {
                                JSValue r = JS_NewStringLen(ctx, (const char*)tc, tlen);
                                lxb_dom_document_destroy_text(opt->owner_document, tc);
                                return r;
                            }
                            return JS_NewString(ctx, "");
                        }
                    }
                }
                opt = opt->next;
            }
            /* No selected option → return first option's value */
            opt = ch->nodes[i]->first_child;
            while (opt) {
                if (opt->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    size_t vl;
                    const lxb_char_t *v = lxb_dom_element_get_attribute(
                        (lxb_dom_element_t *)opt,
                        (const lxb_char_t*)"value", 5, &vl);
                    if (v) return JS_NewStringLen(ctx, (const char*)v, vl);
                }
                opt = opt->next;
            }
            return JS_UNDEFINED;
        }
        /* For non-select elements, return value attribute */
        size_t vl;
        const lxb_char_t *v = lxb_dom_element_get_attribute(el,
            (const lxb_char_t*)"value", 5, &vl);
        if (v) return JS_NewStringLen(ctx, (const char*)v, vl);
        return JS_UNDEFINED;
    }
    return JS_UNDEFINED;
}

/* .is(selector) */
static JSValue qjs_ch_is(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_FALSE;
    const char *sel; size_t sl;
    sel = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!sel) return JS_EXCEPTION;
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_relative_list(ch->doc->css_parser, (const lxb_char_t *)sel, sl);
    JS_FreeCString(ctx, sel);
    if (!list) return JS_FALSE;

    JSValue result = JS_FALSE;
    for (size_t i = 0; i < ch->count; i++) {
        qjs_found_ctx_t fctx = { false };
        lxb_selectors_match_node(ch->doc->selectors, ch->nodes[i], list,
            qjs_selectors_found_cb, &fctx);
        if (fctx.found) { result = JS_TRUE; break; }
    }
    /* list lives in css_parser memory pool, freed on doc destroy */
    return result;
}

/* .filter(selector) or .filter(fn) */
static JSValue qjs_ch_filter(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);

    /* filter(function(i, el)) — keep elements where function returns truthy */
    if (JS_IsFunction(ctx, argv[0])) {
        lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
            ch->count * sizeof(lxb_dom_node_t *));
        if (!nodes) return JS_EXCEPTION;
        size_t cnt = 0;
        for (size_t i = 0; i < ch->count; i++) {
            JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
            JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), node };
            JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
            JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
            if (JS_IsException(ret)) { js_free(ctx, nodes); return ret; }
            bool keep = JS_ToBool(ctx, ret);
            JS_FreeValue(ctx, ret);
            if (keep) nodes[cnt++] = ch->nodes[i];
        }
        JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
        js_free(ctx, nodes);
        return ret;
    }

    const char *sel; size_t sl;
    sel = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!sel) return JS_EXCEPTION;
    /* filter() matches the selector against each node *itself*, so we
     * parse an absolute selector list (not a :scope-relative one which
     * is meant for finding descendants of a scope node). Using the
     * relative-list parser can cause a class selector such as ".s10"
     * to match incorrectly; the absolute list parser matches the node
     * directly. */
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_complex_list(ch->doc->css_parser, (const lxb_char_t *)sel, sl);
    JS_FreeCString(ctx, sel);
    if (!list) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);

    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        qjs_found_ctx_t fctx = { false };
        lxb_selectors_match_node(ch->doc->selectors, ch->nodes[i], list,
            qjs_selectors_found_cb, &fctx);
        if (fctx.found) nodes[cnt++] = ch->nodes[i];
    }
    /* list lives in css_parser memory pool, freed on doc destroy */
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .not(selector) or .not(fn) */
static JSValue qjs_ch_not(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);

    /* not(function(i, el)) — remove elements where function returns truthy */
    if (JS_IsFunction(ctx, argv[0])) {
        lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
            ch->count * sizeof(lxb_dom_node_t *));
        if (!nodes) return JS_EXCEPTION;
        size_t cnt = 0;
        for (size_t i = 0; i < ch->count; i++) {
            JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
            JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), node };
            JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
            JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
            if (JS_IsException(ret)) { js_free(ctx, nodes); return ret; }
            bool remove = JS_ToBool(ctx, ret);
            JS_FreeValue(ctx, ret);
            if (!remove) nodes[cnt++] = ch->nodes[i];
        }
        JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
        js_free(ctx, nodes);
        return ret;
    }

    const char *sel; size_t sl;
    sel = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!sel) return JS_EXCEPTION;
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_relative_list(ch->doc->css_parser, (const lxb_char_t *)sel, sl);
    JS_FreeCString(ctx, sel);
    if (!list) return JS_DupValue(ctx, this_val);

    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        qjs_found_ctx_t fctx = { false };
        lxb_selectors_match_node(ch->doc->selectors, ch->nodes[i], list,
            qjs_selectors_found_cb, &fctx);
        if (!fctx.found) nodes[cnt++] = ch->nodes[i];
    }
    /* list lives in css_parser memory pool, freed on doc destroy */
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .append(content) */
static JSValue qjs_ch_append(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i],
            (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *c = frag->first_child;
            while (c) { lxb_dom_node_t *n = c->next;
                lxb_dom_node_insert_child(ch->nodes[i], c); c = n; }
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .prepend(content) */
static JSValue qjs_ch_prepend(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i], (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *first = ch->nodes[i]->first_child;
            lxb_dom_node_t *c = frag->first_child;
            while (c) { lxb_dom_node_t *n = c->next;
                if (first) lxb_dom_node_insert_before(first, c);
                else lxb_dom_node_insert_child(ch->nodes[i], c);
                c = n; }
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .before(content) / .after(content) */
static JSValue qjs_ch_before(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i], (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *c = frag->first_child;
            while (c) { lxb_dom_node_t *n = c->next;
                lxb_dom_node_insert_before(ch->nodes[i], c); c = n; }
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

static JSValue qjs_ch_after(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i], (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *c = frag->first_child;
            while (c) { lxb_dom_node_t *n = c->next;
                lxb_dom_node_insert_after(ch->nodes[i], c); c = n; }
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* ------------------------------------------------------------------ */
/* .appendTo / .prependTo / .insertAfter / .insertBefore              */
/*                                                                    */
/* These are the reverse of append/prepend/after/before. The current  */
/* cheerio object holds the *content* (already parsed HTML); the      */
/* argument is a *selector* describing the targets. For every target  */
/* the content nodes are cloned and inserted, and a cheerio object    */
/* wrapping all the inserted nodes is returned.                       */
/* ------------------------------------------------------------------ */

typedef enum {
    QJS_INSERT_TO_APPEND,
    QJS_INSERT_TO_PREPEND,
    QJS_INSERT_TO_AFTER,
    QJS_INSERT_TO_BEFORE
} qjs_insert_to_mode_t;

static JSValue qjs_ch_insert_to(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv,
                                qjs_insert_to_mode_t mode)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1 || ch->count == 0)
        return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);

    /* Parse the selector argument and find target nodes against the
     * whole document (same as $(selector)). */
    const char *sel; size_t sl;
    sel = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!sel) return JS_EXCEPTION;

    lxb_dom_node_t *root = (lxb_dom_node_t *)
        lxb_dom_document_root(&ch->doc->document->dom_document);
    size_t tcount = 0;
    bool borrowed = false;
    lxb_dom_node_t **targets = qjs_find_nodes(ctx, ch->doc, root, sel, sl,
                                               &tcount, &borrowed);
    JS_FreeCString(ctx, sel);

    if (tcount == 0) {
        if (targets && !borrowed) free(targets);
        return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    }

    /* For each target, clone the content nodes and insert them, using
     * the same approach as append/prepend/after/before. Collect every
     * inserted clone so we can return it. */
    size_t max_inserted = tcount * ch->count;
    lxb_dom_node_t **inserted = (lxb_dom_node_t **)js_malloc(ctx,
        max_inserted * sizeof(lxb_dom_node_t *));
    if (!inserted) {
        if (!borrowed) free(targets);
        return JS_EXCEPTION;
    }
    size_t ins_cnt = 0;

    for (size_t t = 0; t < tcount; t++) {
        lxb_dom_node_t *target = targets[t];
        if (target->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;

        lxb_dom_node_t *first = NULL;   /* prepend anchor (orig. 1st child) */
        lxb_dom_node_t *last = NULL;    /* after anchor (last inserted)      */
        if (mode == QJS_INSERT_TO_PREPEND) first = target->first_child;
        else if (mode == QJS_INSERT_TO_AFTER) last = target;

        for (size_t c = 0; c < ch->count; c++) {
            lxb_dom_node_t *clone = lxb_dom_node_clone(ch->nodes[c], true);
            if (!clone) continue;

            switch (mode) {
                case QJS_INSERT_TO_APPEND:
                    lxb_dom_node_insert_child(target, clone);
                    break;
                case QJS_INSERT_TO_PREPEND:
                    if (first) lxb_dom_node_insert_before(first, clone);
                    else lxb_dom_node_insert_child(target, clone);
                    break;
                case QJS_INSERT_TO_AFTER:
                    lxb_dom_node_insert_after(last, clone);
                    last = clone;
                    break;
                case QJS_INSERT_TO_BEFORE:
                    lxb_dom_node_insert_before(target, clone);
                    break;
            }
            inserted[ins_cnt++] = clone;
        }
    }

    if (!borrowed) free(targets);

    qjs_index_mark_dirty(ch->doc->idx);

    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, inserted, ins_cnt);
    js_free(ctx, inserted);
    return ret;
}

/* $(content).appendTo(selector) */
static JSValue qjs_ch_appendTo(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    return qjs_ch_insert_to(ctx, this_val, argc, argv, QJS_INSERT_TO_APPEND);
}

/* $(content).prependTo(selector) */
static JSValue qjs_ch_prependTo(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    return qjs_ch_insert_to(ctx, this_val, argc, argv, QJS_INSERT_TO_PREPEND);
}

/* $(content).insertAfter(selector) */
static JSValue qjs_ch_insertAfter(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    return qjs_ch_insert_to(ctx, this_val, argc, argv, QJS_INSERT_TO_AFTER);
}

/* $(content).insertBefore(selector) */
static JSValue qjs_ch_insertBefore(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    return qjs_ch_insert_to(ctx, this_val, argc, argv, QJS_INSERT_TO_BEFORE);
}

/* .replaceWith(content) */
static JSValue qjs_ch_replaceWith(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i], (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *c = frag->first_child;
            while (c) { lxb_dom_node_t *n = c->next;
                lxb_dom_node_insert_before(ch->nodes[i], c); c = n; }
            lxb_dom_node_remove(ch->nodes[i]);
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .clone() */
static JSValue qjs_ch_clone(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (ch->count == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    /* Deep clone each node (cheerio clone is deep by default) */
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        ch->count * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *cloned = lxb_dom_node_clone(ch->nodes[i], true);
        if (cloned) nodes[cnt++] = cloned;
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .tagName() */
static JSValue qjs_ch_tagName(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t l;
            const lxb_char_t *n = lxb_dom_element_qualified_name(
                (lxb_dom_element_t *)ch->nodes[i], &l);
            if (n) return JS_NewStringLen(ctx, (const char*)n, l);
        }
    }
    return JS_UNDEFINED;
}

/* .toArray() */
static JSValue qjs_ch_toArray(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < ch->count; i++)
        JS_SetPropertyUint32(ctx, arr, i,
            qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1));
    return arr;
}

/* .closest(selector) - get the first ancestor that matches the selector */
static JSValue qjs_ch_closest(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "closest() requires a selector");

    const char *sel; size_t sl;
    sel = JS_ToCStringLen(ctx, &sl, argv[0]);
    if (!sel) return JS_EXCEPTION;

    /* Use selector cache (reuses doc->css_parser) */
    lxb_css_selector_list_t *list = lxb_css_selectors_parse_relative_list(ch->doc->css_parser, (const lxb_char_t *)sel, sl);
    JS_FreeCString(ctx, sel);
    if (!list) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);

    lxb_dom_node_t *result = NULL;
    for (size_t i = 0; i < ch->count && !result; i++) {
        lxb_dom_node_t *node = ch->nodes[i];
        while (node) {
            lxb_selectors_clean(ch->doc->selectors);
            qjs_found_ctx_t fctx = { false };
            lxb_selectors_match_node(ch->doc->selectors, node, list,
                qjs_selectors_found_cb, &fctx);
            if (fctx.found) { result = node; break; }
            node = node->parent;
        }
    }
    /* list lives in css_parser memory pool, freed on doc destroy */

    if (result)
        return qjs_cheerio_wrap(ctx, ch->doc, &result, 1);
    return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
}

/* .contents() - get all children including text nodes */
static JSValue qjs_ch_contents(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    size_t total = 0;
    for (size_t i = 0; i < ch->count; i++)
        for (lxb_dom_node_t *c = ch->nodes[i]->first_child; c; c = c->next) total++;
    if (total == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        total * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++)
        for (lxb_dom_node_t *c = ch->nodes[i]->first_child; c; c = c->next)
            nodes[cnt++] = c;
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .prop(name) or .prop(name, value) - get/set DOM property */
static JSValue qjs_ch_prop(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "prop() requires a name");

    const char *name; size_t nlen;
    name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!name) return JS_EXCEPTION;

    JSValue ret = JS_UNDEFINED;

    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        /* Setter - only support tagName (read-only), class, id */
        const char *val; size_t vlen;
        val = JS_ToCStringLen(ctx, &vlen, argv[1]);
        if (val) {
            for (size_t i = 0; i < ch->count; i++) {
                if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                    lxb_dom_element_set_attribute(
                        (lxb_dom_element_t *)ch->nodes[i],
                        (const lxb_char_t *)name, nlen,
                        (const lxb_char_t *)val, vlen);
                }
            }
            JS_FreeCString(ctx, val);
        }
        JS_FreeCString(ctx, name);
        return JS_DupValue(ctx, this_val);
    }

    /* Getter */
    if (strcmp(name, "tagName") == 0 || strcmp(name, "nodeName") == 0) {
        for (size_t i = 0; i < ch->count; i++) {
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                size_t l;
                const lxb_char_t *n = lxb_dom_element_qualified_name(
                    (lxb_dom_element_t *)ch->nodes[i], &l);
                if (n) {
                    /* tagName/nodeName are uppercase in browsers and cheerio */
                    char *upper = (char *)js_malloc(ctx, l + 1);
                    if (upper) {
                        for (size_t j = 0; j < l; j++)
                            upper[j] = toupper((char)n[j]);
                        upper[l] = '\0';
                        ret = JS_NewStringLen(ctx, upper, l);
                        js_free(ctx, upper);
                    }
                }
                break;
            }
        }
    } else if (strcmp(name, "innerHTML") == 0) {
        qjs_sb_t sb; qjs_sb_init(&sb);
        for (size_t i = 0; i < ch->count; i++) {
            lxb_dom_node_t *child = ch->nodes[i]->first_child;
            while (child) {
                lxb_html_serialize_tree_cb(child, qjs_ser_cb, &sb);
                child = child->next;
            }
        }
        ret = sb.buf ? qjs_sb_to_js(ctx, &sb) : JS_NewString(ctx, "");
    } else if (strcmp(name, "outerHTML") == 0) {
        qjs_sb_t sb; qjs_sb_init(&sb);
        for (size_t i = 0; i < ch->count; i++)
            lxb_html_serialize_cb(ch->nodes[i], qjs_ser_cb, &sb);
        ret = qjs_sb_to_js(ctx, &sb);
    } else if (strcmp(name, "textContent") == 0) {
        qjs_sb_t sb; qjs_sb_init(&sb);
        for (size_t i = 0; i < ch->count; i++) {
            size_t len;
            const lxb_char_t *t = lxb_dom_node_text_content(ch->nodes[i], &len);
            if (t && len > 0) qjs_ser_cb(t, len, &sb);
        }
        ret = qjs_sb_to_js(ctx, &sb);
    } else {
        /* Treat as attribute */
        for (size_t i = 0; i < ch->count; i++) {
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                size_t vlen;
                const lxb_char_t *v = lxb_dom_element_get_attribute(
                    (lxb_dom_element_t *)ch->nodes[i],
                    (const lxb_char_t *)name, nlen, &vlen);
                if (v) ret = JS_NewStringLen(ctx, (const char*)v, vlen);
                break;
            }
        }
    }
    JS_FreeCString(ctx, name);
    return ret;
}

/* .data(name) get / .data(name, value) set / .data({k:v}) set */
static JSValue qjs_ch_data(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_UNDEFINED;

    /* data({key: val, ...}) — batch set from object */
    if (JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0]) &&
        !JS_IsArray(ctx, argv[0])) {
        JSPropertyEnum *props = NULL;
        uint32_t n_props = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &n_props, argv[0],
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t p = 0; p < n_props; p++) {
                JSAtom atom = props[p].atom;
                const char *kname = JS_AtomToCString(ctx, atom);
                if (kname) {
                    JSValue kv = JS_GetProperty(ctx, argv[0], atom);
                    if (!JS_IsUndefined(kv) && !JS_IsNull(kv)) {
                        const char *vstr; size_t vlen;
                        vstr = JS_ToCStringLen(ctx, &vlen, kv);
                        if (vstr) {
                            char attr_name[256];
                            snprintf(attr_name, sizeof(attr_name), "data-%s", kname);
                            for (size_t i = 0; i < ch->count; i++)
                                if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                                    lxb_dom_element_set_attribute(
                                        (lxb_dom_element_t *)ch->nodes[i],
                                        (const lxb_char_t *)attr_name, strlen(attr_name),
                                        (const lxb_char_t *)vstr, vlen);
                            JS_FreeCString(ctx, vstr);
                        }
                    }
                    JS_FreeValue(ctx, kv);
                    JS_FreeCString(ctx, kname);
                }
                JS_FreeAtom(ctx, atom);
            }
            js_free(ctx, props);
        }
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    const char *name; size_t nlen;
    name = JS_ToCStringLen(ctx, &nlen, argv[0]);
    if (!name) return JS_EXCEPTION;

    /* Setter: data(name, value) */
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        char attr_name[256];
        snprintf(attr_name, sizeof(attr_name), "data-%s", name);
        JS_FreeCString(ctx, name);
        size_t alen = strlen(attr_name);
        const char *val; size_t vlen;
        val = JS_ToCStringLen(ctx, &vlen, argv[1]);
        if (!val) return JS_EXCEPTION;
        for (size_t i = 0; i < ch->count; i++)
            if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT)
                lxb_dom_element_set_attribute(
                    (lxb_dom_element_t *)ch->nodes[i],
                    (const lxb_char_t *)attr_name, alen,
                    (const lxb_char_t *)val, vlen);
        JS_FreeCString(ctx, val);
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    /* Getter: read data-{name} attribute */
    char attr_name[256];
    snprintf(attr_name, sizeof(attr_name), "data-%s", name);
    JS_FreeCString(ctx, name);

    JSValue ret = JS_UNDEFINED;
    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            size_t vlen;
            const lxb_char_t *v = lxb_dom_element_get_attribute(
                (lxb_dom_element_t *)ch->nodes[i],
                (const lxb_char_t *)attr_name, strlen(attr_name), &vlen);
            if (v) ret = JS_NewStringLen(ctx, (const char*)v, vlen);
            break;
        }
    }
    return ret;
}

/* .index() / .index(selector) / .index(element) */
static JSValue qjs_ch_index(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (ch->count == 0) return JS_NewInt32(ctx, -1);

    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        /* index(cheerioElement) — position of that element in current set */
        qjs_cheerio_t *arg_ch = JS_GetOpaque(argv[0], QJS_CORE_CLASS_ID_CHEERIO);
        if (arg_ch) {
            if (arg_ch->count == 0) return JS_NewInt32(ctx, -1);
            lxb_dom_node_t *target = arg_ch->nodes[0];
            for (size_t i = 0; i < ch->count; i++)
                if (ch->nodes[i] == target) return JS_NewInt32(ctx, (int32_t)i);
            return JS_NewInt32(ctx, -1);
        }
        /* index(selector) — position of first current element within the
           set of nodes matched by the selector (document-wide) */
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (!sel) return JS_EXCEPTION;
        lxb_dom_node_t *root = (lxb_dom_node_t *)
            lxb_dom_document_root(&ch->doc->document->dom_document);
        size_t found_count = 0; bool borrowed = false;
        lxb_dom_node_t **found = qjs_find_nodes(ctx, ch->doc, root, sel, sl,
                                                &found_count, &borrowed);
        JS_FreeCString(ctx, sel);
        int result = -1;
        if (found) {
            lxb_dom_node_t *target = ch->nodes[0];
            for (size_t i = 0; i < found_count; i++)
                if (found[i] == target) { result = (int)i; break; }
            if (!borrowed) free(found);
        }
        return JS_NewInt32(ctx, result);
    }

    /* index() — position of first element among its siblings */
    lxb_dom_node_t *node = ch->nodes[0];
    lxb_dom_node_t *parent = node->parent;
    if (!parent) return JS_NewInt32(ctx, 0);

    int idx = 0;
    for (lxb_dom_node_t *c = parent->first_child; c; c = c->next) {
        if (c == node) return JS_NewInt32(ctx, idx);
        idx++;
    }
    return JS_NewInt32(ctx, -1);
}

/* .nextAll() - get all following siblings */
static JSValue qjs_ch_nextAll(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* Parse optional selector filter — cheerio: nextAll(selector) only
       returns following siblings that match the selector. */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
        }
    }

    size_t cap = ch->count * 8;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *n = ch->nodes[i]->next;
        while (n) {
            /* If filter provided, only include matching element nodes */
            bool include = true;
            if (filter_list) {
                if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) include = false;
                else {
                    qjs_found_ctx_t fctx = { false };
                    lxb_selectors_match_node(ch->doc->selectors, n, filter_list,
                        qjs_selectors_found_cb, &fctx);
                    include = fctx.found;
                }
            }
            if (include) {
                if (cnt >= cap) {
                    cap = cap * 2 + 8;
                    lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                    if (!nn) { js_free(ctx, nodes); return JS_EXCEPTION; }
                    nodes = nn;
                }
                nodes[cnt++] = n;
            }
            n = n->next;
        }
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .prevAll() - get all preceding siblings */
static JSValue qjs_ch_prevAll(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* Parse optional selector filter — cheerio: prevAll(selector) only
       returns preceding siblings that match the selector. */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
        }
    }

    size_t cap = ch->count * 8;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *n = ch->nodes[i]->prev;
        while (n) {
            /* If filter provided, only include matching element nodes */
            bool include = true;
            if (filter_list) {
                if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) include = false;
                else {
                    qjs_found_ctx_t fctx = { false };
                    lxb_selectors_match_node(ch->doc->selectors, n, filter_list,
                        qjs_selectors_found_cb, &fctx);
                    include = fctx.found;
                }
            }
            if (include) {
                if (cnt >= cap) {
                    cap = cap * 2 + 8;
                    lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                    if (!nn) { js_free(ctx, nodes); return JS_EXCEPTION; }
                    nodes = nn;
                }
                nodes[cnt++] = n;
            }
            n = n->prev;
        }
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .nextUntil(selector, [filter]) - get following siblings until selector */
static JSValue qjs_ch_nextUntil(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    const char *sel = NULL; size_t sl = 0;
    lxb_css_selector_list_t *until_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            until_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
        }
    }
    /* Second argument is a filter selector — only include matching nodes */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char *fsel; size_t fsl;
        fsel = JS_ToCStringLen(ctx, &fsl, argv[1]);
        if (fsel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)fsel, fsl);
            JS_FreeCString(ctx, fsel);
        }
    }
    size_t cap = ch->count * 8;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) { if (sel) JS_FreeCString(ctx, sel); return JS_EXCEPTION; }
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *n = ch->nodes[i]->next;
        while (n) {
            if (until_list && n->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                /* CSS selector match — stop if matches */
                qjs_found_ctx_t fctx = { false };
                lxb_selectors_match_node(ch->doc->selectors, n, until_list,
                    qjs_selectors_found_cb, &fctx);
                if (fctx.found) break;
            }
            /* If filter provided, only include matching element nodes */
            bool include = true;
            if (filter_list) {
                if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) include = false;
                else {
                    qjs_found_ctx_t fctx2 = { false };
                    lxb_selectors_match_node(ch->doc->selectors, n, filter_list,
                        qjs_selectors_found_cb, &fctx2);
                    include = fctx2.found;
                }
            }
            if (include) {
                if (cnt >= cap) {
                    cap = cap * 2 + 8;
                    lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                    if (!nn) { js_free(ctx, nodes); if (sel) JS_FreeCString(ctx, sel); return JS_EXCEPTION; }
                    nodes = nn;
                }
                nodes[cnt++] = n;
            }
            n = n->next;
        }
    }
    if (sel) JS_FreeCString(ctx, sel);
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .parents() - get all ancestors */
static JSValue qjs_ch_parents(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* Parse optional selector filter */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
        }
    }

    size_t cap = ch->count * 32;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *p = ch->nodes[i]->parent;
        while (p && p->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            bool dup = false;
            for (size_t j = 0; j < cnt; j++) if (nodes[j]==p) { dup=true; break; }
            if (!dup) {
                /* If filter provided, check if node matches */
                if (filter_list) {
                    qjs_found_ctx_t fctx = { false };
                    lxb_selectors_match_node(ch->doc->selectors, p, filter_list,
                        qjs_selectors_found_cb, &fctx);
                    if (!fctx.found) { p = p->parent; continue; }
                }
                if (cnt >= cap) {
                    cap = cap * 2 + 8;
                    lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                    if (!nn) { js_free(ctx, nodes); return JS_EXCEPTION; }
                    nodes = nn;
                }
                nodes[cnt++] = p;
            }
            p = p->parent;
        }
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .parentsUntil(selector, [filter]) - get ancestors until selector */
static JSValue qjs_ch_parentsUntil(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    const char *sel = NULL; size_t sl = 0;
    lxb_css_selector_list_t *until_list = NULL;
    if (argc >= 1 && !JS_IsUndefined(argv[0])) {
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            until_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)sel, sl);
        }
    }
    /* Second argument is a filter selector — only include matching nodes */
    lxb_css_selector_list_t *filter_list = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char *fsel; size_t fsl;
        fsel = JS_ToCStringLen(ctx, &fsl, argv[1]);
        if (fsel) {
            filter_list = lxb_css_selectors_parse_relative_list(
                ch->doc->css_parser, (const lxb_char_t *)fsel, fsl);
            JS_FreeCString(ctx, fsel);
        }
    }
    size_t cap = ch->count * 32;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        cap * sizeof(lxb_dom_node_t *));
    if (!nodes) { if (sel) JS_FreeCString(ctx, sel); return JS_EXCEPTION; }
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *p = ch->nodes[i]->parent;
        while (p && p->type != LXB_DOM_NODE_TYPE_DOCUMENT) {
            if (until_list && p->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                /* CSS selector match — stop if matches */
                qjs_found_ctx_t fctx = { false };
                lxb_selectors_match_node(ch->doc->selectors, p, until_list,
                    qjs_selectors_found_cb, &fctx);
                if (fctx.found) break;
            }
            /* If filter provided, only include matching element nodes */
            bool include = true;
            if (filter_list) {
                if (p->type != LXB_DOM_NODE_TYPE_ELEMENT) include = false;
                else {
                    qjs_found_ctx_t fctx2 = { false };
                    lxb_selectors_match_node(ch->doc->selectors, p, filter_list,
                        qjs_selectors_found_cb, &fctx2);
                    include = fctx2.found;
                }
            }
            if (include) {
                bool dup = false;
                for (size_t j = 0; j < cnt; j++) if (nodes[j]==p) { dup=true; break; }
                if (!dup) {
                    if (cnt >= cap) {
                        cap = cap * 2 + 8;
                        lxb_dom_node_t **nn = (lxb_dom_node_t **)js_realloc(ctx, nodes, cap * sizeof(lxb_dom_node_t *));
                        if (!nn) { js_free(ctx, nodes); if (sel) JS_FreeCString(ctx, sel); return JS_EXCEPTION; }
                        nodes = nn;
                    }
                    nodes[cnt++] = p;
                }
            }
            p = p->parent;
        }
    }
    if (sel) JS_FreeCString(ctx, sel);
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* .slice(start, end) - subset of elements */
static JSValue qjs_ch_slice(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    int32_t start = 0, end = (int32_t)ch->count;
    if (argc >= 1) JS_ToInt32(ctx, &start, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &end, argv[1]);
    if (start < 0) start += (int32_t)ch->count;
    if (end < 0) end += (int32_t)ch->count;
    if (start < 0) start = 0;
    if (end > (int32_t)ch->count) end = (int32_t)ch->count;
    if (start >= end) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);
    return qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[start], end - start);
}

/* .add(selector) - add elements to current selection */
static JSValue qjs_ch_add(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);

    /* If it's a cheerio object, combine nodes */
    qjs_cheerio_t *new_ch = JS_GetOpaque(argv[0], QJS_CORE_CLASS_ID_CHEERIO);
    if (new_ch) {
        size_t total = ch->count + new_ch->count;
        lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
            total * sizeof(lxb_dom_node_t *));
        if (!nodes) return JS_EXCEPTION;
        size_t cnt = 0;
        for (size_t i = 0; i < ch->count; i++) nodes[cnt++] = ch->nodes[i];
        for (size_t i = 0; i < new_ch->count; i++) {
            bool dup = false;
            for (size_t j = 0; j < cnt; j++) if (nodes[j]==new_ch->nodes[i]) { dup=true; break; }
            if (!dup) nodes[cnt++] = new_ch->nodes[i];
        }
        JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
        js_free(ctx, nodes);
        return ret;
    }

    /* If it's HTML string, parse and add */
    const char *str; size_t len;
    str = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!str) return JS_EXCEPTION;

    if (len > 0 && str[0] == '<') {
        lxb_dom_element_t *fctx = qjs_get_frag_ctx(ch->doc->document);
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, fctx, (const lxb_char_t *)str, len);
        JS_FreeCString(ctx, str);
        if (!frag) return JS_DupValue(ctx, this_val);

        size_t cnt2 = 0;
        for (lxb_dom_node_t *c = frag->first_child; c; c = c->next) cnt2++;
        if (cnt2 == 0) return qjs_cheerio_wrap(ctx, ch->doc, NULL, 0);

        lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
            (ch->count + cnt2) * sizeof(lxb_dom_node_t *));
        if (!nodes) return JS_EXCEPTION;
        size_t cnt = 0;
        for (size_t i = 0; i < ch->count; i++) nodes[cnt++] = ch->nodes[i];
        for (lxb_dom_node_t *c = frag->first_child; c; c = c->next) nodes[cnt++] = c;
        JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
        js_free(ctx, nodes);
        return ret;
    }

    /* Otherwise treat as selector against document */
    lxb_dom_node_t *root = (lxb_dom_node_t *)
        lxb_dom_document_root(&ch->doc->document->dom_document);
    JSValue ret = qjs_cheerio_from_selector(ctx, ch->doc, str, len, root);
    JS_FreeCString(ctx, str);

    /* Combine with original */
    qjs_cheerio_t *sel_ch = JS_GetOpaque(ret, QJS_CORE_CLASS_ID_CHEERIO);
    if (!sel_ch) return ret;

    size_t total = ch->count + sel_ch->count;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        total * sizeof(lxb_dom_node_t *));
    if (!nodes) { JS_FreeValue(ctx, ret); return JS_EXCEPTION; }
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) nodes[cnt++] = ch->nodes[i];
    for (size_t i = 0; i < sel_ch->count; i++) {
        bool dup = false;
        for (size_t j = 0; j < cnt; j++) if (nodes[j]==sel_ch->nodes[i]) { dup=true; break; }
        if (!dup) nodes[cnt++] = sel_ch->nodes[i];
    }
    JS_FreeValue(ctx, ret);
    JSValue combined = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return combined;
}

/* .end() - end the current chain, returning the previous selection */
static JSValue qjs_ch_end(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (!JS_IsNull(ch->prev) && !JS_IsUndefined(ch->prev)) {
        return JS_DupValue(ctx, ch->prev);
    }
    /* No prevObject: cheerio returns the current selection */
    return JS_DupValue(ctx, this_val);
}

/* .text(value) - set text content */
static JSValue qjs_ch_textSet(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return qjs_ch_text(ctx, this_val, argc, argv);

    /* text(function(i, oldText)) — function callback */
    if (JS_IsFunction(ctx, argv[0])) {
        for (size_t i = 0; i < ch->count; i++) {
            /* get old text content */
            JSValue oldt = JS_UNDEFINED;
            size_t tlen;
            const lxb_char_t *t = lxb_dom_node_text_content(ch->nodes[i], &tlen);
            if (t) oldt = JS_NewStringLen(ctx, (const char *)t, tlen);
            JSValue node = qjs_cheerio_wrap(ctx, ch->doc, &ch->nodes[i], 1);
            JSValue args[2] = { JS_NewInt32(ctx, (int32_t)i), oldt };
            JSValue ret = JS_Call(ctx, argv[0], node, 2, args);
            JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
            JS_FreeValue(ctx, node);
            if (JS_IsException(ret)) return ret;
            const char *txt; size_t tl;
            txt = JS_ToCStringLen(ctx, &tl, ret);
            JS_FreeValue(ctx, ret);
            if (txt) {
                lxb_dom_node_t *c = ch->nodes[i]->first_child;
                while (c) { lxb_dom_node_t *n = c->next; lxb_dom_node_remove(c); c = n; }
                lxb_dom_text_t *tn = lxb_dom_document_create_text_node(
                    &ch->doc->document->dom_document,
                    (const lxb_char_t *)txt, tl);
                if (tn) lxb_dom_node_insert_child(ch->nodes[i], (lxb_dom_node_t *)tn);
                JS_FreeCString(ctx, txt);
            }
        }
        qjs_index_mark_dirty(ch->doc->idx);
        return JS_DupValue(ctx, this_val);
    }

    const char *text; size_t tlen;
    text = JS_ToCStringLen(ctx, &tlen, argv[0]);
    if (!text) return JS_EXCEPTION;

    for (size_t i = 0; i < ch->count; i++) {
        /* Remove all children */
        lxb_dom_node_t *c = ch->nodes[i]->first_child;
        while (c) { lxb_dom_node_t *n = c->next; lxb_dom_node_remove(c); c = n; }
        /* Create text node */
        lxb_dom_text_t *tn = lxb_dom_document_create_text_node(
            &ch->doc->document->dom_document,
            (const lxb_char_t *)text, tlen);
        if (tn) lxb_dom_node_insert_child(ch->nodes[i], (lxb_dom_node_t *)tn);
    }
    JS_FreeCString(ctx, text);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .html(value) - set inner HTML */
static JSValue qjs_ch_htmlSet(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return qjs_ch_html(ctx, this_val, argc, argv);

    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        /* Remove all children */
        lxb_dom_node_t *c = ch->nodes[i]->first_child;
        while (c) { lxb_dom_node_t *n = c->next; lxb_dom_node_remove(c); c = n; }
        /* Parse and insert new content */
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, (lxb_dom_element_t *)ch->nodes[i],
            (const lxb_char_t*)html, hl);
        if (frag) {
            lxb_dom_node_t *fc = frag->first_child;
            while (fc) { lxb_dom_node_t *n = fc->next;
                lxb_dom_node_insert_child(ch->nodes[i], fc); fc = n; }
        }
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .wrap(html) - wrap each element in the matched set inside the given HTML */
static JSValue qjs_ch_wrap(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");
    if (argc < 1) return JS_DupValue(ctx, this_val);
    const char *html; size_t hl;
    html = JS_ToCStringLen(ctx, &hl, argv[0]);
    if (!html) return JS_EXCEPTION;

    for (size_t i = 0; i < ch->count; i++) {
        if (ch->nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        lxb_dom_node_t *node = ch->nodes[i];
        lxb_dom_element_t *fctx = qjs_get_frag_ctx(ch->doc->document);
        lxb_dom_node_t *frag = lxb_html_document_parse_fragment(
            ch->doc->document, fctx, (const lxb_char_t *)html, hl);
        if (!frag) continue;
        /* Find first element child of fragment as the wrapper */
        lxb_dom_node_t *wrapper = frag->first_child;
        while (wrapper && wrapper->type != LXB_DOM_NODE_TYPE_ELEMENT)
            wrapper = wrapper->next;
        if (!wrapper) continue;
        /* Detach wrapper from fragment */
        lxb_dom_node_remove(wrapper);
        /* Insert wrapper before the node (in node's parent child list) */
        lxb_dom_node_insert_before(node, wrapper);
        /* Find innermost element of wrapper (deepest first element child) */
        lxb_dom_node_t *inner = wrapper;
        while (inner->first_child &&
               inner->first_child->type == LXB_DOM_NODE_TYPE_ELEMENT)
            inner = inner->first_child;
        /* Move node inside innermost element (must detach first to avoid
           tree corruption — lexbor insert_child does not auto-detach) */
        lxb_dom_node_remove(node);
        lxb_dom_node_insert_child(inner, node);
    }
    JS_FreeCString(ctx, html);
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .unwrap() - remove the parent of each element, keeping element and siblings */
static JSValue qjs_ch_unwrap(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* Collect unique parents to unwrap */
    lxb_dom_node_t *parents[256]; size_t np = 0;
    for (size_t i = 0; i < ch->count; i++) {
        lxb_dom_node_t *parent = ch->nodes[i]->parent;
        if (!parent) continue;
        if (parent->type == LXB_DOM_NODE_TYPE_DOCUMENT) continue;
        bool dup = false;
        for (size_t j = 0; j < np; j++)
            if (parents[j] == parent) { dup = true; break; }
        if (!dup && np < 256) parents[np++] = parent;
    }
    for (size_t i = 0; i < np; i++) {
        lxb_dom_node_t *parent = parents[i];
        /* Move all children of parent before parent (preserving order).
           Must detach each child first — lexbor insert_before does not
           auto-detach, which would corrupt the tree. */
        lxb_dom_node_t *c = parent->first_child;
        while (c) {
            lxb_dom_node_t *n = c->next;
            lxb_dom_node_remove(c);
            lxb_dom_node_insert_before(parent, c);
            c = n;
        }
        /* Remove the parent */
        lxb_dom_node_remove(parent);
    }
    qjs_index_mark_dirty(ch->doc->idx);
    return JS_DupValue(ctx, this_val);
}

/* .addBack([selector]) - add previous selection (prevObject) to current */
static JSValue qjs_ch_addBack(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    qjs_cheerio_t *ch = JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_CHEERIO);
    if (!ch) return JS_ThrowInternalError(ctx, "not a cheerio object");

    /* No prevObject: return current selection */
    if (JS_IsNull(ch->prev) || JS_IsUndefined(ch->prev))
        return JS_DupValue(ctx, this_val);

    qjs_cheerio_t *prev_ch = JS_GetOpaque(ch->prev, QJS_CORE_CLASS_ID_CHEERIO);
    if (!prev_ch) return JS_DupValue(ctx, this_val);

    lxb_dom_node_t **prev_nodes = prev_ch->nodes;
    size_t prev_count = prev_ch->count;

    /* If selector provided, filter prevObject nodes by selector */
    lxb_dom_node_t *filt_nodes[256]; size_t filt_count = 0;
    if (argc >= 1 && JS_IsString(argv[0])) {
        const char *sel; size_t sl;
        sel = JS_ToCStringLen(ctx, &sl, argv[0]);
        if (sel) {
            lxb_css_selector_list_t *list =
                lxb_css_selectors_parse_relative_list(
                    ch->doc->css_parser, (const lxb_char_t *)sel, sl);
            JS_FreeCString(ctx, sel);
            if (list) {
                for (size_t i = 0; i < prev_count && filt_count < 256; i++) {
                    if (prev_nodes[i]->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
                    qjs_found_ctx_t fctx = { false };
                    lxb_selectors_match_node(ch->doc->selectors,
                        prev_nodes[i], list, qjs_selectors_found_cb, &fctx);
                    if (fctx.found) filt_nodes[filt_count++] = prev_nodes[i];
                }
                /* list lives in css_parser memory pool, freed on doc destroy */
            }
        }
        prev_nodes = filt_nodes;
        prev_count = filt_count;
    }

    /* Combine current + prev (dedup) */
    size_t total = ch->count + prev_count;
    lxb_dom_node_t **nodes = (lxb_dom_node_t **)js_malloc(ctx,
        total * sizeof(lxb_dom_node_t *));
    if (!nodes) return JS_EXCEPTION;
    size_t cnt = 0;
    for (size_t i = 0; i < ch->count; i++) nodes[cnt++] = ch->nodes[i];
    for (size_t i = 0; i < prev_count; i++) {
        bool dup = false;
        for (size_t j = 0; j < cnt; j++)
            if (nodes[j] == prev_nodes[i]) { dup = true; break; }
        if (!dup) nodes[cnt++] = prev_nodes[i];
    }
    JSValue ret = qjs_cheerio_wrap(ctx, ch->doc, nodes, cnt);
    js_free(ctx, nodes);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Module registration                                                 */
/* ------------------------------------------------------------------ */

static const JSCFunctionListEntry qjs_cheerio_proto[] = {
    JS_CFUNC_DEF("find",        1, qjs_ch_find ),
    JS_CFUNC_DEF("text",        1, qjs_ch_textSet ),
    JS_CFUNC_DEF("html",        1, qjs_ch_htmlSet ),
    JS_CFUNC_DEF("outerHTML",   0, qjs_ch_outer ),
    JS_CFUNC_DEF("toString",    0, qjs_ch_outer ),
    JS_CFUNC_DEF("attr",        2, qjs_ch_attr ),
    JS_CFUNC_DEF("removeAttr",  1, qjs_ch_removeAttr ),
    JS_CFUNC_DEF("addClass",    1, qjs_ch_addClass ),
    JS_CFUNC_DEF("removeClass", 1, qjs_ch_removeClass ),
    JS_CFUNC_DEF("hasClass",    1, qjs_ch_hasClass ),
    JS_CFUNC_DEF("toggleClass", 1, qjs_ch_toggleClass ),
    JS_CFUNC_DEF("parent",      0, qjs_ch_parent ),
    JS_CFUNC_DEF("children",    0, qjs_ch_children ),
    JS_CFUNC_DEF("next",        0, qjs_ch_next ),
    JS_CFUNC_DEF("prev",        0, qjs_ch_prev ),
    JS_CFUNC_DEF("siblings",    0, qjs_ch_siblings ),
    JS_CFUNC_DEF("first",       0, qjs_ch_first ),
    JS_CFUNC_DEF("last",        0, qjs_ch_last ),
    JS_CFUNC_DEF("eq",          1, qjs_ch_eq ),
    JS_CFUNC_DEF("get",         1, qjs_ch_get ),
    JS_CFUNC_DEF("each",        1, qjs_ch_each ),
    JS_CFUNC_DEF("map",         1, qjs_ch_map ),
    JS_CFUNC_DEF("remove",      0, qjs_ch_remove ),
    JS_CFUNC_DEF("empty",       0, qjs_ch_empty ),
    JS_CFUNC_DEF("val",         1, qjs_ch_val ),
    JS_CFUNC_DEF("is",          1, qjs_ch_is ),
    JS_CFUNC_DEF("filter",      1, qjs_ch_filter ),
    JS_CFUNC_DEF("not",         1, qjs_ch_not ),
    JS_CFUNC_DEF("append",      1, qjs_ch_append ),
    JS_CFUNC_DEF("prepend",     1, qjs_ch_prepend ),
    JS_CFUNC_DEF("before",      1, qjs_ch_before ),
    JS_CFUNC_DEF("after",       1, qjs_ch_after ),
    JS_CFUNC_DEF("appendTo",    1, qjs_ch_appendTo ),
    JS_CFUNC_DEF("prependTo",   1, qjs_ch_prependTo ),
    JS_CFUNC_DEF("insertAfter", 1, qjs_ch_insertAfter ),
    JS_CFUNC_DEF("insertBefore",1, qjs_ch_insertBefore ),
    JS_CFUNC_DEF("replaceWith", 1, qjs_ch_replaceWith ),
    JS_CFUNC_DEF("wrap",        1, qjs_ch_wrap ),
    JS_CFUNC_DEF("unwrap",      0, qjs_ch_unwrap ),
    JS_CFUNC_DEF("clone",       0, qjs_ch_clone ),
    JS_CFUNC_DEF("tagName",     0, qjs_ch_tagName ),
    JS_CFUNC_DEF("toArray",     0, qjs_ch_toArray ),
    JS_CFUNC_DEF("closest",     1, qjs_ch_closest ),
    JS_CFUNC_DEF("contents",    0, qjs_ch_contents ),
    JS_CFUNC_DEF("prop",        2, qjs_ch_prop ),
    JS_CFUNC_DEF("data",        1, qjs_ch_data ),
    JS_CFUNC_DEF("index",       0, qjs_ch_index ),
    JS_CFUNC_DEF("nextAll",     0, qjs_ch_nextAll ),
    JS_CFUNC_DEF("prevAll",     0, qjs_ch_prevAll ),
    JS_CFUNC_DEF("nextUntil",   1, qjs_ch_nextUntil ),
    JS_CFUNC_DEF("parents",     0, qjs_ch_parents ),
    JS_CFUNC_DEF("parentsUntil",1, qjs_ch_parentsUntil ),
    JS_CFUNC_DEF("slice",       2, qjs_ch_slice ),
    JS_CFUNC_DEF("add",         1, qjs_ch_add ),
    JS_CFUNC_DEF("addBack",     0, qjs_ch_addBack ),
    JS_CFUNC_DEF("end",         0, qjs_ch_end ),
};

static const JSCFunctionListEntry qjs_cheerio_globals[] = {
    JS_CFUNC_DEF("load", 1, qjs_cheerio_load ),
};

int qjs_html_init(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue global = JS_GetGlobalObject(ctx);

    JSClassDef doc_def = {
        .class_name = "CheerioDoc",
        .finalizer = qjs_doc_finalizer,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_CHEERIO_DOC, &doc_def);

    JSClassDef ch_def = {
        .class_name = "Cheerio",
        .finalizer = qjs_cheerio_finalizer,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_CHEERIO, &ch_def);

    JSValue ch_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, ch_proto, qjs_cheerio_proto,
        sizeof(qjs_cheerio_proto) / sizeof(JSCFunctionListEntry));
    JS_SetClassProto(ctx, QJS_CORE_CLASS_ID_CHEERIO, ch_proto);

    JSValue cheerio_obj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, cheerio_obj, qjs_cheerio_globals,
        sizeof(qjs_cheerio_globals) / sizeof(JSCFunctionListEntry));
    JS_SetPropertyStr(ctx, global, "cheerio", cheerio_obj);

    JS_FreeValue(ctx, global);
    return 0;
}
