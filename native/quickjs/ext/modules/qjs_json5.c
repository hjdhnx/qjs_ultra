/*
 * qjs_json5.c — 可选扩展模块：全局 JSON5 对象（实验性示例）
 *
 * 注册（仅在 QJS_ENABLE_JSON5 编译开关打开时）：
 *   globalThis.JSON5 = { parse(text), stringify(value[, replacer[, space]]) }
 *
 * JSON5 语法（https://json5.org/）相对 JSON 的扩展：
 *   - 注释：行注释与块注释
 *   - 对象键：可省略引号（IdentifierName），允许尾逗号
 *   - 字符串：单/双引号、\x / \u 转义、续行、跨行字符串
 *   - 数字：+/- 前缀、十六进制 0x、前导/尾随小数点、Infinity、NaN
 *   - 空白：NBSP / BOM / U+2028 / U+2029 等
 *
 * stringify 直接复用 JSON.stringify（JSON 输出是合法 JSON5），因此
 * 不额外维护一套序列化逻辑；需要 JSON5 风格输出时可在后续迭代补充。
 *
 * 编译开关：cmake -DQJS_ENABLE_JSON5=ON（默认 OFF，见 ext/CMakeLists.txt）。
 */
#include "qjs_native.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define J5_MAX_DEPTH 512

typedef struct {
    JSContext  *ctx;
    const char *start;
    const char *p;
    const char *end;
} J5Parser;

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} J5Buf;

/* ------------------------------------------------------------------ */
/* 基础工具                                                            */
/* ------------------------------------------------------------------ */

static void j5_syntax(J5Parser *s, const char *msg)
{
    JS_ThrowSyntaxError(s->ctx, "JSON5: %s (offset %d)", msg,
                        (int)(s->p - s->start));
}

static int j5_is_ascii_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static int j5_is_line_term(const J5Parser *s)
{
    unsigned char c;
    if (s->p >= s->end)
        return 0;
    c = (unsigned char)*s->p;
    if (c == '\n' || c == '\r')
        return 1;
    /* U+2028 / U+2029 */
    return s->end - s->p >= 3 && c == 0xE2 && s->p[1] == (char)0x80 &&
           ((unsigned char)s->p[2] == 0xA8 || (unsigned char)s->p[2] == 0xA9);
}

/* 跳过 JSON5 空白：ASCII ws + NBSP + BOM + U+2028/U+2029。返回 1 表示消费了空白。 */
static int j5_skip_one_ws(J5Parser *s)
{
    unsigned char c = (unsigned char)*s->p;
    if (j5_is_ascii_ws(c)) {
        s->p++;
        return 1;
    }
    if (s->end - s->p >= 2 && c == 0xC2 && (unsigned char)s->p[1] == 0xA0) {
        s->p += 2; /* U+00A0 NBSP */
        return 1;
    }
    if (s->end - s->p >= 3 && c == 0xEF &&
        (unsigned char)s->p[1] == 0xBB && (unsigned char)s->p[2] == 0xBF) {
        s->p += 3; /* U+FEFF BOM */
        return 1;
    }
    if (s->end - s->p >= 3 && c == 0xE2 &&
        (unsigned char)s->p[1] == 0x80 &&
        ((unsigned char)s->p[2] == 0xA8 || (unsigned char)s->p[2] == 0xA9)) {
        s->p += 3; /* U+2028 / U+2029 */
        return 1;
    }
    return 0;
}

/* 跳过空白与注释。返回 0 成功，-1 失败（异常已挂到 ctx）。 */
static int j5_skip_ws_comments(J5Parser *s)
{
    for (;;) {
        if (s->p < s->end && j5_skip_one_ws(s))
            continue;
        if (s->end - s->p >= 2 && s->p[0] == '/' && s->p[1] == '/') {
            s->p += 2;
            while (s->p < s->end && !j5_is_line_term(s))
                s->p++;
            continue;
        }
        if (s->end - s->p >= 2 && s->p[0] == '/' && s->p[1] == '*') {
            s->p += 2;
            while (s->end - s->p >= 2 &&
                   !(s->p[0] == '*' && s->p[1] == '/'))
                s->p++;
            if (s->end - s->p < 2) {
                j5_syntax(s, "unterminated block comment");
                return -1;
            }
            s->p += 2;
            continue;
        }
        return 0;
    }
}

