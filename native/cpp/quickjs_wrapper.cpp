/*
 * quickjs_wrapper.cpp - QuickJS JNI 包装器主实现文件
 *
 * 实现 QuickJSWrapper 类，封装 QuickJS 引擎的初始化、模块加载、
脚本执行和内存管理。提供 Java/JNI 接口供 Android 层调用。
主要功能：
  - 创建和销毁 JSRuntime/JSContext
  - 注册内置模块（crypto/webcrypto/buffer/fs/zlib/sqlite/wasm 等）
  - 执行 JS 脚本并处理 Promise 微任务
  - JS 异常到 Java 异常的转换
  - 模块导入与 require() 支持
 *
 * Copyright (C) QuickJS Native 项目 contributors
 */

//
// Created by yonglan.whl on 2021/7/14.
//
#include "quickjs_wrapper.h"
#include <cstring>
#include <cmath>
#include <atomic>

#define MAX_SAFE_INTEGER (((int64_t)1 << 53) - 1)

// util
static string getJavaName(JNIEnv* env, jobject javaClass) {
    auto classType = env->GetObjectClass(javaClass);
    const auto method = env->GetMethodID(classType, "getName", "()Ljava/lang/String;");
    auto javaString = (jstring)(env->CallObjectMethod(javaClass, method));
    const auto s = env->GetStringUTFChars(javaString, nullptr);

    std::string str(s);
    env->ReleaseStringUTFChars(javaString, s);
    env->DeleteLocalRef(javaString);
    env->DeleteLocalRef(classType);
    return str;
}

static void tryToTriggerOnError(JSContext *ctx, JSValueConst *error) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue onerror = JS_GetPropertyStr(ctx, global, "onError");
    if (JS_IsNull(onerror)) {
        // may be lowercase
        onerror = JS_GetPropertyStr(ctx, global, "onerror");
    }

    if (JS_IsNullOrUndefined(onerror)) {
        JS_FreeValue(ctx, onerror);
        JS_FreeValue(ctx, global);
        return;
    }

    JSValue ret = JS_Call(ctx, onerror, global, 1, error);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, onerror);
    JS_FreeValue(ctx, global);
}

static string getJSErrorStr(JSContext *ctx, JSValueConst error) {
    JSValue val;
    bool is_error;
    is_error = JS_IsError(ctx, error);
    string jsException;
    if (is_error) {
        tryToTriggerOnError(ctx, &error);

        JSValue message = JS_GetPropertyStr(ctx, error, "message");
        const char *msg_str = JS_ToCString(ctx, message);
        if (msg_str) jsException += msg_str;
        JS_FreeCString(ctx, msg_str);
        JS_FreeValue(ctx, message);

        val = JS_GetPropertyStr(ctx, error, "stack");
        if (!JS_IsUndefined(val)) {
            jsException += "\n";

            const char *stack_str = JS_ToCString(ctx, val);
            if (stack_str) jsException += stack_str;
            JS_FreeCString(ctx, stack_str);
        }
        JS_FreeValue(ctx, val);
    } else {
        const char *error_str = JS_ToCString(ctx, error);
        if (error_str) jsException += error_str;
        JS_FreeCString(ctx, error_str);
    }
    return jsException;
}

static string getJSErrorStr(JSContext *ctx) {
    JSValue error = JS_GetException(ctx);
    string error_str = getJSErrorStr(ctx, error);
    JS_FreeValue(ctx, error);
    return error_str;
}

