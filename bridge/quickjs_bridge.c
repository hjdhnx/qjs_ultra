/*
 * quickjs_bridge.c — quickjs-ng 与 dart:ffi 之间的稳定 ABI 层。
 *
 * 设计要点：
 * 1. quickjs 的 JSValue 是 16 字节 struct（按值传递/返回），Dart FFI 虽然能
 *    表达，但回调里返回 struct 的 ABI 路径风险高。本层把所有 JSValue 改为
 *    堆指针传递，Dart 侧只看到 16 字节内存。
 * 2. 所有从 C 调回 Dart 的回调一律使用 void 签名（返回值通过输出参数写回），
 *    完全避开 struct 返回值。
 * 3. Dart 通过 package:ffi 的 malloc 分配的内存与 C runtime malloc 同源，
 *    跨界 free 安全。
 *
 * 构建：与 quickjs-ng（quickjs.a）一起编译为动态库。
 */
#include "quickjs.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define QJS_API __declspec(dllexport)
#else
#define QJS_API __attribute__((visibility("default")))
#endif

/* ---------- Dart 回调签名（全部 void） ---------- */

/* 宿主函数：id, ctx, argc, argv, ret（Dart 把返回值写入 ret） */
typedef void (*QJSHostCall)(int32_t id, JSContext *ctx, int32_t argc,
                            JSValueConst *argv, JSValue *ret);
/* 模块加载：返回 1=bytecode、2=源码文本，out_buf/out_len 有效（malloc 分配，C 侧 free）；
 * 返回 0 = 加载失败 */
typedef int32_t (*QJSModuleLoad)(JSContext *ctx, const char *name,
                                 uint8_t **out_buf, int32_t *out_len);
/* 模块名规范化：返回 1 时 out 有效（malloc 分配，引擎 js_free） */
typedef int32_t (*QJSNormalize)(const char *base, const char *name,
                                char **out);

/* Per-engine 回调表（DsPlayer 补丁，根治跨 isolate SIGABRT）。
 *
 * 旧实现是三个文件级全局变量：qjs_set_callbacks 无 ctx 参数，任何新引擎
 * 一创建就覆盖旧值。此时若旧 isolate 仍有 native 任务在跑（dr2 的同步 req、
 * wasm 解密），它回调时命中新 isolate 的 Dart 闭包 → Dart VM 判定
 * "Cannot invoke native callback from a different isolate" → SIGABRT 闪退。
 *
 * 改为按 JSContext 存储（JS_GetContextOpaque）：每个引擎在自己的 ctx 注册，
 * 互不干扰；引擎 dispose 时调 qjs_clear_callbacks 释放。 */
typedef struct QjsCallbacks {
  QJSHostCall host_call;
  QJSModuleLoad module_load;
  QJSNormalize normalize;
} QjsCallbacks;

static QjsCallbacks *callbacks_of(JSContext *ctx) {
  return (QjsCallbacks *)JS_GetContextOpaque(ctx);
}

/* ABI 变更：首参为 ctx（Dart 侧 QjsBridge.setCallbacks(ctx, ...) 对应） */
QJS_API void qjs_set_callbacks(void *ctx_ptr, QJSHostCall host_call,
                               QJSModuleLoad module_load,
                               QJSNormalize normalize) {
  JSContext *ctx = (JSContext *)ctx_ptr;
  if (!ctx) return;
  QjsCallbacks *cb = callbacks_of(ctx);
  if (!cb) {
    cb = (QjsCallbacks *)malloc(sizeof(QjsCallbacks));
    if (!cb) return;
    cb->host_call = NULL;
    cb->module_load = NULL;
    cb->normalize = NULL;
    JS_SetContextOpaque(ctx, cb);
  }
  cb->host_call = host_call;
  cb->module_load = module_load;
  cb->normalize = normalize;
}

/* 释放回调表（Dart dispose 在 qjs_free_context 之前调用） */
QJS_API void qjs_clear_callbacks(void *ctx_ptr) {
  JSContext *ctx = (JSContext *)ctx_ptr;
  if (!ctx) return;
  QjsCallbacks *cb = callbacks_of(ctx);
  if (cb) {
    free(cb);
    JS_SetContextOpaque(ctx, NULL);
  }
}