static int j5_buf_reserve(J5Parser *s, J5Buf *b, size_t extra)
{
    size_t need, cap;
    char *nb;
    if (extra > (size_t)-1 - b->len)
        return -1;
    need = b->len + extra;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 32;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2)
            return -1;
        cap *= 2;
    }
    nb = (char *)js_realloc(s->ctx, b->buf, cap);
    if (!nb)
        return -1;
    b->buf = nb;
    b->cap = cap;
    return 0;
}

static int j5_buf_putc(J5Parser *s, J5Buf *b, unsigned char c)
{
    if (j5_buf_reserve(s, b, 1))
        return -1;
    b->buf[b->len++] = (char)c;
    return 0;
}

static int j5_buf_put_utf8(J5Parser *s, J5Buf *b, uint32_t cp)
{
    char tmp[4];
    int n;
    if (cp < 0x80) {
        tmp[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (j5_buf_reserve(s, b, (size_t)n))
        return -1;
    memcpy(b->buf + b->len, tmp, (size_t)n);
    b->len += (size_t)n;
    return 0;
}

static int j5_hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int j5_is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '$' || c >= 0x80;
}

static int j5_is_ident_char(unsigned char c)
{
    return j5_is_ident_start(c) || (c >= '0' && c <= '9');
}

/* ------------------------------------------------------------------ */
/* 字符串                                                              */
/* ------------------------------------------------------------------ */

static JSValue j5_parse_string(J5Parser *s)
{
    J5Buf b = { NULL, 0, 0 };
    unsigned char quote = (unsigned char)*s->p++;
    JSValue ret = JS_EXCEPTION;

    for (;;) {
        unsigned char c;
        if (s->p >= s->end) {
            j5_syntax(s, "unterminated string");
            goto fail;
        }
        c = (unsigned char)*s->p++;
        if (c == quote)
            break;

        if (c != '\\') {
            if (j5_buf_putc(s, &b, c))
                goto oom;
            continue;
        }

        if (s->p >= s->end) {
            j5_syntax(s, "unterminated string escape");
            goto fail;
        }
        c = (unsigned char)*s->p++;
        switch (c) {
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'v': c = '\v'; break;
        case '0': c = '\0'; break; /* 宽松：不检查后随十进制数字 */
        case 'x': {
            int h1, h2;
            if (s->end - s->p < 2) {
                j5_syntax(s, "invalid \\x escape");
                goto fail;
            }
            h1 = j5_hexval((unsigned char)s->p[0]);
            h2 = j5_hexval((unsigned char)s->p[1]);
            if (h1 < 0 || h2 < 0) {
                j5_syntax(s, "invalid \\x escape");
                goto fail;
            }
            c = (unsigned char)((h1 << 4) | h2);
            s->p += 2;
            break;
        }
        case 'u': {
            uint32_t cp = 0;
            int i;
            if (s->end - s->p < 4) {
                j5_syntax(s, "invalid \\u escape");
                goto fail;
            }
            for (i = 0; i < 4; i++) {
                int h = j5_hexval((unsigned char)s->p[i]);
                if (h < 0) {
                    j5_syntax(s, "invalid \\u escape");
                    goto fail;
                }
                cp = (cp << 4) | (uint32_t)h;
            }
            s->p += 4;
            /* 代理对合并；孤立代理替换为 U+FFFD（避免产出非法 UTF-8） */
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                s->end - s->p >= 6 && s->p[0] == '\\' && s->p[1] == 'u') {
                uint32_t lo = 0;
                int ok = 1;
                for (i = 0; i < 4; i++) {
                    int h = j5_hexval((unsigned char)s->p[2 + i]);
                    if (h < 0) { ok = 0; break; }
                    lo = (lo << 4) | (uint32_t)h;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s->p += 6;
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF)
                cp = 0xFFFD;
            if (j5_buf_put_utf8(s, &b, cp))
                goto oom;
            continue;
        }
        case '\r':
            if (s->p < s->end && *s->p == '\n')
                s->p++;
            continue; /* 续行：不产出字符 */
        case '\n':
            continue; /* 续行 */
        case 0xE2:
            /* \ + U+2028 / U+2029 续行 */
            if (s->end - s->p >= 2 && (unsigned char)s->p[0] == 0x80 &&
                ((unsigned char)s->p[1] == 0xA8 ||
                 (unsigned char)s->p[1] == 0xA9)) {
                s->p += 2;
                continue;
            }
            break;
        default:
            break; /* 其他转义：字符本身（如 \" \' \\ \/） */
        }
        if (j5_buf_putc(s, &b, c))
            goto oom;
    }

    ret = JS_NewStringLen(s->ctx, b.buf ? b.buf : "", b.len);
    js_free(s->ctx, b.buf);
    return ret;

oom:
    JS_ThrowOutOfMemory(s->ctx);
fail:
    js_free(s->ctx, b.buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 数字                                                                */
/* ------------------------------------------------------------------ */

static int j5_is_number_delim(const J5Parser *s)
{
    unsigned char c;
    if (s->p >= s->end)
        return 1;
    c = (unsigned char)*s->p;
    if (j5_is_ascii_ws(c) || c == ',' || c == ']' || c == '}' || c == '/')
        return 1;
    /* 多字节空白/注释起始（保守放行，交给上层统一报错或继续解析） */
    if (c == 0xC2 || c == 0xE2 || c == 0xEF)
        return 1;
    return 0;
}

static JSValue j5_parse_number(J5Parser *s)
{
    const char *num_start = s->p;
    int neg = 0;
    double v;

    if (s->p < s->end && (*s->p == '+' || *s->p == '-')) {
        neg = (*s->p == '-');
        s->p++;
    }

    if (s->end - s->p >= 8 && memcmp(s->p, "Infinity", 8) == 0) {
        s->p += 8;
        if (!j5_is_number_delim(s)) {
            j5_syntax(s, "invalid number");
            return JS_EXCEPTION;
        }
        return JS_NewFloat64(s->ctx, neg ? -INFINITY : INFINITY);
    }
    if (s->end - s->p >= 3 && memcmp(s->p, "NaN", 3) == 0) {
        s->p += 3;
        if (!j5_is_number_delim(s)) {
            j5_syntax(s, "invalid number");
            return JS_EXCEPTION;
        }
        return JS_NewFloat64(s->ctx, NAN);
    }

    /* 十六进制整数（JSON5 扩展） */
    if (s->end - s->p >= 2 && s->p[0] == '0' &&
        (s->p[1] == 'x' || s->p[1] == 'X')) {
        s->p += 2;
        if (s->p >= s->end || j5_hexval((unsigned char)*s->p) < 0) {
            j5_syntax(s, "invalid hexadecimal number");
            return JS_EXCEPTION;
        }
        v = 0;
        while (s->p < s->end) {
            int h = j5_hexval((unsigned char)*s->p);
            if (h < 0)
                break;
            v = v * 16 + h;
            s->p++;
        }
        if (!j5_is_number_delim(s)) {
            j5_syntax(s, "invalid number");
            return JS_EXCEPTION;
        }
        return JS_NewFloat64(s->ctx, neg ? -v : v);
    }

    /* 十进制：digits [. digits] | . digits，可带指数 */
    {
        int digits = 0;
        while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
            s->p++;
            digits = 1;
        }
        if (s->p < s->end && *s->p == '.') {
            s->p++;
            while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
                s->p++;
                digits = 1;
            }
        }
        if (!digits) {
            j5_syntax(s, "invalid number");
            return JS_EXCEPTION;
        }
        if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
            const char *save = s->p;
            int exp_digits = 0;
            s->p++;
            if (s->p < s->end && (*s->p == '+' || *s->p == '-'))
                s->p++;
            while (s->p < s->end && *s->p >= '0' && *s->p <= '9') {
                s->p++;
                exp_digits = 1;
            }
            if (!exp_digits)
                s->p = save; /* 回退：把 e 当作非法分隔，下面统一报错 */
        }
    }

    if (!j5_is_number_delim(s)) {
        j5_syntax(s, "invalid number");
        return JS_EXCEPTION;
    }

    {
        size_t n = (size_t)(s->p - num_start);
        char *tmp = (char *)js_malloc(s->ctx, n + 1);
        if (!tmp) {
            JS_ThrowOutOfMemory(s->ctx);
            return JS_EXCEPTION;
        }
        memcpy(tmp, num_start, n);
        tmp[n] = '\0';
        v = strtod(tmp, NULL);
        js_free(s->ctx, tmp);
    }
    return JS_NewFloat64(s->ctx, v);
}