static void throwJavaException(JNIEnv *env, const char *exceptionClass, const char *fmt, ...) {
    char msg[512];
    va_list args;
    va_start (args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end (args);
    jclass e = env->FindClass(exceptionClass);
    env->ThrowNew(e, msg);
    env->DeleteLocalRef(e);
}

static void throwJSException(JNIEnv *env, const char* msg) {
    if (env->ExceptionCheck()) {
        return;
    }

    jclass e = env->FindClass("com/whl/quickjs/wrapper/QuickJSException");
    jmethodID init = env->GetMethodID(e, "<init>", "(Ljava/lang/String;Z)V");
    jstring ret = env->NewStringUTF(msg);
    auto t = (jthrowable)env->NewObject(e, init, ret, JNI_TRUE);
    env->Throw(t);
    env->DeleteLocalRef(e);
}

static void throwJSException(JNIEnv *env, JSContext *ctx) {
    string error = getJSErrorStr(ctx);
    throwJSException(env, error.c_str());
}

// js function callback

static void jsFuncCallbackFinalizer(JSRuntime *rt, JSValue val) {
    auto wrapper = reinterpret_cast<const QuickJSWrapper*>(JS_GetRuntimeOpaque(rt));
    if (wrapper) {
        /* 使用 JS_GetOpaque（不需要 ctx，不抛异常），finalizer 中不能用 JS_GetOpaque2 */
        int *callbackId = (int *)(JS_GetOpaque(val, QJS_CORE_CLASS_ID_JS_FUNC_CALLBACK));
        if (callbackId) {
            wrapper->removeCallFunction(*callbackId);
            delete callbackId;
        }
    }
}

static JSClassDef js_func_callback_class = {
        "JSFuncCallback",
        .finalizer = jsFuncCallbackFinalizer,
};

static JSValue jsFnCallback(JSContext *ctx,
                            JSValueConst this_obj,
                            int argc, JSValueConst *argv,
                            int magic, JSValue *func_data) {

    int *pid = (int *)JS_GetOpaque2(ctx, func_data[0], QJS_CORE_CLASS_ID_JS_FUNC_CALLBACK);
    if (!pid) return JS_EXCEPTION;
    int callbackId = *pid;
    auto wrapper = reinterpret_cast<QuickJSWrapper*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    JSValue value = wrapper->jsFuncCall(callbackId, this_obj, argc, argv);
    return value;
}

static void initJSFuncCallback(JSContext *ctx) {
    // JSFuncCallback class
    JS_NewClass(JS_GetRuntime(ctx), QJS_CORE_CLASS_ID_JS_FUNC_CALLBACK, &js_func_callback_class);
}

// js module
static char *jsModuleNormalizeFunc(JSContext *ctx, const char *module_base_name,
                                   const char *module_name, void *opaque) {
    auto wrapper = reinterpret_cast<const QuickJSWrapper*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    auto env = wrapper->jniEnv;

    // module loader handle.
    jobject moduleLoader = env->CallObjectMethod(wrapper->jniThiz, env->GetMethodID(wrapper->quickjsContextClass, "getModuleLoader", "()Lcom/whl/quickjs/wrapper/ModuleLoader;"));
    if (moduleLoader == nullptr) {
        JS_ThrowInternalError(ctx, "Failed to load module, the ModuleLoader can not be null!");
        return nullptr;
    }
    jmethodID moduleNormalizeName = env->GetMethodID(wrapper->moduleLoaderClass, "moduleNormalizeName", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");

    jstring j_module_base_name =  env->NewStringUTF(module_base_name);
    jstring j_module_name = env->NewStringUTF(module_name);
    auto result = env->CallObjectMethod(moduleLoader, moduleNormalizeName, j_module_base_name, j_module_name);
    if (result == nullptr) {
        throwJSException(env, "Failed to load module, cause moduleName was null!");
        return nullptr;
    }

    env->DeleteLocalRef(j_module_base_name);
    env->DeleteLocalRef(j_module_name);
    env->DeleteLocalRef(moduleLoader);

    /* 引擎用 js_free(ctx, ret) 释放返回值，必须用 js_malloc 分配 */
    const char *tmp = env->GetStringUTFChars((jstring) result, nullptr);
    if (!tmp) {
        env->DeleteLocalRef(result);
        return nullptr;
    }
    char *ret = (char *)js_malloc(ctx, strlen(tmp) + 1);
    if (!ret) {
        env->ReleaseStringUTFChars((jstring) result, tmp);
        env->DeleteLocalRef(result);
        return nullptr;
    }
    strcpy(ret, tmp);
    env->ReleaseStringUTFChars((jstring) result, tmp);
    env->DeleteLocalRef(result);
    return ret;
}

static int json_module_init(JSContext *ctx, JSModuleDef *m)
{
    JSValue val;
    val = JS_GetModulePrivateValue(ctx, m);
    JS_SetModuleExport(ctx, m, "default", val);
    return 0;
}

static JSModuleDef *create_json_module(JSContext *ctx, const char *module_name, JSValue val)
{
    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, json_module_init);
    if (!m) {
        JS_FreeValue(ctx, val);
        return NULL;
    }
    /* only export the "default" symbol which will contain the JSON object */
    JS_AddModuleExport(ctx, m, "default");
    JS_SetModulePrivateValue(ctx, m, val);
    return m;
}

/* return > 0 if the attributes indicate a JSON module */
static int js_module_test_json(JSContext *ctx, JSValueConst attributes)
{
    JSValue str;
    const char *cstr;
    size_t len;
    BOOL res;

    if (JS_IsUndefined(attributes))
        return FALSE;
    str = JS_GetPropertyStr(ctx, attributes, "type");
    if (!JS_IsString(str)) {
        JS_FreeValue(ctx, str);
        return FALSE;
    }
    cstr = JS_ToCStringLen(ctx, &len, str);
    JS_FreeValue(ctx, str);
    if (!cstr)
        return FALSE;
    /* XXX: raise an error if unknown type ? */
    if (len == 4 && !memcmp(cstr, "json", len)) {
        res = 1;
    } else if (len == 5 && !memcmp(cstr, "json5", len)) {
        res = 2;
    } else {
        res = 0;
    }
    JS_FreeCString(ctx, cstr);
    return res;
}

static int js_module_check_attributes(JSContext *ctx, void *opaque,
                                      JSValueConst attributes)
{
    JSPropertyEnum *tab;
    uint32_t i, len;
    int ret;
    const char *cstr;
    size_t cstr_len;

    if (JS_GetOwnPropertyNames(ctx, &tab, &len, attributes, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK))
        return -1;
    ret = 0;
    for(i = 0; i < len; i++) {
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

static JSModuleDef *
jsModuleLoaderFunc(JSContext *ctx, const char *module_name, void *opaque, JSValueConst attributes) {
    auto wrapper = reinterpret_cast<const QuickJSWrapper*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    auto env = wrapper->jniEnv;
    auto arg = env->NewStringUTF(module_name);

    // module loader handle.
    jobject moduleLoader = env->CallObjectMethod(wrapper->jniThiz, env->GetMethodID(wrapper->quickjsContextClass, "getModuleLoader", "()Lcom/whl/quickjs/wrapper/ModuleLoader;"));
    if (moduleLoader == nullptr) {
        JS_ThrowInternalError(ctx, "Failed to load module, the ModuleLoader can not be null!");
        return (JSModuleDef *) JS_VALUE_GET_PTR(JS_EXCEPTION);
    }

    bool isBytecodeModule = env->CallBooleanMethod(moduleLoader, env->GetMethodID(wrapper->moduleLoaderClass, "isBytecodeMode", "()Z"));

    void *m;
    if (isBytecodeModule) {
        jmethodID getModuleBytecode = env->GetMethodID(wrapper->moduleLoaderClass, "getModuleBytecode", "(Ljava/lang/String;)[B");

        auto bytecode = (jbyteArray) (env->CallObjectMethod(moduleLoader, getModuleBytecode, arg));
        if (bytecode == nullptr) {
            throwJSException(env, "Failed to load module, cause bytecode was null!");
            return nullptr;
        }
        auto fg = JS_READ_OBJ_BYTECODE;
        if (has_suffix(module_name, ".json") || has_suffix(module_name, ".json5")) {
            fg = 0;
        }
        const auto buffer = env->GetByteArrayElements(bytecode, nullptr);
        const auto bufferLength = env->GetArrayLength(bytecode);
        auto obj = JS_ReadObject(ctx, reinterpret_cast<const uint8_t*>(buffer), bufferLength, fg);
        env->ReleaseByteArrayElements(bytecode, buffer, JNI_ABORT);

        if (JS_IsException(obj)) {
            throwJSException(env, ctx);
            return (JSModuleDef *) JS_VALUE_GET_PTR(JS_EXCEPTION);
        }
        if (has_suffix(module_name, ".json") || has_suffix(module_name, ".json5")) {
            m = create_json_module(ctx, module_name, obj);
            env->DeleteLocalRef(bytecode);
        } else {
            if (JS_ResolveModule(ctx, obj)) {
                JS_FreeValue(ctx, obj);
                throwJSException(env, "Failed to resolve JS module");
                return nullptr;
            }
            js_module_set_import_meta(ctx, obj, FALSE, FALSE, module_name);

            m = JS_VALUE_GET_PTR(obj);
            JS_FreeValue(ctx, obj);
            env->DeleteLocalRef(bytecode);
        }
    } else {
        jmethodID getModuleStringCode = env->GetMethodID(wrapper->moduleLoaderClass, "getModuleStringCode", "(Ljava/lang/String;)Ljava/lang/String;");

        auto result = env->CallObjectMethod(moduleLoader, getModuleStringCode, arg);
        if (result == nullptr) {
            throwJSException(env, "Failed to load module, cause string code was null!");
            return nullptr;
        }

        const auto script = env->GetStringUTFChars((jstring)(result), JNI_FALSE);
        int scriptLen = env->GetStringUTFLength((jstring) result);
        int res = js_module_test_json(ctx, attributes);
        if (has_suffix(module_name, ".json") || res > 0) {
            /* compile as JSON or JSON5 depending on "type" */
            JSValue val;
            int flags;
            if (res == 2)
                flags = JS_PARSE_JSON_EXT;
            else
                flags = 0;
            val = JS_ParseJSON2(ctx, script, scriptLen, module_name, flags);
            env->ReleaseStringUTFChars((jstring)(result), script);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, val);
                throwJSException(env, ctx);
                return (JSModuleDef *) JS_VALUE_GET_PTR(JS_EXCEPTION);
            }
            m = create_json_module(ctx, module_name, val);
        } else {
            JSValue func_val = JS_Eval(ctx, script, scriptLen, module_name,
                                       JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
            env->ReleaseStringUTFChars((jstring) (result), script);
            if (JS_IsException(func_val)) {
                JS_FreeValue(ctx, func_val);
                throwJSException(env, ctx);
                return (JSModuleDef *) JS_VALUE_GET_PTR(JS_EXCEPTION);
            }
            m = JS_VALUE_GET_PTR(func_val);
            JS_FreeValue(ctx, func_val);
        }
    }

    env->DeleteLocalRef(arg);
    env->DeleteLocalRef(moduleLoader);
    return (JSModuleDef *) m;
}

static bool throwIfUnhandledRejections(QuickJSWrapper *wrapper, JSContext *ctx) {
    string error;
    while (!wrapper->unhandledRejections.empty()) {
        JSValueConst reason = wrapper->unhandledRejections.front();
        error += getJSErrorStr(ctx, reason);
        error += "\n";
        JS_FreeValue(ctx, reason);
        wrapper->unhandledRejections.pop();
    }

    bool is_error = !error.empty();
    if (is_error) {
        error = "UnhandledPromiseRejectionException: " + error;
        throwJSException(wrapper->jniEnv, error.c_str());
    }
    return is_error;
}

/*
 * 停止运行时（原 uv__stop + qjs_stop 的合并实现）。
 * 移除 libuv 后不再需要异步通知，直接设置 success 标记即可。
 */
/*
 * 执行所有待处理的 JS 微任务（Promise 回调等），出错时抛 JS 异常。
 * 返回 false 表示执行出错或存在未处理的 Promise 拒绝。
 * 原实现拆为 qjs_stop / qjs__execute_jobs / executePendingJobLoop 三个函数，
 * 不依赖任何运行时级共享状态，全部使用调用方局部变量。
 */
static bool executePendingJobLoop(JNIEnv *env, JSRuntime *rt, JSContext *ctx) {
    /* 提前终止：若 JVM 层已有异常，清除后返回 */
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    /* 循环执行待处理任务，直到没有任务或出错 */
    for (;;) {
        int err = JS_ExecutePendingJob(rt, NULL);
        if (err <= 0) {
            if (err < 0) {
                string error = getJSErrorStr(ctx);
                throwJSException(env, error.c_str());
                return false;
            }
            break;
        }
    }
    /* 检查未处理的 Promise 拒绝 */
    return !throwIfUnhandledRejections(reinterpret_cast<QuickJSWrapper *>(JS_GetRuntimeOpaque(rt)), ctx);
}
static void promiseRejectionTracker(JSContext *ctx, JSValueConst promise,
                                    JSValueConst reason, BOOL is_handled, void *opaque) {
    auto unhandledRejections = static_cast<queue<JSValue> *>(opaque);
    if (!is_handled) {
        unhandledRejections->push(JS_DupValue(ctx, reason));
    } else {
        if (!unhandledRejections->empty()) {
            JSValueConst rej = unhandledRejections->front();
            JS_FreeValue(ctx, rej);
            unhandledRejections->pop();
        }
    }
}

QuickJSWrapper::QuickJSWrapper(JNIEnv *env, jobject thiz, JSRuntime *rt) {
    jniEnv = env;
    runtime = rt;
    jniThiz = jniEnv->NewGlobalRef(thiz);

    JS_SetModuleLoaderFunc2(runtime, jsModuleNormalizeFunc, jsModuleLoaderFunc, js_module_check_attributes, nullptr);

    JS_SetHostPromiseRejectionTracker(runtime, promiseRejectionTracker, &unhandledRejections);
    /* 引擎级扩展模块（Buffer/zlib/fs/crypto/webcrypto/sqlite/wasm/path/
     * HTML/URL/TextCodec + JS 层封装库）与 wasm3 环境
     * 均已由 JS_NewContext 内部的 qjs_ext_install_all() 自动完成注册，
     * JNI 层不再负责模块注册与 wasm3 生命周期管理。 */
    context = JS_NewContext(runtime);
    if (!context) {
        throwJavaException(env, "java/lang/RuntimeException",
                           "Failed to create QuickJS context (extension modules registration failed)");
        return;
    }

    /* wasm3 环境已由 JS_NewContext 内部的
     * qjs_ext_install_all() 自动创建，JNI 层无需再补充任何状态 */

    JS_SetRuntimeOpaque(runtime, this);
    initJSFuncCallback(context);

    const char *getOwnPropertyNames = "Object.getOwnPropertyNames";
    ownPropertyNames = JS_Eval(context, getOwnPropertyNames, strlen(getOwnPropertyNames), getOwnPropertyNames, JS_EVAL_TYPE_GLOBAL);

    objectClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Object")));
    booleanClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Boolean")));
    integerClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Integer")));
    longClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Long")));
    doubleClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/Double")));
    stringClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("java/lang/String")));
    jsObjectClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSObject")));
    jsArrayClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSArray")));
    jsFunctionClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSFunction")));
    jsCallFunctionClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSCallFunction")));
    quickjsContextClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/QuickJSContext")));
    moduleLoaderClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/ModuleLoader")));
    creatorClass = (jclass)(jniEnv->NewGlobalRef(jniEnv->FindClass("com/whl/quickjs/wrapper/JSObjectCreator")));
        byteArrayClass = (jclass) jniEnv->NewGlobalRef(env->FindClass("[B"));

    booleanValueOf = jniEnv->GetStaticMethodID(booleanClass, "valueOf", "(Z)Ljava/lang/Boolean;");
    integerValueOf = jniEnv->GetStaticMethodID(integerClass, "valueOf", "(I)Ljava/lang/Integer;");
    longValueOf = jniEnv->GetStaticMethodID(longClass, "valueOf", "(J)Ljava/lang/Long;");
    doubleValueOf = jniEnv->GetStaticMethodID(doubleClass, "valueOf", "(D)Ljava/lang/Double;");

    booleanGetValue = jniEnv->GetMethodID(booleanClass, "booleanValue", "()Z");
    integerGetValue = jniEnv->GetMethodID(integerClass, "intValue", "()I");
    longGetValue = jniEnv->GetMethodID(longClass, "longValue", "()J");
    doubleGetValue = jniEnv->GetMethodID(doubleClass, "doubleValue", "()D");
    jsObjectGetValue = jniEnv->GetMethodID(jsObjectClass, "getPointer", "()J");

    callFunctionBackM = jniEnv->GetMethodID(quickjsContextClass, "callFunctionBack", "(I[Ljava/lang/Object;)Ljava/lang/Object;");
    removeCallFunctionM = jniEnv->GetMethodID(quickjsContextClass, "removeCallFunction", "(I)V");
    callFunctionHashCodeM = jniEnv->GetMethodID(objectClass, "hashCode", "()I");
    creatorM = jniEnv->GetMethodID(quickjsContextClass, "getCreator", "()Lcom/whl/quickjs/wrapper/JSObjectCreator;");
    newObjectM = jniEnv->GetMethodID(creatorClass, "newObject",
                                     "(Lcom/whl/quickjs/wrapper/QuickJSContext;J)Lcom/whl/quickjs/wrapper/JSObject;");
    newArrayM = jniEnv->GetMethodID(creatorClass, "newArray",
                                    "(Lcom/whl/quickjs/wrapper/QuickJSContext;J)Lcom/whl/quickjs/wrapper/JSArray;");
    newFunctionM = jniEnv->GetMethodID(creatorClass, "newFunction",
                                       "(Lcom/whl/quickjs/wrapper/QuickJSContext;JJI)Lcom/whl/quickjs/wrapper/JSFunction;");
}

