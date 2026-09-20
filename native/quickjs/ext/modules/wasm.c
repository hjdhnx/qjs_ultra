#include "qjs_native.h"
#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

/* 引擎级 wasm3 环境（存于 rt->ext_data，引擎层创建、JS_FreeRuntime 销毁）。
 * 含懒创建兜底：正常路径下安装时即已创建。 */
static IM3Environment qjs_wasm_env(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    IM3Environment env = (IM3Environment)qjs_ext_get_data(rt);
    if (!env) {
        env = m3_NewEnvironment();
        qjs_ext_set_data(rt, env);
    }
    return env;
}


/* --- 常量定义 --- */
#define QJS_WASM_MAX_ARGS 1024
#define QJS_WASM_MAX_RETS 1024
#define QJS_WASM_DEFAULT_STACK_SIZE (512 * 1024)

static M3Result qjs_wasm_js_error = "JS error was thrown";

/* --- 辅助宏 --- */
#define OPAQUE_EXCEPTION() do { \
    JS_FreeValue(ctx, opaque); \
    return JS_EXCEPTION; \
} while(0)

#define INSTANCE_CLEANUP() do { \
    JS_FreeValue(ctx, exports_array); \
    JS_FreeValue(ctx, opaque); \
} while(0)

#define INSTANCE_EXCEPTION() do { \
    INSTANCE_CLEANUP(); \
    return JS_EXCEPTION; \
} while(0)

#define INSTANCE_ERROR_THROW(msg) do { \
    INSTANCE_CLEANUP(); \
    JS_ThrowInternalError(ctx, msg); \
    return JS_EXCEPTION; \
} while(0)


/* --- 类型转换辅助函数 --- */

// https://webassembly.github.io/spec/js-api/index.html#towebassemblyvalue
static int nx__wasm_towebassemblyvalue(JSContext *ctx, JSValueConst val, M3ValueType type, void *stack)
{
    int r = 0;
    switch (type) {
        case c_m3Type_i32: {
            r = JS_ToInt32(ctx, (int32_t *)stack, val);
            break;
        };
        case c_m3Type_i64: {
            if (JS_IsBigInt(ctx, val)) {
                r = JS_ToBigInt64(ctx, (int64_t *)stack, val);
            } else {
                double d;
                r = JS_ToFloat64(ctx, &d, val);
                if (r == 0) {
                    *(int64_t *)stack = (int64_t)d;
                }
            }
            break;
        };
        case c_m3Type_f32: {
            double d;
            r = JS_ToFloat64(ctx, &d, val);
            if (r == 0) {
                *(float *)stack = (float)d;
            }
            break;
        };
        case c_m3Type_f64: {
            r = JS_ToFloat64(ctx, (double *)stack, val);
            break;
        };
        case c_m3Type_none:
        case c_m3Type_unknown: {
            break;
        }
    }
    return r;
}

// https://webassembly.github.io/spec/js-api/index.html#tojsvalue
static JSValue nx__wasm_tojsvalue(JSContext *ctx, M3ValueType type, const void *stack)
{
    switch (type) {
        case c_m3Type_i32: {
            int32_t val = *(int32_t *)stack;
            return JS_NewInt32(ctx, val);
        }
        case c_m3Type_i64: {
            int64_t val = *(int64_t *)stack;
            if (val == (int32_t)val)
                return JS_NewInt32(ctx, (int32_t)val);
            else
                return JS_NewBigInt64(ctx, val);
        }
        case c_m3Type_f32: {
            float val = *(float *)stack;
            return JS_NewFloat64(ctx, (double)val);
        }
        case c_m3Type_f64: {
            double val = *(double *)stack;
            return JS_NewFloat64(ctx, val);
        }
        default:
            return JS_UNDEFINED;
    }
}

JSValue nx_throw_wasm_error(JSContext *ctx, const char *name, M3Result r)
{
    JSValue obj = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, obj, "message", JS_NewString(ctx, r), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, obj, "wasmError", JS_NewString(ctx, name), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (JS_IsException(obj))
        obj = JS_NULL;
    return JS_Throw(ctx, obj);
}

static char qjs_wasm_type_to_char(M3ValueType type)
{
    switch (type) {
        case c_m3Type_none: return 'v';
        case c_m3Type_i32: return 'i';
        case c_m3Type_i64: return 'I';
        case c_m3Type_f32: return 'f';
        case c_m3Type_f64: return 'F';
        default: return 0;
    }
}

static void qjs_wasm_create_signature(IM3Function func, char *sig, size_t sig_size)
{
    u32 argc = m3_GetArgCount(func);
    u32 retc = m3_GetRetCount(func);
    size_t pos = 0;
    for (u32 i = 0; i < retc && pos < sig_size - 1; ++i) {
        sig[pos++] = qjs_wasm_type_to_char(m3_GetRetType(func, i));
    }
    if (pos < sig_size - 1) {
        sig[pos++] = '(';
    }
    for (u32 i = 0; i < argc && pos < sig_size - 1; ++i) {
        sig[pos++] = qjs_wasm_type_to_char(m3_GetArgType(func, i));
    }
    if (pos < sig_size - 1) {
        sig[pos++] = ')';
    }
    sig[pos] = '\0';
}

/* --- 类定义与 ID --- */

typedef struct {
    IM3Memory mem;
    IM3Runtime runtime;
    bool needs_free;
    int is_shared;
    JSValue cached_buffer;      // Cached ArrayBuffer for .buffer getter
    size_t cached_buffer_len;   // Length when cached (invalidate on grow)
} qjs_wasm_memory_t;

static qjs_wasm_memory_t *qjs_wasm_memory_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_WASM_MEMORY);
}

static void finalizer_wasm_memory(JSRuntime *rt, JSValue val)
{
    qjs_wasm_memory_t *data = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_MEMORY);
    if (data) {
        if (!JS_IsUndefined(data->cached_buffer)) {
            JS_FreeValueRT(rt, data->cached_buffer);
        }
        if (data->needs_free) {
            if (data->mem) {
                m3_FreeMemory(data->mem);
            }
            if (data->runtime) {
                m3_FreeRuntime(data->runtime);
            }
        }
        js_free_rt(rt, data);
    }
}

/* GC 标记阶段：追踪 cached_buffer JSValue 引用，防止 GC 过早回收 */
static void gc_mark_wasm_memory(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    qjs_wasm_memory_t *data = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_MEMORY);
    if (data && !JS_IsUndefined(data->cached_buffer)) {
        JS_MarkValue(rt, data->cached_buffer, mark_func);
    }
}

static JSValue qjs_wasm_memory_new_(JSContext *ctx)
{
    qjs_wasm_memory_t *data = js_mallocz(ctx, sizeof(qjs_wasm_memory_t));
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    data->cached_buffer = JS_UNDEFINED;
    data->cached_buffer_len = 0;
    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_MEMORY);
    if (JS_IsException(obj)) {
        js_free(ctx, data);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

typedef struct {
    IM3Table table;
    u32 *table_size;
    bool needs_free;
} qjs_wasm_table_t;

static qjs_wasm_table_t *qjs_wasm_table_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_WASM_TABLE);
}

static void finalizer_wasm_table(JSRuntime *rt, JSValue val)
{
    qjs_wasm_table_t *data = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_TABLE);
    if (data) {
        if (data->needs_free && data->table) {
            m3_FreeTable(data->table);
        }
        js_free_rt(rt, data);
    }
}

