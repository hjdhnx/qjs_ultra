/*
 * qjs_json5.c — 可选扩展模块：全局 JSON5 对象
 *
 * 注册（仅在 QJS_ENABLE_JSON5 编译开关打开时）：
 *   globalThis.JSON5 = { parse(text[, reviver]), stringify(value[, replacer[, space]]) }
 *
 * 实现分工：
 *   - parse：vendored libpdjson5（Unlicense，完整 JSON5 语法、流式事件 API），
 *     本文件把事件流组装成 JSValue，并实现官方语义的 reviver；
 *   - stringify：注入官方 json5 的 stringify 算法（MIT，见
 *     qjs_json5_stringify.js / qjs_json5_stringify_js.h），保证引号选择、
 *     toJSON5/toJSON、replacer、space、尾逗号、循环检测、BigInt 抛错等
 *     行为与 json5 包一致。
 *
 * 编译开关：cmake -DQJS_ENABLE_JSON5=ON（默认 OFF，见 ext/CMakeLists.txt）。
 */
#include "qjs_native.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/libpdjson5/pdjson5.h"
#include "qjs_json5_stringify_js.h"

#define J5_MAX_DEPTH 512

/* ------------------------------------------------------------------ */
/* libpdjson5 分配器（复用 QuickJS ctx 堆）                            */
/* ------------------------------------------------------------------ */

static void *j5_alloc(size_t size, void *ud)
{
    return js_malloc((JSContext *)ud, size);
}

static void *j5_realloc(void *ptr, size_t size, void *ud)
{
    return js_realloc((JSContext *)ud, ptr, size);
}

static void j5_dealloc(void *ptr, size_t size, void *ud)
{
    (void)size;
    js_free((JSContext *)ud, ptr);
}

static const pdjson_allocator j5_allocator = {
    j5_alloc, j5_realloc, j5_dealloc
};

/* ------------------------------------------------------------------ */
/* 解析                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    JSContext    *ctx;
    pdjson_stream stream;
} J5Parser;

static JSValue j5_parse_value(J5Parser *p, int depth);

static JSValue j5_throw_parse_error(J5Parser *p)
{
    const char *msg = pdjson_get_error(&p->stream);
    JS_ThrowSyntaxError(p->ctx, "JSON5: %s (line %llu, column %llu)",
                        msg ? msg : "parse error",
                        (unsigned long long)pdjson_get_line(&p->stream),
                        (unsigned long long)pdjson_get_column(&p->stream));
    return JS_EXCEPTION;
}

static int j5_hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* JSON5 number token -> double。libpdjson5 已校验语法；这里只做转换。 */
static JSValue j5_make_number(J5Parser *p, const char *s, size_t len)
{
    JSContext *ctx = p->ctx;
    char stackbuf[128];
    char *buf = stackbuf;
    const char *q;
    double d;
    int neg = 0;

    if (len + 1 > sizeof(stackbuf)) {
        buf = (char *)js_malloc(ctx, len + 1);
        if (!buf)
            return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    q = buf;
    if (*q == '+' || *q == '-') {
        neg = (*q == '-');
        q++;
    }

    if (!strcmp(q, "Infinity")) {
        d = neg ? -INFINITY : INFINITY;
    } else if (!strcmp(q, "NaN")) {
        d = NAN;
    } else if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) {
        const char *h = q + 2;
        double v = 0;
        while (*h) {
            int hv = j5_hexval((unsigned char)*h);
            if (hv < 0) {
                if (buf != stackbuf) js_free(ctx, buf);
                return JS_ThrowSyntaxError(ctx, "JSON5: invalid hexadecimal number");
            }
            v = v * 16 + hv;
            h++;
        }
        d = neg ? -v : v;
    } else {
        char *end = NULL;
        d = strtod(buf, &end);
        if (!end || end != buf + len) {
            if (buf != stackbuf) js_free(ctx, buf);
            return JS_ThrowSyntaxError(ctx, "JSON5: invalid number");
        }
    }

    if (buf != stackbuf) js_free(ctx, buf);
    return JS_NewFloat64(ctx, d);
}