QuickJSWrapper::~QuickJSWrapper() {
    if (!context) {
        if (runtime) {
            JS_FreeRuntime(runtime);
        }
        return;
    }

    /* wasm3 环境的释放已下沉到 JS_FreeRuntime 内部
     * （qjs_ext_free 钩子，在所有 finalizer 执行完毕后运行），
     * JNI 层只需按顺序释放 JS 引用、context 与 runtime。 */
    JS_FreeValue(context, ownPropertyNames);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);

    jniEnv->DeleteGlobalRef(jniThiz);
    jniEnv->DeleteGlobalRef(objectClass);
    jniEnv->DeleteGlobalRef(doubleClass);
    jniEnv->DeleteGlobalRef(integerClass);
    jniEnv->DeleteGlobalRef(longClass);
    jniEnv->DeleteGlobalRef(booleanClass);
    jniEnv->DeleteGlobalRef(stringClass);
    jniEnv->DeleteGlobalRef(jsObjectClass);
    jniEnv->DeleteGlobalRef(jsArrayClass);
    jniEnv->DeleteGlobalRef(jsFunctionClass);
    jniEnv->DeleteGlobalRef(jsCallFunctionClass);
    jniEnv->DeleteGlobalRef(moduleLoaderClass);
    jniEnv->DeleteGlobalRef(quickjsContextClass);
    jniEnv->DeleteGlobalRef(creatorClass);
        jniEnv->DeleteGlobalRef(byteArrayClass);
}

