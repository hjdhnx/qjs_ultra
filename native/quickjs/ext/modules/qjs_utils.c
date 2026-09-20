
/*
 * Copyright (C) Dmitry Volyntsev
 * Copyright (C) NGINX, Inc.
 */

/*
 * qjs_utils.c - 公共工具函数实现（原 qjs.c 并入本文件）
 *
 * 本文件是所有扩展模块共享的底层工具集合：
 *   - qjs_strncasecmp           大小写不敏感字符串比较
 *   - qjs_to_bytes/qjs_bytes_free  JS 值 → 字节序列统一转换
 *   - qjs_typed_array_data      TypedArray/DataView/ArrayBuffer 数据提取
 *   - qjs_new_array_buffer      ArrayBuffer 构造（含释放回调）
 *   - qjs_string_hex/base64/base64url  编码结果转 JS 字符串
 *   - qjs_promise_result        Promise 封装（resolve/reject trampoline）
 *   - qjs_errno_string          errno 错误码 → 可读字符串
 *
 * 接口声明统一位于 include/qjs_native.h。
 */

#include <qjs_native.h>


#define qjs_errno_case(e)                                                   \
    case e:                                                                 \
        return #e;


const char*
qjs_errno_string(int errnum)
{
    switch (errnum) {
#ifdef EACCES
    qjs_errno_case(EACCES);
#endif

#ifdef EADDRINUSE
    qjs_errno_case(EADDRINUSE);
#endif

#ifdef EADDRNOTAVAIL
    qjs_errno_case(EADDRNOTAVAIL);
#endif

#ifdef EAFNOSUPPORT
    qjs_errno_case(EAFNOSUPPORT);
#endif

#ifdef EAGAIN
    qjs_errno_case(EAGAIN);
#endif

#ifdef EWOULDBLOCK
#if EAGAIN != EWOULDBLOCK
    qjs_errno_case(EWOULDBLOCK);
#endif
#endif

#ifdef EALREADY
    qjs_errno_case(EALREADY);
#endif

#ifdef EBADF
    qjs_errno_case(EBADF);
#endif

#ifdef EBADMSG
    qjs_errno_case(EBADMSG);
#endif

#ifdef EBUSY
    qjs_errno_case(EBUSY);
#endif

#ifdef ECANCELED
    qjs_errno_case(ECANCELED);
#endif

#ifdef ECHILD
    qjs_errno_case(ECHILD);
#endif

#ifdef ECONNABORTED
    qjs_errno_case(ECONNABORTED);
#endif

#ifdef ECONNREFUSED
    qjs_errno_case(ECONNREFUSED);
#endif

#ifdef ECONNRESET
    qjs_errno_case(ECONNRESET);
#endif

#ifdef EDEADLK
    qjs_errno_case(EDEADLK);
#endif

#ifdef EDESTADDRREQ
    qjs_errno_case(EDESTADDRREQ);
#endif

#ifdef EDOM
    qjs_errno_case(EDOM);
#endif

#ifdef EDQUOT
    qjs_errno_case(EDQUOT);
#endif

#ifdef EEXIST
    qjs_errno_case(EEXIST);
#endif

#ifdef EFAULT
    qjs_errno_case(EFAULT);
#endif

#ifdef EFBIG
    qjs_errno_case(EFBIG);
#endif

#ifdef EHOSTUNREACH
    qjs_errno_case(EHOSTUNREACH);
#endif

#ifdef EIDRM
    qjs_errno_case(EIDRM);
#endif

#ifdef EILSEQ
    qjs_errno_case(EILSEQ);
#endif

#ifdef EINPROGRESS
    qjs_errno_case(EINPROGRESS);
#endif

#ifdef EINTR
    qjs_errno_case(EINTR);
#endif

#ifdef EINVAL
    qjs_errno_case(EINVAL);
#endif

#ifdef EIO
    qjs_errno_case(EIO);
#endif

#ifdef EISCONN
    qjs_errno_case(EISCONN);
#endif

#ifdef EISDIR
    qjs_errno_case(EISDIR);
#endif

#ifdef ELOOP
    qjs_errno_case(ELOOP);
#endif

#ifdef EMFILE
    qjs_errno_case(EMFILE);
#endif

#ifdef EMLINK
    qjs_errno_case(EMLINK);
#endif

#ifdef EMSGSIZE
    qjs_errno_case(EMSGSIZE);
#endif

#ifdef EMULTIHOP
    qjs_errno_case(EMULTIHOP);
#endif

#ifdef ENAMETOOLONG
    qjs_errno_case(ENAMETOOLONG);
#endif

#ifdef ENETDOWN
    qjs_errno_case(ENETDOWN);
#endif

#ifdef ENETRESET
    qjs_errno_case(ENETRESET);
#endif

#ifdef ENETUNREACH
    qjs_errno_case(ENETUNREACH);
#endif

#ifdef ENFILE
    qjs_errno_case(ENFILE);
#endif

#ifdef ENOBUFS
    qjs_errno_case(ENOBUFS);
#endif

#ifdef ENODATA
    qjs_errno_case(ENODATA);
#endif

#ifdef ENODEV
    qjs_errno_case(ENODEV);
#endif

#ifdef ENOENT
    qjs_errno_case(ENOENT);
#endif

#ifdef ENOEXEC
    qjs_errno_case(ENOEXEC);
#endif

#ifdef ENOLINK
    qjs_errno_case(ENOLINK);
#endif

#ifdef ENOLCK
#if ENOLINK != ENOLCK
    qjs_errno_case(ENOLCK);
#endif
#endif

#ifdef ENOMEM
    qjs_errno_case(ENOMEM);
#endif

#ifdef ENOMSG
    qjs_errno_case(ENOMSG);
#endif

#ifdef ENOPROTOOPT
    qjs_errno_case(ENOPROTOOPT);
#endif

#ifdef ENOSPC
    qjs_errno_case(ENOSPC);
#endif

#ifdef ENOSR
    qjs_errno_case(ENOSR);
#endif

#ifdef ENOSTR
    qjs_errno_case(ENOSTR);
#endif

#ifdef ENOSYS
    qjs_errno_case(ENOSYS);
#endif

#ifdef ENOTCONN
    qjs_errno_case(ENOTCONN);
#endif

#ifdef ENOTDIR
    qjs_errno_case(ENOTDIR);
#endif

#ifdef ENOTEMPTY
#if ENOTEMPTY != EEXIST
    qjs_errno_case(ENOTEMPTY);
#endif
#endif

#ifdef ENOTSOCK
    qjs_errno_case(ENOTSOCK);
#endif

#ifdef ENOTSUP
    qjs_errno_case(ENOTSUP);
#else
#ifdef EOPNOTSUPP
    qjs_errno_case(EOPNOTSUPP);
#endif
#endif

#ifdef ENOTTY
    qjs_errno_case(ENOTTY);
#endif

#ifdef ENXIO
    qjs_errno_case(ENXIO);
#endif

#ifdef EOVERFLOW
    qjs_errno_case(EOVERFLOW);
#endif

#ifdef EPERM
    qjs_errno_case(EPERM);
#endif

#ifdef EPIPE
    qjs_errno_case(EPIPE);
#endif

#ifdef EPROTO
    qjs_errno_case(EPROTO);
#endif

#ifdef EPROTONOSUPPORT
    qjs_errno_case(EPROTONOSUPPORT);
#endif

#ifdef EPROTOTYPE
    qjs_errno_case(EPROTOTYPE);
#endif

#ifdef ERANGE
    qjs_errno_case(ERANGE);
#endif

#ifdef EROFS
    qjs_errno_case(EROFS);
#endif

#ifdef ESPIPE
    qjs_errno_case(ESPIPE);
#endif

#ifdef ESRCH
    qjs_errno_case(ESRCH);
#endif

#ifdef ESTALE
    qjs_errno_case(ESTALE);
#endif

#ifdef ETIME
    qjs_errno_case(ETIME);
#endif

#ifdef ETIMEDOUT
    qjs_errno_case(ETIMEDOUT);
#endif

#ifdef ETXTBSY
    qjs_errno_case(ETXTBSY);
#endif

#ifdef EXDEV
    qjs_errno_case(EXDEV);
#endif

    default:
        break;
    }

    return "UNKNOWN CODE";
}


