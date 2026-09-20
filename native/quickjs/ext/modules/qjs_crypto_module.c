/*
 * Copyright (C) 2024 qjs-native contributors
 *
 * Crypto module - hash and HMAC algorithms powered by OpenSSL EVP_MD and HMAC.
 */

#include <qjs_native.h>
#include "qjs_openssl.h"
#include <string.h>

/* ---------- Hash 上下文 ---------- */
typedef struct {
    const EVP_MD  *md;
    EVP_MD_CTX    *ctx;
} qjs_hash_ctx_t;

/* ---------- HMAC 上下文 ---------- */
typedef struct {
    const EVP_MD  *md;
    HMAC_CTX      *ctx;
    int            finalized;
} qjs_hmac_ctx_t;

/* ---------- 支持的算法列表 ---------- */
static const struct {
    const char      *name;
    const EVP_MD    *(*fn)(void);
} qjs_hash_algorithms[] = {
    { "md5",    EVP_md5 },
    { "sha1",   EVP_sha1 },
    { "sha224", EVP_sha224 },
    { "sha256", EVP_sha256 },
    { "sha384", EVP_sha384 },
    { "sha512", EVP_sha512 },
    { NULL, NULL },
};

static const EVP_MD *
qjs_hash_lookup(const char *name, size_t len)
{
    int i;
    for (i = 0; qjs_hash_algorithms[i].name != NULL; i++) {
        if (strlen(qjs_hash_algorithms[i].name) == len
            && qjs_strncasecmp((const u_char *)qjs_hash_algorithms[i].name,
                               (const u_char *)name, len) == 0)
        {
            return qjs_hash_algorithms[i].fn();
        }
    }
    return NULL;
}

/* 抽取：统一的格式化输出函数 */
static JSValue
qjs_crypto_format_result(JSContext *ctx, const u_char *result, unsigned int md_len, int argc, JSValueConst *argv)
{
    JSValue ret;
    qjs_str_t src = { .length = md_len, .start = (u_char *)result };

    if (argc > 0 && JS_IsString(argv[0])) {
        const char *enc;
        size_t enc_len;

        enc = JS_ToCStringLen(ctx, &enc_len, argv[0]);
        if (!enc) return JS_EXCEPTION;

        if (enc_len == 6 && memcmp(enc, "base64", 6) == 0) {
            ret = qjs_string_base64(ctx, &src);
        } else if (enc_len == 9 && memcmp(enc, "base64url", 9) == 0) {
            ret = qjs_string_base64url(ctx, &src);
        } else {
            ret = qjs_string_hex(ctx, &src);   /* 默认 hex */
        }
        JS_FreeCString(ctx, enc);
        return ret;
    }

    /* 修复原代码致命BUG：必须分配堆内存给 ArrayBuffer */
    u_char *buf = js_malloc(ctx, md_len);
    if (!buf) {
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(buf, result, md_len);
    ret = qjs_new_array_buffer(ctx, buf, md_len);
    if (JS_IsException(ret)) {
        js_free(ctx, buf);
    }
    return ret;
}

/* ---------- Hash 实现 ---------- */
static void
qjs_hash_finalizer(JSRuntime *rt, JSValue val)
{
    qjs_hash_ctx_t *hctx = JS_GetOpaque(val, QJS_CORE_CLASS_CRYPTO_HASH);
    if (hctx) {
        if (hctx->ctx) {
            EVP_MD_CTX_free(hctx->ctx);
        }
        js_free_rt(rt, hctx);
    }
}

static JSValue
qjs_hash_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    size_t name_len;
    const EVP_MD *md;
    qjs_hash_ctx_t *hctx;
    JSValue obj = JS_UNDEFINED;

    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "algorithm name expected");
    }

    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) return JS_EXCEPTION;

    md = qjs_hash_lookup(name, name_len);
    JS_FreeCString(ctx, name);
    if (!md) {
        return JS_ThrowTypeError(ctx, "unknown hash algorithm");
    }

    hctx = js_malloc(ctx, sizeof(qjs_hash_ctx_t));
    if (!hctx) return JS_ThrowOutOfMemory(ctx);
    
    hctx->md = md;
    hctx->ctx = EVP_MD_CTX_new();
    if (!hctx->ctx) {
        js_free(ctx, hctx);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    if (EVP_DigestInit_ex(hctx->ctx, md, NULL) != 1) {
        EVP_MD_CTX_free(hctx->ctx);
        js_free(ctx, hctx);
        return JS_ThrowTypeError(ctx, "EVP_DigestInit_ex failed");
    }

    /* Use JS_NewObjectClass to use the class prototype (with update/digest/copy methods).
     * JS_NewObjectProtoClass(JS_NULL, ...) would bypass the class prototype. */
    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_CRYPTO_HASH);
    if (JS_IsException(obj)) {
        EVP_MD_CTX_free(hctx->ctx);
        js_free(ctx, hctx);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, hctx);
    return obj;
}