jobject QuickJSWrapper::toJavaObject(JNIEnv *env, jobject thiz, JSValueConst this_obj, JSValueConst value) const{
    jobject result = nullptr;
    switch (JS_VALUE_GET_NORM_TAG(value)) {
        case JS_TAG_EXCEPTION: {
            result = nullptr;
            break;
        }

        case JS_TAG_NULL: {
            /* JS null → Java null，语义明确 */
            result = nullptr;
            break;
        }

        case JS_TAG_UNDEFINED: {
            /* JS undefined → Java null
             * Java 无 undefined 概念，统一映射为 null。
             * 注意：round-trip 时 Java null → JS_NULL（见 toJSValue），
             * 因此 undefined 经 Java 中转后会变为 null，
             * 这是 Java 类型系统的固有限制。 */
            result = nullptr;
            break;
        }

        case JS_TAG_STRING:
        case JS_TAG_STRING_ROPE:{
            result = toJavaString(env, value);
            break;
        }

        case JS_TAG_BOOL: {
            jvalue v;
            v.z = static_cast<jboolean>(JS_VALUE_GET_BOOL(value));
            result = env->CallStaticObjectMethodA(booleanClass, booleanValueOf, &v);
            break;
        }

        case JS_TAG_INT: {
            jvalue v;
            v.j = static_cast<jint>(JS_VALUE_GET_INT(value));
            result = env->CallStaticObjectMethodA(integerClass, integerValueOf, &v);
            break;
        }

        case JS_TAG_BIG_INT:
        case JS_TAG_SHORT_BIG_INT: {
            int64_t e;
            if (JS_ToBigInt64(context, &e, value) != 0) {
                result = nullptr;
            } else {
                jvalue v;
                v.j = e;
                result = env->CallStaticObjectMethodA(longClass, longValueOf, &v);
            }
            JS_FreeValue(context, value);
            break;
        }

        case JS_TAG_FLOAT64: {
            jvalue v;
            double d = JS_VALUE_GET_FLOAT64(value);
            bool isInteger = floor(d) == d;
            if (isInteger) {
                v.j = static_cast<jlong>(d);
                result = env->CallStaticObjectMethodA(longClass, longValueOf, &v);
            } else {
                v.d = static_cast<jdouble>(d);
                result = env->CallStaticObjectMethodA(doubleClass, doubleValueOf, &v);
            }
            break;
        }

        case JS_TAG_OBJECT: {
            auto value_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(value));
            jobject creatorObj = env->CallObjectMethod(thiz, creatorM);
            if (JS_IsFunction(context, value)) {
                auto obj_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(this_obj));
                result = env->CallObjectMethod(creatorObj, newFunctionM, thiz, value_ptr, obj_ptr, JS_VALUE_GET_TAG(this_obj));
            } else if (JS_IsArray(context, value)) {
                result = env->CallObjectMethod(creatorObj, newArrayM, thiz, value_ptr);
            } else if (JS_IsArrayBuffer(value)) {
                size_t byteLength = 0;
                uint8_t *buffer = JS_GetArrayBuffer(context, &byteLength, value);
                if (buffer && byteLength > 0) {
                    jbyteArray byteArray = env->NewByteArray(byteLength);
                    if (byteArray) {
                        void *elementsPtr = env->GetPrimitiveArrayCritical(byteArray, nullptr);
                        if (elementsPtr) {
                            jbyte *elements = reinterpret_cast<jbyte *>(elementsPtr);
                            memcpy(elements, buffer, byteLength);
                            env->ReleasePrimitiveArrayCritical(byteArray, elements, 0);
                            result = byteArray;
                        }
                    }
                }
                JS_FreeValue(context, value);
                env->DeleteLocalRef(creatorObj);
                break;
            } else if (JS_IsError(context, value)) {
                /* JS Error 对象 → Java QuickJSException
                 * 提取 message 和 stack，拼接为异常信息，
                 * jsError=true 标记来源为 JS 引擎。 */
                std::string errorMsg;

                JSValue nameVal = JS_GetPropertyStr(context, value, "name");
                const char *nameStr = JS_ToCString(context, nameVal);
                if (nameStr && *nameStr) {
                    errorMsg += nameStr;
                    errorMsg += ": ";
                }
                JS_FreeCString(context, nameStr);
                JS_FreeValue(context, nameVal);

                JSValue messageVal = JS_GetPropertyStr(context, value, "message");
                const char *messageStr = JS_ToCString(context, messageVal);
                if (messageStr) {
                    errorMsg += messageStr;
                }
                JS_FreeCString(context, messageStr);
                JS_FreeValue(context, messageVal);

                JSValue stackVal = JS_GetPropertyStr(context, value, "stack");
                if (!JS_IsUndefined(stackVal) && !JS_IsNull(stackVal)) {
                    const char *stackStr = JS_ToCString(context, stackVal);
                    if (stackStr && *stackStr) {
                        errorMsg += "\n";
                        errorMsg += stackStr;
                    }
                    JS_FreeCString(context, stackStr);
                }
                JS_FreeValue(context, stackVal);

                jclass exClass = env->FindClass("com/whl/quickjs/wrapper/QuickJSException");
                jmethodID exInit = env->GetMethodID(exClass, "<init>", "(Ljava/lang/String;Z)V");
                jstring jMsg = env->NewStringUTF(errorMsg.c_str());
                result = env->NewObject(exClass, exInit, jMsg, JNI_TRUE);
                env->DeleteLocalRef(jMsg);
                env->DeleteLocalRef(exClass);

                JS_FreeValue(context, value);
                env->DeleteLocalRef(creatorObj);
                break;
            } else {
                result = env->CallObjectMethod(creatorObj, newObjectM, thiz, value_ptr);
            }
            env->DeleteLocalRef(creatorObj);
            break;
        }

        default:
            result = nullptr;
            break;
    }

    return result;
}


