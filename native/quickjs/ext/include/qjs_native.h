#ifndef _QJS_NATIVE_H_INCLUDED_
#define _QJS_NATIVE_H_INCLUDED_

/* ========================================================================
 * 第一部分：平台与类型定义
 * ======================================================================== */

/* 返回值常量，与 NGINX 风格保持一致 */
#define QJS_OK             0
#define QJS_ERROR          (-1)
#define QJS_AGAIN          (-2)
#define QJS_DECLINED       (-3)
#define QJS_DONE           (-4)

/*
 * off_t 在 Linux 上默认 32 位，需在包含 <sys/types.h> 之前
 * 定义 _FILE_OFFSET_BITS=64 以启用大文件支持。
 */
#define _FILE_OFFSET_BITS  64

/* QuickJS 引擎核心头文件 */
#include "../quickjs/quickjs.h"
#include "../quickjs/cutils.h"
#include "m3_env.h"
#include <sys/types.h>
#include <stdint.h>

/* Windows（MSVC 与 MinGW 的 sys/types.h 均无 BSD 风格类型），手动补齐 */
#if defined(_WIN32)
typedef unsigned char  u_char;
typedef unsigned int   u_int;
typedef unsigned short u_short;
typedef unsigned long  u_long;
#endif

/*
 * qjs_int_t 对应最高效的整数类型（一个机器字）。
 * 在 AMD64 上 64 位乘除法较慢且指令较长，故使用 32 位 int。
 */
#if defined(__amd64__) || defined(__x86_64__)
typedef int             qjs_int_t;
typedef u_int           qjs_uint_t;
#else
typedef intptr_t        qjs_int_t;
typedef uintptr_t       qjs_uint_t;
#endif

typedef qjs_uint_t      qjs_bool_t;

#define QJS_MAX_ERROR_STR    2048


/* ========================================================================
 * 第二部分：编译器属性与通用宏
 * ======================================================================== */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>

#include <unistd.h>
#if defined(_WIN32)
#include <errno.h>
#include <direct.h>   /* _mkdir（MinGW 的 mkdir 为单参，见下） */
#include <io.h>       /* _lseek/_read/_write（pread/pwrite 模拟用） */
/* MinGW (mingw-w64) 自带 dirent.h/unistd.h 原生 POSIX 实现 */
#include <dirent.h>
/* MinGW 只有单参 _mkdir，补 POSIX 双参形式 */
#define mkdir(path, mode)   _mkdir(path)
/* lstat 与 stat 等价（Windows 无独立 lstat 语义） */
#define lstat(path, sb)     stat(path, sb)
/* symlink/readlink/fdopendir：MinGW 无此函数，返回明确错误 */
static inline int mingw_symlink(const char *target, const char *path) {
    (void) target; (void) path; errno = ENOSYS; return -1;
}
#define symlink(target, path)   mingw_symlink(target, path)
static inline ssize_t mingw_readlink(const char *path, char *buf, size_t size) {
    (void) path; (void) buf; (void) size; errno = ENOSYS; return -1;
}
#define readlink(path, buf, size)   ((ssize_t) mingw_readlink(path, buf, size))
static inline DIR *mingw_fdopendir(int fd) {
    (void) fd; errno = ENOSYS; return NULL;
}
#define fdopendir(fd)   mingw_fdopendir(fd)
/* realpath → _fullpath */
static inline char *mingw_realpath(const char *path, char *resolved) {
    return _fullpath(resolved, path, 4096);
}
#define realpath(path, resolved)   mingw_realpath(path, resolved)
/* pread/pwrite → _lseek + read/write（非线程安全，fs 模块单线程可接受） */
static inline ssize_t mingw_pread(int fd, void *buf, size_t n, off_t off) {
    off_t saved = lseek(fd, 0, SEEK_CUR); ssize_t r;
    if (saved < 0 || lseek(fd, (off_t) off, SEEK_SET) < 0) return -1;
    r = (ssize_t) read(fd, buf, n);
    lseek(fd, saved, SEEK_SET);
    return r;
}
#define pread(fd, buf, n, off)   mingw_pread(fd, buf, n, off)
static inline ssize_t mingw_pwrite(int fd, const void *buf, size_t n, off_t off) {
    off_t saved = lseek(fd, 0, SEEK_CUR); ssize_t r;
    if (saved < 0 || lseek(fd, (off_t) off, SEEK_SET) < 0) return -1;
    r = (ssize_t) write(fd, buf, n);
    lseek(fd, saved, SEEK_SET);
    return r;
}
#define pwrite(fd, buf, n, off)   mingw_pwrite(fd, buf, n, off)
/* MinGW struct stat 无 st_blksize/st_blocks 成员 */
#ifndef S_IFLNK
#define S_IFLNK     0120000
#endif
#ifndef S_IFSOCK
#define S_IFSOCK    0140000
#endif
#define QJS_ST_NO_BLKSIZE   1
/* MinGW fcntl.h 缺失的 POSIX 标志 */
#ifndef O_SYNC
#define O_SYNC      0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0
#endif
/* stat 类型判定宏 */
#ifndef S_ISDIR
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m)  (0)
#endif
#endif /* _WIN32 */