/* ---------- runtime / context ---------- */

QJS_API JSRuntime *qjs_new_runtime(void) { return JS_NewRuntime(); }

QJS_API void qjs_free_runtime(JSRuntime *rt) { JS_FreeRuntime(rt); }

QJS_API JSContext *qjs_new_context(JSRuntime *rt) { return JS_NewContext(rt); }

QJS_API void qjs_free_context(JSContext *ctx) { JS_FreeContext(ctx); }

QJS_API void qjs_set_memory_limit(JSRuntime *rt, size_t limit) {
  JS_SetMemoryLimit(rt, (size_t)limit);
}

QJS_API void qjs_set_max_stack_size(JSRuntime *rt, size_t size) {
  JS_SetMaxStackSize(rt, (size_t)size);
}

QJS_API void qjs_run_gc(JSRuntime *rt) { JS_RunGC(rt); }

/* ---------- eval / bytecode ---------- */

/* flags: quickjs 的 JS_EVAL_* 原样透传。返回 0 成功（out 有效），-1 异常 */
QJS_API int32_t qjs_eval(JSContext *ctx, const char *script, int32_t len,
                         const char *filename, int32_t flags, JSValue *out) {
  JSValue v = JS_Eval(ctx, script, (size_t)len, filename, (int)flags);
  *out = v;
  return JS_IsException(v) ? -1 : 0;
}

/* 编译为 bytecode（模块或脚本）。out_buf 为 malloc 分配，调用方 free */
QJS_API int32_t qjs_compile(JSContext *ctx, const char *source, int32_t len,
                            const char *filename, int32_t is_module,
                            uint8_t **out_buf, int32_t *out_len) {
  int flags = JS_EVAL_FLAG_COMPILE_ONLY |
              (is_module ? JS_EVAL_TYPE_MODULE : JS_EVAL_TYPE_GLOBAL);
  JSValue v = JS_Eval(ctx, source, (size_t)len, filename, flags);
  if (JS_IsException(v)) {
    return -1;
  }
  size_t size = 0;
  uint8_t *buf = JS_WriteObject(ctx, &size, v, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, v);
  if (!buf) {
    return -1;
  }
  /* JS_WriteObject 用 js_malloc 分配，必须用 js_free 归还；
   * 而调用方（Dart/FFI）只能用 free。这里拷贝到 malloc 缓冲，
   * js 缓冲立即归还，避免 allocator 不匹配造成堆损坏。 */
  uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
  if (!copy) {
    js_free(ctx, buf);
    return -1;
  }
  memcpy(copy, buf, size);
  js_free(ctx, buf);
  *out_buf = copy;
  *out_len = (int32_t)size;
  return 0;
}

/* 执行 bytecode：module 自动 ResolveModule；返回 0 成功 */
QJS_API int32_t qjs_eval_bytecode(JSContext *ctx, const uint8_t *buf,
                                  int32_t len, JSValue *out) {
  JSValue func = JS_ReadObject(ctx, buf, (size_t)len, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(func)) {
    *out = JS_EXCEPTION;
    return -1;
  }
  if (JS_VALUE_GET_TAG(func) == JS_TAG_MODULE) {
    if (JS_ResolveModule(ctx, func) < 0) {
      JS_FreeValue(ctx, func);
      *out = JS_EXCEPTION;
      return -1;
    }
    js_module_set_import_meta(ctx, func, 0, 0, NULL);
  }
  JSValue val = JS_EvalFunction(ctx, func);
  *out = val;
  return JS_IsException(val) ? -1 : 0;
}
/* 返回仍待执行的 job 数（执行一个） */
QJS_API int32_t qjs_execute_pending_job(JSRuntime *rt) {
  JSContext *pctx = NULL;
  int rc = JS_ExecutePendingJob(rt, &pctx);
  if (rc < 0) return -1;
  return rc == 1 ? 1 : 0; /* 1=执行了一个, 0=队列空 */
}

/* ---------- 模块加载（对齐 quickjs_wrapper.cpp jsModuleLoaderFunc） ---------- */