static JSValue qjs_wasm_table_new_(JSContext *ctx)
{
    qjs_wasm_table_t *data = js_mallocz(ctx, sizeof(qjs_wasm_table_t));
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_TABLE);
    if (JS_IsException(obj)) {
        js_free(ctx, data);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

typedef struct {
    IM3Function function;
    /* 持有对 instance 的引用，防止 instance 被 GC 回收后 module 被释放，
     * 导致 function 指针变成悬垂指针（use-after-free → "function body is missing"）。
     * JS_UNDEFINED 表示不持有 instance 引用（如从 table.get 创建的函数）。 */
    JSValue instance;
} qjs_wasm_exported_func_t;

static qjs_wasm_exported_func_t *qjs_wasm_exported_func_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_WASM_FUNC);
}

static void finalizer_wasm_exported_func(JSRuntime *rt, JSValue val)
{
    qjs_wasm_exported_func_t *data = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_FUNC);
    if (data) {
        if (!JS_IsUndefined(data->instance)) {
            JS_FreeValueRT(rt, data->instance);
        }
        js_free_rt(rt, data);
    }
}

/* GC 标记：追踪 instance 引用，防止 instance 被 GC 过早回收 */
static void gc_mark_wasm_exported_func(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    qjs_wasm_exported_func_t *data = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_FUNC);
    if (data && !JS_IsUndefined(data->instance)) {
        JS_MarkValue(rt, data->instance, mark_func);
    }
}

static JSValue qjs_wasm_exported_func_new(JSContext *ctx, IM3Function func)
{
    qjs_wasm_exported_func_t *data = js_mallocz(ctx, sizeof(qjs_wasm_exported_func_t));
    if (!data) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    data->function = func;
    data->instance = JS_UNDEFINED;  /* 默认不持有 instance 引用 */
    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_FUNC);
    if (JS_IsException(obj)) {
        js_free(ctx, data);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, data);
    return obj;
}

typedef struct {
    IM3Module module;
    uint8_t *data;
    size_t size;
    JSValue js_buffer;  /* 持有对原始 ArrayBuffer 的引用，防止 GC 回收后 data 悬垂 */
} qjs_wasm_module_t;

static qjs_wasm_module_t *qjs_wasm_module_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_WASM_MODULE);
}

static void finalizer_wasm_module(JSRuntime *rt, JSValue val)
{
    qjs_wasm_module_t *m = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_MODULE);
    if (m) {
        if (m->module) m3_FreeModule(m->module);
        /* 释放对原始 JS ArrayBuffer 的引用 */
        if (!JS_IsUndefined(m->js_buffer)) {
            JS_FreeValueRT(rt, m->js_buffer);
        }
        js_free_rt(rt, m);
    }
}

/* GC 标记阶段：追踪 js_buffer JSValue 引用，防止 GC 过早回收原始 ArrayBuffer
 * 导致 m->data 变成悬垂指针（use-after-free） */
static void gc_mark_wasm_module(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    qjs_wasm_module_t *m = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_MODULE);
    if (m && !JS_IsUndefined(m->js_buffer)) {
        JS_MarkValue(rt, m->js_buffer, mark_func);
    }
}

typedef struct {
    IM3Global global;
} qjs_wasm_global_t;

static qjs_wasm_global_t *qjs_wasm_global_get(JSContext *ctx, JSValueConst obj)
{
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_WASM_GLOBAL);
}

static void finalizer_wasm_global(JSRuntime *rt, JSValue val)
{
    qjs_wasm_global_t *g = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_GLOBAL);
    if (g) {
        // Don't need to free `global` since the Runtime instance owns it
        g->global = NULL;
        js_free_rt(rt, g);
    }
}

static JSValue qjs_wasm_new_global(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_global_t *g = js_mallocz(ctx, sizeof(qjs_wasm_global_t));
    if (!g) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_GLOBAL);
    if (JS_IsException(obj)) {
        js_free(ctx, g);
        return JS_EXCEPTION;
    }
    JS_SetOpaque(obj, g);
    g->global = NULL;
    return obj;
}

static JSValue qjs_wasm_global_value_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_global_t *g = qjs_wasm_global_get(ctx, argv[0]);
    if (!g) return JS_EXCEPTION;

    IM3Global global = g->global;
    if (!global) {
        // Not bound
        return JS_ThrowTypeError(ctx, "Global not defined");
    }

    M3TaggedValue val;
    M3Result r = m3_GetGlobal(global, &val);
    if (r) {
        return nx_throw_wasm_error(ctx, "LinkError", r);
    }
    return nx__wasm_tojsvalue(ctx, val.type, &val.value);
}

static JSValue qjs_wasm_global_value_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_global_t *g = qjs_wasm_global_get(ctx, argv[0]);
    if (!g) return JS_EXCEPTION;

    IM3Global global = g->global;
    if (!global) {
        return JS_ThrowTypeError(ctx, "Global not defined");
    }

    M3TaggedValue val;
    val.type = global->type;
    if (nx__wasm_towebassemblyvalue(ctx, argv[1], global->type, &val.value))
        return JS_EXCEPTION;

    m3_SetGlobal(global, &val);
    return JS_UNDEFINED;
}

typedef struct {
    JSContext *ctx;
    JSValue func;
} qjs_wasm_imported_func_t;

typedef struct {
    IM3Runtime runtime;
    IM3Module module;
    IM3Memory memory;
    bool loaded;
    qjs_wasm_imported_func_t **imported_funcs;
    size_t num_imported_funcs;
    JSValue imported_memory;
    JSValue imported_table;
    JSValue *imported_globals;
    size_t num_imported_globals;
} qjs_wasm_instance_t;

static void finalizer_wasm_instance(JSRuntime *rt, JSValue val)
{
    qjs_wasm_instance_t *i = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_INSTANCE);
    if (i) {
        if (i->imported_funcs) {
            for (size_t j = 0; j < i->num_imported_funcs; j++) {
                qjs_wasm_imported_func_t *imported = i->imported_funcs[j];
                if (imported) {
                    JS_FreeValueRT(rt, imported->func);
                    js_free_rt(rt, imported);
                }
            }
            js_free_rt(rt, i->imported_funcs);
        }
        if (!JS_IsUndefined(i->imported_memory)) {
            JS_FreeValueRT(rt, i->imported_memory);
        }
        if (!JS_IsUndefined(i->imported_table)) {
            JS_FreeValueRT(rt, i->imported_table);
        }
        if (i->imported_globals) {
            for (size_t j = 0; j < i->num_imported_globals; j++) {
                if (!JS_IsUndefined(i->imported_globals[j])) {
                    JS_FreeValueRT(rt, i->imported_globals[j]);
                }
            }
            js_free_rt(rt, i->imported_globals);
        }
        if (i->runtime) {
            m3_FreeRuntime(i->runtime);
        } else if (i->module && !i->loaded) {
            m3_FreeModule(i->module);
        }
        js_free_rt(rt, i);
    }
}

/* GC 标记阶段：追踪所有持有的 JSValue 引用（导入的函数/Memory/Table/Global），
 * 防止 GC 过早回收这些对象，导致 WASM 调用时 use-after-free */
