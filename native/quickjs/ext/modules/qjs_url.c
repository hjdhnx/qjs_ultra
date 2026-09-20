/*
 * qjs_url.c - URL and URLSearchParams
 *
 * Uses the Lexbor URL parser (WHATWG URL Standard implementation) to provide
 * full URL parsing and serialization. Supports base URLs, all standard URL
 * components (protocol, hostname, port, pathname, search, hash, username, password),
 * and URLSearchParams for query string manipulation.
 */

#include "qjs_native.h"
#include <lexbor/url/url.h>
#include <lexbor/core/core.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Serialization helper */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} qjs_strbuf_t;

static lxb_status_t qjs_serialize_cb(const lxb_char_t *data, size_t len, void *ctx) {
    qjs_strbuf_t *sb = (qjs_strbuf_t *)ctx;
    if (sb->len + len + 1 > sb->cap) {
        size_t new_cap = (sb->len + len + 1) * 2;
        char *new_buf = (char *)realloc(sb->buf, new_cap);
        if (!new_buf) return LXB_STATUS_ERROR;
        sb->buf = new_buf;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, data, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
    return LXB_STATUS_OK;
}

static JSValue qjs_strbuf_to_js(JSContext *ctx, qjs_strbuf_t *sb) {
    if (!sb->buf) {
        return JS_NewString(ctx, "");
    }
    JSValue ret = JS_NewStringLen(ctx, sb->buf, sb->len);
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return ret;
}

static void qjs_strbuf_init(qjs_strbuf_t *sb) {
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/* ------------------------------------------------------------------ */
/* URL class */
/* ------------------------------------------------------------------ */

typedef struct {
    lxb_url_parser_t *parser;
    lxb_url_t *url;
    lxb_url_t *base_url;
    JSValue sp_obj;    /* cached URLSearchParams (live link) */
} qjs_url_obj_t;

static void qjs_url_finalizer(JSRuntime *rt, JSValue val) {
    qjs_url_obj_t *u = (qjs_url_obj_t *)JS_GetOpaque(val, QJS_CORE_CLASS_ID_URL);
    if (u) {
        if (u->url) lxb_url_destroy(u->url);
        if (u->base_url) lxb_url_destroy(u->base_url);
        if (u->parser) {
            lxb_url_parser_memory_destroy(u->parser);
            lxb_url_parser_destroy(u->parser, true);
        }
        JS_FreeValueRT(rt, u->sp_obj);
        js_free_rt(rt, u);
    }
}

/* GC mark: tell GC about cached searchParams (cycle detection) */
static void qjs_url_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    qjs_url_obj_t *u = (qjs_url_obj_t *)JS_GetOpaque(val, QJS_CORE_CLASS_ID_URL);
    if (u) {
        JS_MarkValue(rt, u->sp_obj, mark_func);
    }
}

static JSValue qjs_url_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    if (argc < 1 || JS_IsUndefined(argv[0]))
        return JS_ThrowTypeError(ctx, "URL requires at least 1 argument");

    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    const char *base_str = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        base_str = JS_ToCString(ctx, argv[1]);
        if (!base_str) {
            JS_FreeCString(ctx, url_str);
            return JS_EXCEPTION;
        }
    }

    qjs_url_obj_t *u = (qjs_url_obj_t *)js_malloc(ctx, sizeof(qjs_url_obj_t));
    if (!u) {
        JS_FreeCString(ctx, url_str);
        if (base_str) JS_FreeCString(ctx, base_str);
        return JS_EXCEPTION;
    }
    memset(u, 0, sizeof(*u));
    u->sp_obj = JS_NULL;

    u->parser = lxb_url_parser_create();
    if (!u->parser) {
        js_free(ctx, u);
        JS_FreeCString(ctx, url_str);
        if (base_str) JS_FreeCString(ctx, base_str);
        return JS_ThrowInternalError(ctx, "URL: failed to create parser");
    }

    lxb_status_t status = lxb_url_parser_init(u->parser, NULL);
    if (status != LXB_STATUS_OK) {
        lxb_url_parser_destroy(u->parser, true);
        js_free(ctx, u);
        JS_FreeCString(ctx, url_str);
        if (base_str) JS_FreeCString(ctx, base_str);
        return JS_ThrowInternalError(ctx, "URL: failed to init parser");
    }

    if (base_str) {
        u->base_url = lxb_url_parse(u->parser, NULL, (const lxb_char_t *)base_str, strlen(base_str));
        lxb_url_parser_clean(u->parser);
        if (!u->base_url) {
            lxb_url_parser_memory_destroy(u->parser);
            lxb_url_parser_destroy(u->parser, true);
            js_free(ctx, u);
            JS_FreeCString(ctx, url_str);
            JS_FreeCString(ctx, base_str);
            return JS_ThrowTypeError(ctx, "Invalid URL");
        }
    }

    u->url = lxb_url_parse(u->parser, u->base_url, (const lxb_char_t *)url_str, strlen(url_str));
    if (!u->url) {
        lxb_url_parser_memory_destroy(u->parser);
        lxb_url_parser_destroy(u->parser, true);
        js_free(ctx, u);
        JS_FreeCString(ctx, url_str);
        if (base_str) JS_FreeCString(ctx, base_str);
        return JS_ThrowTypeError(ctx, "Invalid URL");
    }

    JS_FreeCString(ctx, url_str);
    if (base_str) JS_FreeCString(ctx, base_str);

    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_URL);
    if (JS_IsException(obj)) {
        lxb_url_destroy(u->url);
        if (u->base_url) lxb_url_destroy(u->base_url);
        lxb_url_parser_memory_destroy(u->parser);
        lxb_url_parser_destroy(u->parser, true);
        js_free(ctx, u);
        return obj;
    }

    JS_SetOpaque(obj, u);
    return obj;
}

static qjs_url_obj_t *qjs_url_get(JSValueConst this_val) {
    return (qjs_url_obj_t *)JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_URL);
}