/* qjs_inline：强制内联；qjs_noinline：禁止内联（GNU/Clang/MinGW 统一 __attribute__） */
#define qjs_inline         static inline __attribute__((always_inline))
#define qjs_noinline       __attribute__((noinline))

/* 将 C 常量导出为 JS 属性 */
#define QJS_CONST(x)          JS_PROP_INT32_DEF(#x, x, JS_PROP_ENUMERABLE)

/* 定义 C 函数为 JS 可调用函数 */
#define QJS_CFUNC_DEF(name, length, func1)                                    \
    {                                                                         \
        name, JS_PROP_C_W_E, JS_DEF_CFUNC, 0, {                               \
            .func = { length, JS_CFUNC_generic, { .generic = func1 } }        \
        }                                                                     \
    }

/* 通过成员指针反推父结构体指针 */
#define qjs_container_of(p, type, field)                                      \
    (type *) ((u_char *) (p) - offsetof(type, field))

/* 数组元素个数 */
#define qjs_nitems(x)                                                         \
    (sizeof(x) / sizeof((x)[0]))

#define qjs_max(val1, val2)   ((val1 < val2) ? (val2) : (val1))
#define qjs_min(val1, val2)   ((val1 < val2) ? (val1) : (val2))

/* 分支预测提示，提升热路径性能 */
#if defined(__GNUC__) || defined(__clang__)
#define qjs_expect(c, x)   __builtin_expect((long) (x), (c))
#define qjs_fast_path(x)   qjs_expect(1, x)
#define qjs_slow_path(x)   qjs_expect(0, x)
#else
#define qjs_expect(c, x)   (x)
#define qjs_fast_path(x)   (x)
#define qjs_slow_path(x)   (x)
#endif

/*
 * packed 属性：用于 packed 结构体进行非对齐内存访问。
 * 必须使用 __attribute__((packed))，否则编译器在 -O3 下可能
 * 基于对齐假设生成 SIMD 指令，对未对齐地址访问导致 SIGBUS 崩溃。
 * 旧代码定义为空是严重 bug。
 */
#if defined(__GNUC__) || defined(__clang__)
#define QJS_PACKED         __attribute__((packed))
#else
#define QJS_PACKED
#endif


/* ========================================================================
 * 第三部分：字符串类型与操作
 * ======================================================================== */

/*
 * qjs_str_t：NGINX 风格的字符串结构，使用 {length, start} 二元组
 * 而非 NUL 结尾，避免频繁 strlen 调用，提升性能。
 */
typedef struct {
    size_t  length;
    u_char  *start;
} qjs_str_t;

#define qjs_length(s)        (sizeof(s) - 1)
#define qjs_str(s)           { qjs_length(s), (u_char *) s }
#define qjs_null_str         { 0, NULL }
/* 注意：不能用 "(qjs_str_t) qjs_str(s)"（对大括号字面量的 struct 强转是 GCC 扩展），
 * 改用 C99 compound literal，MSVC 2019+ 的 C 模式同样支持 */
#define qjs_str_value(s)     (qjs_str_t) { qjs_length(s), (u_char *) s }

#define qjs_cpymem(dst, src, n)  (((u_char *) memcpy(dst, src, n)) + (n))
#define qjs_memzero(buf, length)    (void) memset(buf, 0, length)

/*
 * 安全清零：防止编译器优化掉 memset，用于清除密钥等敏感数据。
 * 使用 volatile 指针的 inline 实现，兼容所有平台。
 */