static void gc_mark_wasm_instance(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    qjs_wasm_instance_t *i = JS_GetOpaque(val, QJS_CORE_CLASS_ID_WASM_INSTANCE);
    if (!i) return;

    /* 追踪导入的 JS 函数 */
    if (i->imported_funcs) {
        for (size_t j = 0; j < i->num_imported_funcs; j++) {
            qjs_wasm_imported_func_t *imported = i->imported_funcs[j];
            if (imported && !JS_IsUndefined(imported->func)) {
                JS_MarkValue(rt, imported->func, mark_func);
            }
        }
    }
    /* 追踪导入的 Memory 对象 */
    if (!JS_IsUndefined(i->imported_memory)) {
        JS_MarkValue(rt, i->imported_memory, mark_func);
    }
    /* 追踪导入的 Table 对象 */
    if (!JS_IsUndefined(i->imported_table)) {
        JS_MarkValue(rt, i->imported_table, mark_func);
    }
    /* 追踪导入的 Global 对象数组 */
    if (i->imported_globals) {
        for (size_t j = 0; j < i->num_imported_globals; j++) {
            if (!JS_IsUndefined(i->imported_globals[j])) {
                JS_MarkValue(rt, i->imported_globals[j], mark_func);
            }
        }
    }
}

static JSValue qjs_wasm_new_module(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    IM3Environment env = qjs_wasm_env(ctx);
    if (!env) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }


    JSValue obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_MODULE);
    if (JS_IsException(obj)) {
        return obj;
    }

    qjs_wasm_module_t *m = js_mallocz(ctx, sizeof(qjs_wasm_module_t));
    if (!m) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    m->js_buffer = JS_UNDEFINED;  /* 初始化 */
    JS_SetOpaque(obj, m);

    /* 持有对原始 ArrayBuffer 的 JS 引用，防止 GC 回收后 m->data 变成悬垂指针 */
    JSValue ab_holder = JS_UNDEFINED;
    if (JS_IsTypedArray(argv[0])) {
        ab_holder = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, NULL, NULL);
    } else if (JS_IsDataView(argv[0])) {
        ab_holder = JS_GetDataViewBuffer(ctx, argv[0], NULL, NULL, NULL);
    } else if (JS_IsArrayBuffer(argv[0])) {
        ab_holder = JS_DupValue(ctx, argv[0]);
    }

    /* 使用 ab_holder（底层 ArrayBuffer）而非 argv[0]（可能是 TypedArray/DataView）
     * 来获取缓冲区指针和大小。JS_GetArrayBuffer 只接受 ArrayBuffer 类型，
     * 对 TypedArray 会抛出 "ArrayBuffer object expected" 错误。 */
    size_t size;
    uint8_t *buf = NULL;
    if (!JS_IsException(ab_holder) && !JS_IsUndefined(ab_holder)) {
        buf = JS_GetArrayBuffer(ctx, &size, ab_holder);
    }
    if (!buf) {
        JS_FreeValue(ctx, obj);  /* 触发 finalizer 释放 m */
        if (!JS_IsException(ab_holder)) JS_FreeValue(ctx, ab_holder);
        return JS_EXCEPTION;
    }

    M3Result r = m3_ParseModule(env, &m->module, buf, size);
    if (r) {
        JS_FreeValue(ctx, obj);  /* 触发 finalizer 释放 m */
        if (!JS_IsException(ab_holder)) JS_FreeValue(ctx, ab_holder);
        return nx_throw_wasm_error(ctx, "CompileError", r);
    }
    m->data = buf;
    m->size = size;
    m->js_buffer = ab_holder;  /* 持有引用，finalizer 中释放 */

    return obj;
}

m3ApiRawFunction(qjs_wasm_imported_func)
{
    IM3Function func = _ctx->function;
    IM3FuncType funcType = func ? func->funcType : NULL;
    qjs_wasm_imported_func_t *js = _ctx->userdata;

    if (!js || !js->ctx || !funcType) {
        if (js && js->ctx) {
            JS_ThrowInternalError(js->ctx, "WASM function callback not properly initialized");
        }
        return qjs_wasm_js_error;
    }

    uint64_t *retValAddr = _sp;
    _sp += funcType->numRets;
    int numArgs = funcType->numArgs;

    JSValue *args = NULL;
    if (numArgs > 0) {
        if (numArgs > QJS_WASM_MAX_ARGS) {
            return m3Err_mallocFailed;
        }
        args = js_malloc(js->ctx, numArgs * sizeof(JSValue));
        if (!args) {
            return m3Err_mallocFailed;
        }
        for (int i = 0; i < numArgs; i++) {
            u8 type = funcType->types[funcType->numRets + i];
            args[i] = nx__wasm_tojsvalue(js->ctx, type, _sp);
            _sp++;
        }
    }

    JSValue ret_val = JS_Call(js->ctx, js->func, JS_NULL, numArgs, args);

    if (args) {
        for (int i = 0; i < numArgs; i++) {
            JS_FreeValue(js->ctx, args[i]);
        }
        js_free(js->ctx, args);
    }

    if (JS_IsException(ret_val)) {
        JS_FreeValue(js->ctx, ret_val);
        return qjs_wasm_js_error;
    }

    if (funcType->numRets > 0) {
        if (nx__wasm_towebassemblyvalue(js->ctx, ret_val, funcType->types[0], retValAddr)) {
            JS_FreeValue(js->ctx, ret_val);
            return qjs_wasm_js_error;
        }
    }

    JS_FreeValue(js->ctx, ret_val);
    m3ApiSuccess();
}

static JSValue find_matching_import(JSContext *ctx, M3ImportInfo *info, JSValue imports_array, size_t imports_array_length)
{
    for (size_t i = 0; i < imports_array_length; i++) {
        JSValue entry = JS_GetPropertyUint32(ctx, imports_array, i);
        JSValue module_val = JS_GetPropertyStr(ctx, entry, "module");
        const char *module_name = !JS_IsUndefined(module_val) ? JS_ToCString(ctx, module_val) : NULL;
        JS_FreeValue(ctx, module_val);

        if (!module_name || strcmp(info->moduleUtf8, module_name) != 0) {
            if (module_name) JS_FreeCString(ctx, module_name);
            JS_FreeValue(ctx, entry);
            continue;
        }
        JS_FreeCString(ctx, module_name);

        JSValue name_val = JS_GetPropertyStr(ctx, entry, "name");
        const char *field_name = !JS_IsUndefined(name_val) ? JS_ToCString(ctx, name_val) : NULL;
        JS_FreeValue(ctx, name_val);

        if (!field_name || strcmp(info->fieldUtf8, field_name) != 0) {
            if (field_name) JS_FreeCString(ctx, field_name);
            JS_FreeValue(ctx, entry);
            continue;
        }
        JS_FreeCString(ctx, field_name);

        return entry;
    }
    return JS_UNDEFINED;
}