static JSValue
qjs_hash_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hash_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HASH);
    qjs_bytes_t data;
    JSValue ret;

    if (!hctx) return JS_EXCEPTION;

    if (argc < 1 || qjs_to_bytes(ctx, &data, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "data expected");
    }

    if (EVP_DigestUpdate(hctx->ctx, data.start, data.length) != 1) {
        qjs_bytes_free(ctx, &data);
        return JS_ThrowTypeError(ctx, "EVP_DigestUpdate failed");
    }
    
    qjs_bytes_free(ctx, &data);
    ret = JS_DupValue(ctx, this_val);
    return ret;
}

static JSValue
qjs_hash_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hash_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HASH);
    u_char result[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;

    if (!hctx) return JS_EXCEPTION;

    if (EVP_DigestFinal_ex(hctx->ctx, result, &md_len) != 1) {
        return JS_ThrowTypeError(ctx, "EVP_DigestFinal_ex failed");
    }

    return qjs_crypto_format_result(ctx, result, md_len, argc, argv);
}

static JSValue
qjs_hash_copy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hash_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HASH);
    qjs_hash_ctx_t *new_hctx;
    JSValue obj;

    if (!hctx) return JS_EXCEPTION;

    new_hctx = js_malloc(ctx, sizeof(qjs_hash_ctx_t));
    if (!new_hctx) return JS_ThrowOutOfMemory(ctx);
    
    new_hctx->md = hctx->md;
    new_hctx->ctx = EVP_MD_CTX_new();
    if (!new_hctx->ctx) {
        js_free(ctx, new_hctx);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    if (EVP_MD_CTX_copy_ex(new_hctx->ctx, hctx->ctx) != 1) {
        EVP_MD_CTX_free(new_hctx->ctx);
        js_free(ctx, new_hctx);
        return JS_ThrowTypeError(ctx, "EVP_MD_CTX_copy_ex failed");
    }

    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_CRYPTO_HASH);
    if (JS_IsException(obj)) {
        EVP_MD_CTX_free(new_hctx->ctx);
        js_free(ctx, new_hctx);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, new_hctx);
    return obj;
}

/* ---------- HMAC 实现 ---------- */
static void
qjs_hmac_finalizer(JSRuntime *rt, JSValue val)
{
    qjs_hmac_ctx_t *hctx = JS_GetOpaque(val, QJS_CORE_CLASS_CRYPTO_HMAC);
    if (hctx) {
        if (hctx->ctx) {
            HMAC_CTX_free(hctx->ctx);
        }
        js_free_rt(rt, hctx);
    }
}

static JSValue
qjs_hmac_create(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *name;
    size_t name_len;
    const EVP_MD *md;
    qjs_bytes_t key;
    qjs_hmac_ctx_t *hctx;
    JSValue obj = JS_UNDEFINED;

    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "algorithm and key expected");
    }

    name = JS_ToCStringLen(ctx, &name_len, argv[0]);
    if (!name) return JS_EXCEPTION;
    
    md = qjs_hash_lookup(name, name_len);
    JS_FreeCString(ctx, name);
    if (!md) {
        return JS_ThrowTypeError(ctx, "unknown hash algorithm");
    }

    if (qjs_to_bytes(ctx, &key, argv[1]) != 0) {
        return JS_ThrowTypeError(ctx, "key must be a string or Buffer");
    }

    hctx = js_malloc(ctx, sizeof(qjs_hmac_ctx_t));
    if (!hctx) {
        qjs_bytes_free(ctx, &key);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    hctx->md = md;
    hctx->finalized = 0;
    hctx->ctx = HMAC_CTX_new();
    if (!hctx->ctx) {
        js_free(ctx, hctx);
        qjs_bytes_free(ctx, &key);
        return JS_ThrowOutOfMemory(ctx);
    }

    if (HMAC_Init_ex(hctx->ctx, key.start, key.length, md, NULL) != 1) {
        HMAC_CTX_free(hctx->ctx);
        js_free(ctx, hctx);
        qjs_bytes_free(ctx, &key);
        return JS_ThrowTypeError(ctx, "HMAC_Init_ex failed");
    }
    
    qjs_bytes_free(ctx, &key);

    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_CRYPTO_HMAC);
    if (JS_IsException(obj)) {
        HMAC_CTX_free(hctx->ctx);
        js_free(ctx, hctx);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, hctx);
    return obj;
}