jobject QuickJSWrapper::getGlobalObject(JNIEnv *env, jobject thiz) const {
    JSValue value = JS_GetGlobalObject(context);

    auto value_ptr = reinterpret_cast<jlong>(JS_VALUE_GET_PTR(value));
    jobject result = env->CallObjectMethod(env->CallObjectMethod(thiz, creatorM), newObjectM, thiz, value_ptr);

    JS_FreeValue(context, value);
    return result;
}

jobject QuickJSWrapper::getProperty(JNIEnv *env, jobject thiz, jlong value, jstring name) {
    JSValue jsObject = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));

    const char *propsName = env->GetStringUTFChars(name, JNI_FALSE);
    JSValue propsValue = JS_GetPropertyStr(context, jsObject, propsName);
    env->ReleaseStringUTFChars(name, propsName);
    if (JS_IsException(propsValue)) {
        throwJSException(env, context);
        return nullptr;
    }

    return toJavaObject(env, thiz, jsObject, propsValue);
}

jobject QuickJSWrapper::call(JNIEnv *env, jobject thiz, jlong func, jlong this_obj,
                             jint this_obj_tag, jobjectArray args) {
    int argc = env->GetArrayLength(args);
    vector<JSValue> arguments;
    vector<JSValue> freeArguments;
    // 预分配容量，减少动态扩容
    arguments.reserve(argc);
    freeArguments.reserve(argc);
    for (int numArgs = 0; numArgs < argc && !env->ExceptionCheck(); numArgs++) {
        jobject arg = env->GetObjectArrayElement(args, numArgs);
        auto jsArg = toJSValue(env, thiz, arg);
        if (JS_IsException(jsArg)) {
            /* 只释放 freeArguments（owned 类型），不释放 arguments（含借用引用的 jsObject） */
            for (JSValue arg : freeArguments) {
                JS_FreeValue(context, arg);
            }
            env->DeleteLocalRef(arg);
            return nullptr;
        }

        /* toJSValue 现在对所有类型（含 jsObjectClass）都返回新引用，
         * 因此所有参数都需要在使用完后 free。
         * 之前 jsObjectClass 返回借用引用（JS_MKPTR），不需要 free，
         * 但这导致 set/arrayAdd/jsFuncCall 中的引用计数管理不一致。
         * 现在 toJSValue 统一返回新引用，所有参数都加入 freeArguments。 */
        if (env->IsInstanceOf(arg, stringClass) || env->IsInstanceOf(arg, doubleClass) ||
            env->IsInstanceOf(arg, integerClass) || env->IsInstanceOf(arg, longClass) ||
            env->IsInstanceOf(arg, booleanClass) || env->IsInstanceOf(arg, jsCallFunctionClass) ||
            env->IsInstanceOf(arg, jsObjectClass) ||
            env->IsInstanceOf(arg, byteArrayClass))  {
            freeArguments.push_back(jsArg);
        }

        env->DeleteLocalRef(arg);
        arguments.push_back(jsArg);
    }

    JSValue jsObj = JS_MKPTR(this_obj_tag, reinterpret_cast<void *>(this_obj));
    JSValue jsFunc = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(func));

    JSValue ret = JS_Call(context, jsFunc, jsObj, arguments.size(), arguments.data());
    if (JS_IsException(ret)) {
        JS_FreeValue(context, ret);
        for (JSValue argument : freeArguments) {
            JS_FreeValue(context, argument);
        }
        throwJSException(env, context);
        return nullptr;
    }

    for (JSValue argument : freeArguments) {
        JS_FreeValue(context, argument);
    }

    // release vector by swap.
    vector<JSValue>().swap(arguments);
    vector<JSValue>().swap(freeArguments);

    if (!executePendingJobLoop(env, runtime, context)) {
        JS_FreeValue(context, ret);
        return nullptr;
    }

    if (JS_IsPromise(context, ret)) {
        JSValue thenRet = JS_PromiseResult(context, ret);
        JS_FreeValue(context, ret);
        return toJavaObject(env, thiz, jsObj, thenRet);
    }

    return toJavaObject(env, thiz, jsObj, ret);
}