static JSValue qjs_wasm_new_instance(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    IM3Environment env = qjs_wasm_env(ctx);
    if (!env) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }

    JSValue opaque = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_WASM_INSTANCE);
    if (JS_IsException(opaque)) {
        return opaque;
    }

    qjs_wasm_instance_t *instance = js_mallocz(ctx, sizeof(qjs_wasm_instance_t));
    if (!instance) {
        JS_FreeValue(ctx, opaque);
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    instance->imported_memory = JS_UNDEFINED;
    instance->imported_table = JS_UNDEFINED;
    JS_SetOpaque(opaque, instance);

    qjs_wasm_module_t *m = qjs_wasm_module_get(ctx, argv[0]);
    if (!m || !m->module || !m->data) {
        JS_FreeValue(ctx, opaque);
        JS_ThrowTypeError(ctx, "Invalid or uninitialized module");
        return JS_EXCEPTION;
    }


    M3Result r = m3_ParseModule(env, &instance->module, m->data, m->size);
    if (r) {
        JS_FreeValue(ctx, opaque);
        return nx_throw_wasm_error(ctx, "CompileError", r);
    }

    if (!instance->module) {
        JS_FreeValue(ctx, opaque);
        JS_ThrowTypeError(ctx, "Failed to parse module");
        return JS_EXCEPTION;
    }

    /* Create a runtime per module to avoid symbol clash. */
    IM3Runtime runtime = NULL;
    JSValue imports_array = argv[1];
    uint32_t imports_array_length;
    JSValue imports_len_val = JS_GetPropertyStr(ctx, imports_array, "length");
    if (JS_ToUint32(ctx, &imports_array_length, imports_len_val)) {
        JS_FreeValue(ctx, imports_len_val);
        OPAQUE_EXCEPTION();
    }
    JS_FreeValue(ctx, imports_len_val);

    /* When the WASM module declares the memory as an import, we need to "map" the provided `WebAssembly.Memory` data into the runtime here, before loading the module. */
    if (instance->module->memoryImportInfo) {
        M3ImportInfo *import = instance->module->memoryImportInfo;
        JSValue matching_import = find_matching_import(
            ctx, import, imports_array, imports_array_length);
        if (JS_IsUndefined(matching_import)) {
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Missing import memory \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, matching_import, "val");
        qjs_wasm_memory_t *data = qjs_wasm_memory_get(ctx, v);
        if (!data) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Invalid memory import for \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        if (!data->mem) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Memory not initialized for \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        if (!data->mem->mallocated) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Memory not allocated for \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        instance->memory = data->mem;
        runtime = data->runtime;
        M3Result link_r = m3_LinkMemory(instance->module, data->mem);
        if (link_r) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            return nx_throw_wasm_error(ctx, "LinkError", link_r);
        }
        data->needs_free = false;
        instance->imported_memory = JS_DupValue(ctx, v);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, matching_import);
    }

    if (instance->module->tableImportInfo) {
        M3ImportInfo *import = instance->module->tableImportInfo;
        JSValue matching_import = find_matching_import(
            ctx, import, imports_array, imports_array_length);
        if (JS_IsUndefined(matching_import)) {
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Missing import table \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        JSValue v = JS_GetPropertyStr(ctx, matching_import, "val");
        qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, v);
        if (!data) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Invalid table import for \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }
        if (!data->table) {
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            JS_ThrowTypeError(ctx, "Table not initialized for \"%s.%s\"", import->moduleUtf8, import->fieldUtf8);
            return JS_EXCEPTION;
        }

        M3Result r = m3_LinkTable(instance->module, data->table);
        if (r) {
            // Provide detailed error info for debugging
            M3TableInfo *imported = &data->table->info;
            M3TableInfo *expected = &instance->module->tableInfo;
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf),
                     "Table import mismatch for \"%s.%s\": "
                     "expected (initial=%u, max=%u, elem=0x%02x), "
                     "got (initial=%u, max=%u, elem=0x%02x)",
                     import->moduleUtf8, import->fieldUtf8,
                     expected->curSize, expected->maxSize, expected->elemTy,
                     imported->curSize, imported->maxSize, imported->elemTy);
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
            JS_FreeValue(ctx, opaque);
            return nx_throw_wasm_error(ctx, "LinkError", err_buf);
        }
        data->table_size = &instance->module->tableInfo.curSize;
        data->needs_free = false;
        instance->imported_table = JS_DupValue(ctx, v);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, matching_import);
    }

    // Process global imports BEFORE m3_LoadModule
    instance->imported_globals = js_mallocz(ctx, instance->module->numGlobals * sizeof(JSValue));
    if (!instance->imported_globals) {
        JS_FreeValue(ctx, opaque);
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    instance->num_imported_globals = instance->module->numGlobals;
    for (size_t i = 0; i < instance->module->numGlobals; i++) {
        instance->imported_globals[i] = JS_UNDEFINED;
    }

    for (size_t i = 0; i < instance->module->numGlobals; i++) {
        IM3Global g = &instance->module->globals[i];
        if (g->imported) {
            JSValue matching_import = find_matching_import(
                ctx, &g->import, imports_array, imports_array_length);
            if (JS_IsUndefined(matching_import)) {
                JS_FreeValue(ctx, opaque);
                JS_ThrowTypeError(ctx, "Missing import global \"%s.%s\"", g->import.moduleUtf8, g->import.fieldUtf8);
                return JS_EXCEPTION;
            }
            JSValue v = JS_GetPropertyStr(ctx, matching_import, "val");
            JSValue initial_value = JS_GetPropertyStr(ctx, matching_import, "i");
            JS_FreeValue(ctx, matching_import);

            M3TaggedValue val;
            val.type = g->type;
            if (nx__wasm_towebassemblyvalue(ctx, initial_value, g->type, &val.value)) {
                JS_FreeValue(ctx, initial_value);
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, opaque);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, initial_value);

            M3Result gr = m3_SetGlobal(g, &val);
            if (gr) {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, opaque);
                return nx_throw_wasm_error(ctx, "LinkError", gr);
            }
            if (g->name) {
                instance->imported_globals[i] = JS_DupValue(ctx, v);
            }
            JS_FreeValue(ctx, v);
        }
    }

    // 如果上面没有导入内存，则创建新的 Runtime
    // (对应没有 Memory Import 的普通WASM 模块)
    if (!runtime) {
        runtime = m3_NewRuntime(env, QJS_WASM_DEFAULT_STACK_SIZE, NULL);
        if (!runtime) {
            JS_FreeValue(ctx, opaque);
            JS_ThrowOutOfMemory(ctx);
            return JS_EXCEPTION;
        }
        runtime->memoryLimit = 0;
    }
    // 保存 runtime 到instance
    instance->runtime = runtime;

    r = m3_LoadModule(runtime, instance->module);
    if (r) {
        JS_FreeValue(ctx, opaque);
        return nx_throw_wasm_error(ctx, "LinkError", r);
    }

    r = m3_LinkLibC(instance->module);
    if (r) {
        JS_FreeValue(ctx, opaque);
        return nx_throw_wasm_error(ctx, "LinkError", r);
    }

    // Process function imports BEFORE CompileModule
    for (size_t i = 0; i < instance->module->numFunctions; ++i) {
        IM3Function f = &instance->module->functions[i];
        if (f->import.moduleUtf8 && f->import.fieldUtf8) {
            JSValue matching_import = find_matching_import(
                ctx, &f->import, imports_array, imports_array_length);
            if (JS_IsUndefined(matching_import)) {
                JS_FreeValue(ctx, opaque);
                JS_ThrowTypeError(ctx, "Missing import function \"%s.%s\"", f->import.moduleUtf8, f->import.fieldUtf8);
                return JS_EXCEPTION;
            }

            // 添加类型检查
            JSValue kind_val = JS_GetPropertyStr(ctx, matching_import, "kind");
            const char *kind_str = !JS_IsUndefined(kind_val) ? JS_ToCString(ctx, kind_val) : NULL;
            JS_FreeValue(ctx, kind_val);
            if (!kind_str || strcmp(kind_str, "function") != 0) {
                JS_ThrowTypeError(ctx, "Import \"%s.%s\" has kind '%s', expected 'function'",
                         f->import.moduleUtf8, f->import.fieldUtf8, kind_str ? kind_str : "unknown");
                if (kind_str) JS_FreeCString(ctx, kind_str);
                JS_FreeValue(ctx, matching_import);
                JS_FreeValue(ctx, opaque);
                return JS_EXCEPTION;
            }
            JS_FreeCString(ctx, kind_str);

            JSValue v = JS_GetPropertyStr(ctx, matching_import, "val");
            if (JS_IsFunction(ctx, v)) {
                // 处理函数导入
                qjs_wasm_imported_func_t *js = js_malloc(ctx, sizeof(qjs_wasm_imported_func_t));
                if (!js) {
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, matching_import);
                    JS_ThrowOutOfMemory(ctx);
                    JS_FreeValue(ctx, opaque);
                    return JS_EXCEPTION;
                }
                js->ctx = ctx;
                js->func = JS_DupValue(ctx, v);

                // Create function signature
                char sig[64];
                qjs_wasm_create_signature(f, sig, sizeof(sig));
                M3Result link_r = m3_LinkRawFunctionEx(
                    instance->module, f->import.moduleUtf8, f->import.fieldUtf8, sig, qjs_wasm_imported_func, js);
                if (link_r) {
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, matching_import);
                    JS_FreeValue(ctx, js->func);
                    js_free(ctx, js);
                    JS_FreeValue(ctx, opaque);
                    JS_ThrowTypeError(ctx, "Failed to link function \"%s.%s\": %s",
                        f->import.moduleUtf8, f->import.fieldUtf8, link_r);
                    return JS_EXCEPTION;
                }

                // 存储以便后续清理
                qjs_wasm_imported_func_t **new_arr = js_realloc(
                    ctx, instance->imported_funcs,
                    (instance->num_imported_funcs + 1) * sizeof(qjs_wasm_imported_func_t*));
                if (!new_arr) {
                    JS_FreeValue(ctx, v);
                    JS_FreeValue(ctx, matching_import);
                    JS_ThrowOutOfMemory(ctx);
                    JS_FreeValue(ctx, opaque);
                    return JS_EXCEPTION;
                }
                instance->imported_funcs = new_arr;
                instance->imported_funcs[instance->num_imported_funcs++] = js;
            } else {
                JS_FreeValue(ctx, v);
                JS_FreeValue(ctx, matching_import);
                JS_FreeValue(ctx, opaque);
                JS_ThrowTypeError(ctx, "Import \"%s.%s\" is not a function", f->import.moduleUtf8, f->import.fieldUtf8);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, v);
            JS_FreeValue(ctx, matching_import);
        }
    }

    r = m3_CompileModule(instance->module);
    if (r) {
        JS_FreeValue(ctx, opaque);
        return nx_throw_wasm_error(ctx, "CompileError", r);
    }

    r = m3_RunStart(instance->module);
    if (r) {
        JS_FreeValue(ctx, opaque);
        return nx_throw_wasm_error(ctx, "StartError", r);
    }

    // Process the provided "imports" into the runtime,
    // instantiate the defined "exports" from the runtime
    JSValue exports_array = JS_NewArray(ctx);
    size_t exports_index = 0;

    for (size_t i = 0; i < instance->module->numFunctions; ++i) {
        IM3Function f = &instance->module->functions[i];
        if (f->import.moduleUtf8 && f->import.fieldUtf8) continue;
        if (f->numNames == 0 || !f->names[0]) continue;

        JSValue val = qjs_wasm_exported_func_new(ctx, f);
        if (JS_IsException(val)) INSTANCE_EXCEPTION();
        /* 让 exported func 持有 instance 的引用，防止 instance 被 GC 回收后
         * module 被释放导致 function 指针悬垂（use-after-free） */
        qjs_wasm_exported_func_t *efunc = qjs_wasm_exported_func_get(ctx, val);
        if (efunc) {
            efunc->instance = JS_DupValue(ctx, opaque);
        }
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "function"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, f->names[0]), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "val", val, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports_array, exports_index++, item, JS_PROP_C_W_E);
    }

    for (size_t i = 0; i < instance->module->numGlobals; i++) {
        const IM3Global g = &instance->module->globals[i];
        if (!g->name) continue;

        JSValue op;
        if (g->imported && instance->imported_globals && !JS_IsUndefined(instance->imported_globals[i])) {
            op = JS_DupValue(ctx, instance->imported_globals[i]);
        } else if (!g->imported) {
            op = qjs_wasm_new_global(ctx, JS_UNDEFINED, 0, NULL);
            if (JS_IsException(op)) INSTANCE_EXCEPTION();
            qjs_wasm_global_t *nx_g = qjs_wasm_global_get(ctx, op);
            if (!nx_g) {
                JS_FreeValue(ctx, op);
                INSTANCE_EXCEPTION();
            }
            nx_g->global = g;
        } else {
            continue;
        }

        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "global"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, g->name), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "val", op, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports_array, exports_index++, item, JS_PROP_C_W_E);
    }

    if (instance->module->memoryNameCount > 0 && instance->module->memoryNames[0]) {
        JSValue val;
        if (!JS_IsUndefined(instance->imported_memory)) {
            val = JS_DupValue(ctx, instance->imported_memory);
        } else {
            if (!instance->module->memory) INSTANCE_ERROR_THROW("WASM memory not initialized");
            val = qjs_wasm_memory_new_(ctx);
            if (JS_IsException(val)) INSTANCE_EXCEPTION();
            qjs_wasm_memory_t *data = qjs_wasm_memory_get(ctx, val);
            if (!data) {
                JS_FreeValue(ctx, val);
                INSTANCE_EXCEPTION();
            }
            data->mem = instance->module->memory;
            data->runtime = instance->runtime;
            data->needs_free = false;
        }
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "memory"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, instance->module->memoryNames[0]), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "val", val, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports_array, exports_index++, item, JS_PROP_C_W_E);
    }

    if (instance->module->tableInfo.tableName) {
        JSValue val;
        if (!JS_IsUndefined(instance->imported_table)) {
            val = JS_DupValue(ctx, instance->imported_table);
        } else {
            IM3Table table = m3_GetTable(instance->module);
            if (!table) INSTANCE_ERROR_THROW("WASM table not initialized");
            val = qjs_wasm_table_new_(ctx);
            if (JS_IsException(val)) INSTANCE_EXCEPTION();
            qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, val);
            if (!data) {
                JS_FreeValue(ctx, val);
                INSTANCE_EXCEPTION();
            }
            data->table = table;
            data->table_size = &instance->module->tableInfo.curSize;
            data->needs_free = false;
        }
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "table"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, instance->module->tableInfo.tableName), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, item, "val", val, JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports_array, exports_index++, item, JS_PROP_C_W_E);
    }

    instance->loaded = true;
    JSValue rtn = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, rtn, 0, opaque);
    JS_SetPropertyUint32(ctx, rtn, 1, exports_array);
    return rtn;
}