/* href getter */
static JSValue qjs_url_get_href(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize(u->url, qjs_serialize_cb, &sb, false);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* origin getter */
static JSValue qjs_url_get_origin(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_scheme(u->url, qjs_serialize_cb, &sb);
    qjs_serialize_cb((const lxb_char_t *)"://", 3, &sb);
    lxb_url_serialize_host(&u->url->host, qjs_serialize_cb, &sb);
    if (u->url->has_port) {
        qjs_serialize_cb((const lxb_char_t *)":", 1, &sb);
        lxb_url_serialize_port(u->url, qjs_serialize_cb, &sb);
    }
    return qjs_strbuf_to_js(ctx, &sb);
}

/* protocol getter */
static JSValue qjs_url_get_protocol(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_scheme(u->url, qjs_serialize_cb, &sb);
    qjs_serialize_cb((const lxb_char_t *)":", 1, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* username getter */
static JSValue qjs_url_get_username(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_username(u->url, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* password getter */
static JSValue qjs_url_get_password(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_password(u->url, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* host getter */
static JSValue qjs_url_get_host(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_host(&u->url->host, qjs_serialize_cb, &sb);
    if (u->url->has_port) {
        qjs_serialize_cb((const lxb_char_t *)":", 1, &sb);
        lxb_url_serialize_port(u->url, qjs_serialize_cb, &sb);
    }
    return qjs_strbuf_to_js(ctx, &sb);
}

/* hostname getter */
static JSValue qjs_url_get_hostname(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_host(&u->url->host, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* port getter */
static JSValue qjs_url_get_port(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    if (!u->url->has_port) return JS_NewString(ctx, "");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_port(u->url, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* pathname getter */
static JSValue qjs_url_get_pathname(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize_path(&u->url->path, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* search getter */
static JSValue qjs_url_get_search(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    if (u->url->query.data == NULL || u->url->query.length == 0) return JS_NewString(ctx, "");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    qjs_serialize_cb((const lxb_char_t *)"?", 1, &sb);
    lxb_url_serialize_query(u->url, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* hash getter */
static JSValue qjs_url_get_hash(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    if (u->url->fragment.data == NULL || u->url->fragment.length == 0) return JS_NewString(ctx, "");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    qjs_serialize_cb((const lxb_char_t *)"#", 1, &sb);
    lxb_url_serialize_fragment(u->url, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* ------------------------------------------------------------------ */
/* URL setters — re-parse with modified component (Node.js compatible) */
/* ------------------------------------------------------------------ */

/* Helper: re-parse URL from string, replacing internal url+base */
static JSValue qjs_url_reparse(JSContext *ctx, qjs_url_obj_t *u,
                               const char *new_href) {
    lxb_url_t *new_url = lxb_url_parse(u->parser, NULL,
        (const lxb_char_t *)new_href, strlen(new_href));
    if (!new_url) return JS_ThrowTypeError(ctx, "Invalid URL");
    /* Swap: destroy old, keep new */
    lxb_url_destroy(u->url);
    u->url = new_url;
    /* Invalidate cached searchParams (will be recreated on next access) */
    if (!JS_IsNull(u->sp_obj) && !JS_IsUndefined(u->sp_obj)) {
        JS_FreeValue(ctx, u->sp_obj);
        u->sp_obj = JS_NULL;
    }
    return JS_UNDEFINED;
}

/* href setter */
static JSValue qjs_url_set_href(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    lxb_url_parser_clean(u->parser);
    JSValue ret = qjs_url_reparse(ctx, u, str);
    JS_FreeCString(ctx, str);
    return ret;
}

/* Helper: get current href as C string (caller frees) */
static char *qjs_url_href_str(qjs_url_obj_t *u) {
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_serialize(u->url, qjs_serialize_cb, &sb, false);
    return sb.buf ? sb.buf : strdup("");
}

/* protocol setter — replace scheme and re-parse */
static JSValue qjs_url_set_protocol(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *proto_cstr = JS_ToCString(ctx, val);
    if (!proto_cstr) return JS_EXCEPTION;
    /* Normalize: ensure trailing : */
    size_t plen = strlen(proto_cstr);
    char *pbuf = NULL;
    const char *proto;
    if (plen > 0 && proto_cstr[plen-1] != ':') {
        pbuf = (char *)malloc(plen + 2);
        memcpy(pbuf, proto_cstr, plen); pbuf[plen] = ':'; pbuf[plen+1] = '\0';
        proto = pbuf;
    } else {
        proto = proto_cstr;
    }
    /* Build new URL: protocol + rest of current URL (after scheme) */
    char *href = qjs_url_href_str(u);
    /* Find :// in href */
    char *sep = strstr(href, "://");
    if (sep) {
        size_t total = strlen(proto) + strlen(sep + 1) + 2;
        char *new_href = (char *)malloc(total + 1);
        snprintf(new_href, total + 1, "%s//%s", proto, sep + 3);
        lxb_url_parser_clean(u->parser);
        qjs_url_reparse(ctx, u, new_href);
        free(new_href);
    }
    free(href);
    free(pbuf);
    JS_FreeCString(ctx, proto_cstr);
    return JS_UNDEFINED;
}

/* username setter */
static JSValue qjs_url_set_username(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    /* lexbor doesn't have a direct setter; re-parse with modified URL */
    char *href = qjs_url_href_str(u);
    /* Build new URL with username */
    char *at = strchr(href, '@');
    char *scheme_sep = strstr(href, "://");
    if (scheme_sep) {
        char *host_start = scheme_sep + 3;
        char *at_in_auth = strchr(host_start, '@');
        char *path_start = at_in_auth ? at_in_auth + 1 : host_start;
        /* Remove existing userinfo if any */
        size_t scheme_len = scheme_sep - href + 3;
        size_t path_len = strlen(path_start);
        size_t ulen = strlen(str);
        char *new_href = (char *)malloc(scheme_len + ulen + 1 + path_len + 1);
        memcpy(new_href, href, scheme_len);
        memcpy(new_href + scheme_len, str, ulen);
        new_href[scheme_len + ulen] = '@';
        memcpy(new_href + scheme_len + ulen + 1, path_start, path_len + 1);
        lxb_url_parser_clean(u->parser);
        qjs_url_reparse(ctx, u, new_href);
        free(new_href);
    }
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* password setter */
static JSValue qjs_url_set_password(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    /* Similar to username setter but includes user:pass@ */
    char *scheme_sep = strstr(href, "://");
    if (scheme_sep) {
        char *host_start = scheme_sep + 3;
        char *at_in_auth = strchr(host_start, '@');
        char *path_start = at_in_auth ? at_in_auth + 1 : host_start;
        /* Get current username */
        char *colon = at_in_auth ? strchr(host_start, ':') : NULL;
        char *username = NULL;
        size_t ulen = 0;
        if (colon && colon < at_in_auth) {
            ulen = colon - host_start;
            username = strndup(host_start, ulen);
        }
        size_t scheme_len = scheme_sep - href + 3;
        size_t plen = strlen(str);
        size_t path_len = strlen(path_start);
        char *new_href = (char *)malloc(scheme_len + (username ? ulen : 0) + 1 + plen + 1 + path_len + 1);
        char *p = new_href;
        memcpy(p, href, scheme_len); p += scheme_len;
        if (username) { memcpy(p, username, ulen); p += ulen; }
        *p++ = ':';
        memcpy(p, str, plen); p += plen;
        *p++ = '@';
        memcpy(p, path_start, path_len + 1);
        lxb_url_parser_clean(u->parser);
        qjs_url_reparse(ctx, u, new_href);
        free(new_href);
        free(username);
    }
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* hostname setter */
static JSValue qjs_url_set_hostname(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    char *scheme_sep = strstr(href, "://");
    if (scheme_sep) {
        char *host_start = scheme_sep + 3;
        /* Skip userinfo */
        char *at = strchr(host_start, '@');
        if (at) host_start = at + 1;
        /* Find end of host (port, path, query, hash) */
        char *end = host_start;
        while (*end && *end != ':' && *end != '/' && *end != '?' && *end != '#') end++;
        size_t prefix = host_start - href;
        size_t suffix = strlen(end);
        size_t hlen = strlen(str);
        char *new_href = (char *)malloc(prefix + hlen + suffix + 1);
        memcpy(new_href, href, prefix);
        memcpy(new_href + prefix, str, hlen);
        memcpy(new_href + prefix + hlen, end, suffix + 1);
        lxb_url_parser_clean(u->parser);
        qjs_url_reparse(ctx, u, new_href);
        free(new_href);
    }
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* port setter */
static JSValue qjs_url_set_port(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    char *scheme_sep = strstr(href, "://");
    if (scheme_sep) {
        char *host_start = scheme_sep + 3;
        char *at = strchr(host_start, '@');
        if (at) host_start = at + 1;
        /* Find port or path */
        char *colon = NULL;
        char *end = host_start;
        while (*end && *end != '/' && *end != '?' && *end != '#') {
            if (*end == ':' && !colon) colon = end;
            end++;
        }
        size_t prefix = colon ? (size_t)(colon - href) + 1 : (size_t)(end - href);
        size_t suffix = strlen(end);
        size_t plen = strlen(str);
        if (plen > 0) {
            /* Need colon before port if not already there */
            bool need_colon = !colon;
            char *new_href = (char *)malloc(prefix + (need_colon ? 1 : 0) + plen + suffix + 1);
            memcpy(new_href, href, prefix);
            char *p = new_href + prefix;
            if (need_colon) *p++ = ':';
            memcpy(p, str, plen); p += plen;
            memcpy(p, end, suffix + 1);
            lxb_url_parser_clean(u->parser);
            qjs_url_reparse(ctx, u, new_href);
            free(new_href);
        } else {
            /* Empty port: remove port */
            if (colon) {
                char *new_href = (char *)malloc((colon - href) + suffix + 1);
                memcpy(new_href, href, colon - href);
                memcpy(new_href + (colon - href), end, suffix + 1);
                lxb_url_parser_clean(u->parser);
                qjs_url_reparse(ctx, u, new_href);
                free(new_href);
            }
        }
    }
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* pathname setter */
static JSValue qjs_url_set_pathname(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    /* Find start of path (after host) */
    char *scheme_sep = strstr(href, "://");
    if (scheme_sep) {
        char *p = scheme_sep + 3;
        char *at = strchr(p, '@');
        if (at) p = at + 1;
        while (*p && *p != '/' && *p != '?' && *p != '#') p++;
        /* Find end of path (query or hash) */
        char *end = p;
        while (*end && *end != '?' && *end != '#') end++;
        size_t prefix = p - href;
        size_t suffix = strlen(end);
        size_t plen = strlen(str);
        char *new_href = (char *)malloc(prefix + plen + suffix + 1);
        memcpy(new_href, href, prefix);
        memcpy(new_href + prefix, str, plen);
        memcpy(new_href + prefix + plen, end, suffix + 1);
        lxb_url_parser_clean(u->parser);
        qjs_url_reparse(ctx, u, new_href);
        free(new_href);
    }
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* search setter */
static JSValue qjs_url_set_search(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    /* Remove existing query */
    char *q = strchr(href, '?');
    char *hash = strchr(href, '#');
    char *end = q ? q : (hash ? hash : href + strlen(href));
    size_t prefix = end - href;
    size_t suffix = hash ? strlen(hash) : 0;
    size_t slen = strlen(str);
    char *new_href;
    if (slen > 0) {
        /* Ensure leading ? */
        bool need_q = (str[0] != '?');
        new_href = (char *)malloc(prefix + (need_q ? 1 : 0) + slen + suffix + 1);
        memcpy(new_href, href, prefix);
        char *p = new_href + prefix;
        if (need_q) *p++ = '?';
        memcpy(p, str, slen); p += slen;
        if (hash) memcpy(p, hash, suffix + 1);
        else *p = '\0';
    } else {
        new_href = (char *)malloc(prefix + suffix + 1);
        memcpy(new_href, href, prefix);
        if (hash) memcpy(new_href + prefix, hash, suffix + 1);
        else new_href[prefix] = '\0';
    }
    lxb_url_parser_clean(u->parser);
    qjs_url_reparse(ctx, u, new_href);
    free(new_href);
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* hash setter */
static JSValue qjs_url_set_hash(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");
    const char *str = JS_ToCString(ctx, val);
    if (!str) return JS_EXCEPTION;
    char *href = qjs_url_href_str(u);
    /* Remove existing hash */
    char *hash = strchr(href, '#');
    size_t prefix = hash ? (size_t)(hash - href) : strlen(href);
    size_t slen = strlen(str);
    char *new_href;
    if (slen > 0) {
        bool need_h = (str[0] != '#');
        new_href = (char *)malloc(prefix + (need_h ? 1 : 0) + slen + 1);
        memcpy(new_href, href, prefix);
        char *p = new_href + prefix;
        if (need_h) *p++ = '#';
        memcpy(p, str, slen + 1);
    } else {
        new_href = (char *)malloc(prefix + 1);
        memcpy(new_href, href, prefix);
        new_href[prefix] = '\0';
    }
    lxb_url_parser_clean(u->parser);
    qjs_url_reparse(ctx, u, new_href);
    free(new_href);
    free(href);
    JS_FreeCString(ctx, str);
    return JS_UNDEFINED;
}

/* searchParams getter - returns a URLSearchParams object */
static JSValue qjs_url_get_searchParams(JSContext *ctx, JSValueConst this_val); /* forward declaration */

/* toString */
static JSValue qjs_url_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return qjs_url_get_href(ctx, this_val);
}

/* toJSON */
static JSValue qjs_url_to_json(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return qjs_url_get_href(ctx, this_val);
}

/* Static canParse method */
static JSValue qjs_url_can_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "URL.canParse requires at least 1 argument");
    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_EXCEPTION;

    const char *base_str = NULL;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        base_str = JS_ToCString(ctx, argv[1]);
        if (!base_str) {
            JS_FreeCString(ctx, url_str);
            return JS_EXCEPTION;
        }
    }

    lxb_url_parser_t *parser = lxb_url_parser_create();
    if (!parser) {
        JS_FreeCString(ctx, url_str);
        if (base_str) JS_FreeCString(ctx, base_str);
        return JS_ThrowInternalError(ctx, "Failed to create parser");
    }
    lxb_url_parser_init(parser, NULL);

    lxb_url_t *base_url = NULL;
    if (base_str) {
        base_url = lxb_url_parse(parser, NULL, (const lxb_char_t *)base_str, strlen(base_str));
        lxb_url_parser_clean(parser);
    }

    lxb_url_t *url = lxb_url_parse(parser, base_url, (const lxb_char_t *)url_str, strlen(url_str));
    JSValue result = url ? JS_TRUE : JS_FALSE;

    JS_FreeCString(ctx, url_str);
    if (base_str) JS_FreeCString(ctx, base_str);
    
    lxb_url_parser_memory_destroy(parser);
    lxb_url_parser_destroy(parser, true);

    return result;
}

/* ------------------------------------------------------------------ */
/* URLSearchParams class */
/* ------------------------------------------------------------------ */

typedef struct {
    lxb_url_search_params_t *sp;
    lexbor_mraw_t *mraw;
    JSValue url_obj;   /* back-reference to parent URL (JS_NULL if standalone) */
} qjs_urlsp_obj_t;

/* Forward declaration — sync searchParams → URL query after mutations */
static void qjs_urlsp_sync_to_url(JSContext *ctx, qjs_urlsp_obj_t *s);

static void qjs_urlsp_finalizer(JSRuntime *rt, JSValue val) {
    qjs_urlsp_obj_t *s = (qjs_urlsp_obj_t *)JS_GetOpaque(val, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
    if (s) {
        if (s->sp) lxb_url_search_params_destroy(s->sp);
        if (s->mraw) lexbor_mraw_destroy(s->mraw, true);
        JS_FreeValueRT(rt, s->url_obj);
        js_free_rt(rt, s);
    }
}

/* GC mark: tell GC about back-reference to parent URL (cycle detection) */
static void qjs_urlsp_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    qjs_urlsp_obj_t *s = (qjs_urlsp_obj_t *)JS_GetOpaque(val, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
    if (s) {
        JS_MarkValue(rt, s->url_obj, mark_func);
    }
}

/* Helper: append items from an init object (Record or Sequence) to search params */
static int qjs_urlsp_init_from_object(JSContext *ctx, lxb_url_search_params_t *sp, JSValueConst obj) {
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;
    int ret = -1;

    if (JS_IsArray(ctx, obj)) {
        uint32_t arr_len = 0;
        JSValue len_val = JS_GetPropertyStr(ctx, obj, "length");
        if (JS_IsException(len_val) || JS_ToUint32(ctx, &arr_len, len_val)) {
            JS_FreeValue(ctx, len_val);
            goto done;
        }
        JS_FreeValue(ctx, len_val);

        for (uint32_t i = 0; i < arr_len; i++) {
            JSValue pair = JS_GetPropertyUint32(ctx, obj, i);
            if (JS_IsException(pair) || JS_IsNull(pair) || JS_IsUndefined(pair)) {
                JS_FreeValue(ctx, pair);
                continue;
            }
            if (JS_IsArray(ctx, pair)) {
                JSValue key_val = JS_GetPropertyUint32(ctx, pair, 0);
                JSValue val_val = JS_GetPropertyUint32(ctx, pair, 1);
                const char *key = JS_ToCString(ctx, key_val);
                const char *val = JS_IsUndefined(val_val) ? "" : JS_ToCString(ctx, val_val);
                if (key && val) {
                    lxb_url_search_params_append(sp, (const lxb_char_t *)key, strlen(key), (const lxb_char_t *)val, strlen(val));
                }
                if (key) JS_FreeCString(ctx, key);
                if (val && val[0] != '\0') JS_FreeCString(ctx, val); 
                JS_FreeValue(ctx, key_val);
                JS_FreeValue(ctx, val_val);
            }
            JS_FreeValue(ctx, pair);
        }
        ret = 0;
    } else {
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) != 0)
            goto done;

        for (uint32_t i = 0; i < prop_count; i++) {
            JSValue key_val = JS_AtomToString(ctx, props[i].atom);
            JSValue val_val = JS_GetProperty(ctx, obj, props[i].atom);
            const char *key = JS_ToCString(ctx, key_val);
            const char *val = NULL;
            if (JS_IsUndefined(val_val) || JS_IsNull(val_val)) {
                val = "";
            } else {
                val = JS_ToCString(ctx, val_val);
            }
            if (key && val) {
                lxb_url_search_params_append(sp, (const lxb_char_t *)key, strlen(key), (const lxb_char_t *)val, strlen(val));
            }
            if (key) JS_FreeCString(ctx, key);
            if (val && val[0] != '\0') JS_FreeCString(ctx, val); 
            JS_FreeValue(ctx, key_val);
            JS_FreeValue(ctx, val_val);
            JS_FreeAtom(ctx, props[i].atom);
        }
        ret = 0;
    }

done:
    if (props) {
        js_free(ctx, props);
    }
    return ret;
}

static JSValue qjs_urlsp_ctor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = (qjs_urlsp_obj_t *)js_malloc(ctx, sizeof(qjs_urlsp_obj_t));
    if (!s) return JS_EXCEPTION;
    memset(s, 0, sizeof(*s));
    s->url_obj = JS_NULL;

    s->mraw = lexbor_mraw_create();
    if (!s->mraw) {
        js_free(ctx, s);
        return JS_ThrowInternalError(ctx, "URLSearchParams: memory allocation failed");
    }
    lexbor_mraw_init(s->mraw, 1024);

    /* 用 init 创建空 sp（query=NULL, length=0 时 init 内部 parse 直接返回 OK） */
    s->sp = lxb_url_search_params_init(s->mraw, NULL, 0);
    if (!s->sp) {
        lexbor_mraw_destroy(s->mraw, true);
        js_free(ctx, s);
        return JS_ThrowInternalError(ctx, "URLSearchParams: init failed");
    }

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (JS_IsString(argv[0])) {
            const char *init_str = JS_ToCString(ctx, argv[0]);
            if (init_str) {
                if (init_str[0] == '?') init_str++;
                /* 用 init 创建新 sp 并解析 query，替换空 sp */
                lxb_url_search_params_destroy(s->sp);
                s->sp = lxb_url_search_params_init(s->mraw, (const lxb_char_t *)init_str, strlen(init_str));
                if (!s->sp) {
                    JS_FreeCString(ctx, init_str);
                    lexbor_mraw_destroy(s->mraw, true);
                    js_free(ctx, s);
                    return JS_ThrowInternalError(ctx, "URLSearchParams: parse failed");
                }
                JS_FreeCString(ctx, init_str);
            }
        } else if (JS_IsObject(argv[0])) {
            qjs_urlsp_obj_t *other = (qjs_urlsp_obj_t *)JS_GetOpaque(argv[0], QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
            if (other) {
                lxb_url_search_entry_t *entry = other->sp->first;
                while (entry) {
                    const lxb_char_t *name = (entry->name.data && entry->name.length > 0) ? entry->name.data : (const lxb_char_t *)"";
                    size_t name_len = entry->name.length;
                    const lxb_char_t *value = (entry->value.data && entry->value.length > 0) ? entry->value.data : (const lxb_char_t *)"";
                    size_t value_len = entry->value.length;
                    
                    lxb_url_search_params_append(s->sp, name, name_len, value, value_len);
                    entry = entry->next;
                }
            } else {
                if (qjs_urlsp_init_from_object(ctx, s->sp, argv[0]) != 0) {
                    lxb_url_search_params_destroy(s->sp);
                    lexbor_mraw_destroy(s->mraw, true);
                    js_free(ctx, s);
                    return JS_ThrowTypeError(ctx, "URLSearchParams: invalid object initializer");
                }
            }
        }
    }

    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
    if (JS_IsException(obj)) {
        lxb_url_search_params_destroy(s->sp);
        lexbor_mraw_destroy(s->mraw, true);
        js_free(ctx, s);
        return obj;
    }
    JS_SetOpaque(obj, s);
    return obj;
}

static qjs_urlsp_obj_t *qjs_urlsp_get_obj(JSValueConst this_val) {
    return (qjs_urlsp_obj_t *)JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
}

/* append(name, value) */
static JSValue qjs_urlsp_append(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 2) return JS_ThrowTypeError(ctx, "URLSearchParams.append requires 2 arguments");
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_ThrowTypeError(ctx, "Invalid arguments for append");
    }
    lxb_url_search_params_append(s->sp, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)value, strlen(value));
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    qjs_urlsp_sync_to_url(ctx, s);
    return JS_UNDEFINED;
}

/* delete(name, value?) */
static JSValue qjs_urlsp_delete(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "URLSearchParams.delete requires at least 1 argument");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_ThrowTypeError(ctx, "Invalid name for delete");
    
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char *value = JS_ToCString(ctx, argv[1]);
        if (value) {
            lxb_url_search_params_delete(s->sp, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)value, strlen(value));
            JS_FreeCString(ctx, value);
        }
    } else {
        lxb_url_search_params_delete(s->sp, (const lxb_char_t *)name, strlen(name), NULL, 0);
    }
    JS_FreeCString(ctx, name);
    qjs_urlsp_sync_to_url(ctx, s);
    return JS_UNDEFINED;
}

/* get(name) */
static JSValue qjs_urlsp_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "URLSearchParams.get requires 1 argument");
    
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NULL;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    lexbor_str_t *val = lxb_url_search_params_get(s->sp, (const lxb_char_t *)name, strlen(name));
    JS_FreeCString(ctx, name);
    if (!val || !val->data || val->length == 0) return JS_NULL;
    return JS_NewStringLen(ctx, (const char *)val->data, val->length);
}

/* getAll(name) */
static JSValue qjs_urlsp_get_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "URLSearchParams.getAll requires 1 argument");
    
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_NewArray(ctx);
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    size_t count = lxb_url_search_params_get_count(s->sp, (const lxb_char_t *)name, strlen(name));
    JSValue arr = JS_NewArray(ctx);
    if (count > 0) {
        lexbor_str_t **vals = (lexbor_str_t **)js_malloc(ctx, sizeof(lexbor_str_t *) * count);
        if (vals) {
            size_t got = lxb_url_search_params_get_all(s->sp, (const lxb_char_t *)name, strlen(name), vals, count);
            for (size_t i = 0; i < got; i++) {
                if (vals[i] && vals[i]->data && vals[i]->length > 0) {
                    JS_SetPropertyUint32(ctx, arr, i, JS_NewStringLen(ctx, (const char *)vals[i]->data, vals[i]->length));
                } else {
                    JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, ""));
                }
            }
            js_free(ctx, vals);
        }
    }
    JS_FreeCString(ctx, name);
    return arr;
}

/* has(name, value?) */
static JSValue qjs_urlsp_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 1) return JS_ThrowTypeError(ctx, "URLSearchParams.has requires at least 1 argument");
    
    if (JS_IsNull(argv[0]) || JS_IsUndefined(argv[0])) return JS_FALSE;
    
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    bool result;
    if (argc >= 2 && !JS_IsUndefined(argv[1])) {
        const char *value = JS_ToCString(ctx, argv[1]);
        if (value) {
            result = lxb_url_search_params_has(s->sp, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)value, strlen(value));
            JS_FreeCString(ctx, value);
        } else {
            result = false;
        }
    } else {
        result = lxb_url_search_params_has(s->sp, (const lxb_char_t *)name, strlen(name), NULL, 0);
    }
    JS_FreeCString(ctx, name);
    return result ? JS_TRUE : JS_FALSE;
}

/* set(name, value) */
static JSValue qjs_urlsp_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 2) return JS_ThrowTypeError(ctx, "URLSearchParams.set requires 2 arguments");
    const char *name = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!name || !value) {
        if (name) JS_FreeCString(ctx, name);
        if (value) JS_FreeCString(ctx, value);
        return JS_ThrowTypeError(ctx, "Invalid arguments for set");
    }
    lxb_url_search_params_set(s->sp, (const lxb_char_t *)name, strlen(name), (const lxb_char_t *)value, strlen(value));
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    qjs_urlsp_sync_to_url(ctx, s);
    return JS_UNDEFINED;
}