/* ===== 以下内容自 qjs.c 并入 ===== */


static void
js_array_buffer_free(JSRuntime *rt, void *opaque, void *ptr)
{
    js_free_rt(rt, ptr);
}



qjs_int_t
qjs_strncasecmp(u_char *s1, u_char *s2, size_t n)
{
    qjs_uint_t  c1, c2;

    while (n) {
        c1 = (qjs_uint_t) *s1++;
        c2 = (qjs_uint_t) *s2++;

        c1 = (c1 >= 'A' && c1 <= 'Z') ? (c1 | 0x20) : c1;
        c2 = (c2 >= 'A' && c2 <= 'Z') ? (c2 | 0x20) : c2;

        if (c1 == c2) {

            if (c1) {
                n--;
                continue;
            }

            return 0;
        }

        return c1 - c2;
    }

    return 0;
}


int
qjs_to_bytes(JSContext *ctx, qjs_bytes_t *bytes, JSValueConst value)
{
    size_t   byte_offset, byte_length;
    JSValue  ab, val;

    /*
     * 多数据类型输入支持，按优先级依次尝试：
     *   1. String (含 STRING_ROPE，JS_IsString 统一处理)
     *   2. TypedArray (Uint8Array/Int32Array/...)
     *   3. DataView
     *   4. ArrayBuffer / SharedArrayBuffer
     *   5. Fallback: JS_ToString 转换后取 C 字符串
     *
     * 旧代码有一个致命的 fallthrough bug：非字符串值经 JS_ToString
     * 转换后 fallthrough 到 string: 标签，对原始值再次调用
     * JS_ToCStringLen（返回 NULL），覆盖了正确结果。
     */

    /* 1. String → 直接获取 C 字符串 */
    if (JS_IsString(value)) {
        bytes->tag = JS_TAG_STRING;
        bytes->start = (u_char *) JS_ToCStringLen(ctx, &bytes->length, value);
        return (bytes->start != NULL) ? 0 : -1;
    }

    /* 2. TypedArray → 获取底层 ArrayBuffer 的数据指针 */
    if (JS_IsTypedArray(value)) {
        ab = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length, NULL);
        if (!JS_IsException(ab)) {
            bytes->start = JS_GetArrayBuffer(ctx, &bytes->length, ab);
            JS_FreeValue(ctx, ab);
            if (bytes->start != NULL) {
                bytes->tag = JS_TAG_OBJECT;
                bytes->start += byte_offset;
                bytes->length = byte_length;
                return 0;
            }
        }
        return -1;
    }

    /* 3. DataView → 获取底层 ArrayBuffer 的数据指针 */
    if (JS_IsDataView(value)) {
        ab = JS_GetDataViewBuffer(ctx, value, &byte_offset, &byte_length, NULL);
        if (!JS_IsException(ab)) {
            bytes->start = JS_GetArrayBuffer(ctx, &bytes->length, ab);
            JS_FreeValue(ctx, ab);
            if (bytes->start != NULL) {
                bytes->tag = JS_TAG_OBJECT;
                bytes->start += byte_offset;
                bytes->length = byte_length;
                return 0;
            }
        }
        return -1;
    }

    /* 4. ArrayBuffer / SharedArrayBuffer → 直接获取数据指针 */
    if (JS_IsArrayBuffer(value)) {
        bytes->start = JS_GetArrayBuffer(ctx, &bytes->length, value);
        if (bytes->start != NULL) {
            bytes->tag = JS_TAG_OBJECT;
            return 0;
        }
    }

    /* 5. Fallback: 转换为字符串后取 C 字符串 */
    val = JS_ToString(ctx, value);
    if (JS_IsException(val)) {
        return -1;
    }

    bytes->tag = JS_TAG_STRING;
    bytes->start = (u_char *) JS_ToCStringLen(ctx, &bytes->length, val);
    JS_FreeValue(ctx, val);

    return (bytes->start != NULL) ? 0 : -1;
}