static JSValue qjs_wasm_module_imports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_module_t *m = qjs_wasm_module_get(ctx, argv[0]);
    if (!m || !m->module) return JS_EXCEPTION;

    JSValue imports = JS_NewArray(ctx);
    if (JS_IsException(imports)) return imports;

    size_t index = 0;
    for (size_t i = 0; i < m->module->numFunctions; ++i) {
        IM3Function f = &m->module->functions[i];
        if (f->import.moduleUtf8 && f->import.fieldUtf8) {
            JSValue item = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "function"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, item, "module", JS_NewString(ctx, f->import.moduleUtf8), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, f->import.fieldUtf8), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, imports, index++, item, JS_PROP_C_W_E);
        }
    }

    for (size_t i = 0; i < m->module->numGlobals; i++) {
        IM3Global g = &m->module->globals[i];
        if (g->imported && g->import.moduleUtf8 && g->import.fieldUtf8) {
            JSValue item = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(
                ctx, item, "kind", JS_NewString(ctx, "global"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, item, "module", JS_NewString(ctx, g->import.moduleUtf8), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, g->import.fieldUtf8), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, imports, index++, item, JS_PROP_C_W_E);
        }
    }

    if (m->module->memoryImportInfo) {
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "memory"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "module", JS_NewString(ctx, m->module->memoryImportInfo->moduleUtf8), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "name", JS_NewString(ctx, m->module->memoryImportInfo->fieldUtf8), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, imports, index++, item, JS_PROP_C_W_E);
    }

    if (m->module->tableImportInfo) {
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "table"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "module", JS_NewString(ctx, m->module->tableImportInfo->moduleUtf8), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "name", JS_NewString(ctx, m->module->tableImportInfo->fieldUtf8), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, imports, index++, item, JS_PROP_C_W_E);
    }

    return imports;
}