static JSValue
qjs_hmac_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hmac_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HMAC);
    qjs_bytes_t data;
    JSValue ret;

    if (!hctx) return JS_EXCEPTION;
    if (hctx->finalized) {
        return JS_ThrowTypeError(ctx, "HMAC digest already called");
    }

    if (argc < 1 || qjs_to_bytes(ctx, &data, argv[0]) != 0) {
        return JS_ThrowTypeError(ctx, "data expected");
    }

    if (HMAC_Update(hctx->ctx, data.start, data.length) != 1) {
        qjs_bytes_free(ctx, &data);
        return JS_ThrowTypeError(ctx, "HMAC_Update failed");
    }
    
    qjs_bytes_free(ctx, &data);
    ret = JS_DupValue(ctx, this_val);
    return ret;
}

static JSValue
qjs_hmac_digest(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hmac_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HMAC);
    u_char result[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;

    if (!hctx) return JS_EXCEPTION;
    if (hctx->finalized) {
        return JS_ThrowTypeError(ctx, "HMAC digest already called");
    }
    hctx->finalized = 1;

    if (HMAC_Final(hctx->ctx, result, &md_len) != 1) {
        return JS_ThrowTypeError(ctx, "HMAC_Final failed");
    }

    return qjs_crypto_format_result(ctx, result, md_len, argc, argv);
}

static JSValue
qjs_hmac_copy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_hmac_ctx_t *hctx = JS_GetOpaque2(ctx, this_val, QJS_CORE_CLASS_CRYPTO_HMAC);
    qjs_hmac_ctx_t *new_hctx;
    JSValue obj;

    if (!hctx) return JS_EXCEPTION;
    if (hctx->finalized) {
        return JS_ThrowTypeError(ctx, "cannot copy after digest");
    }

    new_hctx = js_malloc(ctx, sizeof(qjs_hmac_ctx_t));
    if (!new_hctx) return JS_ThrowOutOfMemory(ctx);
    
    new_hctx->md = hctx->md;
    new_hctx->finalized = 0;
    new_hctx->ctx = HMAC_CTX_new();
    if (!new_hctx->ctx) {
        js_free(ctx, new_hctx);
        return JS_ThrowOutOfMemory(ctx);
    }
    
    if (HMAC_CTX_copy(new_hctx->ctx, hctx->ctx) != 1) {
        HMAC_CTX_free(new_hctx->ctx);
        js_free(ctx, new_hctx);
        return JS_ThrowTypeError(ctx, "HMAC_CTX_copy failed");
    }

    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_CRYPTO_HMAC);
    if (JS_IsException(obj)) {
        HMAC_CTX_free(new_hctx->ctx);
        js_free(ctx, new_hctx);
        return JS_EXCEPTION;
    }
    
    JS_SetOpaque(obj, new_hctx);
    return obj;
}

/* ---------- 快速哈希（consume） ---------- */
static JSValue
qjs_hash_consume(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue hash, ret;
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "algorithm name expected");
    }

    /* 复用 create 逻辑创建内部状态 */
    hash = qjs_hash_create(ctx, this_val, 1, argv);
    if (JS_IsException(hash)) return hash;

    /* 若传入 data，则先 update */
    if (argc > 1) {
        JSValue update_ret = qjs_hash_update(ctx, hash, 1, &argv[1]);
        if (JS_IsException(update_ret)) {
            JS_FreeValue(ctx, hash);
            return update_ret;
        }
        /* update 返回的是 dup 的 this_val，需释放 */
        JS_FreeValue(ctx, update_ret); 
    }

    /* 执行 digest 输出，如果有编码参数则透传第3个参数 */
    if (argc > 2) {
        ret = qjs_hash_digest(ctx, hash, 1, &argv[2]);
    } else {
        ret = qjs_hash_digest(ctx, hash, 0, NULL);
    }
    
    JS_FreeValue(ctx, hash);
    return ret;
}