qjs_inline void
qjs_explicit_memzero(void *buf, size_t length)
{
    volatile u_char  *p = (volatile u_char *) buf;
    while (length != 0) {
        *p++ = 0;
        length--;
    }
}

#define qjs_strstr_eq(s1, s2)                                                \
    (((s1)->length == (s2)->length)                                          \
     && (memcmp((s1)->start, (s2)->start, (s1)->length) == 0))

#define qjs_strstr_case_eq(s1, s2)                                           \
    (((s1)->length == (s2)->length)                                          \
     && (qjs_strncasecmp((s1)->start, (s2)->start, (s1)->length) == 0))

qjs_int_t qjs_strncasecmp(u_char *s1, u_char *s2, size_t n);


/* ========================================================================
 * 第四部分：工具函数与字节序操作
 * ======================================================================== */

/* 浮点数与整数的位级转换联合体 */
typedef union {
    float       f;
    uint32_t    u;
} qjs_conv_f32_t;

typedef union {
    double      f;
    uint64_t    u;
} qjs_conv_f64_t;

/* packed 结构体，用于非对齐内存访问（QJS_PACKED 已修复） */
struct QJS_PACKED qjs_packed_u16_t { uint16_t v; };
struct QJS_PACKED qjs_packed_u32_t { uint32_t v; };
struct QJS_PACKED qjs_packed_u64_t { uint64_t v; };

const char *qjs_errno_string(int errnum);

/* 字节序交换 */
qjs_inline uint16_t qjs_bswap_u16(uint16_t u16)
{
    return (u16 >> 8) | (u16 << 8);
}

qjs_inline uint32_t qjs_bswap_u32(uint32_t u32)
{
    return ((u32 & 0xff000000) >> 24)
           | ((u32 & 0x00ff0000) >> 8)
           | ((u32 & 0x0000ff00) << 8)
           | ((u32 & 0x000000ff) << 24);
}

qjs_inline uint64_t qjs_bswap_u64(uint64_t u64)
{
    return ((u64 & 0xff00000000000000ULL) >> 56)
           | ((u64 & 0x00ff000000000000ULL) >> 40)
           | ((u64 & 0x0000ff0000000000ULL) >> 24)
           | ((u64 & 0x00000000ff000000ULL) << 8)
           | ((u64 & 0x0000000000ff0000ULL) << 24)
           | ((u64 & 0x000000000000ff00ULL) << 40)
           | ((u64 & 0x00000000000000ffULL) << 56);
}

/* 小端序读取（packed 访问，避免对齐问题） */
qjs_inline uint16_t qjs_get_u16(const uint8_t *p)
{
    return ((const struct qjs_packed_u16_t *) p)->v;
}

qjs_inline uint32_t qjs_get_u32(const uint8_t *p)
{
    return ((const struct qjs_packed_u32_t *) p)->v;
}

qjs_inline uint64_t qjs_get_u64(const uint8_t *p)
{
    return ((const struct qjs_packed_u64_t *) p)->v;
}

/* 小端序写入 */
qjs_inline void qjs_set_u16(uint8_t *p, uint16_t val)
{
    ((struct qjs_packed_u16_t *) p)->v = val;
}

qjs_inline void qjs_set_u32(uint8_t *p, uint32_t val)
{
    ((struct qjs_packed_u32_t *) p)->v = val;
}

qjs_inline void qjs_set_u64(uint8_t *p, uint64_t val)
{
    ((struct qjs_packed_u64_t *) p)->v = val;
}


/* ========================================================================
 * 第五部分：UTF-8 内联辅助
 * ======================================================================== */

/*
 * UTF-8 字节级导航函数。
 * 前导字节为 0xxxxxxx 或 11xxxxxx，续接字节为 10xxxxxx。
 * 完整的 UTF-8 编解码由 qjs_textcodec.c（Lexbor）实现。
 */

/* 向前移动一个 UTF-8 字符 */
qjs_inline const u_char *
qjs_utf8_next(const u_char *p, const u_char *end)
{
    u_char  c;
    c = *p++;
    if ((c & 0x80) != 0) {
        if (qjs_slow_path(p >= end)) {
            return p;
        }
        do {
            c = *p;
            if ((c & 0xC0) != 0x80) {
                return p;
            }
            p++;
        } while (p < end);
    }
    return p;
}


/* ========================================================================
 * 第六部分：Unix 平台头文件
 * ======================================================================== */