static JSValue qjs_wasm_module_exports(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_module_t *m = qjs_wasm_module_get(ctx, argv[0]);
    if (!m || !m->module) return JS_EXCEPTION;

    JSValue exports = JS_NewArray(ctx);
    if (JS_IsException(exports)) return exports;

    size_t index = 0;
    for (size_t i = 0; i < m->module->numFunctions; ++i) {
        IM3Function f = &m->module->functions[i];
        if (f->numNames > 0 && f->names[0]) {
            JSValue item = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "function"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, item, "name", JS_NewString(ctx, f->names[0]), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, exports, index++, item, JS_PROP_C_W_E);
        }
    }

    for (size_t i = 0; i < m->module->numGlobals; ++i) {
        IM3Global g = &m->module->globals[i];
        if (!g->imported && g->name) {
            JSValue item = JS_NewObject(ctx);
            JS_DefinePropertyValueStr(
                ctx, item, "kind", JS_NewString(ctx, "global"), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(
                ctx, item, "name", JS_NewString(ctx, g->name), JS_PROP_C_W_E);
            JS_DefinePropertyValueUint32(ctx, exports, index++, item, JS_PROP_C_W_E);
        }
    }

    if (!m->module->memoryImportInfo && m->module->memoryNameCount > 0 && m->module->memoryNames[0]) {
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "memory"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "name", JS_NewString(ctx, m->module->memoryNames[0]), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports, index++, item, JS_PROP_C_W_E);
    }

    if (m->module->tableInfo.tableName) {
        JSValue item = JS_NewObject(ctx);
        JS_DefinePropertyValueStr(ctx, item, "kind", JS_NewString(ctx, "table"), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(
            ctx, item, "name", JS_NewString(ctx, m->module->tableInfo.tableName), JS_PROP_C_W_E);
        JS_DefinePropertyValueUint32(ctx, exports, index++, item, JS_PROP_C_W_E);
    }

    return exports;
}

static JSValue qjs_wasm_call_func(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_exported_func_t *data = qjs_wasm_exported_func_get(ctx, argv[0]);
    if (!data) return JS_EXCEPTION;

    IM3Function func = data->function;
    if (!func) {
        return nx_throw_wasm_error(ctx, "RuntimeError", "Missing function reference");
    }

    M3Result r = m3Err_none;
    if (!func->compiled) {
        r = CompileFunction(func);
    }
    if (r) {
        return nx_throw_wasm_error(ctx, "RuntimeError", r);
    }

    uint32_t nargs = m3_GetArgCount(func);
    uint64_t *valbuff = NULL;
    const void **valptrs = NULL;

    if (nargs > 0) {
        if (nargs > QJS_WASM_MAX_ARGS) {
            return nx_throw_wasm_error(ctx, "RuntimeError", "Too many arguments");
        }
        valbuff = js_malloc(ctx, nargs * sizeof(uint64_t));
        valptrs = js_malloc(ctx, nargs * sizeof(void *));
        if (!valbuff || !valptrs) {
            js_free(ctx, valbuff);
            js_free(ctx, valptrs);
            JS_ThrowOutOfMemory(ctx);
            return JS_EXCEPTION;
        }
        memset(valbuff, 0, nargs * sizeof(uint64_t));
    } else {
        static uint64_t dummy_valbuff[1];
        static const void *dummy_valptrs[1];
        valbuff = dummy_valbuff;
        valptrs = dummy_valptrs;
    }

    for (uint32_t i = 0; i < nargs; i++) {
        valptrs[i] = &valbuff[i];
        M3ValueType type = m3_GetArgType(func, i);
        if (nx__wasm_towebassemblyvalue(ctx, argv[i + 1], type, &valbuff[i])) {
            if (nargs > 0) {
                js_free(ctx, valbuff);
                js_free(ctx, valptrs);
            }
            return JS_EXCEPTION;
        }
    }

    r = m3_Call(func, nargs, valptrs);
    if (nargs > 0) {
        js_free(ctx, valbuff);
        js_free(ctx, valptrs);
    }

    if (r) {
        if (r == qjs_wasm_js_error) {
            return JS_EXCEPTION;
        } else {
            return nx_throw_wasm_error(ctx, "RuntimeError", r);
        }
    }

    int ret_count = m3_GetRetCount(func);
    if (ret_count == 0) {
        return JS_UNDEFINED;
    }

    if (ret_count > QJS_WASM_MAX_RETS) {
        return nx_throw_wasm_error(ctx, "RuntimeError", "Too many return values");
    }

    uint64_t *retbuff = js_malloc(ctx, ret_count * sizeof(uint64_t));
    const void **retptrs = js_malloc(ctx, ret_count * sizeof(void *));
    if (!retbuff || !retptrs) {
        js_free(ctx, retbuff);
        js_free(ctx, retptrs);
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    memset(retbuff, 0, ret_count * sizeof(uint64_t));

    for (int i = 0; i < ret_count; i++) {
        retptrs[i] = &retbuff[i];
    }

    r = m3_GetResults(func, ret_count, retptrs);
    if (r) {
        js_free(ctx, retbuff);
        js_free(ctx, retptrs);
        return nx_throw_wasm_error(ctx, "RuntimeError", r);
    }

    if (ret_count == 1) {
        JSValue ret = nx__wasm_tojsvalue(ctx, m3_GetRetType(func, 0), retptrs[0]);
        js_free(ctx, retbuff);
        js_free(ctx, retptrs);
        return ret;
    } else {
        JSValue rets = JS_NewArray(ctx);
        for (int i = 0; i < ret_count; i++) {
            JS_SetPropertyUint32(
                ctx, rets, i, nx__wasm_tojsvalue(ctx, m3_GetRetType(func, i), retptrs[i]));
        }
        js_free(ctx, retbuff);
        js_free(ctx, retptrs);
        return rets;
    }
}

static JSValue qjs_wasm_memory_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue obj = qjs_wasm_memory_new_(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;

    qjs_wasm_memory_t *data = qjs_wasm_memory_get(ctx, obj);
    if (!data) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    u32 initial;
    JSValue initial_val = JS_GetPropertyStr(ctx, argv[0], "initial");
    if (JS_ToUint32(ctx, &initial, initial_val)) {
        JS_FreeValue(ctx, initial_val);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, initial_val);

    u32 maxPages;
    JSValue max_val = JS_GetPropertyStr(ctx, argv[0], "maximum");
    if (JS_ToUint32(ctx, &maxPages, max_val)) {
        maxPages = 0;
    }
    JS_FreeValue(ctx, max_val);
    if (!maxPages) {
        maxPages = 65536;
    }

    JSValue shared_val = JS_GetPropertyStr(ctx, argv[0], "shared");
    data->is_shared = JS_ToBool(ctx, shared_val);
    JS_FreeValue(ctx, shared_val);
    if (data->is_shared == -1) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    IM3Environment env = qjs_wasm_env(ctx);
    if (!env) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }

    IM3Runtime runtime = m3_NewRuntime(env, QJS_WASM_DEFAULT_STACK_SIZE, NULL);
    if (!runtime) {
        JS_ThrowOutOfMemory(ctx);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    runtime->memoryLimit = 0;
    data->runtime = runtime;

    IM3Memory mem = NULL;
    M3Result r = m3_NewMemory(&mem, runtime, initial, maxPages);
    if (r) {
        m3_FreeRuntime(runtime);
        data->runtime = NULL;
        JS_FreeValue(ctx, obj);
        return nx_throw_wasm_error(ctx, "RuntimeError", r);
    }
    data->mem = mem;
    data->needs_free = true;

    return obj;
}

// `Memory#buffer` getter function
static JSValue qjs_wasm_memory_buffer_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_memory_t *data = qjs_wasm_memory_get(ctx, this_val);
    if (!data) {
        return JS_EXCEPTION;
    }

    IM3Memory mem = data->mem;
    if (!mem || !mem->mallocated) {
        JS_ThrowTypeError(ctx, "Memory not allocated");
        return JS_EXCEPTION;
    }

    uint32_t size;
    uint8_t *memory = m3_GetMemory(mem, &size, 0);
    if (!memory) {
        JS_ThrowTypeError(ctx, "Memory buffer not available");
        return JS_EXCEPTION;
    }

    // Return cached buffer if size hasn't changed (invalidated by grow)
    if (!JS_IsUndefined(data->cached_buffer) && data->cached_buffer_len == size) {
        return JS_DupValue(ctx, data->cached_buffer);
    }

    // Invalidate old cache
    if (!JS_IsUndefined(data->cached_buffer)) {
        JS_FreeValue(ctx, data->cached_buffer);
        data->cached_buffer = JS_UNDEFINED;
    }

    JSValue buf = JS_NewArrayBuffer(ctx, memory, size, NULL, NULL, data->is_shared);
    if (JS_IsException(buf)) {
        return JS_EXCEPTION;
    }

    // Cache the buffer and its size
    data->cached_buffer = JS_DupValue(ctx, buf);
    data->cached_buffer_len = size;

    return buf;
}

