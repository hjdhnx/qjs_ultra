/*
 * zlib_internal.h - Zlib 内部头文件
 *
 * 定义 zlib 模块内部使用的结构体、宏和函数声明。
包含压缩上下文、内存池、选项解析等内部接口。
 *
 * Copyright (C) QuickJS Native 项目 contributors
 */

#ifndef QJS_NODE_ZLIB_INTERNAL_H
#define QJS_NODE_ZLIB_INTERNAL_H

/* 项目统一头文件（含 QuickJS 类型与扩展模块声明） */
#include "qjs_native.h"
#include <stdbool.h>
#include <stdint.h>
#include <zlib.h>

// Forward declarations
typedef struct ZlibContext ZlibContext;
typedef struct ZlibOptions ZlibOptions;

// Zlib options structure
struct ZlibOptions {
  int level;         // Compression level (0-9)
  int windowBits;    // Window size (8-15)
  int memLevel;      // Memory level (1-9)
  int strategy;      // Compression strategy
  size_t chunkSize;  // Chunk size for streaming
  int flush;         // Flush mode
  int finishFlush;   // Finish flush mode
  bool has_dictionary;
  uint8_t* dictionary;
  size_t dictionary_len;
};

// Zlib context structure
struct ZlibContext {
  z_stream strm;
  ZlibOptions opts;
  bool initialized;
  bool is_deflate;  // true for deflate, false for inflate
  uint8_t* output_buffer;
  size_t output_capacity;
  size_t output_size;
};

// Core functions
ZlibContext* zlib_context_new(JSContext* ctx);
void zlib_context_free(ZlibContext* ctx);
int zlib_init_deflate(ZlibContext* ctx, const ZlibOptions* opts, int format);
int zlib_init_inflate(ZlibContext* ctx, const ZlibOptions* opts, int format);
void zlib_cleanup(ZlibContext* ctx);

// Options parsing
int zlib_parse_options(JSContext* ctx, JSValue opts_val, ZlibOptions* opts);
void zlib_options_init_defaults(ZlibOptions* opts);
void zlib_options_cleanup(ZlibOptions* opts);

// Synchronous operations
JSValue zlib_deflate_sync(JSContext* ctx, const uint8_t* input, size_t input_len, const ZlibOptions* opts, int format);
JSValue zlib_inflate_sync(JSContext* ctx, const uint8_t* input, size_t input_len, const ZlibOptions* opts, int format);

// Error handling
JSValue zlib_throw_error(JSContext* ctx, int err_code, const char* msg);
const char* zlib_error_message(int err_code);

// Constants and utilities
void zlib_export_constants(JSContext* ctx, JSValue exports);
void zlib_export_utilities(JSContext* ctx, JSValue exports);

// Compression formats
#define ZLIB_FORMAT_GZIP 16    // Add 16 to windowBits for gzip
#define ZLIB_FORMAT_DEFLATE 0  // Default windowBits for deflate
#define ZLIB_FORMAT_RAW (-1)   // Negative windowBits for raw deflate

#endif  // QJS_NODE_ZLIB_INTERNAL_H
