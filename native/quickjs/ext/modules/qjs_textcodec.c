/*
 * qjs_textcodec.c - TextEncoder / TextDecoder
 *
 * Uses the Lexbor encoding library to provide full encoding/decoding
 * support for all WHATWG-defined encodings (UTF-8, UTF-16, GBK, GB18030,
 * Big5, Shift_JIS, EUC-JP, ISO-8859-x, Windows-125x, etc.).
 *
 * The TextEncoder supports both UTF-8 (default) and custom named encodings.
 * The TextDecoder supports all named encodings.
 */

#include "qjs_native.h"
#include <lexbor/encoding/encoding.h>
#include <stdlib.h>
#include <string.h>



/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Convert a JS string to an array of Unicode codepoints.
 * Returns malloc'd array; sets *out_count.
 * Returns NULL on error. */
static lxb_codepoint_t *qjs_str_to_codepoints(JSContext *ctx,
                                               JSValueConst val,
                                               size_t *out_count)
{
    const char *str;
    size_t len;
    str = JS_ToCStringLen(ctx, &len, val);
    if (!str)
        return NULL;

    /* Worst case: each byte is one codepoint */
    lxb_codepoint_t *cps = (lxb_codepoint_t *)malloc(sizeof(lxb_codepoint_t) * (len + 1));
    if (!cps) {
        JS_FreeCString(ctx, str);
        return NULL;
    }

    /* Decode UTF-8 string to codepoints */
    size_t i = 0, count = 0;
    while (i < len) {
        unsigned char c = (unsigned char)str[i];
        lxb_codepoint_t cp;
        int n;

        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 4;
        } else {
            cp = 0xFFFD;
            n = 1;
        }

        for (int j = 1; j < n && i + j < len; j++) {
            unsigned char cc = (unsigned char)str[i + j];
            if ((cc & 0xC0) == 0x80)
                cp = (cp << 6) | (cc & 0x3F);
            else {
                cp = 0xFFFD;
                n = j + 1;
                break;
            }
        }

        if (i + n > len) {
            cp = 0xFFFD;
            n = (int)(len - i);
        }

        cps[count++] = cp;
        i += n;
    }

    JS_FreeCString(ctx, str);
    *out_count = count;
    return cps;
}