static JSValue j5_parse_object(J5Parser *p, int depth)
{
    JSContext *ctx = p->ctx;
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return JS_EXCEPTION;

    for (;;) {
        enum pdjson_type t = pdjson_next(&p->stream);
        if (t == PDJSON_OBJECT_END) {
            return obj;
        }
        if (t == PDJSON_ERROR) {
            JS_FreeValue(ctx, obj);
            return j5_throw_parse_error(p);
        }
        if (t != PDJSON_NAME) {
            JS_FreeValue(ctx, obj);
            return JS_ThrowSyntaxError(ctx, "JSON5: object key expected");
        }

        {
            size_t name_len = 0;
            const char *name = pdjson_get_name(&p->stream, &name_len);
            JSValue key, val;
            JSAtom atom;

            if (!name) {
                JS_FreeValue(ctx, obj);
                return JS_ThrowOutOfMemory(ctx);
            }
            key = JS_NewStringLen(ctx, name, name_len > 0 ? name_len - 1 : 0);
            if (JS_IsException(key)) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            val = j5_parse_value(p, depth + 1);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, key);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            atom = JS_ValueToAtom(ctx, key);
            JS_FreeValue(ctx, key);
            if (atom == JS_ATOM_NULL) {
                JS_FreeValue(ctx, val);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            /* Define 而非 Set：__proto__ 也作为普通自有属性 */
            if (JS_DefinePropertyValue(ctx, obj, atom, val, JS_PROP_C_W_E) < 0) {
                JS_FreeAtom(ctx, atom);
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
            JS_FreeAtom(ctx, atom);
        }
    }
}

static JSValue j5_parse_array(J5Parser *p, int depth)
{
    JSContext *ctx = p->ctx;
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;

    if (JS_IsException(arr))
        return JS_EXCEPTION;

    for (;;) {
        enum pdjson_type t = pdjson_next(&p->stream);
        if (t == PDJSON_ARRAY_END)
            return arr;
        if (t == PDJSON_ERROR) {
            JS_FreeValue(ctx, arr);
            return j5_throw_parse_error(p);
        }
        if (idx == UINT32_MAX) {
            JS_FreeValue(ctx, arr);
            return JS_ThrowSyntaxError(ctx, "JSON5: array too long");
        }
        /* 把刚消费的事件交给值解析（用 peek/reset 不可行，改为按类型分发） */
        {
            JSValue val;
            switch (t) {
            case PDJSON_OBJECT:
                val = j5_parse_object(p, depth + 1);
                break;
            case PDJSON_ARRAY:
                val = j5_parse_array(p, depth + 1);
                break;
            case PDJSON_STRING: {
                size_t n = 0;
                const char *s = pdjson_get_value(&p->stream, &n);
                val = s ? JS_NewStringLen(ctx, s, n > 0 ? n - 1 : 0)
                        : JS_ThrowOutOfMemory(ctx);
                break;
            }
            case PDJSON_NUMBER: {
                size_t n = 0;
                const char *s = pdjson_get_value(&p->stream, &n);
                val = s ? j5_make_number(p, s, n > 0 ? n - 1 : 0)
                        : JS_ThrowOutOfMemory(ctx);
                break;
            }
            case PDJSON_TRUE:  val = JS_TRUE; break;
            case PDJSON_FALSE: val = JS_FALSE; break;
            case PDJSON_NULL:  val = JS_NULL; break;
            default:
                JS_FreeValue(ctx, arr);
                return JS_ThrowSyntaxError(ctx, "JSON5: unexpected token in array");
            }
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
            if (JS_SetPropertyUint32(ctx, arr, idx++, val) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
    }
}

/* 消费下一个事件并解析为一个值（对象/数组内部递归入口）。 */
static JSValue j5_parse_value(J5Parser *p, int depth)
{
    JSContext *ctx = p->ctx;
    enum pdjson_type t;

    if (depth >= J5_MAX_DEPTH)
        return JS_ThrowSyntaxError(ctx, "JSON5: nesting too deep");

    t = pdjson_next(&p->stream);
    switch (t) {
    case PDJSON_OBJECT:
        return j5_parse_object(p, depth);
    case PDJSON_ARRAY:
        return j5_parse_array(p, depth);
    case PDJSON_STRING: {
        size_t n = 0;
        const char *s = pdjson_get_value(&p->stream, &n);
        if (!s)
            return JS_ThrowOutOfMemory(ctx);
        return JS_NewStringLen(ctx, s, n > 0 ? n - 1 : 0);
    }
    case PDJSON_NUMBER: {
        size_t n = 0;
        const char *s = pdjson_get_value(&p->stream, &n);
        if (!s)
            return JS_ThrowOutOfMemory(ctx);
        return j5_make_number(p, s, n > 0 ? n - 1 : 0);
    }
    case PDJSON_TRUE:  return JS_TRUE;
    case PDJSON_FALSE: return JS_FALSE;
    case PDJSON_NULL:  return JS_NULL;
    case PDJSON_ERROR: return j5_throw_parse_error(p);
    default:
        return JS_ThrowSyntaxError(ctx, "JSON5: unexpected end of input");
    }
}

/* ------------------------------------------------------------------ */
/* reviver（与 JSON.parse 相同的内部化语义）                          */
/* ------------------------------------------------------------------ */

static JSValue j5_internalize(JSContext *ctx, JSValueConst holder,
                              JSValueConst key, JSValue val,
                              JSValueConst reviver, int depth)
{
    JSValue args[2];

    if (depth > J5_MAX_DEPTH) {
        JS_FreeValue(ctx, val);
        return JS_ThrowSyntaxError(ctx, "JSON5: nesting too deep");
    }

    if (JS_IsObject(val)) {
        if (JS_IsArray(ctx, val)) {
            JSValue len_val = JS_GetPropertyStr(ctx, val, "length");
            uint32_t len = 0;
            uint32_t i;
            if (JS_IsException(len_val)) {
                JS_FreeValue(ctx, val);
                return JS_EXCEPTION;
            }
            if (JS_ToUint32(ctx, &len, len_val)) {
                JS_FreeValue(ctx, len_val);
                JS_FreeValue(ctx, val);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, len_val);
            for (i = 0; i < len; i++) {
                JSValue child = JS_GetPropertyUint32(ctx, val, i);
                JSValue new_val, keyv;
                char keybuf[16];
                if (JS_IsException(child)) {
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                snprintf(keybuf, sizeof(keybuf), "%u", (unsigned)i);
                keyv = JS_NewString(ctx, keybuf);
                if (JS_IsException(keyv)) {
                    JS_FreeValue(ctx, child);
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                new_val = j5_internalize(ctx, val, keyv, child,
                                         reviver, depth + 1);
                JS_FreeValue(ctx, keyv);
                if (JS_IsException(new_val)) {
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                if (JS_IsUndefined(new_val)) {
                    JSAtom atom = JS_NewAtomUInt32(ctx, i);
                    if (atom == JS_ATOM_NULL) {
                        JS_FreeValue(ctx, new_val);
                        JS_FreeValue(ctx, val);
                        return JS_EXCEPTION;
                    }
                    JS_DeleteProperty(ctx, val, atom, 0);
                    JS_FreeAtom(ctx, atom);
                    JS_FreeValue(ctx, new_val);
                } else if (JS_SetPropertyUint32(ctx, val, i, new_val) < 0) {
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
            }
        } else {
            JSPropertyEnum *tab = NULL;
            uint32_t len = 0, i;
            if (JS_GetOwnPropertyNames(ctx, &tab, &len, val,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
                JS_FreeValue(ctx, val);
                return JS_EXCEPTION;
            }
            for (i = 0; i < len; i++) {
                JSValue child, new_val, keyv;
                child = JS_GetProperty(ctx, val, tab[i].atom);
                if (JS_IsException(child)) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                keyv = JS_AtomToString(ctx, tab[i].atom);
                if (JS_IsException(keyv)) {
                    JS_FreeValue(ctx, child);
                    JS_FreePropertyEnum(ctx, tab, len);
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                new_val = j5_internalize(ctx, val, keyv, child,
                                         reviver, depth + 1);
                JS_FreeValue(ctx, keyv);
                if (JS_IsException(new_val)) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
                if (JS_IsUndefined(new_val)) {
                    JS_DeleteProperty(ctx, val, tab[i].atom, 0);
                    JS_FreeValue(ctx, new_val);
                } else if (JS_SetProperty(ctx, val, tab[i].atom, new_val) < 0) {
                    JS_FreePropertyEnum(ctx, tab, len);
                    JS_FreeValue(ctx, val);
                    return JS_EXCEPTION;
                }
            }
            JS_FreePropertyEnum(ctx, tab, len);
        }
    }

    if (!JS_IsFunction(ctx, reviver)) {
        return val;
    }

    args[0] = JS_DupValue(ctx, key);
    args[1] = val;
    {
        JSValue ret = JS_Call(ctx, reviver, holder, 2, args);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, val);
        return ret;
    }
}

static JSValue js_json5_parse(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    const char *text;
    size_t len;
    J5Parser p;
    JSValue val;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "JSON5.parse requires a string");

    text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text)
        return JS_EXCEPTION;

    memset(&p, 0, sizeof(p));
    p.ctx = ctx;
    pdjson_open_null(&p.stream);
    pdjson_set_allocator(&p.stream, &j5_allocator, ctx);
    pdjson_set_language(&p.stream, PDJSON_LANGUAGE_JSON5);
    pdjson_reopen_buffer(&p.stream, text, len);

    val = j5_parse_value(&p, 0);
    if (!JS_IsException(val)) {
        /* 顶层值之后只允许空白；libpdjson5 非流式模式下多值/尾随垃圾报错 */
        enum pdjson_type t = pdjson_next(&p.stream);
        if (t != PDJSON_DONE) {
            JS_FreeValue(ctx, val);
            val = t == PDJSON_ERROR ? j5_throw_parse_error(&p)
                                    : JS_ThrowSyntaxError(ctx,
                                          "JSON5: unexpected trailing characters");
        }
    }

    pdjson_close(&p.stream);
    JS_FreeCString(ctx, text);

    if (JS_IsException(val))
        return val;

    /* reviver：包一层 {"": value}，按 JSON.parse 语义自底向上转换 */
    if (argc >= 2 && JS_IsFunction(ctx, argv[1])) {
        JSValue root, keyv, holder_val, out;
        root = JS_NewObject(ctx);
        if (JS_IsException(root)) {
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        keyv = JS_NewString(ctx, "");
        if (JS_IsException(keyv)) {
            JS_FreeValue(ctx, root);
            JS_FreeValue(ctx, val);
            return JS_EXCEPTION;
        }
        holder_val = JS_DupValue(ctx, val);
        if (JS_DefinePropertyValueStr(ctx, root, "",
                                      val, JS_PROP_C_W_E) < 0) {
            JS_FreeValue(ctx, holder_val);
            JS_FreeValue(ctx, keyv);
            JS_FreeValue(ctx, root);
            return JS_EXCEPTION;
        }
        out = j5_internalize(ctx, root, keyv, holder_val, argv[1], 0);
        JS_FreeValue(ctx, keyv);
        JS_FreeValue(ctx, root);
        return out;
    }
    return val;
}

/* ------------------------------------------------------------------ */
/* 注册                                                                */
/* ------------------------------------------------------------------ */

static const JSCFunctionListEntry js_json5_funcs[] = {
    JS_CFUNC_DEF("parse", 2, js_json5_parse),
};

int qjs_json5_init(JSContext *ctx, JSValueConst global_obj)
{
    JSValue obj, stringify;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return -1;

    JS_SetPropertyFunctionList(ctx, obj, js_json5_funcs,
                               countof(js_json5_funcs));

    /* 注入官方 json5 stringify 算法（IIFE 返回函数） */
    stringify = JS_Eval(ctx, qjs_json5_stringify_js,
                        strlen(qjs_json5_stringify_js),
                        "<json5-stringify>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(stringify)) {
        JS_FreeValue(ctx, obj);
        return -1;
    }
    if (JS_SetPropertyStr(ctx, obj, "stringify", stringify) < 0) {
        JS_FreeValue(ctx, obj);
        return -1;
    }
    JS_SetPropertyStr(ctx, obj, "version", JS_NewString(ctx, "2.2.3"));

    if (JS_SetPropertyStr(ctx, global_obj, "JSON5", obj) < 0)
        return -1;
    return 0;
}