static int has_json_suffix(const char *name) {
  size_t n = strlen(name);
  return (n >= 5 && !strcmp(name + n - 5, ".json")) ||
         (n >= 6 && !strcmp(name + n - 6, ".json5"));
}

/* JSON 模块：初始化时把私有值导出为 default */
static int json_module_init(JSContext *ctx, JSModuleDef *m) {
  JSValue val = JS_GetModulePrivateValue(ctx, m);
  JS_SetModuleExport(ctx, m, "default", val);
  return 0;
}

static JSModuleDef *create_json_module(JSContext *ctx, const char *module_name,
                                       JSValue val) {
  JSModuleDef *m = JS_NewCModule(ctx, module_name, json_module_init);
  if (!m) {
    JS_FreeValue(ctx, val);
    return NULL;
  }
  /* 只导出 default 符号，值为解析后的 JSON 对象 */
  JS_AddModuleExport(ctx, m, "default");
  JS_SetModulePrivateValue(ctx, m, val);
  return m;
}

/* return > 0 if the import attributes indicate a JSON module */
static int js_module_test_json(JSContext *ctx, JSValueConst attributes) {
  JSValue str;
  const char *cstr;
  size_t len;
  int res;
  if (JS_IsUndefined(attributes))
    return 0;
  str = JS_GetPropertyStr(ctx, attributes, "type");
  if (!JS_IsString(str))
    return 0;
  cstr = JS_ToCStringLen(ctx, &len, str);
  JS_FreeValue(ctx, str);
  if (!cstr)
    return 0;
  if (len == 4 && !memcmp(cstr, "json", len))
    res = 1;
  else if (len == 5 && !memcmp(cstr, "json5", len))
    res = 2;
  else
    res = 0;
  JS_FreeCString(ctx, cstr);
  return res;
}

/* 拒绝不支持的 import attributes（仅允许 type） */
static int js_module_check_attributes(JSContext *ctx, void *opaque,
                                      JSValueConst attributes) {
  JSPropertyEnum *tab;
  uint32_t i, len;
  int ret;
  const char *cstr;
  size_t cstr_len;
  (void)opaque;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, attributes,
                             JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
    return -1;
  ret = 0;
  for (i = 0; i < len; i++) {
    cstr = JS_AtomToCStringLen(ctx, &cstr_len, tab[i].atom);
    if (!cstr) {
      ret = -1;
      break;
    }
    if (!(cstr_len == 4 && !memcmp(cstr, "type", cstr_len))) {
      JS_ThrowTypeError(ctx, "import attribute '%s' is not supported", cstr);
      ret = -1;
    }
    JS_FreeCString(ctx, cstr);
    if (ret)
      break;
  }
  JS_FreePropertyEnum(ctx, tab, len);
  return ret;
}

static JSModuleDef *module_loader_trampoline(JSContext *ctx,
                                             const char *module_name,
                                             void *opaque,
                                             JSValueConst attributes) {
  (void)opaque;
  QjsCallbacks *cb = callbacks_of(ctx);
  if (!cb || !cb->module_load) {
    JS_ThrowInternalError(ctx, "Failed to load module, the ModuleLoader can not be null!");
    return NULL;
  }
  uint8_t *buf = NULL;
  int32_t len = 0;
  int32_t mode = cb->module_load(ctx, module_name, &buf, &len);
  if (mode == 0 || !buf) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", module_name);
    return NULL;
  }

  void *m;
  if (mode == 1) {
    /* ---- bytecode 模式（对齐 Java 版 bytecode 分支） ---- */
    uint32_t flags = JS_READ_OBJ_BYTECODE;
    int is_json = has_json_suffix(module_name);
    if (is_json)
      flags = 0;
    JSValue obj = JS_ReadObject(ctx, buf, (size_t)len, flags);
    free(buf);
    if (JS_IsException(obj))
      return NULL;
    if (is_json) {
      m = create_json_module(ctx, module_name, obj);
    } else {
      if (JS_ResolveModule(ctx, obj) < 0) {
        JS_FreeValue(ctx, obj);
        return NULL;
      }
      js_module_set_import_meta(ctx, obj, 0, 0, module_name);
      m = JS_VALUE_GET_PTR(obj);
      JS_FreeValue(ctx, obj);
    }
  } else {
    /* ---- 源码模式（对齐 Java 版 getModuleStringCode 分支） ---- */
    const char *script = (const char *)buf;
    size_t script_len = (size_t)len;
    int res = js_module_test_json(ctx, attributes);
    if (has_json_suffix(module_name) || res > 0) {
      /* 按 import attributes 的 type 断言以 JSON 或 JSON5 解析 */
      int jflags = (res == 2) ? JS_PARSE_JSON_EXT : 0;
      JSValue val = JS_ParseJSON2(ctx, script, script_len, module_name, jflags);
      free(buf);
      if (JS_IsException(val))
        return NULL;
      m = create_json_module(ctx, module_name, val);
    } else {
      JSValue func_val =
          JS_Eval(ctx, script, script_len, module_name,
                  JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
      free(buf);
      if (JS_IsException(func_val))
        return NULL;
      /* compile-only 的 module value 的 ptr 即 JSModuleDef，
       * 引用由引擎持有；不在 loader 内 ResolveModule/import_meta
       * （与 Java 版一致，由引擎在实例化阶段处理）。 */
      m = JS_VALUE_GET_PTR(func_val);
      JS_FreeValue(ctx, func_val);
    }
  }
  return (JSModuleDef *)m;
}

