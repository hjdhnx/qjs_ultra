/*
 * sqlite3.c - SQLite 模块实现（中文说明）
 *
 * 提供 Node.js 风格的 SQLite3 API，基于 SQLite C 接口实现。
 */

#include "qjs_native.h"

#include <sqlite3.h>



typedef struct {
    sqlite3 *handle;
} QJSSqlite3Handle;

static void qjs_sqlite3_finalizer(JSRuntime *rt, JSValue val) {
    QJSSqlite3Handle *h = JS_GetOpaque(val, QJS_CORE_CLASS_ID_SQLITE3);
    if (!h) {
        return;
    }
    if (h->handle) {
        sqlite3_close(h->handle);
    }
    js_free_rt(rt, h);
}

static JSClassDef qjs_sqlite3_class = {
    "Handle",
    .finalizer = qjs_sqlite3_finalizer,
};

static JSValue qjs_new_sqlite3(JSContext *ctx, sqlite3 *handle) {
    QJSSqlite3Handle *h;
    JSValue obj;

    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_SQLITE3);
    if (JS_IsException(obj)) {
        return obj;
    }

    h = js_mallocz(ctx, sizeof(*h));
    if (!h) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    h->handle = handle;

    JS_SetOpaque(obj, h);
    return obj;
}

static QJSSqlite3Handle *qjs_sqlite3_get(JSContext *ctx, JSValue obj) {
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_SQLITE3);
}


typedef struct {
    sqlite3_stmt *stmt;
} QJSSqlite3Stmt;

static void qjs_sqlite3_stmt_finalizer(JSRuntime *rt, JSValue val) {
    QJSSqlite3Stmt *h = JS_GetOpaque(val, QJS_CORE_CLASS_ID_SQLITE3_STMT);
    if (!h) {
        return;
    }
    if (h->stmt) {
        sqlite3_reset(h->stmt);
        sqlite3_finalize(h->stmt);
    }
    js_free_rt(rt, h);
}

static JSClassDef qjs_sqlite3_stmt_class = {
    "Statement",
    .finalizer = qjs_sqlite3_stmt_finalizer,
};

static JSValue qjs_new_sqlite3_stmt(JSContext *ctx, sqlite3_stmt *stmt) {
    QJSSqlite3Stmt *h;
    JSValue obj;

    obj = JS_NewObjectClass(ctx, QJS_CORE_CLASS_ID_SQLITE3_STMT);
    if (JS_IsException(obj)) {
        return obj;
    }

    h = js_mallocz(ctx, sizeof(*h));
    if (!h) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    h->stmt = stmt;

    JS_SetOpaque(obj, h);
    return obj;
}

static QJSSqlite3Stmt *qjs_sqlite3_stmt_get(JSContext *ctx, JSValue obj) {
    return JS_GetOpaque2(ctx, obj, QJS_CORE_CLASS_ID_SQLITE3_STMT);
}

static JSValue qjs_sqlite3_open(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    const char *db_name = JS_ToCString(ctx, argv[0]);

    if (!db_name) {
        return JS_EXCEPTION;
    }

    int flags;
    if (JS_ToInt32(ctx, &flags, argv[1])) {
        JS_FreeCString(ctx, db_name);
        return JS_EXCEPTION;
    }

    sqlite3 *handle = NULL;
    int r = sqlite3_open_v2(db_name, &handle, flags, NULL);

    JS_FreeCString(ctx, db_name);

    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    JSValue obj = qjs_new_sqlite3(ctx, handle);
    if (JS_IsException(obj)) {
        sqlite3_close(handle);
    }

    return obj;
}

static JSValue qjs_sqlite3_close(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Handle *h = qjs_sqlite3_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    int r = sqlite3_close(h->handle);
    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    h->handle = NULL;

    return JS_TRUE;
}

static JSValue qjs_sqlite3_exec(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Handle *h = qjs_sqlite3_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    const char *sql = JS_ToCString(ctx, argv[1]);

    if (!sql) {
        return JS_EXCEPTION;
    }

    int r = sqlite3_exec(h->handle, sql, NULL, NULL, NULL);

    JS_FreeCString(ctx, sql);

    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    return JS_TRUE;
}