jstring QuickJSWrapper::jsonStringify(JNIEnv *env, jlong value) const {
    JSValue obj = JS_JSONStringify(context, JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value)), JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(obj)){
        throwJSException(env, context);
        return nullptr;
    }

    return toJavaString(env, obj);
}

jint QuickJSWrapper::length(JNIEnv *env, jlong value) const {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));

    JSValue length = JS_GetPropertyStr(context, jsObj, "length");
    if (JS_IsException(length)) {
        throwJSException(env, context);
        return -1;
    }

    jint result;
    if (JS_ToInt32(context, &result, length)) {
        JS_FreeValue(context, length);
        throwJSException(env, "Failed to get length");
        return -1;
    }
    JS_FreeValue(context, length);

    return result;
}

jstring QuickJSWrapper::toJavaString(JNIEnv *env, JSValue value) const {
    jstring result;
#ifdef IS_ANDROID
    const char* string = JS_ToCString(context, value);
    result = env->NewStringUTF(string);
    JS_FreeCString(context, string);
    // JSString 类型的 JSValue 需要手动释放掉，不然会泄漏
    JS_FreeValue(context, value);
#else
    // 这里需要注意，JVM 平台下 NewStringUTF 方法对部分 unicode 的转换有问题，会出现乱码，换了另一种方式解决。
    const char *str;
    size_t len;
    str = JS_ToCStringLen(context, &len, value);

    jbyteArray jba = env->NewByteArray(len);
    env->SetByteArrayRegion(jba, 0, len, reinterpret_cast<const jbyte *>(str));

    result = static_cast<jstring>(env->NewObject(stringClass,
                                                 env->GetMethodID(stringClass, "<init>", "([B)V"),
                                                 jba));

    JS_FreeCString(context, str);
    env->DeleteLocalRef(jba);
    // JSString 类型的 JSValue 需要手动释放掉，不然会泄漏
    JS_FreeValue(context, value);
#endif

    return result;
}

jobject QuickJSWrapper::get(JNIEnv *env, jobject thiz, jlong value, jint index) {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JSValue child = JS_GetPropertyUint32(context, jsObj, index);

    return toJavaObject(env, thiz, jsObj, child);
}

void QuickJSWrapper::set(JNIEnv *env, jobject thiz, jlong this_obj, jobject value, jint index) {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(this_obj));
    /* toJSValue 现在对所有类型统一返回新引用，
     * JS_SetPropertyUint32 会 steal 这个新引用，无需 DupValue/FreeValue。
     * 之前的 if/else 分支对 jsObjectClass 的借用引用执行 FreeValue，
     * 导致引用计数下溢 → use-after-free → 崩溃。 */
    JSValue child = toJSValue(env, thiz, value);
    JS_SetPropertyUint32(context, jsObj, index, child);
}

void
QuickJSWrapper::setProperty(JNIEnv *env, jobject thiz, jlong this_obj, jstring name, jobject value) const {
    const char* propName = env->GetStringUTFChars(name, JNI_FALSE);
    JSValue propValue = toJSValue(env, thiz, value);
    /* toJSValue 现在对所有类型统一返回新引用（含 jsObjectClass 已 DupValue），
     * 不再需要在此处手动 DupValue。
     * JS_SetPropertyStr 会 steal 这个新引用。 */
    if (env->IsInstanceOf(value, jsCallFunctionClass)) {
        // 通过 JS_NewCFunctionData 创建的 fn 对象的 name 属性值被定义为 Empty 了，
        // 这里需要额外定义下，不然 js 层拿到的 fn.name 的值为空.
        JSAtom name_atom = JS_NewAtom(context, propName);
        JSAtom name_atom_key = JS_NewAtom(context, "name");
        JS_DefinePropertyValue(context, propValue, name_atom_key,
                               JS_AtomToString(context, name_atom), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(context, name_atom);
        JS_FreeAtom(context, name_atom_key);
    }

    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(this_obj));
    JS_SetPropertyStr(context, jsObj, propName, propValue);

    env->ReleaseStringUTFChars(name, propName);
}

JSValue QuickJSWrapper::jsFuncCall(int callback_id, JSValueConst this_val, int argc, JSValueConst *argv){
    jobjectArray javaArgs = jniEnv->NewObjectArray((jsize)argc, objectClass, nullptr);
    for (int i = 0; i < argc; i++) {
        JSValue v = JS_DupValue(context, argv[i]);
        auto java_arg = toJavaObject(jniEnv, jniThiz, this_val, v);
        jniEnv->SetObjectArrayElement(javaArgs, (jsize)i, java_arg);
        jniEnv->DeleteLocalRef(java_arg);
    }
    auto result = jniEnv->CallObjectMethod(jniThiz, callFunctionBackM, callback_id, javaArgs);

    if (jniEnv->ExceptionCheck()) {     // Java 层抛了异常
        jthrowable jEx = jniEnv->ExceptionOccurred();
        jniEnv->ExceptionClear();       // 清掉 JVM 异常，否则 JNI 会崩

        jclass cls = jniEnv->GetObjectClass(jEx);
        jmethodID getMsg = jniEnv->GetMethodID(cls, "getMessage", "()Ljava/lang/String;");
        jstring jMsg = (jstring)jniEnv->CallObjectMethod(jEx, getMsg);

        const char *cMsg = "Java exception";
        if (jMsg != nullptr) {
            cMsg = jniEnv->GetStringUTFChars(jMsg, nullptr);
        }

        // 把异常抛回 QuickJS
        JS_ThrowTypeError(context, "Java: %s", cMsg);

        // 释放资源
        if (jMsg != nullptr) {
            jniEnv->ReleaseStringUTFChars(jMsg, cMsg);
            jniEnv->DeleteLocalRef(jMsg);
        }
        jniEnv->DeleteLocalRef(jEx);
        jniEnv->DeleteLocalRef(javaArgs);
        jniEnv->DeleteLocalRef(result);
        return JS_EXCEPTION;
    }

    jniEnv->DeleteLocalRef(javaArgs);

    JSValue jsValue = toJSValue(jniEnv, jniThiz, result);

    jniEnv->DeleteLocalRef(result);
    return jsValue;
}