/* sort() */
static JSValue qjs_urlsp_sort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    lxb_url_search_params_sort(s->sp);
    qjs_urlsp_sync_to_url(ctx, s);
    return JS_UNDEFINED;
}

/* toString() */
static JSValue qjs_urlsp_to_string(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_search_params_serialize(s->sp, qjs_serialize_cb, &sb);
    return qjs_strbuf_to_js(ctx, &sb);
}

/* ------------------------------------------------------------------ */
/* 手动实现安全的 ES6 Iterator (兼容低版本 QuickJS) */
/* ------------------------------------------------------------------ */

typedef struct {
    lxb_url_search_entry_t *current;
    int mode; /* 0: entries, 1: keys, 2: values */
} qjs_urlsp_iter_state_t;


static void qjs_urlsp_iter_finalizer(JSRuntime *rt, JSValue val) {
    qjs_urlsp_iter_state_t *state = (qjs_urlsp_iter_state_t *)JS_GetOpaque(val, QJS_CORE_CLASS_ID_URL_SP_ITER);
    if (state) {
        js_free_rt(rt, state);
    }
}

static JSValue qjs_urlsp_iter_next(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_iter_state_t *state = (qjs_urlsp_iter_state_t *)JS_GetOpaque(this_val, QJS_CORE_CLASS_ID_URL_SP_ITER);
    JSValue obj = JS_NewObject(ctx);
    
    if (!state || !state->current) {
        JS_SetPropertyStr(ctx, obj, "done", JS_TRUE);
        JS_SetPropertyStr(ctx, obj, "value", JS_UNDEFINED);
    } else {
        JSValue value;
        const char *name_str = (state->current->name.data && state->current->name.length > 0) ? (const char *)state->current->name.data : "";
        size_t name_len = state->current->name.length;
        
        const char *val_str = (state->current->value.data && state->current->value.length > 0) ? (const char *)state->current->value.data : "";
        size_t val_len = state->current->value.length;

        if (state->mode == 1) { /* keys */
            value = JS_NewStringLen(ctx, name_str, name_len);
        } else if (state->mode == 2) { /* values */
            value = JS_NewStringLen(ctx, val_str, val_len);
        } else { /* entries */
            JSValue pair = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, pair, 0, JS_NewStringLen(ctx, name_str, name_len));
            JS_SetPropertyUint32(ctx, pair, 1, JS_NewStringLen(ctx, val_str, val_len));
            value = pair;
        }
        JS_SetPropertyStr(ctx, obj, "done", JS_FALSE);
        JS_SetPropertyStr(ctx, obj, "value", value);
        state->current = state->current->next;
    }
    return obj;
}