static JSValue qjs_sqlite3_prepare(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Handle *h = qjs_sqlite3_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    const char *sql = JS_ToCString(ctx, argv[1]);

    if (!sql) {
        return JS_EXCEPTION;
    }

    sqlite3_stmt *stmt = NULL;
    int r = sqlite3_prepare_v2(h->handle, sql, -1, &stmt, NULL);

    JS_FreeCString(ctx, sql);

    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    JSValue obj = qjs_new_sqlite3_stmt(ctx, stmt);
    if (JS_IsException(obj)) {
        sqlite3_finalize(stmt);
    }

    return obj;
}

static JSValue qjs_sqlite3_in_transaction(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Handle *h = qjs_sqlite3_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    return JS_NewBool(ctx, !sqlite3_get_autocommit(h->handle));
}

static JSValue qjs_sqlite3_stmt_finalize(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Stmt *h = qjs_sqlite3_stmt_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    if (!h->stmt) {
        return JS_UNDEFINED;
    }

    sqlite3_reset(h->stmt);

    int r = sqlite3_finalize(h->stmt);
    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    h->stmt = NULL;

    return JS_TRUE;
}

static JSValue qjs_sqlite3_stmt_expand(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Stmt *h = qjs_sqlite3_stmt_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    if (!h->stmt) {
        return JS_NewString(ctx, "");
    }

    char *sql = sqlite3_expanded_sql(h->stmt);
    if (sql == NULL) {
        return JS_ThrowOutOfMemory(ctx);
    }

    JSValue ret = JS_NewString(ctx, sql);
    sqlite3_free(sql);
    return ret;
}

static JSValue qjs__stmt2obj(JSContext *ctx, QJSSqlite3Stmt *h) {
    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    int count = sqlite3_column_count(h->stmt);

    for (int i = 0; i < count; i++) {
        const char *name = sqlite3_column_name(h->stmt, i);
        JSValue value;

        switch (sqlite3_column_type(h->stmt, i)) {
            case SQLITE_INTEGER: {
                value = JS_NewInt64(ctx, sqlite3_column_int64(h->stmt, i));
                break;
            }
            case SQLITE_FLOAT: {
                value = JS_NewFloat64(ctx, sqlite3_column_double(h->stmt, i));
                break;
            }
            case SQLITE3_TEXT: {
                value = JS_NewString(ctx, (const char *) sqlite3_column_text(h->stmt, i));
                break;
            }
            case SQLITE_BLOB: {
                value = JS_NewUint8ArrayCopy(ctx,
                                             (uint8_t *) sqlite3_column_blob(h->stmt, i),
                                             sqlite3_column_bytes(h->stmt, i));
                break;
            }
            default: {
                value = JS_NULL;
                break;
            }
        }

        JS_DefinePropertyValueStr(ctx, obj, name, value, JS_PROP_C_W_E);
    }

    return obj;
}