static char *normalize_trampoline(JSContext *ctx, const char *base_name,
                                  const char *name, void *opaque) {
  (void)opaque;
  QjsCallbacks *cb = callbacks_of(ctx);
  if (!cb || !cb->normalize) {
    char *out = (char *)js_malloc(ctx, strlen(name) + 1);
    if (out) strcpy(out, name);
    return out;
  }
  char *out = NULL;
  if (cb->normalize(base_name, name, &out) == 1 && out) {
    /* Dart 侧用 malloc 分配；引擎以 js_free(ctx, ret) 释放返回值。
     * 这里拷贝到 js_malloc 分配的缓冲区再交给引擎，
     * Dart 侧的 malloc 缓冲区由本层用 free 归还，归属清晰。 */
    char *ret = (char *)js_malloc(ctx, strlen(out) + 1);
    if (!ret) { free(out); return NULL; }
    strcpy(ret, out);
    free(out);
    return ret;
  }
  return NULL;
}

QJS_API void qjs_install_module_loader(JSRuntime *rt) {
  JS_SetModuleLoaderFunc2(rt, normalize_trampoline, module_loader_trampoline,
                          js_module_check_attributes, NULL);
}

/* ---------- 值操作 ---------- */

/* tag 常量与 quickjs.h 一致 */
#define QJS_TAG_OBJECT (-1)
#define QJS_TAG_STRING (-7)

QJS_API void qjs_free_value(JSContext *ctx, JSValue *v) {
  JS_FreeValue(ctx, *v);
  v->u.ptr = NULL;
  v->tag = JS_TAG_UNDEFINED;
}

QJS_API void qjs_dup_value(JSContext *ctx, JSValue *v) { JS_DupValue(ctx, *v); }

/* 把 src 移交到 dst（Dart 回调写返回值用） */
QJS_API void qjs_value_move(JSValue *dst, JSValue *src) { *dst = *src; }

QJS_API void qjs_make_undefined(JSValue *out) { *out = JS_UNDEFINED; }
QJS_API void qjs_make_null(JSValue *out) { *out = JS_NULL; }
QJS_API void qjs_make_bool(JSContext *ctx, JSValue *out, int32_t b) {
  (void)ctx;
  *out = JS_NewBool(ctx, b ? 1 : 0);
}
QJS_API void qjs_make_int32(JSContext *ctx, JSValue *out, int32_t n) {
  (void)ctx;
  *out = JS_NewInt32(ctx, n);
}
QJS_API void qjs_make_float64(JSContext *ctx, JSValue *out, double d) {
  *out = __JS_NewFloat64(ctx, d);
}

QJS_API int32_t qjs_get_tag(JSValue *v) { return (int32_t)JS_VALUE_GET_TAG(*v); }