/* Symbol.iterator for iterator objects: returns this */
static JSValue qjs_urlsp_self_iterator(JSContext *ctx, JSValueConst this_val) {
    return JS_DupValue(ctx, this_val);
}

static JSValue qjs_create_iterator(JSContext *ctx, JSValueConst this_val, bool is_keys, bool is_values)
{
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");

    JSValue iterator = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_URL_SP_ITER);
    if (JS_IsException(iterator)) return iterator;

    qjs_urlsp_iter_state_t *state = (qjs_urlsp_iter_state_t *)js_malloc(ctx, sizeof(qjs_urlsp_iter_state_t));
    if (!state) {
        JS_FreeValue(ctx, iterator);
        return JS_EXCEPTION;
    }
    
    state->current = s->sp->first;
    state->mode = is_keys ? 1 : (is_values ? 2 : 0);

    JS_SetOpaque(iterator, state);
    JS_SetPropertyStr(ctx, iterator, "next", JS_NewCFunction(ctx, qjs_urlsp_iter_next, "next", 0));
    
    JS_SetPropertyStr(ctx, iterator, "return", JS_NewCFunction(ctx, (JSCFunction *)qjs_urlsp_iter_next, "return", 0));
    
    /* Add Symbol.iterator = function() { return this; } for [...spread] support */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue symbol_obj = JS_GetPropertyStr(ctx, global, "Symbol");
    JSValue iter_val = JS_GetPropertyStr(ctx, symbol_obj, "iterator");
    if (!JS_IsUndefined(iter_val)) {
        /* Create a function that returns its this (the iterator itself) */
        JSValue self_iter_fn = JS_NewCFunction(ctx, (JSCFunction *)qjs_urlsp_self_iterator, "[Symbol.iterator]", 0);
        JS_SetProperty(ctx, iterator, JS_ValueToAtom(ctx, iter_val), self_iter_fn);
    }
    JS_FreeValue(ctx, iter_val);
    JS_FreeValue(ctx, symbol_obj);
    JS_FreeValue(ctx, global);
    
    return iterator;
}