/* ------------------------------------------------------------------ */
/* 值 / 对象 / 数组                                                    */
/* ------------------------------------------------------------------ */

static JSValue j5_parse_value(J5Parser *s, int depth);

static JSValue j5_parse_object(J5Parser *s, int depth)
{
    JSValue obj;

    if (depth >= J5_MAX_DEPTH) {
        j5_syntax(s, "nesting too deep");
        return JS_EXCEPTION;
    }
    s->p++; /* '{' */
    obj = JS_NewObject(s->ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    for (;;) {
        JSValue key = JS_UNDEFINED, val;
        JSAtom atom;

        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return obj;
        }

        /* 键：字符串或 IdentifierName */
        if (s->p < s->end && (*s->p == '"' || *s->p == '\'')) {
            key = j5_parse_string(s);
            if (JS_IsException(key)) {
                JS_FreeValue(s->ctx, obj);
                return JS_EXCEPTION;
            }
        } else if (s->p < s->end &&
                   j5_is_ident_start((unsigned char)*s->p)) {
            const char *kstart = s->p;
            while (s->p < s->end && j5_is_ident_char((unsigned char)*s->p))
                s->p++;
            key = JS_NewStringLen(s->ctx, kstart, (size_t)(s->p - kstart));
            if (JS_IsException(key)) {
                JS_FreeValue(s->ctx, obj);
                return JS_EXCEPTION;
            }
        } else {
            j5_syntax(s, "object key expected");
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }

        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, key);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        if (s->p >= s->end || *s->p != ':') {
            j5_syntax(s, "':' expected after object key");
            JS_FreeValue(s->ctx, key);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        s->p++;
        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, key);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }

        val = j5_parse_value(s, depth + 1);
        if (JS_IsException(val)) {
            JS_FreeValue(s->ctx, key);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }

        atom = JS_ValueToAtom(s->ctx, key);
        JS_FreeValue(s->ctx, key);
        if (atom == JS_ATOM_NULL) {
            JS_FreeValue(s->ctx, val);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        /* 用 Define 而非 Set：__proto__ 等键也作为普通自有属性 */
        if (JS_DefinePropertyValue(s->ctx, obj, atom, val, JS_PROP_C_W_E) < 0) {
            JS_FreeAtom(s->ctx, atom);
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        JS_FreeAtom(s->ctx, atom);

        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, obj);
            return JS_EXCEPTION;
        }
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue; /* 允许尾逗号：下一轮先看 '}' */
        }
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return obj;
        }
        j5_syntax(s, "',' or '}' expected");
        JS_FreeValue(s->ctx, obj);
        return JS_EXCEPTION;
    }
}