QJS_API int32_t qjs_get_float64(JSValue *v, double *out) {
  if (JS_VALUE_GET_TAG(*v) != JS_TAG_FLOAT64) return -1;
  *out = JS_VALUE_GET_FLOAT64(*v);
  return 0;
}

QJS_API int32_t qjs_get_int32(JSValue *v, int32_t *out) {
  if (JS_VALUE_GET_TAG(*v) != JS_TAG_INT) return -1;
  *out = JS_VALUE_GET_INT(*v);
  return 0;
}

/* out 由 malloc 分配（含结尾 0），调用方 qjs_free_buffer 释放。失败返回 -1 */
QJS_API int32_t qjs_to_cstring(JSContext *ctx, JSValue *v, char **out,
                               int32_t *out_len) {
  size_t len = 0;
  /* JS_ToCStringLen / JS_ToString 对 JS_TAG_SYMBOL 均失败（quickjs-ng 抛
   * "cannot convert symbol to string"）。改走公开 API：原始 symbol 属性
   * 访问会自动装箱，取 Symbol.prototype 的 description getter，产出
   * "Symbol(desc)" 形式，符合 core 契约"不得静默丢弃"。 */
  if (JS_VALUE_GET_TAG(*v) == JS_TAG_SYMBOL) {
    /* 隔离测试实证：C API 对原始 symbol 的属性访问会自动装箱，直接取
     * Symbol.prototype 的 description getter 即可，无需 JS_ToObject。 */
    JSValue desc = JS_GetPropertyStr(ctx, *v, "description");
    if (JS_IsException(desc) || JS_IsUndefined(desc)) {
      JS_FreeValue(ctx, desc);
      return -1;
    }
    const char *d = JS_ToCStringLen(ctx, &len, desc);
    JS_FreeValue(ctx, desc);
    if (!d) return -1;
    const char *pre = "Symbol(";
    size_t pl = strlen(pre);
    char *copy = (char *)malloc(pl + len + 2);
    if (!copy) {
      JS_FreeCString(ctx, d);
      return -1;
    }
    memcpy(copy, pre, pl);
    memcpy(copy + pl, d, len);
    copy[pl + len] = ')';
    copy[pl + len + 1] = 0;
    JS_FreeCString(ctx, d);
    *out = copy;
    *out_len = (int32_t)(pl + len + 1);
    return 0;
  }
  const char *s = JS_ToCStringLen(ctx, &len, *v);
  if (!s) return -1;
  char *copy = (char *)malloc(len + 1);
  if (!copy) {
    JS_FreeCString(ctx, s);
    return -1;
  }
  memcpy(copy, s, len);
  copy[len] = 0;
  JS_FreeCString(ctx, s);
  *out = copy;
  *out_len = (int32_t)len;
  return 0;
}

QJS_API int32_t qjs_new_string(JSContext *ctx, const char *s, int32_t len,
                               JSValue *out) {
  *out = JS_NewStringLen(ctx, s, (size_t)len);
  return JS_IsException(*out) ? -1 : 0;
}

QJS_API int32_t qjs_new_object(JSContext *ctx, JSValue *out) {
  *out = JS_NewObject(ctx);
  return JS_IsException(*out) ? -1 : 0;
}

QJS_API int32_t qjs_new_array(JSContext *ctx, JSValue *out) {
  *out = JS_NewArray(ctx);
  return JS_IsException(*out) ? -1 : 0;
}

/* out 由 malloc 分配，调用方 free */
QJS_API int32_t qjs_stringify(JSContext *ctx, JSValue *v, char **out,
                              int32_t *out_len) {
  JSValue json = JS_JSONStringify(ctx, *v, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json)) return -1;
  size_t len = 0;
  const char *s = JS_ToCStringLen(ctx, &len, json);
  JS_FreeValue(ctx, json);
  if (!s) return -1;
  char *copy = (char *)malloc(len + 1);
  if (!copy) {
    JS_FreeCString(ctx, s);
    return -1;
  }
  memcpy(copy, s, len);
  copy[len] = 0;
  JS_FreeCString(ctx, s);
  *out = copy;
  *out_len = (int32_t)len;
  return 0;
}