void QuickJSWrapper::removeCallFunction(int callback_id) const {
    if (jniEnv->ExceptionCheck()) {
        return;
    }

    jniEnv->CallVoidMethod(jniThiz, removeCallFunctionM, callback_id);
}

JSValue QuickJSWrapper::toJSValue(JNIEnv *env, jobject thiz, jobject value) const {
    if (value == nullptr) {
        /* Java null → JS null（非 undefined）
         * 在 JS 中 null === undefined 为 false，
         * Java 的 null 语义上更接近 JS 的 null（有值为空），
         * 而非 undefined（未定义/未传参）。 */
        return JS_NULL;
    }

    JSValue result;
    if (env->IsInstanceOf(value, stringClass)) {
        const auto s = env->GetStringUTFChars((jstring)(value), JNI_FALSE);
        result = JS_NewString(context, s);
        env->ReleaseStringUTFChars((jstring)(value), s);
    } else if (env->IsInstanceOf(value, doubleClass)) {
        result = JS_NewFloat64(context, env->CallDoubleMethod(value, doubleGetValue));
    } else if (env->IsInstanceOf(value, integerClass)) {
        result = JS_NewInt32(context, env->CallIntMethod(value, integerGetValue));
    } else if(env->IsInstanceOf(value, longClass)) {
        jlong l_val = env->CallLongMethod(value, longGetValue);
        if (l_val > MAX_SAFE_INTEGER || l_val < -MAX_SAFE_INTEGER) {
            result = JS_NewBigInt64(context, l_val);
        } else {
            result = JS_NewInt64(context, l_val);
        }
    } else if (env->IsInstanceOf(value, booleanClass)) {
        result = JS_NewBool(context, env->CallBooleanMethod(value, booleanGetValue));
    } else if (env->IsInstanceOf(value, byteArrayClass)) {
        jbyteArray bytes = static_cast<jbyteArray>(value);
        jbyte* byteData = env->GetByteArrayElements(bytes, nullptr);
        jsize length = env->GetArrayLength(bytes);
        result = JS_NewArrayBufferCopy(context, reinterpret_cast<uint8_t*>(byteData), length);
        env->ReleaseByteArrayElements(bytes, byteData, JNI_ABORT);
    } else if (env->IsInstanceOf(value, jsObjectClass)) {
        /* JS_MKPTR 创建的是借用引用（不增加引用计数）。
         * 为保持 toJSValue 对所有类型统一返回"新引用"的语义，
         * 这里必须 JS_DupValue，使调用方可以统一地 free 或 steal。
         * 之前的代码未 DupValue，导致 set/arrayAdd 中 JS_FreeValue
         * 对借用引用减计数，引发引用计数下溢 → use-after-free → 崩溃。 */
        JSValue tmp = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(env->CallLongMethod(value, jsObjectGetValue)));
        result = JS_DupValue(context, tmp);
    } else if (env->IsInstanceOf(value, jsCallFunctionClass)) {
        // 这里的 obj 是用来获取 JSFuncCallback 对象的
        JSValue obj = JS_NewObjectClass(context, QJS_CORE_CLASS_ID_JS_FUNC_CALLBACK);
        int *callbackId = new int(jniEnv->CallIntMethod(value, callFunctionHashCodeM));
        JS_SetOpaque(obj, callbackId);
        result = JS_NewCFunctionData(context, jsFnCallback, 1, 0, 1, &obj);
        // JS_NewCFunctionData 有 dupValue obj，这里需要对 obj 计数减一，保持计数平衡
        JS_FreeValue(context, obj);
    } else {
        auto classType = env->GetObjectClass(value);
        const auto typeName = getJavaName(env, classType);
        env->DeleteLocalRef(classType);
        // Throw an exception for unsupported argument type.
        throwJavaException(env, "java/lang/IllegalArgumentException", "Unsupported Java type %s",
                           typeName.c_str());
        result = JS_EXCEPTION;
    }

    return result;
}

void QuickJSWrapper::freeValue(jlong value) const {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_FreeValue(context, jsObj);
}

void QuickJSWrapper::dupValue(jlong value) const {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_DupValue(context, jsObj);
}

/**
 * @deprecated
 * See {@link freeValue(String)}
 * @param value
 */
void QuickJSWrapper::freeDupValue(jlong value) const {
    JSValue jsObj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JS_FreeValue(context, jsObj);
}

jobject QuickJSWrapper::parseJSON(JNIEnv *env, jobject thiz, jstring json) {
    const char *c_json = env->GetStringUTFChars(json, JNI_FALSE);
    auto jsonObj = JS_ParseJSON2(context, c_json, strlen(c_json), "parseJSON.js", JS_PARSE_JSON_EXT);
    if (JS_IsException(jsonObj)) {
        throwJSException(env, context);
        return nullptr;
    }

    JSValue jsObj = JS_UNDEFINED;
    jobject result = toJavaObject(env, thiz, jsObj, jsonObj);
    env->ReleaseStringUTFChars(json, c_json);
    return result;
}

jobject QuickJSWrapper::evaluate(JNIEnv *env, jobject thiz, jstring script, jstring file_name) {
    const char *c_script = env->GetStringUTFChars(script, JNI_FALSE);
    const char *c_file_name = env->GetStringUTFChars(file_name, JNI_FALSE);

    JSValue result = JS_Eval(context, c_script, strlen(c_script), c_file_name, JS_EVAL_TYPE_GLOBAL);
    env->ReleaseStringUTFChars(script, c_script);
    env->ReleaseStringUTFChars(file_name, c_file_name);
    if (JS_IsException(result)) {
        throwJSException(env, context);
        return nullptr;
    }

    if (!executePendingJobLoop(env, runtime, context)) {
        JS_FreeValue(context, result);
        return nullptr;
    }

    return toJavaObject(env, thiz, JS_UNDEFINED, result);
}