static JSValue qjs__sqlite3_bind_param(JSContext *ctx, sqlite3_stmt *stmt, int idx, JSValue v) {
    int r;

#define CHECK_VALUE(ret, i)                                                                                            \
    if (ret == -1) {                                                                                                   \
        return JS_ThrowTypeError(ctx, "Failed to convert type at position %d", idx);                                   \
    }

#define CHECK_RET(ret)                                                                                                 \
    if (r != SQLITE_OK) {                                                                                              \
        return JS_FALSE;                                                                      \
    }

    switch (JS_VALUE_GET_NORM_TAG(v)) {
        case JS_TAG_BIG_INT:
        case JS_TAG_SHORT_BIG_INT: {
            int64_t x;
            r = JS_ToBigInt64(ctx, &x, v);
            CHECK_VALUE(r, idx);
            r = sqlite3_bind_int64(stmt, idx, x);
            CHECK_RET(r);
            break;
        }
        case JS_TAG_STRING:
        case JS_TAG_STRING_ROPE: {
            size_t len;
            const char *x = JS_ToCStringLen(ctx, &len, v);
            if (!x) {
                return JS_EXCEPTION;
            }
            r = sqlite3_bind_text(stmt, idx, x, len, SQLITE_TRANSIENT);
            JS_FreeCString(ctx, x);
            CHECK_RET(r);
            break;
        }
        case JS_TAG_OBJECT: {
            size_t len = 0;
            const uint8_t *x = JS_GetUint8Array(ctx, &len, v);
            if (!x) {
                return JS_EXCEPTION;
            }
            r = sqlite3_bind_blob(stmt, idx, x, len, SQLITE_TRANSIENT);
            CHECK_RET(r);
            break;
        }
        case JS_TAG_INT: {
            int64_t x;
            r = JS_ToInt64(ctx, &x, v);
            CHECK_VALUE(r, idx);
            if (x < INT_MIN || x > INT_MAX) {
                r = sqlite3_bind_int64(stmt, idx, x);
            } else {
                r = sqlite3_bind_int(stmt, idx, x);
            }
            CHECK_RET(r);
            break;
        }
        case JS_TAG_BOOL: {
            r = JS_ToBool(ctx, v);
            CHECK_VALUE(r, idx);
            r = sqlite3_bind_int(stmt, idx, r);
            CHECK_RET(r);
            break;
        }
        case JS_TAG_NULL: {
            r = sqlite3_bind_null(stmt, idx);
            CHECK_RET(r);
            break;
        }
        case JS_TAG_FLOAT64: {
            double x;
            r = JS_ToFloat64(ctx, &x, v);
            CHECK_VALUE(r, idx);
            r = sqlite3_bind_double(stmt, idx, x);
            CHECK_RET(r);
            break;
        }
        default:
            return JS_ThrowTypeError(ctx, "Invalid bound parameter type at position %d", idx);
    }

    return JS_TRUE;

#undef CHECK_VALUE
#undef CHECK_RET
}