#ifdef __linux__
#ifdef _FORTIFY_SOURCE
/* _FORTIFY_SOURCE 不允许使用 "(void) write();" */
#undef _FORTIFY_SOURCE
#endif
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>

#if !defined(_WIN32)
#include <sys/time.h>
#include <sys/param.h>
#endif
#include <sys/stat.h>   /* MinGW 也有原生 sys/stat.h */
#include <unistd.h>

extern char  **environ;

#if defined(PATH_MAX)
#define QJS_MAX_PATH             PATH_MAX
#else
#define QJS_MAX_PATH             4096
#endif

#if defined(_WIN32)
/* Windows：MSVC 用 Win32 临界区；MinGW 优先 pthread（winpthreads），否则退化到临界区包装 */
#include <windows.h>
typedef struct {
    union {
        CRITICAL_SECTION    cs;
        void               *pt;
    } u;
    int use_pthread;
} qjs_mutex_t;
#define qjs_mutex_init(m)        qjs_mutex_init_impl(m)
#define qjs_mutex_lock(m)        qjs_mutex_lock_impl(m)
#define qjs_mutex_unlock(m)      qjs_mutex_unlock_impl(m)
#define qjs_mutex_destroy(m)     qjs_mutex_destroy_impl(m)
void qjs_mutex_init_impl(qjs_mutex_t *m);
void qjs_mutex_lock_impl(qjs_mutex_t *m);
void qjs_mutex_unlock_impl(qjs_mutex_t *m);
void qjs_mutex_destroy_impl(qjs_mutex_t *m);
#else
#include <pthread.h>
typedef pthread_mutex_t     qjs_mutex_t;
#define qjs_mutex_init(m)        pthread_mutex_init(m, NULL)
#define qjs_mutex_lock(m)        pthread_mutex_lock(m)
#define qjs_mutex_unlock(m)      pthread_mutex_unlock(m)
#define qjs_mutex_destroy(m)     pthread_mutex_destroy(m)
#endif


/* ========================================================================
 * 第七部分：QuickJS 引擎头文件与核心类 ID
 * ======================================================================== */

#define qjs_assert(condition)

/*
 * 核心类 ID 偏移量，从 64 开始避免与 QuickJS 内置类冲突。
 * 每个模块注册时分配一个唯一 ID。
 */
#define QJS_CORE_CLASS_ID_OFFSET    64
#define QJS_CORE_CLASS_ID_BUFFER            (QJS_CORE_CLASS_ID_OFFSET)
#define QJS_CORE_CLASS_ID_UINT8_ARRAY_CTOR  (QJS_CORE_CLASS_ID_OFFSET + 1)
/* TextEncoder/TextDecoder 由 qjs_textcodec（Lexbor）提供 */
#define QJS_CORE_CLASS_ID_FS_STATS          (QJS_CORE_CLASS_ID_OFFSET + 4)
#define QJS_CORE_CLASS_ID_FS_DIRENT         (QJS_CORE_CLASS_ID_OFFSET + 5)
#define QJS_CORE_CLASS_ID_FS_FILEHANDLE     (QJS_CORE_CLASS_ID_OFFSET + 6)
#define QJS_CORE_CLASS_ID_WEBCRYPTO_KEY     (QJS_CORE_CLASS_ID_OFFSET + 7)
#define QJS_CORE_CLASS_CRYPTO_HASH          (QJS_CORE_CLASS_ID_OFFSET + 8)
#define QJS_CORE_CLASS_CRYPTO_HMAC          (QJS_CORE_CLASS_ID_OFFSET + 9)

/*
 * 扩展模块静态类 ID（编译期常量，替代运行时 JS_NewClassID 动态分配）。
 * 使用静态 ID 可避免 tvbox 等频繁创建/销毁上下文场景下的全局 class ID
 * 计数器无限增长，同时消除 JS_NewClassID 内部的原子操作开销。
 */