/* Convert codepoints to a JS string (UTF-8 encoded) */
static JSValue qjs_codepoints_to_str(JSContext *ctx,
                                     const lxb_codepoint_t *cps,
                                     size_t count)
{
    /* Allocate buffer: max 4 bytes per codepoint */
    size_t buf_size = count * 4 + 1;
    char *buf = (char *)malloc(buf_size);
    if (!buf)
        return JS_EXCEPTION;

    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        lxb_codepoint_t cp = cps[i];
        if (cp < 0x80) {
            buf[pos++] = (char)cp;
        } else if (cp < 0x800) {
            buf[pos++] = (char)(0xC0 | (cp >> 6));
            buf[pos++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[pos++] = (char)(0xE0 | (cp >> 12));
            buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[pos++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[pos++] = (char)(0xF0 | (cp >> 18));
            buf[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    }

    JSValue ret = JS_NewStringLen(ctx, buf, pos);
    free(buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* TextEncoder                                                         */
/* ------------------------------------------------------------------ */


static JSValue qjs_textencoder_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    lxb_encoding_t encoding = LXB_ENCODING_UTF_8;

    /* 解析自定义编码名称 */
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        const char *label = JS_ToCString(ctx, argv[0]);
        if (label) {
            const lxb_encoding_data_t *enc_data =
                lxb_encoding_data_by_name((const lxb_char_t *)label, strlen(label));
            if (enc_data) {
                encoding = enc_data->encoding;
            } else {
                JSValue err = JS_ThrowTypeError(ctx, "TextEncoder: unknown encoding '%s'", label);
                JS_FreeCString(ctx, label);
                return err;
            }
            JS_FreeCString(ctx, label);
        }
    }

    /* Use the prototype from new_target for inheritance */
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    JSValue obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;

    /* 将编码信息保存在对象属性中 */
    const lxb_encoding_data_t *enc_data = lxb_encoding_data(encoding);
    const char *enc_name = enc_data && enc_data->name ? (const char *)enc_data->name : "utf-8";
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, enc_name));
    JS_SetPropertyStr(ctx, obj, "_encoding", JS_NewInt32(ctx, (int)encoding));

    return obj;
}

static JSValue qjs_textencoder_encode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0]))
        return JS_NewUint8ArrayCopy(ctx, NULL, 0);

    /* 获取目标编码 */
    int enc_int = LXB_ENCODING_UTF_8;
    JSValue enc_val = JS_GetPropertyStr(ctx, this_val, "_encoding");
    if (!JS_IsException(enc_val) && !JS_IsUndefined(enc_val)) {
        JS_ToInt32(ctx, &enc_int, enc_val);
    }
    JS_FreeValue(ctx, enc_val);

    /* 将输入字符串转换为 Unicode 码点 */
    size_t cp_count;
    lxb_codepoint_t *cps = qjs_str_to_codepoints(ctx, argv[0], &cp_count);
    if (!cps)
        return JS_EXCEPTION;

    const lxb_encoding_data_t *enc_data = lxb_encoding_data((lxb_encoding_t)enc_int);
    if (!enc_data) {
        free(cps);
        return JS_ThrowTypeError(ctx, "TextEncoder: unknown encoding");
    }

    /* 
     * 如果是 UTF-8，直接手动编码
     */
    if (enc_int == LXB_ENCODING_UTF_8) {
        size_t buf_size = cp_count * 4 + 16;
        uint8_t *out_buf = (uint8_t *)malloc(buf_size);
        if (!out_buf) {
            free(cps);
            return JS_ThrowInternalError(ctx, "TextEncoder: out of memory");
        }

        size_t pos = 0;
        for (size_t i = 0; i < cp_count; i++) {
            lxb_codepoint_t cp = cps[i];
            if (cp < 0x80) {
                out_buf[pos++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                out_buf[pos++] = (uint8_t)(0xC0 | (cp >> 6));
                out_buf[pos++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out_buf[pos++] = (uint8_t)(0xE0 | (cp >> 12));
                out_buf[pos++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                out_buf[pos++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else {
                out_buf[pos++] = (uint8_t)(0xF0 | (cp >> 18));
                out_buf[pos++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                out_buf[pos++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                out_buf[pos++] = (uint8_t)(0x80 | (cp & 0x3F));
            }
        }
        free(cps);
        JSValue arr = JS_NewUint8ArrayCopy(ctx, out_buf, pos);
        free(out_buf);
        return arr;
    }

    /* --- 非 UTF-8 编码：使用最底层、最稳定的回调函数进行流式编码 --- */
    size_t out_buf_size = cp_count * 4 + 64;
    uint8_t *out_buf = (uint8_t *)malloc(out_buf_size);
    if (!out_buf) {
        free(cps);
        return JS_ThrowInternalError(ctx, "TextEncoder: out of memory");
    }

    lxb_encoding_encode_t encoder;
    lxb_status_t status = lxb_encoding_encode_init(&encoder, enc_data, out_buf, out_buf_size);
    if (status != LXB_STATUS_OK) {
        free(out_buf);
        free(cps);
        return JS_ThrowInternalError(ctx, "TextEncoder: encode init failed");
    }

    const lxb_codepoint_t *cp_ptr = cps;
    const lxb_codepoint_t *cp_end = cps + cp_count;

    while (cp_ptr < cp_end) {
        /* 直接调用编码底层的回调函数，兼容所有 Lexbor 版本 */
        status = enc_data->encode(&encoder, &cp_ptr, cp_end);
        if (status == LXB_STATUS_SMALL_BUFFER) {
            /* 缓冲区不足，需要扩容 */
            size_t used = lxb_encoding_encode_buf_used(&encoder);
            size_t new_size = out_buf_size * 2;
            uint8_t *new_buf = (uint8_t *)realloc(out_buf, new_size);
            if (!new_buf) {
                free(out_buf);
                free(cps);
                return JS_ThrowInternalError(ctx, "TextEncoder: out of memory on realloc");
            }
            out_buf = new_buf;
            out_buf_size = new_size;
            /* 安全重设缓冲区指针 */
            lxb_encoding_encode_buf_set(&encoder, out_buf + used, out_buf_size - used);
        } else if (status != LXB_STATUS_OK) {
            free(out_buf);
            free(cps);
            return JS_ThrowInternalError(ctx, "TextEncoder: encoding failed (status %d)", status);
        }
    }

    size_t written = lxb_encoding_encode_buf_used(&encoder);
    free(cps);

    /* 创建 Uint8Array 并返回 */
    JSValue arr = JS_NewUint8ArrayCopy(ctx, out_buf, written);
    free(out_buf);
    return arr;
}

static JSValue qjs_textencoder_encodeInto(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "encodeInto requires 2 arguments");

    /* 获取目标编码 */
    int enc_int = LXB_ENCODING_UTF_8;
    JSValue enc_val = JS_GetPropertyStr(ctx, this_val, "_encoding");
    if (!JS_IsException(enc_val) && !JS_IsUndefined(enc_val)) {
        JS_ToInt32(ctx, &enc_int, enc_val);
    }
    JS_FreeValue(ctx, enc_val);

    size_t cp_count;
    lxb_codepoint_t *cps = qjs_str_to_codepoints(ctx, argv[0], &cp_count);
    if (!cps)
        return JS_EXCEPTION;

    /* Get the Uint8Array */
    qjs_str_t arr_data_info;
    if (JS_IsException(qjs_typed_array_data(ctx, argv[1], &arr_data_info))) {
        free(cps);
        return JS_ThrowTypeError(ctx, "second argument must be a TypedArray or ArrayBuffer");
    }

    uint8_t *arr_data = arr_data_info.start;
    size_t arr_size = arr_data_info.length;

    const lxb_encoding_data_t *enc_data = lxb_encoding_data((lxb_encoding_t)enc_int);
    if (!enc_data) {
        free(cps);
        return JS_ThrowTypeError(ctx, "TextEncoder: unknown encoding");
    }

    /* UTF-8 直接手动处理 */
    if (enc_int == LXB_ENCODING_UTF_8) {
        size_t written = 0;
        size_t read = 0;
        for (size_t i = 0; i < cp_count; i++) {
            lxb_codepoint_t cp = cps[i];
            size_t needed;
            if (cp < 0x80) needed = 1;
            else if (cp < 0x800) needed = 2;
            else if (cp < 0x10000) needed = 3;
            else needed = 4;

            if (written + needed > arr_size) break;

            if (cp < 0x80) {
                arr_data[written++] = (uint8_t)cp;
            } else if (cp < 0x800) {
                arr_data[written++] = (uint8_t)(0xC0 | (cp >> 6));
                arr_data[written++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                arr_data[written++] = (uint8_t)(0xE0 | (cp >> 12));
                arr_data[written++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                arr_data[written++] = (uint8_t)(0x80 | (cp & 0x3F));
            } else {
                arr_data[written++] = (uint8_t)(0xF0 | (cp >> 18));
                arr_data[written++] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                arr_data[written++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                arr_data[written++] = (uint8_t)(0x80 | (cp & 0x3F));
            }
            read = i + 1;
        }
        free(cps);

        JSValue result = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, result, "read", JS_NewInt32(ctx, (int)read));
        JS_SetPropertyStr(ctx, result, "written", JS_NewInt32(ctx, (int)written));
        return result;
    }

    /* --- 非 UTF-8 编码：使用最底层回调函数进行流式编码 --- */
    lxb_encoding_encode_t encoder;
    lxb_status_t status = lxb_encoding_encode_init(&encoder, enc_data, arr_data, arr_size);
    if (status != LXB_STATUS_OK) {
        free(cps);
        return JS_ThrowInternalError(ctx, "TextEncoder: encode init failed");
    }

    const lxb_codepoint_t *cp_ptr = cps;
    const lxb_codepoint_t *cp_end = cps + cp_count;
    size_t read = 0;

    while (cp_ptr < cp_end) {
        size_t processed_before = cp_end - cp_ptr;
        status = enc_data->encode(&encoder, &cp_ptr, cp_end);
        size_t processed_now = cp_end - cp_ptr;
        read += processed_before - processed_now;

        if (status == LXB_STATUS_SMALL_BUFFER) {
            /* 目标空间不足，停止写入 */
            break;
        } else if (status != LXB_STATUS_OK) {
            free(cps);
            return JS_ThrowInternalError(ctx, "TextEncoder: encoding failed (status %d)", status);
        }
    }

    free(cps);

    size_t written = lxb_encoding_encode_buf_used(&encoder);
    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "read", JS_NewInt32(ctx, (int)read));
    JS_SetPropertyStr(ctx, result, "written", JS_NewInt32(ctx, (int)written));
    return result;
}

/* ------------------------------------------------------------------ */
/* TextDecoder                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    lxb_encoding_t encoding;
    int fatal;
    int ignoreBOM;
} qjs_textdecoder_t;


static JSValue qjs_textdecoder_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv)
{
    qjs_textdecoder_t td;
    td.encoding = LXB_ENCODING_UTF_8;
    td.fatal = 0;
    td.ignoreBOM = 0;

    if (argc >= 1) {
        const char *label = JS_ToCString(ctx, argv[0]);
        if (label) {
            const lxb_encoding_data_t *enc_data =
                lxb_encoding_data_by_name((const lxb_char_t *)label, strlen(label));
            if (enc_data) {
                td.encoding = enc_data->encoding;
            }
            JS_FreeCString(ctx, label);
        }
    }

    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue fatal_val = JS_GetPropertyStr(ctx, argv[1], "fatal");
        if (!JS_IsUndefined(fatal_val)) {
            JS_ToInt32(ctx, &td.fatal, fatal_val);
        }
        JS_FreeValue(ctx, fatal_val);

        JSValue bom_val = JS_GetPropertyStr(ctx, argv[1], "ignoreBOM");
        if (!JS_IsUndefined(bom_val)) {
            JS_ToInt32(ctx, &td.ignoreBOM, bom_val);
        }
        JS_FreeValue(ctx, bom_val);
    }

    JSValue obj;
    /* Use the prototype from new_target for inheritance */
    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    obj = JS_NewObjectProto(ctx, proto);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        return obj;

    /* Store the encoding as properties */
    const lxb_encoding_data_t *enc_data = lxb_encoding_data(td.encoding);
    const char *enc_name = enc_data && enc_data->name ? (const char *)enc_data->name : "utf-8";
    JS_SetPropertyStr(ctx, obj, "encoding", JS_NewString(ctx, enc_name));
    JS_SetPropertyStr(ctx, obj, "_encoding", JS_NewInt32(ctx, (int)td.encoding));
    JS_SetPropertyStr(ctx, obj, "_fatal", JS_NewInt32(ctx, td.fatal));
    JS_SetPropertyStr(ctx, obj, "_ignoreBOM", JS_NewInt32(ctx, td.ignoreBOM));

    return obj;
}

static JSValue qjs_textdecoder_decode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv)
{
    int enc_int = 0, fatal = 0;
    JSValue enc_val = JS_GetPropertyStr(ctx, this_val, "_encoding");
    JS_ToInt32(ctx, &enc_int, enc_val);
    JS_FreeValue(ctx, enc_val);

    JSValue fatal_val = JS_GetPropertyStr(ctx, this_val, "_fatal");
    JS_ToInt32(ctx, &fatal, fatal_val);
    JS_FreeValue(ctx, fatal_val);

    lxb_encoding_t encoding = (lxb_encoding_t)enc_int;

    /* Get input bytes */
    const uint8_t *input;
    size_t input_len;
    const char *cstr_alloc = NULL;

    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        /* 优先尝试作为 TypedArray/DataView/ArrayBuffer 获取 */
        qjs_str_t data_info;
        if (!JS_IsException(qjs_typed_array_data(ctx, argv[0], &data_info))) {
            input = data_info.start;
            input_len = data_info.length;
        } else {
            /* Try as string */
            size_t slen;
            const char *s = JS_ToCStringLen(ctx, &slen, argv[0]);
            if (!s)
                return JS_EXCEPTION;
            input = (const uint8_t *)s;
            input_len = slen;
            cstr_alloc = s;
        }
    } else {
        input = (const uint8_t *)"";
        input_len = 0;
    }

    if (input_len == 0) {
        if (cstr_alloc) JS_FreeCString(ctx, cstr_alloc);
        return JS_NewString(ctx, "");
    }

    /* For UTF-8, do manual decoding (most common case) */
    if (encoding == LXB_ENCODING_UTF_8) {
        /* Skip BOM if present */
        size_t start = 0;
        if (input_len >= 3 && input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF) {
            start = 3;
        }

        /* Decode UTF-8 to codepoints, then build JS string */
        /* QuickJS JS_NewStringLen expects UTF-8, so we can pass through directly */
        JSValue result = JS_NewStringLen(ctx, (const char *)(input + start), input_len - start);
        if (cstr_alloc) JS_FreeCString(ctx, cstr_alloc);
        return result;
    }

    /* For other encodings, use Lexbor decode API */
    const lxb_encoding_data_t *enc_data = lxb_encoding_data(encoding);
    if (!enc_data) {
        if (cstr_alloc) JS_FreeCString(ctx, cstr_alloc);
        return JS_ThrowTypeError(ctx, "TextDecoder: unknown encoding");
    }

    /* Allocate output buffer for codepoints */
    size_t cp_buf_size = input_len * 2 + 16;
    lxb_codepoint_t *cp_buf = (lxb_codepoint_t *)malloc(sizeof(lxb_codepoint_t) * cp_buf_size);
    if (!cp_buf) {
        if (cstr_alloc) JS_FreeCString(ctx, cstr_alloc);
        return JS_ThrowInternalError(ctx, "TextDecoder: out of memory");
    }

    lxb_encoding_decode_t decoder;
    lxb_encoding_decode_init(&decoder, enc_data, cp_buf, cp_buf_size);

    const lxb_char_t *data_ptr = (const lxb_char_t *)input;
    const lxb_char_t *data_end = (const lxb_char_t *)input + input_len;

    /* 直接调用解码的底层回调函数，兼容所有 Lexbor 版本 */
    lxb_status_t status = enc_data->decode(&decoder, &data_ptr, data_end);
    size_t cp_count = lxb_encoding_decode_buf_used(&decoder);

    /* 转换码点为 JS 字符串 */
    JSValue result = qjs_codepoints_to_str(ctx, cp_buf, cp_count);

    /* decoder 为栈变量，无需显式销毁；仅释放码点缓冲区 */
    free(cp_buf);
    if (cstr_alloc) {
        JS_FreeCString(ctx, cstr_alloc);
    }

    if (status != LXB_STATUS_OK && status != LXB_STATUS_SMALL_BUFFER) {
        if (fatal) {
            JS_FreeValue(ctx, result);
            return JS_ThrowTypeError(ctx, "TextDecoder: decoding failed (status %d)", status);
        }
    }

    return result;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

int qjs_textcodec_init(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);

    /* TextEncoder - use constructor with prototype */
    JSValue te_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, te_proto, "encode",
        JS_NewCFunction(ctx, qjs_textencoder_encode, "encode", 1));
    JS_SetPropertyStr(ctx, te_proto, "encodeInto",
        JS_NewCFunction(ctx, qjs_textencoder_encodeInto, "encodeInto", 2));

    JSValue te_ctor = JS_NewCFunction2(ctx, qjs_textencoder_ctor, "TextEncoder",
                                       0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, te_ctor, te_proto);
    JS_SetPropertyStr(ctx, global, "TextEncoder", te_ctor);
    JS_FreeValue(ctx, te_proto);

    /* TextDecoder - use constructor with prototype */
    JSValue td_proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, td_proto, "decode",
        JS_NewCFunction(ctx, qjs_textdecoder_decode, "decode", 1));

    JSValue td_ctor = JS_NewCFunction2(ctx, qjs_textdecoder_ctor, "TextDecoder",
                                       1, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, td_ctor, td_proto);
    JS_SetPropertyStr(ctx, global, "TextDecoder", td_ctor);
    JS_FreeValue(ctx, td_proto);

    JS_FreeValue(ctx, global);
    return 0;
}