static JSValue j5_parse_array(J5Parser *s, int depth)
{
    JSValue arr;
    uint32_t idx = 0;

    if (depth >= J5_MAX_DEPTH) {
        j5_syntax(s, "nesting too deep");
        return JS_EXCEPTION;
    }
    s->p++; /* '[' */
    arr = JS_NewArray(s->ctx);
    if (JS_IsException(arr))
        return JS_EXCEPTION;

    for (;;) {
        JSValue val;
        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, arr);
            return JS_EXCEPTION;
        }
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return arr;
        }
        if (idx == UINT32_MAX) {
            j5_syntax(s, "array too long");
            JS_FreeValue(s->ctx, arr);
            return JS_EXCEPTION;
        }
        val = j5_parse_value(s, depth + 1);
        if (JS_IsException(val)) {
            JS_FreeValue(s->ctx, arr);
            return JS_EXCEPTION;
        }
        if (JS_SetPropertyUint32(s->ctx, arr, idx++, val) < 0) {
            JS_FreeValue(s->ctx, arr);
            return JS_EXCEPTION;
        }
        if (j5_skip_ws_comments(s)) {
            JS_FreeValue(s->ctx, arr);
            return JS_EXCEPTION;
        }
        if (s->p < s->end && *s->p == ',') {
            s->p++;
            continue; /* 允许尾逗号 */
        }
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return arr;
        }
        j5_syntax(s, "',' or ']' expected");
        JS_FreeValue(s->ctx, arr);
        return JS_EXCEPTION;
    }
}