static JSValue qjs_urlsp_entries(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return qjs_create_iterator(ctx, this_val, false, false);
}

static JSValue qjs_urlsp_keys(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return qjs_create_iterator(ctx, this_val, true, false);
}

static JSValue qjs_urlsp_values(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return qjs_create_iterator(ctx, this_val, false, true);
}

/* forEach(callback) */
static JSValue qjs_urlsp_for_each(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "URLSearchParams.forEach requires a callback");
    
    JSValue callback = argv[0];
    JSValue this_arg = (argc >= 2) ? argv[1] : JS_UNDEFINED;
    lxb_url_search_entry_t *entry = s->sp->first;
    
    while (entry) {
        const char *name_str = (entry->name.data && entry->name.length > 0) ? (const char *)entry->name.data : "";
        size_t name_len = entry->name.length;
        
        const char *val_str = (entry->value.data && entry->value.length > 0) ? (const char *)entry->value.data : "";
        size_t val_len = entry->value.length;

        JSValue val = JS_NewStringLen(ctx, val_str, val_len);
        JSValue key = JS_NewStringLen(ctx, name_str, name_len);
        JSValueConst args[3] = { val, key, this_val };
        JSValue ret = JS_Call(ctx, callback, this_arg, 3, args);
        if (JS_IsException(ret)) {
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, key);
            return ret;
        }
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, key);
        entry = entry->next;
    }
    return JS_UNDEFINED;
}