// `Memory#grow()` function
static JSValue qjs_wasm_memory_grow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_memory_t *data = qjs_wasm_memory_get(ctx, this_val);
    if (!data) return JS_EXCEPTION;

    IM3Memory memory = data->mem;
    if (!memory || !memory->mallocated) {
        JS_ThrowTypeError(ctx, "Memory not allocated");
        return JS_EXCEPTION;
    }

    i32 numPagesToGrow;
    if (JS_ToInt32(ctx, &numPagesToGrow, argv[0])) return JS_EXCEPTION;

    if (numPagesToGrow < 0) {
        JS_ThrowTypeError(
            ctx, "WebAssembly.Memory.grow(): Argument 0 must be non-negative");
        return JS_EXCEPTION;
    }

    JSValue prevSize = JS_NewUint32(ctx, memory->numPages);
    if (numPagesToGrow > 0) {
        u32 requiredPages = memory->numPages + numPagesToGrow;
        if (requiredPages > memory->maxPages) {
            return nx_throw_wasm_error(ctx, "RuntimeError", "Memory.grow would exceed maximum");
        }

        IM3Runtime runtime = data->runtime;
        if (!runtime) {
            return nx_throw_wasm_error(ctx, "RuntimeError", "Memory runtime not available");
        }

        M3Result r = m3_GrowMemory(memory, runtime, numPagesToGrow);
        if (r) return nx_throw_wasm_error(ctx, "RuntimeError", r);
        
        // Invalidate cache on grow
        if (!JS_IsUndefined(data->cached_buffer)) {
            JS_FreeValue(ctx, data->cached_buffer);
            data->cached_buffer = JS_UNDEFINED;
            data->cached_buffer_len = 0;
        }
    }
    return prevSize;
}

static JSValue qjs_wasm_table_get_impl(JSContext *ctx, qjs_wasm_table_t *data, u32 index, bool throw_on_oob)
{
    if (!data->table_size || !data->table) {
        JS_ThrowTypeError(ctx, "Table not initialized");
        return JS_EXCEPTION;
    }
    u32 size = *data->table_size;
    if (index >= size) {
        if (throw_on_oob) {
            JS_ThrowRangeError(ctx, "WebAssembly.Table.get(): invalid index %u into "
                                 "funcref table of size %u",
                                 index, size);
        }
        return JS_NULL;
    }
    IM3Function func = data->table->funcs[index];
    if (!func) {
        return JS_NULL;
    }
    return qjs_wasm_exported_func_new(ctx, func);
}

static JSValue qjs_wasm_table_get_fn(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, this_val);
    if (!data) return JS_EXCEPTION;

    u32 index;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;

    return qjs_wasm_table_get_impl(ctx, data, index, true);
}

static JSValue qjs_wasm_table_get_wrapper(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, argv[0]);
    if (!data) return JS_EXCEPTION;

    u32 index;
    if (JS_ToUint32(ctx, &index, argv[1])) return JS_EXCEPTION;

    return qjs_wasm_table_get_impl(ctx, data, index, false);
}

// `Table#length` getter function
static JSValue qjs_wasm_table_length_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, this_val);
    if (!data) {
        return JS_EXCEPTION;
    }
    if (!data->table_size) {
        JS_ThrowTypeError(ctx, "Table size not set");
        return JS_EXCEPTION;
    }
    return JS_NewUint32(ctx, *data->table_size);
}

// `Table` constructor function
static JSValue qjs_wasm_table_new(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue obj = qjs_wasm_table_new_(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;

    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, obj);
    if (!data) return JS_EXCEPTION;

    // Get descriptor properties
    JSValue descriptor = argv[0];
    JSValue element_type_val = JS_GetPropertyStr(ctx, descriptor, "element");
    const char *element_type = !JS_IsUndefined(element_type_val) ? JS_ToCString(ctx, element_type_val) : NULL;
    JS_FreeValue(ctx, element_type_val);

    // Only support "anyfunc" for now
    if (!element_type || strcmp(element_type, "anyfunc") != 0) {
        const char *type_str = element_type ? element_type : "unknown";
        JS_ThrowTypeError(ctx, "Unsupported table element type: %s", type_str);
        if (element_type) JS_FreeCString(ctx, element_type);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    JS_FreeCString(ctx, element_type);

    u32 initial;
    if (JS_ToUint32(ctx, &initial, JS_GetPropertyStr(ctx, descriptor, "initial"))) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    u32 maximum;
    JSValue max_val = JS_GetPropertyStr(ctx, descriptor, "maximum");
    if (JS_IsUndefined(max_val)) {
        maximum = 100000; // d_m3MaxSaneTableSize - must match wasm3's default
    } else {
        if (JS_ToUint32(ctx, &maximum, max_val)) {
            JS_FreeValue(ctx, max_val);
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, max_val);

    // Create the table using wasm3 API
    M3TableElemType elem_type = kFuncRef;
    M3Result r = m3_NewTable(&data->table, elem_type, initial, maximum);
    if (r) {
        JS_FreeValue(ctx, obj);
        return nx_throw_wasm_error(ctx, "RuntimeError", r);
    }
    data->table_size = &data->table->info.curSize;
    data->needs_free = true;

    // Handle optional initial value
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        // Fill the table with the provided value
        for (u32 i = 0; i < initial; i++) {
            // For now, only null is supported as initial value
            if (!JS_IsNull(argv[1])) {
                JS_FreeValue(ctx, obj);
                JS_ThrowTypeError(ctx, "Only null is supported as initial table value");
                return JS_EXCEPTION;
            }
            // Setting to null (already initialized to NULL by m3_NewTable)
        }
    }
    return obj;
}

// `Table#grow()` function
static JSValue qjs_wasm_table_grow(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, this_val);
    if (!data) return JS_EXCEPTION;

    if (!data->table) {
        JS_ThrowTypeError(ctx, "Table not initialized");
        return JS_EXCEPTION;
    }

    i32 delta;
    if (JS_ToInt32(ctx, &delta, argv[0])) return JS_EXCEPTION;

    if (delta < 0) {
        JS_ThrowTypeError(ctx, "WebAssembly.Table.grow(): delta must be non-negative");
        return JS_EXCEPTION;
    }

    // Get the old size before growing
    JSValue prevSize = JS_NewUint32(ctx, data->table->info.curSize);
    if (delta > 0) {
        // Validate the new value parameter (only null supported for now)
        if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
            JS_FreeValue(ctx, prevSize);
            JS_ThrowTypeError(ctx, "Only null is supported as grow value");
            return JS_EXCEPTION;
        }
        M3Result r = m3_GrowTable(data->table, delta);
        if (r) {
            JS_FreeValue(ctx, prevSize);
            return nx_throw_wasm_error(ctx, "RuntimeError", r);
        }
    }
    return prevSize;
}