static JSValue j5_parse_value(J5Parser *s, int depth)
{
    unsigned char c;
    if (s->p >= s->end) {
        j5_syntax(s, "unexpected end of input");
        return JS_EXCEPTION;
    }
    c = (unsigned char)*s->p;
    if (c == '{')
        return j5_parse_object(s, depth);
    if (c == '[')
        return j5_parse_array(s, depth);
    if (c == '"' || c == '\'')
        return j5_parse_string(s);
    if (c == '+' || c == '-' || c == '.' ||
        (c >= '0' && c <= '9') || c == 'I' || c == 'N')
        return j5_parse_number(s);
    if (s->end - s->p >= 4 && memcmp(s->p, "null", 4) == 0) {
        s->p += 4;
        return JS_NULL;
    }
    if (s->end - s->p >= 4 && memcmp(s->p, "true", 4) == 0) {
        s->p += 4;
        return JS_TRUE;
    }
    if (s->end - s->p >= 5 && memcmp(s->p, "false", 5) == 0) {
        s->p += 5;
        return JS_FALSE;
    }
    j5_syntax(s, "unexpected token");
    return JS_EXCEPTION;
}

/* ------------------------------------------------------------------ */
/* 全局函数                                                            */
/* ------------------------------------------------------------------ */

static JSValue js_json5_parse(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *text;
    size_t len;
    J5Parser s;
    JSValue val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "JSON5.parse requires a string");

    text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text)
        return JS_EXCEPTION;

    s.ctx = ctx;
    s.start = text;
    s.p = text;
    s.end = text + len;

    if (j5_skip_ws_comments(&s)) {
        JS_FreeCString(ctx, text);
        return JS_EXCEPTION;
    }
    val = j5_parse_value(&s, 0);
    if (!JS_IsException(val)) {
        if (j5_skip_ws_comments(&s)) {
            JS_FreeValue(ctx, val);
            val = JS_EXCEPTION;
        } else if (s.p != s.end) {
            j5_syntax(&s, "unexpected trailing characters");
            JS_FreeValue(ctx, val);
            val = JS_EXCEPTION;
        }
    }
    JS_FreeCString(ctx, text);
    return val;
}

/* stringify 复用 JSON.stringify：JSON 输出本身就是合法 JSON5。 */
static JSValue js_json5_stringify(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    JSValue global, json, fn, ret;

    global = JS_GetGlobalObject(ctx);
    if (JS_IsException(global))
        return JS_EXCEPTION;
    json = JS_GetPropertyStr(ctx, global, "JSON");
    JS_FreeValue(ctx, global);
    if (JS_IsException(json))
        return JS_EXCEPTION;
    fn = JS_GetPropertyStr(ctx, json, "stringify");
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, json);
        return JS_EXCEPTION;
    }
    if (JS_IsFunction(ctx, fn)) {
        ret = JS_Call(ctx, fn, json, argc, argv);
    } else {
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, json);
        return JS_ThrowTypeError(ctx, "JSON.stringify is not available");
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, json);
    return ret;
}

static const JSCFunctionListEntry js_json5_funcs[] = {
    JS_CFUNC_DEF("parse", 1, js_json5_parse),
    JS_CFUNC_DEF("stringify", 3, js_json5_stringify),
};

/*
 * 注册全局 JSON5 对象。
 * 由 quickjs_ext.cpp 在 QJS_ENABLE_JSON5 打开时调用。
 * 返回 0 成功、-1 失败（异常已挂到 ctx）。
 */
int qjs_json5_init(JSContext *ctx, JSValueConst global_obj)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return -1;
    JS_SetPropertyFunctionList(ctx, obj, js_json5_funcs,
                               countof(js_json5_funcs));
    if (JS_SetPropertyStr(ctx, global_obj, "JSON5", obj) < 0)
        return -1;
    return 0;
}