void
qjs_bytes_free(JSContext *ctx, qjs_bytes_t *bytes)
{
    if (bytes->tag == JS_TAG_STRING) {
        JS_FreeCString(ctx, (char *) bytes->start);
    }
}

JSValue
qjs_typed_array_data(JSContext *ctx, JSValueConst value, qjs_str_t *data)
{
    size_t   byte_offset, byte_length;
    JSValue  ab;

    /*
     * 统一处理多种二进制数据类型，返回底层字节指针和长度：
     *   1. TypedArray (Uint8Array/Int32Array/...)
     *   2. DataView
     *   3. ArrayBuffer / SharedArrayBuffer
     *
     * 成功返回 JS_UNDEFINED，失败返回 JS_EXCEPTION。
     * data->start 指向 JS 管理的内存，调用者无需释放。
     */

    /* 1. TypedArray → 获取底层 ArrayBuffer */
    if (JS_IsTypedArray(value)) {
        ab = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length, NULL);
        if (JS_IsException(ab)) {
            return JS_EXCEPTION;
        }
        data->start = JS_GetArrayBuffer(ctx, &data->length, ab);
        JS_FreeValue(ctx, ab);
        if (data->start == NULL) {
            return JS_EXCEPTION;
        }
        data->start += byte_offset;
        data->length = byte_length;
        return JS_UNDEFINED;
    }

    /* 2. DataView → 获取底层 ArrayBuffer */
    if (JS_IsDataView(value)) {
        ab = JS_GetDataViewBuffer(ctx, value, &byte_offset, &byte_length, NULL);
        if (JS_IsException(ab)) {
            return JS_EXCEPTION;
        }
        data->start = JS_GetArrayBuffer(ctx, &data->length, ab);
        JS_FreeValue(ctx, ab);
        if (data->start == NULL) {
            return JS_EXCEPTION;
        }
        data->start += byte_offset;
        data->length = byte_length;
        return JS_UNDEFINED;
    }

    /* 3. ArrayBuffer / SharedArrayBuffer → 直接获取数据指针 */
    if (JS_IsArrayBuffer(value)) {
        data->start = JS_GetArrayBuffer(ctx, &data->length, value);
        if (data->start == NULL) {
            return JS_EXCEPTION;
        }
        return JS_UNDEFINED;
    }

    /* 非二进制数据类型 */
    return JS_EXCEPTION;
}