#define QJS_CORE_CLASS_ID_CHEERIO_DOC       (QJS_CORE_CLASS_ID_OFFSET + 10)
#define QJS_CORE_CLASS_ID_CHEERIO           (QJS_CORE_CLASS_ID_OFFSET + 11)
#define QJS_CORE_CLASS_ID_URL               (QJS_CORE_CLASS_ID_OFFSET + 12)
#define QJS_CORE_CLASS_ID_URL_SEARCH_PARAMS (QJS_CORE_CLASS_ID_OFFSET + 13)
#define QJS_CORE_CLASS_ID_URL_SP_ITER       (QJS_CORE_CLASS_ID_OFFSET + 14)
#define QJS_CORE_CLASS_ID_TEXT_ENCODER      (QJS_CORE_CLASS_ID_OFFSET + 15)
#define QJS_CORE_CLASS_ID_TEXT_DECODER      (QJS_CORE_CLASS_ID_OFFSET + 16)
#define QJS_CORE_CLASS_ID_SQLITE3           (QJS_CORE_CLASS_ID_OFFSET + 17)
#define QJS_CORE_CLASS_ID_SQLITE3_STMT      (QJS_CORE_CLASS_ID_OFFSET + 18)
#define QJS_CORE_CLASS_ID_WASM_GLOBAL       (QJS_CORE_CLASS_ID_OFFSET + 19)
#define QJS_CORE_CLASS_ID_WASM_MEMORY       (QJS_CORE_CLASS_ID_OFFSET + 20)
#define QJS_CORE_CLASS_ID_WASM_TABLE        (QJS_CORE_CLASS_ID_OFFSET + 21)
#define QJS_CORE_CLASS_ID_WASM_FUNC         (QJS_CORE_CLASS_ID_OFFSET + 22)
#define QJS_CORE_CLASS_ID_WASM_MODULE       (QJS_CORE_CLASS_ID_OFFSET + 23)
#define QJS_CORE_CLASS_ID_WASM_INSTANCE     (QJS_CORE_CLASS_ID_OFFSET + 24)
#define QJS_CORE_CLASS_ID_JS_FUNC_CALLBACK  (QJS_CORE_CLASS_ID_OFFSET + 25)


/* ========================================================================
 * 第八部分：引擎扩展数据（wasm3 环境）
 * ======================================================================== */

/*
 * wasm3 环境由引擎层直接持有（存于 rt->ext_data：JS_NewContext 时创建、
 * JS_FreeRuntime 时销毁），引擎与 JNI 彻底解耦，不再设 QJSRuntime 结构体。
 * ext_data 访问器实现在 quickjs.c（JSRuntime 结构不透明）。
 */
#ifdef __cplusplus
extern "C" {
#endif
void *qjs_ext_get_data(JSRuntime *rt);
void qjs_ext_set_data(JSRuntime *rt, void *p);
#ifdef __cplusplus
} /* extern "C" */
#endif


/* ========================================================================
 * 第九部分：模块注册接口声明
 * ======================================================================== */

typedef JSModuleDef *(*qjs_addon_init_pt)(JSContext *ctx, const char *name);

typedef struct {
    const char                     *name;
    qjs_addon_init_pt               init;
} qjs_module_t;

#ifdef __cplusplus
extern "C" {
#endif

extern int  qjs_crypto_install_global(JSContext *cx, JSValueConst global);
extern int  qjs_webcrypto_install_global(JSContext *cx, JSValueConst global);
extern JSModuleDef *qjs_buffer_init(JSContext *ctx, const char *name);
extern void qjs_zlib_install_global(JSContext *ctx, JSValueConst global);
extern int  qjs_fs_install_global(JSContext *cx, JSValueConst global);
extern void qjs__mod_sqlite3_init(JSContext *ctx, JSValue ns);
extern void qjs__mod_wasm_init(JSContext *ctx, JSValue ns);
extern void qjs_setup_path(JSContext *ctx, JSValueConst global);
extern qjs_module_t  *qjs_modules[];

#ifdef __cplusplus
} /* extern "C" { */
#endif

/* Uint8Array / ArrayBuffer 创建辅助 */
JSValue qjs_new_uint8_array(JSContext *ctx, int argc, JSValueConst *argv);
JSValue qjs_new_array_buffer(JSContext *cx, uint8_t *src, size_t len);
JSValue qjs_buffer_alloc(JSContext *ctx, size_t size);
JSValue qjs_buffer_create(JSContext *ctx, u_char *start, size_t size);

/* Buffer 编解码类型与接口 */
typedef int (*qjs_buffer_encode_t)(JSContext *ctx, const qjs_str_t *src,
    qjs_str_t *dst);
typedef size_t (*qjs_buffer_encode_length_t)(JSContext *ctx,
    const qjs_str_t *src);