/* size getter */
static JSValue qjs_urlsp_get_size(JSContext *ctx, JSValueConst this_val) {
    qjs_urlsp_obj_t *s = qjs_urlsp_get_obj(this_val);
    if (!s) return JS_ThrowTypeError(ctx, "not a URLSearchParams object");
    return JS_NewInt32(ctx, (int)s->sp->length);
}

/* ------------------------------------------------------------------ */
/* URL.searchParams getter — returns cached live-linked searchParams  */
/* ------------------------------------------------------------------ */

/* Sync searchParams → URL query (called after each mutation) */
static void qjs_urlsp_sync_to_url(JSContext *ctx, qjs_urlsp_obj_t *s) {
    if (!s || JS_IsNull(s->url_obj)) return;
    qjs_url_obj_t *u = (qjs_url_obj_t *)JS_GetOpaque(s->url_obj, QJS_CORE_CLASS_ID_URL);
    if (!u || !u->url) return;

    /* Serialize search params via callback */
    qjs_strbuf_t sb;
    qjs_strbuf_init(&sb);
    lxb_url_search_params_serialize(s->sp, (lexbor_callback_f)qjs_serialize_cb, &sb);

    if (sb.buf && sb.len > 0) {
        /* Re-parse URL with new query */
        char *href = qjs_url_href_str(u);
        if (href) {
            char *q = strchr(href, '?');
            char *h = strchr(href, '#');
            char *end = q ? q : (h ? h : href + strlen(href));
            size_t prefix = end - href;
            size_t qslen = sb.len;
            size_t suffix = h ? strlen(h) : 0;

            char *new_href = (char *)malloc(prefix + 1 + qslen + suffix + 1);
            memcpy(new_href, href, prefix);
            char *p = new_href + prefix;
            *p++ = '?';
            memcpy(p, sb.buf, qslen); p += qslen;
            if (h) memcpy(p, h, suffix + 1);
            else *p = '\0';

            lxb_url_parser_clean(u->parser);
            lxb_url_t *new_url = lxb_url_parse(u->parser, NULL,
                (const lxb_char_t *)new_href, strlen(new_href));
            if (new_url) {
                lxb_url_destroy(u->url);
                u->url = new_url;
            }
            free(new_href);
            free(href);
        }
    } else {
        /* Empty query — remove ? from URL */
        char *href = qjs_url_href_str(u);
        if (href) {
            char *q = strchr(href, '?');
            char *h = strchr(href, '#');
            if (q) {
                size_t prefix = q - href;
                if (h) {
                    /* Move hash to right after prefix (before ?) */
                    memmove(href + prefix, h, strlen(h) + 1);
                } else {
                    /* No hash — truncate at ? */
                    href[prefix] = '\0';
                }
                lxb_url_parser_clean(u->parser);
                lxb_url_t *new_url = lxb_url_parse(u->parser, NULL,
                    (const lxb_char_t *)href, strlen(href));
                if (new_url) {
                    lxb_url_destroy(u->url);
                    u->url = new_url;
                }
            }
            free(href);
        }
    }
    free(sb.buf);
}