// `Table#set()` function
static JSValue qjs_wasm_table_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    qjs_wasm_table_t *data = qjs_wasm_table_get(ctx, this_val);
    if (!data) return JS_EXCEPTION;

    if (!data->table) {
        JS_ThrowTypeError(ctx, "Table not initialized");
        return JS_EXCEPTION;
    }

    u32 index;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;

    if (index >= data->table->info.curSize) {
        JS_ThrowRangeError(ctx, "WebAssembly.Table.set(): invalid index %u into "
                             "funcref table of size %u",
                             index, data->table->info.curSize);
        return JS_EXCEPTION;
    }

    // Get the function value
    JSValue func_val = argv[1];
    // Only support null or exported functions
    if (JS_IsNull(func_val) || JS_IsUndefined(func_val)) {
        data->table->funcs[index] = NULL;
    } else {
        // Check if it's an exported function
        qjs_wasm_exported_func_t *exported_func = qjs_wasm_exported_func_get(ctx, func_val);
        if (!exported_func) {
            JS_ThrowTypeError(ctx, "Table.set() only accepts WebAssembly.Function or null");
            return JS_EXCEPTION;
        }
        data->table->funcs[index] = exported_func->function;
    }
    return JS_UNDEFINED;
}

/* Initialize the `Memory` class */
static JSValue qjs_wasm_init_memory_class(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, argv[0], "prototype");
    qjs_def_get(proto, "buffer", qjs_wasm_memory_buffer_get);
    qjs_def_func(proto, "grow", qjs_wasm_memory_grow, 1);
    JS_FreeValue(ctx, proto);
    return JS_UNDEFINED;
}

/* Initialize the `Table` class */
static JSValue qjs_wasm_init_table_class(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    JSValue proto = JS_GetPropertyStr(ctx, argv[0], "prototype");
    qjs_def_func(proto, "get", qjs_wasm_table_get_fn, 1);
    qjs_def_func(proto, "set", qjs_wasm_table_set, 2);
    qjs_def_func(proto, "grow", qjs_wasm_table_grow, 1);
    qjs_def_get(proto, "length", qjs_wasm_table_length_get);
    JS_FreeValue(ctx, proto);
    return JS_UNDEFINED;
}

static JSValue qjs_wasm_validate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    IM3Environment env = qjs_wasm_env(ctx);
    if (!env) {
        JS_ThrowOutOfMemory(ctx);
        return JS_EXCEPTION;
    }
    size_t size;
    uint8_t *buf = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!buf) {
        return JS_FALSE;
    }
    IM3Module module = NULL;
    M3Result r = m3_ParseModule(env, &module, buf, size);
    if (r) {
        return JS_FALSE;
    }
    // Successfully parsed, free the module and return true
    m3_FreeModule(module);
    return JS_TRUE;
}

static const JSCFunctionListEntry init_function_list[] = {
    JS_CFUNC_DEF("wasmCallFunc", 1, qjs_wasm_call_func),
    JS_CFUNC_DEF("wasmMemNew", 1, qjs_wasm_memory_new),
    JS_CFUNC_DEF("wasmInitMemory", 1, qjs_wasm_init_memory_class),
    JS_CFUNC_DEF("wasmTableNew", 2, qjs_wasm_table_new),
    JS_CFUNC_DEF("wasmTableGet", 2, qjs_wasm_table_get_wrapper),
    JS_CFUNC_DEF("wasmInitTable", 1, qjs_wasm_init_table_class),
    JS_CFUNC_DEF("wasmNewModule", 1, qjs_wasm_new_module),
    JS_CFUNC_DEF("wasmNewInstance", 1, qjs_wasm_new_instance),
    JS_CFUNC_DEF("wasmNewGlobal", 1, qjs_wasm_new_global),
    JS_CFUNC_DEF("wasmModuleExports", 1, qjs_wasm_module_exports),
    JS_CFUNC_DEF("wasmModuleImports", 1, qjs_wasm_module_imports),
    JS_CFUNC_DEF("wasmGlobalGet", 1, qjs_wasm_global_value_get),
    JS_CFUNC_DEF("wasmGlobalSet", 1, qjs_wasm_global_value_set),
    JS_CFUNC_DEF("wasmValidate", 1, qjs_wasm_validate),
};

void qjs__mod_wasm_init(JSContext *ctx, JSValueConst ns)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* WebAssembly.Global */
    JSClassDef qjs_wasm_global_class = {
        "WebAssembly.Global",
        .finalizer = finalizer_wasm_global,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_GLOBAL, &qjs_wasm_global_class);

    /* WebAssembly.Memory */
    JSClassDef qjs_wasm_memory_class = {
        "WebAssembly.Memory",
        .finalizer = finalizer_wasm_memory,
        .gc_mark = gc_mark_wasm_memory,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_MEMORY, &qjs_wasm_memory_class);

    /* WebAssembly.Table */
    JSClassDef qjs_wasm_table_class = {
        "WebAssembly.Table",
        .finalizer = finalizer_wasm_table,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_TABLE, &qjs_wasm_table_class);

    /* WebAssembly.Function */
    JSClassDef qjs_wasm_exported_func_class = {
        "WebAssembly.Function",
        .finalizer = finalizer_wasm_exported_func,
        .gc_mark = gc_mark_wasm_exported_func,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_FUNC, &qjs_wasm_exported_func_class);

    /* WebAssembly.Module */
    JSClassDef qjs_wasm_module_class = {
        "WebAssembly.Module",
        .finalizer = finalizer_wasm_module,
        .gc_mark = gc_mark_wasm_module,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_MODULE, &qjs_wasm_module_class);

    /* WebAssembly.Instance */
    JSClassDef qjs_wasm_instance_class = {
        "WebAssembly.Instance",
        .finalizer = finalizer_wasm_instance,
        .gc_mark = gc_mark_wasm_instance,
    };
    JS_NewClass(rt, QJS_CORE_CLASS_ID_WASM_INSTANCE, &qjs_wasm_instance_class);

    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    JS_SetPropertyFunctionList(ctx, obj, init_function_list, countof(init_function_list));
    JS_DefinePropertyValueStr(ctx, ns, "wasm", obj, JS_PROP_C_W_E);
}