QJS_API int32_t qjs_parse_json(JSContext *ctx, const char *json, int32_t len,
                               JSValue *out) {
  *out = JS_ParseJSON(ctx, json, (size_t)len, "<json>");
  return JS_IsException(*out) ? -1 : 0;
}

QJS_API int32_t qjs_get_global(JSContext *ctx, JSValue *out) {
  *out = JS_GetGlobalObject(ctx);
  return 0;
}

QJS_API int32_t qjs_get_prop(JSContext *ctx, JSValue *obj, const char *name,
                             JSValue *out) {
  *out = JS_GetPropertyStr(ctx, *obj, name);
  return JS_IsException(*out) ? -1 : 0;
}

/* val 移交给属性（引擎接管引用） */
QJS_API int32_t qjs_set_prop(JSContext *ctx, JSValue *obj, const char *name,
                             JSValue *val) {
  int rc = JS_SetPropertyStr(ctx, *obj, name, *val);
  val->u.ptr = NULL;
  val->tag = JS_TAG_UNDEFINED;
  return rc < 0 ? -1 : 0;
}

QJS_API int32_t qjs_get_prop_u32(JSContext *ctx, JSValue *obj, uint32_t idx,
                                 JSValue *out) {
  *out = JS_GetPropertyUint32(ctx, *obj, idx);
  return JS_IsException(*out) ? -1 : 0;
}

/* val 移交给数组元素（引擎接管引用） */
QJS_API int32_t qjs_set_prop_u32(JSContext *ctx, JSValue *obj, uint32_t idx,
                                 JSValue *val) {
  int rc = JS_SetPropertyUint32(ctx, *obj, idx, *val);
  val->u.ptr = NULL;
  val->tag = JS_TAG_UNDEFINED;
  return rc < 0 ? -1 : 0;
}

QJS_API int32_t qjs_array_length(JSContext *ctx, JSValue *obj,
                                 uint32_t *out) {
  JSValue len_val = JS_GetPropertyStr(ctx, *obj, "length");
  if (JS_IsException(len_val)) return -1;
  int32_t n = 0;
  int rc = JS_ToInt32(ctx, &n, len_val);
  JS_FreeValue(ctx, len_val);
  if (rc < 0 || n < 0) return -1;
  *out = (uint32_t)n;
  return 0;
}

/* 枚举自有可枚举属性名。names 为 malloc 数组，每项是 malloc 字符串 */
QJS_API int32_t qjs_own_property_names(JSContext *ctx, JSValue *obj,
                                       char ***names, uint32_t *count) {
  JSPropertyEnum *tab = NULL;
  uint32_t len = 0;
  int flags = JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY;
  if (JS_GetOwnPropertyNames(ctx, &tab, &len, *obj, flags) < 0) return -1;
  char **out = (char **)malloc(sizeof(char *) * (len ? len : 1));
  if (!out) {
    JS_FreePropertyEnum(ctx, tab, len);
    return -1;
  }
  for (uint32_t i = 0; i < len; i++) {
    const char *s = JS_AtomToCString(ctx, tab[i].atom);
    out[i] = s ? strdup(s) : NULL;
    JS_FreeCString(ctx, s);
    /* 注意：atom 由 JS_FreePropertyEnum 统一释放，这里不能 JS_FreeAtom，
     * 否则双重释放导致 atom refcount 损坏，freeRuntime 阶段段错误。 */
  }
  JS_FreePropertyEnum(ctx, tab, len);
  *names = out;
  *count = len;
  return 0;
}

QJS_API void qjs_free_string_array(char **names, uint32_t count) {
  for (uint32_t i = 0; i < count; i++) free(names[i]);
  free(names);
}

QJS_API int32_t qjs_call(JSContext *ctx, JSValue *func, JSValue *this_obj,
                         int32_t argc, JSValue *argv, JSValue *out) {
  JSValue v = JS_Call(ctx, *func,
                      this_obj ? *this_obj : JS_UNDEFINED, (int)argc,
                      (JSValueConst *)argv);
  *out = v;
  return JS_IsException(v) ? -1 : 0;
}

/* ---------- 宿主函数注册 ---------- */