static JSValue qjs_url_get_searchParams(JSContext *ctx, JSValueConst this_val) {
    qjs_url_obj_t *u = qjs_url_get(this_val);
    if (!u || !u->url) return JS_ThrowTypeError(ctx, "not a URL object");

    /* Return cached searchParams if it exists (live link) */
    if (!JS_IsNull(u->sp_obj) && !JS_IsUndefined(u->sp_obj)) {
        return JS_DupValue(ctx, u->sp_obj);
    }

    /* Create new searchParams linked to this URL */
    char query_buf[4096];
    const char *query = "";
    size_t query_len = 0;

    if (u->url->query.data && u->url->query.length > 0) {
        query_len = u->url->query.length;
        if (query_len < sizeof(query_buf)) {
            memcpy(query_buf, u->url->query.data, query_len);
            query_buf[query_len] = '\0';
            query = query_buf;
        } else {
            query = (const char *)u->url->query.data;
        }
    }

    qjs_urlsp_obj_t *s = (qjs_urlsp_obj_t *)js_malloc(ctx, sizeof(qjs_urlsp_obj_t));
    if (!s) return JS_EXCEPTION;
    memset(s, 0, sizeof(*s));
    s->url_obj = JS_NULL;

    s->mraw = lexbor_mraw_create();
    if (!s->mraw) {
        js_free(ctx, s);
        return JS_ThrowInternalError(ctx, "Failed to allocate memory for URLSearchParams");
    }
    lexbor_mraw_init(s->mraw, 1024);

    if (query_len > 0) {
        s->sp = lxb_url_search_params_init(s->mraw, (const lxb_char_t *)query, query_len);
    } else {
        s->sp = lxb_url_search_params_init(s->mraw, NULL, 0);
    }
    if (!s->sp) {
        lexbor_mraw_destroy(s->mraw, true);
        js_free(ctx, s);
        return JS_ThrowInternalError(ctx, "Failed to init URLSearchParams");
    }

    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS);
    if (JS_IsException(obj)) {
        lxb_url_search_params_destroy(s->sp);
        lexbor_mraw_destroy(s->mraw, true);
        js_free(ctx, s);
        return obj;
    }
    JS_SetOpaque(obj, s);

    /* Set back-reference to parent URL */
    s->url_obj = JS_DupValue(ctx, this_val);

    /* Cache on URL object */
    u->sp_obj = JS_DupValue(ctx, obj);

    return obj;
}