static JSValue qjs__sqlite3_bind_params(JSContext *ctx, sqlite3_stmt *stmt, JSValue params) {
    sqlite3_clear_bindings(stmt);

    if (JS_IsArray(ctx, params)) {
        JSValue js_length = JS_GetPropertyStr(ctx, params, "length");
        uint64_t len;
        if (JS_ToIndex(ctx, &len, js_length)) {
            JS_FreeValue(ctx, js_length);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, js_length);
        for (int i = 0; i < len; i++) {
            JSValue v = JS_GetPropertyUint32(ctx, params, i);
            if (JS_IsException(v)) {
                return v;
            }
            bool is_exception = JS_IsException(qjs__sqlite3_bind_param(ctx, stmt, i + 1, v));
            JS_FreeValue(ctx, v);
            if (is_exception) {
                return JS_EXCEPTION;
            }
        }
    } else if (JS_IsObject(params)) {
        JSPropertyEnum *ptab;
        uint32_t plen;
        if (JS_GetOwnPropertyNames(ctx, &ptab, &plen, params, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            return JS_EXCEPTION;
        }
        for (int i = 0; i < plen; i++) {
            JSAtom patom = ptab[i].atom;
            JSValue prop = JS_GetProperty(ctx, params, patom);
            if (JS_IsException(prop)) {
                JS_FreePropertyEnum(ctx, ptab, plen);
                return JS_EXCEPTION;
            }
            const char *key = JS_AtomToCString(ctx, patom);
            int idx = sqlite3_bind_parameter_index(stmt, key);
            if (idx == 0 || JS_IsException(qjs__sqlite3_bind_param(ctx, stmt, idx, prop))) {
                if (idx == 0) {
                    JS_ThrowReferenceError(ctx, "Could not find parameter '%s'", key);
                }
                JS_FreeValue(ctx, prop);
                JS_FreeCString(ctx, key);
                JS_FreePropertyEnum(ctx, ptab, plen);
                return JS_EXCEPTION;
            }
            JS_FreeValue(ctx, prop);
            JS_FreeCString(ctx, key);
        }
        JS_FreePropertyEnum(ctx, ptab, plen);
    } else {
        return JS_ThrowTypeError(ctx, "Invalid bind parameters type: expected object or array");
    }

    return JS_UNDEFINED;
}

static JSValue qjs_sqlite3_stmt_all(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Stmt *h = qjs_sqlite3_stmt_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    if (!h->stmt) {
        return JS_ThrowInternalError(ctx, "Statement has been finalized");
    }

    int r = sqlite3_reset(h->stmt);
    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    if (argc == 2) {
        JSValue params = argv[1];

        if (JS_IsException(qjs__sqlite3_bind_params(ctx, h->stmt, params))) {
            return JS_EXCEPTION;
        }
    }

    JSValue result = JS_NewArray(ctx);
    uint32_t i = 0;

    while ((r = sqlite3_step(h->stmt)) == SQLITE_ROW) {
        JS_DefinePropertyValueUint32(ctx, result, i, qjs__stmt2obj(ctx, h), JS_PROP_C_W_E);
        i++;
    }

    if (r != SQLITE_OK && r != SQLITE_DONE) {
        JS_FreeValue(ctx, result);
        return JS_FALSE;
    }

    return result;
}

static JSValue qjs_sqlite3_stmt_run(JSContext *ctx, JSValue this_val, int argc, JSValue *argv) {
    QJSSqlite3Stmt *h = qjs_sqlite3_stmt_get(ctx, argv[0]);

    if (!h) {
        return JS_EXCEPTION;
    }

    if (!h->stmt) {
        return JS_ThrowInternalError(ctx, "Statement has been finalized");
    }

    int r = sqlite3_reset(h->stmt);
    if (r != SQLITE_OK) {
        return JS_FALSE;
    }

    if (argc == 2) {
        JSValue params = argv[1];

        if (JS_IsException(qjs__sqlite3_bind_params(ctx, h->stmt, params))) {
            return JS_EXCEPTION;
        }
    }

    r = sqlite3_step(h->stmt);
    if (r != SQLITE_OK && r != SQLITE_DONE && r != SQLITE_ROW) {
        return JS_FALSE;
    }

    return JS_TRUE;
}

static const JSCFunctionListEntry qjs_sqlite3_funcs[] = {
    QJS_CFUNC_DEF("open", 2, qjs_sqlite3_open),
    QJS_CFUNC_DEF("close", 1, qjs_sqlite3_close),
    QJS_CFUNC_DEF("exec", 2, qjs_sqlite3_exec),
    QJS_CFUNC_DEF("prepare", 2, qjs_sqlite3_prepare),
    QJS_CFUNC_DEF("in_transaction", 1, qjs_sqlite3_in_transaction),
    QJS_CFUNC_DEF("stmt_finalize", 1, qjs_sqlite3_stmt_finalize),
    QJS_CFUNC_DEF("stmt_expand", 1, qjs_sqlite3_stmt_expand),
    QJS_CFUNC_DEF("stmt_all", 2, qjs_sqlite3_stmt_all),
    QJS_CFUNC_DEF("stmt_run", 2, qjs_sqlite3_stmt_run),
    QJS_CONST(SQLITE_OPEN_CREATE),
    QJS_CONST(SQLITE_OPEN_READONLY),
    QJS_CONST(SQLITE_OPEN_READWRITE),
};

void qjs__mod_sqlite3_init(JSContext *ctx, JSValue ns) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* Handle object */
    JS_NewClass(rt, QJS_CORE_CLASS_ID_SQLITE3, &qjs_sqlite3_class);
    JS_SetClassProto(ctx, QJS_CORE_CLASS_ID_SQLITE3, JS_NULL);

    /* Statement object */
    JS_NewClass(rt, QJS_CORE_CLASS_ID_SQLITE3_STMT, &qjs_sqlite3_stmt_class);
    JS_SetClassProto(ctx, QJS_CORE_CLASS_ID_SQLITE3_STMT, JS_NULL);

    JSValue obj = JS_NewObjectProto(ctx, JS_NULL);
    JS_SetPropertyFunctionList(ctx, obj, qjs_sqlite3_funcs, countof(qjs_sqlite3_funcs));

    JS_DefinePropertyValueStr(ctx, ns, "sqlite", obj, JS_PROP_C_W_E);
}