static JSValue host_trampoline(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *func_data) {
  (void)this_val;
  (void)func_data;
  JSValue ret = JS_UNDEFINED;
  QjsCallbacks *cb = callbacks_of(ctx);
  if (cb && cb->host_call) {
    cb->host_call((int32_t)magic, ctx, (int32_t)argc, argv, &ret);
  }
  return ret;
}

/* 创建一个由 Dart 实现的函数值（out 有效，Dart 负责 move/free） */
QJS_API int32_t qjs_new_host_function(JSContext *ctx, int32_t function_id,
                                      JSValue *out) {
  *out = JS_NewCFunctionData(ctx, host_trampoline, 0, (int)function_id, 0,
                             NULL);
  return JS_IsException(*out) ? -1 : 0;
}

QJS_API int32_t qjs_register_function(JSContext *ctx, const char *name,
                                      int32_t function_id) {
  JSValue fn = JS_UNDEFINED;
  if (qjs_new_host_function(ctx, function_id, &fn) != 0) return -1;
  JSValue global = JS_GetGlobalObject(ctx);
  int rc = JS_SetPropertyStr(ctx, global, name, fn);
  JS_FreeValue(ctx, global);
  return rc < 0 ? -1 : 0;
}

/* 把 Dart 异常转成 JS Error 抛出，让 JS 侧可以 try/catch */
QJS_API int32_t qjs_throw_error(JSContext *ctx, const char *msg) {
  JSValue err = JS_NewError(ctx);
  JS_SetPropertyStr(ctx, err, "message", JS_NewStringLen(ctx, msg,
                                                         strlen(msg)));
  JS_Throw(ctx, err);
  return 0;
}

/* 异常处理 */
QJS_API int32_t qjs_is_exception(JSValue *v) { return JS_IsException(*v); }

QJS_API int32_t qjs_get_exception(JSContext *ctx, JSValue *out) {
  *out = JS_GetException(ctx);
  return 0;
}
QJS_API int32_t qjs_is_function(JSContext *ctx, JSValue *v) {
  return JS_IsFunction(ctx, *v) ? 1 : 0;
}
QJS_API int32_t qjs_memory_usage(JSRuntime *rt, int64_t *malloc_size,
                                 int64_t *used_size) {
  JSMemoryUsage mu;
  JS_ComputeMemoryUsage(rt, &mu);
  *malloc_size = mu.malloc_size;
  *used_size = mu.memory_used_size;
  return 0;
}

QJS_API void qjs_free_buffer(void *p) { free(p); }

/* ---------- 二进制 / 数组判定（Dart 桥接契约补充） ---------- */

/* JS IsArray 精确判定（引擎内置语义，非 length 属性猜测） */
QJS_API int32_t qjs_is_array(JSContext *ctx, JSValue *v) {
  return JS_IsArray(ctx, *v) ? 1 : 0;
}

/* JS -> Dart：TypedArray / ArrayBuffer 取字节副本。
 * out 为 malloc 分配，调用方用 qjs_free_buffer 释放。
 * 返回 0 成功；-1 非二进制值或出错（内部已清理 pending exception）。 */
QJS_API int32_t qjs_get_bytes(JSContext *ctx, JSValue *v, uint8_t **out,
                              int32_t *out_len) {
  size_t size = 0;
  uint8_t *buf = JS_GetUint8Array(ctx, &size, *v);
  if (!buf) {
    buf = JS_GetArrayBuffer(ctx, &size, *v);
    if (!buf) {
      JSValue exc = JS_GetException(ctx);
      JS_FreeValue(ctx, exc);
      return -1;
    }
  }
  uint8_t *copy = (uint8_t *)malloc(size ? size : 1);
  if (!copy) return -1;
  memcpy(copy, buf, size);
  *out = copy;
  *out_len = (int32_t)size;
  return 0;
}

/* Dart -> JS：拷贝构造 Uint8Array，值写入 out。
 * 返回 0 成功；-1 失败（异常已挂到 ctx）。 */
QJS_API int32_t qjs_bridge_new_uint8_array(JSContext *ctx, const uint8_t *buf,
                                    int32_t len, JSValue *out) {
  JSValue v = JS_NewUint8ArrayCopy(ctx, buf, (size_t)len);
  if (JS_IsException(v)) return -1;
  *out = v;
  return 0;
}
