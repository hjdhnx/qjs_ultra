/*
 * qjs_test_runner.c — qjs_engine 静态库的轻量测试宿主
 *
 * 用法: qjs_test_runner <test.js>
 *  - JS_NewContext 会经 quickjs.c 自动调用 qjs_ext_install_all()，
 *    安装全部扩展模块（Buffer/zlib/fs/crypto/webcrypto/sqlite/wasm/path/HTML/URL/TextCodec）
 *    及 JS 层注入库（console/Date polyfill 等）。
 *  - JS_Eval 执行测试脚本（全局类型），随后循环 JS_ExecutePendingJob
 *    泵空微任务队列，使 async/await 测试（WebCrypto 等）得以完成。
 *  - 脚本执行或作业执行抛出异常 => 非零退出码；否则以脚本退出码为准
 *    （脚本可在失败时 throw new Error(...)）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "quickjs.h"

static void dump_exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    if (!JS_IsNull(exc) && !JS_IsUndefined(exc)) {
        const char *str = JS_ToCString(ctx, exc);
        fprintf(stderr, "Exception: %s\n", str ? str : "<no message>");
        if (str) JS_FreeCString(ctx, str);
        /* 带栈信息的 Error */
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsUndefined(stack)) {
            const char *s = JS_ToCString(ctx, stack);
            fprintf(stderr, "%s\n", s ? s : "");
            if (s) JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, exc);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, len, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        const char *s = JS_ToCString(ctx, argv[i]);
        if (!s) return JS_EXCEPTION;
        fputs(s, stdout);
        JS_FreeCString(ctx, s);
    }
    fputc('\n', stdout);
    return JS_UNDEFINED;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <test.js>\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    char *buf = read_file(argv[1], &len);
    if (!buf) return 2;

    int failed = 0;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { fprintf(stderr, "JS_NewRuntime failed\n"); return 2; }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { fprintf(stderr, "JS_NewContext failed\n"); return 2; }

    /* 平台 stdout 注入：console 模块（JS 层）要求 console.stdout 可用 */
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__qjs_print",
                      JS_NewCFunction(ctx, js_print, "__qjs_print", 1));
    JS_FreeValue(ctx, global);
    JSValue cret = JS_Eval(ctx,
        "console.stdout = function(){ return __qjs_print.apply(null, arguments); };",
        strlen("console.stdout = function(){ return __qjs_print.apply(null, arguments); };"),
        "<stdout-inject>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(cret)) {
        fprintf(stderr, "stdout inject failed:\n");
        dump_exception(ctx);
        failed = 1;
    }
    JS_FreeValue(ctx, cret);

    JSValue r = JS_Eval(ctx, buf, len, argv[1], JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        fprintf(stderr, "Script evaluation failed:\n");
        dump_exception(ctx);
        failed = 1;
    } else {
        JS_FreeValue(ctx, r);
    }

    /* 泵空 pending job 队列（async 测试） */
    for (;;) {
        JSContext *ctx1 = NULL;
        int ret = JS_ExecutePendingJob(rt, &ctx1);
        if (ret < 0) {
            fprintf(stderr, "Pending job (async test) failed:\n");
            dump_exception(ctx1);
            failed = 1;
            break;
        }
        if (ret == 0) break;
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    free(buf);
    return failed ? 1 : 0;
}