jbyteArray QuickJSWrapper::compile(JNIEnv *env, jstring source, jstring file_name, jboolean isModule) const {
    const auto sourceCode = env->GetStringUTFChars(source, JNI_FALSE);
    const auto fileName = env->GetStringUTFChars(file_name, JNI_FALSE);
    auto eval_flags =  JS_EVAL_FLAG_COMPILE_ONLY;
    if (isModule)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;
    //auto compiled = JS_Eval(context, sourceCode, strlen(sourceCode), fileName, eval_flags);
    JSValue compiled;
    if (has_suffix(fileName, ".json") || has_suffix(fileName, ".json5")) {
        int flags;
        if (has_suffix(fileName, ".json5"))
            flags = JS_PARSE_JSON_EXT;
        else
            flags = 0;
        compiled = JS_ParseJSON2(context, sourceCode, strlen(sourceCode), fileName, flags);
    } else {
        compiled = JS_Eval(context, sourceCode, strlen(sourceCode), fileName, eval_flags);
    }
    env->ReleaseStringUTFChars(source, sourceCode);
    env->ReleaseStringUTFChars(file_name, fileName);

    if (JS_IsException(compiled)) {
        throwJSException(env, context);
        return nullptr;
    }

    size_t bufferLength = 0;
    auto fg = JS_WRITE_OBJ_BYTECODE;
    if (has_suffix(fileName, ".json") || has_suffix(fileName, ".json5")) {
        fg = 0;
    }
    auto buffer = JS_WriteObject(context, &bufferLength, compiled, fg);

    auto result = buffer && bufferLength > 0 ? env->NewByteArray(bufferLength) : nullptr;
    if (result) {
        env->SetByteArrayRegion(result, 0, bufferLength, reinterpret_cast<const jbyte*>(buffer));
    } else {
        throwJSException(env, context);
    }

    JS_FreeValue(context, compiled);
    js_free(context, buffer);

    return result;
}

jobject QuickJSWrapper::execute(JNIEnv *env, jobject thiz, jbyteArray bytecode) {
    if(bytecode == nullptr) {
        throwJSException(env, "bytecode can not be null");
        return nullptr;
    }

    const auto buffer = env->GetByteArrayElements(bytecode, nullptr);
    const auto bufferLength = env->GetArrayLength(bytecode);
    auto obj = JS_ReadObject(context, reinterpret_cast<const uint8_t*>(buffer), bufferLength, JS_READ_OBJ_BYTECODE);
    env->ReleaseByteArrayElements(bytecode, buffer, JNI_ABORT);

    if (JS_IsException(obj)) {
        throwJSException(env, context);
        return nullptr;
    }

    if (JS_ResolveModule(context, obj)) {
        JS_FreeValue(context, obj);
        throwJSException(env, "Failed to resolve JS module");
        return nullptr;
    }

    auto val = JS_EvalFunction(context, obj);

    if (!executePendingJobLoop(env, runtime, context)) {
        JS_FreeValue(context, val);
        return nullptr;
    }

    jobject result;
    if (!JS_IsException(val)) {
        result = toJavaObject(env, thiz, JS_UNDEFINED, val);
    } else {
        result = nullptr;
        throwJSException(env, context);
    }

    return result;
}

jobject
QuickJSWrapper::evaluateModule(JNIEnv *env, jobject thiz, jstring script, jstring file_name) {
    const char *c_script = env->GetStringUTFChars(script, JNI_FALSE);
    const char *c_file_name = env->GetStringUTFChars(file_name, JNI_FALSE);

    JSValue result = JS_Eval(context, c_script, strlen(c_script), c_file_name, JS_EVAL_TYPE_MODULE);
    env->ReleaseStringUTFChars(script, c_script);
    env->ReleaseStringUTFChars(file_name, c_file_name);
    if (JS_IsException(result)) {
        throwJSException(env, context);
        return nullptr;
    }

    if (!executePendingJobLoop(env, runtime, context)) {
        JS_FreeValue(context, result);
        return nullptr;
    }

    JSValue global = JS_GetGlobalObject(context);
    jobject jsObj = toJavaObject(env, thiz, global, result);
    JS_FreeValue(context, global);
    return jsObj;
}

jobject QuickJSWrapper::getOwnPropertyNames(JNIEnv *env, jobject thiz, jlong obj) {
    if (JS_IsException(ownPropertyNames)) {
        throwJSException(env, context);
        return nullptr;
    }

    JSValue jsObject = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(obj));
    JSValue ret = JS_Call(context, ownPropertyNames, JS_NULL, 1, &jsObject);
    if (JS_IsException(ret)) {
        throwJSException(env, context);
        JS_FreeValue(context, ret);
        return nullptr;
    }

    return toJavaObject(env, thiz, JS_UNDEFINED, ret);
}

jboolean QuickJSWrapper::contains(JNIEnv *env, jlong object_handle, jstring key) const {
    JSValue this_obj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(object_handle));
    const char *key_ = env->GetStringUTFChars(key, nullptr);
    JSAtom atom = JS_NewAtom(context, key_);
    int result = JS_HasProperty(context, this_obj, atom);
    JS_FreeAtom(context, atom);
    env->ReleaseStringUTFChars(key, key_);
    return result;
}

jobjectArray QuickJSWrapper::getKeys(JNIEnv *env, jlong value) const {
    JSValue val = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(value));
    JSPropertyEnum *props = nullptr;
    uint32_t prop_count = 0;
    int ret = JS_GetOwnPropertyNames(context, &props, &prop_count, val, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY);
    if (ret < 0) {
        return nullptr;
    }
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) {
        JS_FreePropertyEnum(context, props, prop_count);
        return nullptr;
    }
    jobjectArray result_array = env->NewObjectArray(prop_count, stringClass, nullptr);
    if (result_array == nullptr) {
        JS_FreePropertyEnum(context, props, prop_count);
        return nullptr;
    }
    for (uint32_t i = 0; i < prop_count; ++i) {
        JSValue key = JS_AtomToValue(context, props[i].atom);
        if (JS_IsString(key)) {
            const char *str = JS_ToCString(context, key);
            if (str != nullptr) {
                jstring java_str = env->NewStringUTF(str);
                env->SetObjectArrayElement(result_array, i, java_str);
                env->DeleteLocalRef(java_str);
                JS_FreeCString(context, str);
            }
        }
        JS_FreeValue(context, key);
    }
    JS_FreePropertyEnum(context, props, prop_count);
    return result_array;
}

void QuickJSWrapper::arrayAdd(JNIEnv *env, jobject thiz, jlong object_handle, jobject value) {
    JSValue this_obj = JS_MKPTR(JS_TAG_OBJECT, reinterpret_cast<void *>(object_handle));
    JSValue lenValue = JS_GetPropertyStr(context, this_obj, "length");
    int len;
    if (JS_ToInt32(context, &len, lenValue)) {
        JS_FreeValue(context, lenValue);
        throwJSException(env, "Failed to get array length");
        return;
    }
    JS_FreeValue(context, lenValue);
    /* toJSValue 现在对所有类型统一返回新引用，
     * JS_SetPropertyUint32 会 steal 这个新引用，无需 DupValue/FreeValue。 */
    JSValue child = toJSValue(env, thiz, value);
    JS_SetPropertyUint32(context, this_obj, len, child);
}