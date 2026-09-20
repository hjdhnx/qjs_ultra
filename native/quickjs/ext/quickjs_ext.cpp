/*
 * quickjs_ext.cpp - QuickJS 引擎级扩展模块注册
 *
 * 将原先在 JNI 层（quickjs_wrapper.cpp）注册的所有扩展模块下沉到引擎层。
 * 由 quickjs.c 的 JS_NewContext 调用 qjs_ext_install_all() 一次性完成：
 *   - wasm3 环境创建（存于 rt->ext_data，随 runtime 生命周期管理）
 *   - C 层全局模块注册：Buffer/zlib/fs/crypto/webcrypto/sqlite/wasm/path
 *   - qjs 扩展模块：HTML/URL/TextCodec
 *   - JS 层封装库注入：Date polyfill / console / wasm.js / sqlite3.js
 *
 * wasm3 环境的销毁在 quickjs.c 的 JS_FreeRuntime 末尾执行（qjs_ext_free），
 * 此时所有 JS 对象（含 wasm finalizer）均已释放，彻底消除析构顺序脆弱性。
 *
 * 纯 C/C++ 实现，无 JNI 依赖，可编译为各平台静态库。
 */

#include <string.h>
#include "qjs_native.h"
#include "quickjs_extend_libraries.h"

/*
 * C 链接导出：供纯 C 的 quickjs.c（JS_NewContext / JS_FreeRuntime）直接调用。
 * extern "C" 必须位于定义之前才能生效。
 */
extern "C" {

int qjs_ext_install_all(JSContext *ctx);
void qjs_ext_free(JSRuntime *rt);
/* ext_data 访问器（实现在 quickjs.c，因 JSRuntime 结构不透明） */
void *qjs_ext_get_data(JSRuntime *rt);
void qjs_ext_set_data(JSRuntime *rt, void *p);

} /* extern "C" */

/* JS 层封装库注入（与原 loadExtendLibraries 一致） */
static void qjs_ext_load_js_libraries(JSContext *ctx)
{
    JS_FreeValue(ctx, JS_Eval(ctx, DATE_POLYFILL, strlen(DATE_POLYFILL), "date-polyfill.js", JS_EVAL_TYPE_GLOBAL));
    JS_FreeValue(ctx, JS_Eval(ctx, CONSOLE, strlen(CONSOLE), "console.js", JS_EVAL_TYPE_GLOBAL));
    JS_FreeValue(ctx, JS_Eval(ctx, WASM, strlen(WASM), "wasm.js", JS_EVAL_TYPE_GLOBAL));
    JS_FreeValue(ctx, JS_Eval(ctx, SQLITE, strlen(SQLITE), "sqlite3.js", JS_EVAL_TYPE_GLOBAL));
}

/*
 * 引擎级模块统一注册入口。
 * 由 JS_NewContext 在 intrinsics 注册完成后调用。
 * 创建 wasm3 环境（存于 rt->ext_data）并注册全部扩展模块。
 * 返回 0 表示注册失败（ctx 中已设置异常）。
 */
int qjs_ext_install_all(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* 初始化 wasm3 环境（用于 WebAssembly 模块加载），
     * 销毁由 JS_FreeRuntime → qjs_ext_free 统一处理 */
    IM3Environment env = m3_NewEnvironment();
    if (!env)
        return 0;
    qjs_ext_set_data(rt, env);
    JSValue global_obj = JS_GetGlobalObject(ctx);
    qjs_buffer_init(ctx, "Buffer");
    qjs_zlib_install_global(ctx, global_obj);
    qjs_fs_install_global(ctx, global_obj);
    qjs_crypto_install_global(ctx, global_obj);
    qjs_webcrypto_install_global(ctx, global_obj);
    qjs__mod_sqlite3_init(ctx, global_obj);
    qjs__mod_wasm_init(ctx, global_obj);
    qjs_setup_path(ctx, global_obj);

    /* QJS 扩展模块：HTML/URL/TextCodec（qjs_jsonpath 注释提及但未注册） */
    qjs_html_init(ctx);
    qjs_url_init(ctx);
    qjs_textcodec_init(ctx);

    JS_FreeValue(ctx, global_obj);

    /* JS 层封装库注入 */
    qjs_ext_load_js_libraries(ctx);

    return 1;
}

/*
 * runtime 释放阶段统一销毁扩展数据。
 * 由 JS_FreeRuntime 在所有 GC 对象/finalizer 执行完毕后调用，
 * 此时 wasm finalizer 已全部执行完毕，可安全销毁 wasm3 环境。
 */
void qjs_ext_free(JSRuntime *rt)
{
    IM3Environment env = (IM3Environment)qjs_ext_get_data(rt);
    if (!env)
        return;
    qjs_ext_set_data(rt, NULL);
    m3_FreeEnvironment(env);
}