/* ---------- 原型方法表 ---------- */
static const JSCFunctionListEntry qjs_hash_proto_funcs[] = {
    JS_CFUNC_DEF("update", 1, qjs_hash_update),
    JS_CFUNC_DEF("digest", 1, qjs_hash_digest),
    JS_CFUNC_DEF("copy",   0, qjs_hash_copy),
};

static const JSCFunctionListEntry qjs_hmac_proto_funcs[] = {
    JS_CFUNC_DEF("update", 1, qjs_hmac_update),
    JS_CFUNC_DEF("digest", 1, qjs_hmac_digest),
    JS_CFUNC_DEF("copy",   0, qjs_hmac_copy),
};

/* ---------- 类定义 ---------- */
static JSClassDef qjs_hash_class = {
    "Hash",
    .finalizer = qjs_hash_finalizer,
};

static JSClassDef qjs_hmac_class = {
    "Hmac",
    .finalizer = qjs_hmac_finalizer,
};

/* 内部辅助：注册类与原型 */
static int qjs_register_crypto_classes(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSValue proto;

    if (!JS_IsRegisteredClass(rt, QJS_CORE_CLASS_CRYPTO_HASH)) {
        if (JS_NewClass(rt, QJS_CORE_CLASS_CRYPTO_HASH, &qjs_hash_class) < 0)
            return -1;
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, qjs_hash_proto_funcs,
                                   qjs_nitems(qjs_hash_proto_funcs));
        JS_SetClassProto(ctx, QJS_CORE_CLASS_CRYPTO_HASH, proto);
    }

    if (!JS_IsRegisteredClass(rt, QJS_CORE_CLASS_CRYPTO_HMAC)) {
        if (JS_NewClass(rt, QJS_CORE_CLASS_CRYPTO_HMAC, &qjs_hmac_class) < 0)
            return -1;
        proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, qjs_hmac_proto_funcs,
                                   qjs_nitems(qjs_hmac_proto_funcs));
        JS_SetClassProto(ctx, QJS_CORE_CLASS_CRYPTO_HMAC, proto);
    }
    return 0;
}

/* ---------- 模块初始化 ---------- */
int
qjs_crypto_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue obj;
    int i;

    if (qjs_register_crypto_classes(ctx) < 0) {
        return -1;
    }

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return -1;

    JS_SetPropertyStr(ctx, obj, "createHash", JS_NewCFunction(ctx, qjs_hash_create, "createHash", 1));
    JS_SetPropertyStr(ctx, obj, "createHmac", JS_NewCFunction(ctx, qjs_hmac_create, "createHmac", 2));
    JS_SetPropertyStr(ctx, obj, "create", JS_NewCFunction(ctx, qjs_hash_create, "create", 1));
    JS_SetPropertyStr(ctx, obj, "consume", JS_NewCFunction(ctx, qjs_hash_consume, "consume", 3));

    for (i = 0; qjs_hash_algorithms[i].name; i++) {
        JS_SetPropertyStr(ctx, obj, qjs_hash_algorithms[i].name,
                          JS_NewString(ctx, qjs_hash_algorithms[i].name));
    }

    return JS_SetModuleExport(ctx, m, "default", obj);
}

/* 挂载到全局 crypto 对象 */
int qjs_crypto_install_global(JSContext *ctx, JSValueConst global)
{
    JSValue crypto_obj;

    if (qjs_register_crypto_classes(ctx) < 0) {
        return -1;
    }

    crypto_obj = JS_NewObject(ctx);
    if (JS_IsException(crypto_obj)) return -1;
    
    JS_SetPropertyStr(ctx, crypto_obj, "createHash",
                      JS_NewCFunction(ctx, qjs_hash_create, "createHash", 1));
    JS_SetPropertyStr(ctx, crypto_obj, "createHmac",
                      JS_NewCFunction(ctx, qjs_hmac_create, "createHmac", 2));

    if (JS_DefinePropertyValueStr(ctx, global, "crypto",
                                  crypto_obj,
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0)
        return -1;
    return 0;
}