JSValue
qjs_new_array_buffer(JSContext *cx, uint8_t *src, size_t len)
{
    return JS_NewArrayBuffer(cx, src, len, js_array_buffer_free, NULL, 0);
}

JSValue
qjs_string_hex(JSContext *cx, const qjs_str_t *src)
{
    JSValue    ret;
    qjs_str_t  dst;
    u_char     buf[1024];

    if (src->length == 0) {
        return JS_NewStringLen(cx, "", 0);
    }

    dst.start = buf;
    dst.length = qjs_hex_encode_length(cx, src);

    if (dst.length <= sizeof(buf)) {
        qjs_hex_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);

    } else {
        dst.start = js_malloc(cx, dst.length);
        if (dst.start == NULL) {
            return JS_ThrowOutOfMemory(cx);
        }

        qjs_hex_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);
        js_free(cx, dst.start);
    }

    return ret;
}


JSValue
qjs_string_base64(JSContext *cx, const qjs_str_t *src)
{
    JSValue    ret;
    qjs_str_t  dst;
    u_char     buf[1024];

    if (src->length == 0) {
        return JS_NewStringLen(cx, "", 0);
    }

    dst.start = buf;
    dst.length = qjs_base64_encode_length(cx, src);

    if (dst.length <= sizeof(buf)) {
        qjs_base64_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);

    } else {
        dst.start = js_malloc(cx, dst.length);
        if (dst.start == NULL) {
            return JS_ThrowOutOfMemory(cx);
        }

        qjs_base64_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);
        js_free(cx, dst.start);
    }

    return ret;
}


JSValue
qjs_string_base64url(JSContext *cx, const qjs_str_t *src)
{
    size_t     padding;
    JSValue    ret;
    qjs_str_t  dst;
    u_char     buf[1024];

    if (src->length == 0) {
        return JS_NewStringLen(cx, "", 0);
    }

    padding = src->length % 3;
    padding = (4 >> padding) & 0x03;

    dst.start = buf;
    dst.length = qjs_base64_encode_length(cx, src) - padding;

    if (dst.length <= sizeof(buf)) {
        qjs_base64url_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);

    } else {
        dst.start = js_malloc(cx, dst.length);
        if (dst.start == NULL) {
            return JS_ThrowOutOfMemory(cx);
        }

        qjs_base64url_encode(cx, src, &dst);
        ret = JS_NewStringLen(cx, (const char *) dst.start, dst.length);
        js_free(cx, dst.start);
    }

    return ret;
}


static JSValue
qjs_promise_fill_trampoline(JSContext *cx, int argc, JSValueConst *argv)
{
    return JS_Call(cx, argv[0], JS_UNDEFINED, 1, &argv[1]);
}


JSValue
qjs_promise_result(JSContext *cx, JSValue result)
{
    JS_BOOL  is_error;
    JSValue  promise, callbacks[2], arguments[2];

    promise = JS_NewPromiseCapability(cx, callbacks);
    if (JS_IsException(promise)) {
        JS_FreeValue(cx, result);
        return JS_EXCEPTION;
    }

    is_error = JS_IsException(result);

    JS_FreeValue(cx, callbacks[!is_error]);
    arguments[0] = callbacks[is_error];
    arguments[1] = is_error ? JS_GetException(cx) : result;

    if (JS_EnqueueJob(cx, qjs_promise_fill_trampoline, 2, arguments) < 0) {
        JS_FreeValue(cx, promise);
        JS_FreeValue(cx, callbacks[is_error]);
        JS_FreeValue(cx, result);
        return JS_EXCEPTION;
    }

    JS_FreeValue(cx, arguments[0]);
    JS_FreeValue(cx, arguments[1]);

    return promise;
}

/* js_module_set_import_meta 已在 quickjs/quickjs.c 中实现（5 参数版本），
 * 无需在此重复定义。 */