/* ------------------------------------------------------------------ */
/* Property lists */
/* ------------------------------------------------------------------ */
static const JSCFunctionListEntry qjs_url_proto[] = {
    JS_CFUNC_DEF("toString", 0, qjs_url_to_string ),
    JS_CFUNC_DEF("toJSON", 0, qjs_url_to_json ),
    JS_CGETSET_DEF("href", qjs_url_get_href, qjs_url_set_href ),
    JS_CGETSET_DEF("origin", qjs_url_get_origin, NULL ),
    JS_CGETSET_DEF("protocol", qjs_url_get_protocol, qjs_url_set_protocol ),
    JS_CGETSET_DEF("username", qjs_url_get_username, qjs_url_set_username ),
    JS_CGETSET_DEF("password", qjs_url_get_password, qjs_url_set_password ),
    JS_CGETSET_DEF("host", qjs_url_get_host, NULL ),
    JS_CGETSET_DEF("hostname", qjs_url_get_hostname, qjs_url_set_hostname ),
    JS_CGETSET_DEF("port", qjs_url_get_port, qjs_url_set_port ),
    JS_CGETSET_DEF("pathname", qjs_url_get_pathname, qjs_url_set_pathname ),
    JS_CGETSET_DEF("search", qjs_url_get_search, qjs_url_set_search ),
    JS_CGETSET_DEF("hash", qjs_url_get_hash, qjs_url_set_hash ),
    JS_CGETSET_DEF("searchParams", qjs_url_get_searchParams, NULL ),
};

static const JSCFunctionListEntry qjs_urlsp_proto[] = {
    JS_CFUNC_DEF("append", 2, qjs_urlsp_append ),
    JS_CFUNC_DEF("delete", 1, qjs_urlsp_delete ),
    JS_CFUNC_DEF("get", 1, qjs_urlsp_get ),
    JS_CFUNC_DEF("getAll", 1, qjs_urlsp_get_all ),
    JS_CFUNC_DEF("has", 1, qjs_urlsp_has ),
    JS_CFUNC_DEF("set", 2, qjs_urlsp_set ),
    JS_CFUNC_DEF("sort", 0, qjs_urlsp_sort ),
    JS_CFUNC_DEF("toString", 0, qjs_urlsp_to_string ),
    JS_CFUNC_DEF("entries", 0, qjs_urlsp_entries ),
    JS_CFUNC_DEF("keys", 0, qjs_urlsp_keys ),
    JS_CFUNC_DEF("values", 0, qjs_urlsp_values ),
    JS_CFUNC_DEF("forEach", 1, qjs_urlsp_for_each ),
    JS_CGETSET_DEF("size", qjs_urlsp_get_size, NULL ),
};

/* ------------------------------------------------------------------ */
/* Init */
/* ------------------------------------------------------------------ */
int qjs_url_init(JSContext *ctx) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue global = JS_GetGlobalObject(ctx);

    /* URL class */
    JSClassDef url_def = {
        .class_name = "URL",
        .finalizer = qjs_url_finalizer,
        .gc_mark = qjs_url_gc_mark,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_URL, &url_def);
    JSValue url_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, url_proto, qjs_url_proto, sizeof(qjs_url_proto) / sizeof(JSCFunctionListEntry));
    JS_SetClassProto(ctx, QJS_CORE_CLASS_ID_URL, url_proto);
    JSValue url_ctor = JS_NewCFunction2(ctx, qjs_url_ctor, "URL", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, url_ctor, url_proto);
    JS_SetPropertyStr(ctx, url_ctor, "canParse", JS_NewCFunction(ctx, qjs_url_can_parse, "canParse", 2));
    JS_SetPropertyStr(ctx, global, "URL", url_ctor);

    /* URLSearchParams class */
    JSClassDef sp_def = {
        .class_name = "URLSearchParams",
        .finalizer = qjs_urlsp_finalizer,
        .gc_mark = qjs_urlsp_gc_mark,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS, &sp_def);
    JSValue sp_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, sp_proto, qjs_urlsp_proto, sizeof(qjs_urlsp_proto) / sizeof(JSCFunctionListEntry));
    JS_SetClassProto(ctx, QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS, sp_proto);
    JSValue sp_ctor = JS_NewCFunction2(ctx, qjs_urlsp_ctor, "URLSearchParams", 1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, sp_ctor, sp_proto);

    /* Iterator class for URLSearchParams (手动管理) */
    JSClassDef iter_def = {
        .class_name = "URLSearchParams Iterator",
        .finalizer = qjs_urlsp_iter_finalizer,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_URL_SP_ITER, &iter_def);

    /* Add Symbol.iterator to URLSearchParams prototype */
    JSValue symbol = JS_GetGlobalObject(ctx);
    JSValue symbol_iterator = JS_GetPropertyStr(ctx, symbol, "Symbol");
    JSValue iterator_val = JS_GetPropertyStr(ctx, symbol_iterator, "iterator");
    
    JSAtom iterator_atom = JS_ValueToAtom(ctx, iterator_val);
    JSValue entries_fn = JS_NewCFunction(ctx, qjs_urlsp_entries, "[Symbol.iterator]", 0);
    JS_DefinePropertyValue(ctx, sp_proto, iterator_atom, entries_fn, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    
    JS_FreeAtom(ctx, iterator_atom);
    JS_FreeValue(ctx, iterator_val);
    JS_FreeValue(ctx, symbol_iterator);
    JS_FreeValue(ctx, symbol);

    JS_SetPropertyStr(ctx, global, "URLSearchParams", sp_ctor);

    JS_FreeValue(ctx, global);
    return 0;
}