typedef struct {
    qjs_str_t                   name;
    qjs_buffer_encode_t         encode;
    qjs_buffer_encode_length_t  encode_length;
    qjs_buffer_encode_t         decode;
    qjs_buffer_encode_length_t  decode_length;
} qjs_buffer_encoding_t;

const qjs_buffer_encoding_t *qjs_buffer_encoding(JSContext *ctx,
    JSValueConst value, JS_BOOL thrw);

int     qjs_base64_encode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
size_t  qjs_base64_encode_length(JSContext *ctx, const qjs_str_t *src);
int     qjs_base64_decode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
size_t  qjs_base64_decode_length(JSContext *ctx, const qjs_str_t *src);
int     qjs_base64url_encode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
int     qjs_base64url_decode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
size_t  qjs_base64url_decode_length(JSContext *ctx, const qjs_str_t *src);
int     qjs_hex_encode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
size_t  qjs_hex_encode_length(JSContext *ctx, const qjs_str_t *src);
int     qjs_hex_decode(JSContext *ctx, const qjs_str_t *src, qjs_str_t *dst);
size_t  qjs_hex_decode_length(JSContext *ctx, const qjs_str_t *src);

/*
 * qjs_bytes_t：JS 值到字节序列的统一抽象
 *
 * tag 字段用于 qjs_bytes_free 判断释放方式：
 *   JS_TAG_STRING → 需要 JS_FreeCString
 *   JS_TAG_OBJECT → 无需释放（指向 JS 管理的 ArrayBuffer 内存）
 */
typedef struct {
    int                         tag;
    size_t                      length;
    u_char                      *start;
} qjs_bytes_t;

/* 判断 JSValue 是否为 TypedArray（使用新版 QuickJS API） */
qjs_inline int
qjs_is_typed_array(JSContext *cx, JSValue val)
{
    return JS_IsTypedArray(val);
}

int     qjs_to_bytes(JSContext *ctx, qjs_bytes_t *data, JSValueConst value);
void    qjs_bytes_free(JSContext *ctx, qjs_bytes_t *data);
JSValue qjs_typed_array_data(JSContext *ctx, JSValueConst value, qjs_str_t *data);
JSValue qjs_promise_result(JSContext *cx, JSValue result);
JSValue qjs_string_hex(JSContext *cx, const qjs_str_t *src);
JSValue qjs_string_base64(JSContext *cx, const qjs_str_t *src);
JSValue qjs_string_base64url(JSContext *cx, const qjs_str_t *src);

/* 判断 JSValue 是否为 null 或 undefined */
static inline JS_BOOL JS_IsNullOrUndefined(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_NULL
           || JS_VALUE_GET_TAG(v) == JS_TAG_UNDEFINED;
}

/* 新版 QuickJS 使用 JS_SameValue（2 参数） */
#define qjs_is_same_value(cx, a, b) JS_SameValue(cx, a, b)

/* 新版 QuickJS 使用 JS_IsArray（2 参数） */
#define qjs_is_array(cx, a) JS_IsArray(cx, a)

/*
 * qjs_def_func - 在对象上定义一个函数属性
 */
#define qjs_def_func(obj, name, func, argc)                                   \
    JS_SetPropertyStr(ctx, obj, name,                                         \
                      JS_NewCFunction(ctx, func, name, argc))

/*
 * qjs_def_get - 在对象上定义一个 getter 属性
 */
#define qjs_def_get(obj, name, func)                                          \
    do {                                                                      \
        JSAtom atom = JS_NewAtom(ctx, name);                                  \
        JS_DefinePropertyGetSet(ctx, obj, atom,                               \
                                JS_NewCFunction(ctx, func, name "_get", 0),  \
                                JS_UNDEFINED, 0);                             \
        JS_FreeAtom(ctx, atom);                                               \
    } while (0)


/* ========================================================================
 * 扩展模块初始化函数（实现位于 modules/ 目录）
 * 返回 0 表示成功，非 0 表示失败。
 * ======================================================================== */

#ifdef __cplusplus
extern "C" {
#endif
/* 初始化 HTML 解析模块（CheerioDoc / Cheerio 类，基于 lexbor） */
int qjs_html_init(JSContext *ctx);

/* 初始化 URL 模块（URL / URLSearchParams 类） */
int qjs_url_init(JSContext *ctx);

/* 初始化文本编解码模块（TextEncoder / TextDecoder 类） */
int qjs_textcodec_init(JSContext *ctx);
#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _QJS_NATIVE_H_INCLUDED_ */
