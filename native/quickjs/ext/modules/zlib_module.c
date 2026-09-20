/*
 * zlib_module.c - Zlib 模块（合并实现，原 5 个拆分文件已并入本文件）
 *
 * 结构（自上而下）：
 *   1. 选项解析    zlib_options_init_defaults / zlib_parse_options / zlib_options_cleanup
 *   2. 核心流操作  zlib_context_new/free、zlib_init_deflate/inflate、zlib_cleanup、错误处理
 *   3. 同步压缩    zlib_deflate_sync / zlib_inflate_sync
 *   4. 常量与工具  zlib_export_constants、crc32/adler32（zlib_export_utilities）
 *   5. 模块注册    qjs_zlib_install_global（注册 gzip/gunzip/deflate/inflate/unzip）
 *
 * 内部接口声明位于 zlib_internal.h。
 *
 * Copyright (C) QuickJS Native 项目 contributors
 */

#include <stdlib.h>
#include <string.h>
#include "zlib_internal.h"



/* ---- ===== 选项解析（原 zlib_options.c） ===== ---- */


// Initialize default options
void zlib_options_init_defaults(ZlibOptions* opts) {
  opts->level = Z_DEFAULT_COMPRESSION;
  opts->windowBits = 15;  // Default window size
  opts->memLevel = 8;     // Default memory level
  opts->strategy = Z_DEFAULT_STRATEGY;
  opts->chunkSize = 16 * 1024;  // 16KB default chunk size
  opts->flush = Z_NO_FLUSH;
  opts->finishFlush = Z_FINISH;
  opts->has_dictionary = false;
  opts->dictionary = NULL;
  opts->dictionary_len = 0;
}

// Parse options from JSValue
int zlib_parse_options(JSContext* ctx, JSValue opts_val, ZlibOptions* opts) {
  // Initialize with defaults first
  zlib_options_init_defaults(opts);

  // If no options provided, use defaults
  if (JS_IsUndefined(opts_val) || JS_IsNull(opts_val)) {
    return 0;
  }

  // Options must be an object
  if (!JS_IsObject(opts_val)) {
    JS_ThrowTypeError(ctx, "options must be an object");
    return -1;
  }

  JSValue val;

  // Parse level (0-9, or -1 for default)
  val = JS_GetPropertyStr(ctx, opts_val, "level");
  if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
    int32_t level;
    if (JS_ToInt32(ctx, &level, val) < 0) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    if (level < -1 || level > 9) {
      JS_FreeValue(ctx, val);
      JS_ThrowRangeError(ctx, "level must be between -1 and 9");
      return -1;
    }
    opts->level = level;
  }
  JS_FreeValue(ctx, val);

  // Parse windowBits (8-15)
  val = JS_GetPropertyStr(ctx, opts_val, "windowBits");
  if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
    int32_t windowBits;
    if (JS_ToInt32(ctx, &windowBits, val) < 0) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    // Allow negative for raw deflate, and +16 for gzip
    if (abs(windowBits) < 8 || abs(windowBits) > 15 + 16) {
      JS_FreeValue(ctx, val);
      JS_ThrowRangeError(ctx, "windowBits must be between 8 and 15");
      return -1;
    }
    opts->windowBits = windowBits;
  }
  JS_FreeValue(ctx, val);

  // Parse memLevel (1-9)
  val = JS_GetPropertyStr(ctx, opts_val, "memLevel");
  if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
    int32_t memLevel;
    if (JS_ToInt32(ctx, &memLevel, val) < 0) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    if (memLevel < 1 || memLevel > 9) {
      JS_FreeValue(ctx, val);
      JS_ThrowRangeError(ctx, "memLevel must be between 1 and 9");
      return -1;
    }
    opts->memLevel = memLevel;
  }
  JS_FreeValue(ctx, val);

  // Parse strategy
  val = JS_GetPropertyStr(ctx, opts_val, "strategy");
  if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
    int32_t strategy;
    if (JS_ToInt32(ctx, &strategy, val) < 0) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    opts->strategy = strategy;
  }
  JS_FreeValue(ctx, val);

  // Parse chunkSize
  val = JS_GetPropertyStr(ctx, opts_val, "chunkSize");
  if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
    int64_t chunkSize;
    if (JS_ToInt64(ctx, &chunkSize, val) < 0) {
      JS_FreeValue(ctx, val);
      return -1;
    }
    if (chunkSize <= 0) {
      JS_FreeValue(ctx, val);
      JS_ThrowRangeError(ctx, "chunkSize must be positive");
      return -1;
    }
    opts->chunkSize = (size_t)chunkSize;
  }
  JS_FreeValue(ctx, val);

  return 0;
}

// Clean up options
void zlib_options_cleanup(ZlibOptions* opts) {
  if (opts->dictionary) {
    free(opts->dictionary);
    opts->dictionary = NULL;
  }
  opts->dictionary_len = 0;
  opts->has_dictionary = false;
}


/* ---- ===== 核心流操作（原 zlib_core.c） ===== ---- */


// Create new zlib context
ZlibContext* zlib_context_new(JSContext* ctx) {
  ZlibContext* zctx = (ZlibContext*)malloc(sizeof(ZlibContext));
  if (!zctx) {
    return NULL;
  }

  memset(zctx, 0, sizeof(ZlibContext));
  zlib_options_init_defaults(&zctx->opts);
  zctx->initialized = false;
  zctx->is_deflate = false;
  zctx->output_buffer = NULL;
  zctx->output_capacity = 0;
  zctx->output_size = 0;

  return zctx;
}

// Free zlib context
void zlib_context_free(ZlibContext* zctx) {
  if (!zctx) {
    return;
  }

  if (zctx->initialized) {
    zlib_cleanup(zctx);
  }

  if (zctx->output_buffer) {
    free(zctx->output_buffer);
    zctx->output_buffer = NULL;
  }

  zlib_options_cleanup(&zctx->opts);
  free(zctx);
}

// Initialize deflate stream
int zlib_init_deflate(ZlibContext* zctx, const ZlibOptions* opts, int format) {
  if (zctx->initialized) {
    return Z_STREAM_ERROR;
  }

  memset(&zctx->strm, 0, sizeof(z_stream));

  // Copy options
  if (opts) {
    memcpy(&zctx->opts, opts, sizeof(ZlibOptions));
  } else {
    zlib_options_init_defaults(&zctx->opts);
  }

  zctx->is_deflate = true;

  // Adjust windowBits based on format
  int windowBits = zctx->opts.windowBits;
  if (format == ZLIB_FORMAT_GZIP) {
    windowBits += 16;  // Add 16 for gzip format
  } else if (format == ZLIB_FORMAT_RAW) {
    windowBits = -windowBits;  // Negative for raw deflate
  }
  // ZLIB_FORMAT_DEFLATE uses windowBits as-is

  int ret =
      deflateInit2(&zctx->strm, zctx->opts.level, Z_DEFLATED, windowBits, zctx->opts.memLevel, zctx->opts.strategy);

  if (ret == Z_OK) {
    zctx->initialized = true;
  }

  return ret;
}

// Initialize inflate stream
int zlib_init_inflate(ZlibContext* zctx, const ZlibOptions* opts, int format) {
  if (zctx->initialized) {
    return Z_STREAM_ERROR;
  }

  memset(&zctx->strm, 0, sizeof(z_stream));

  // Copy options
  if (opts) {
    memcpy(&zctx->opts, opts, sizeof(ZlibOptions));
  } else {
    zlib_options_init_defaults(&zctx->opts);
  }

  zctx->is_deflate = false;

  // Adjust windowBits based on format
  int windowBits = zctx->opts.windowBits;
  if (format == ZLIB_FORMAT_GZIP) {
    windowBits += 16;  // Add 16 for gzip format
  } else if (format == ZLIB_FORMAT_RAW) {
    windowBits = -windowBits;  // Negative for raw deflate
  }
  // ZLIB_FORMAT_DEFLATE uses windowBits as-is
  // Use +32 to auto-detect format (gzip or deflate)

  int ret = inflateInit2(&zctx->strm, windowBits);

  if (ret == Z_OK) {
    zctx->initialized = true;
  }

  return ret;
}

// Cleanup zlib stream
void zlib_cleanup(ZlibContext* zctx) {
  if (!zctx || !zctx->initialized) {
    return;
  }

  if (zctx->is_deflate) {
    deflateEnd(&zctx->strm);
  } else {
    inflateEnd(&zctx->strm);
  }

  zctx->initialized = false;
}

// Get error message for zlib error code
const char* zlib_error_message(int err_code) {
  switch (err_code) {
    case Z_OK:
      return "OK";
    case Z_STREAM_END:
      return "Stream end";
    case Z_NEED_DICT:
      return "Need dictionary";
    case Z_ERRNO:
      return "File error";
    case Z_STREAM_ERROR:
      return "Stream error";
    case Z_DATA_ERROR:
      return "Data error";
    case Z_MEM_ERROR:
      return "Memory error";
    case Z_BUF_ERROR:
      return "Buffer error";
    case Z_VERSION_ERROR:
      return "Version error";
    default:
      return "Unknown error";
  }
}

// Throw zlib error
JSValue zlib_throw_error(JSContext* ctx, int err_code, const char* msg) {
  const char* err_msg = zlib_error_message(err_code);

  if (msg) {
    return JS_ThrowInternalError(ctx, "%s: %s", msg, err_msg);
  } else {
    return JS_ThrowInternalError(ctx, "%s", err_msg);
  }
}


/* ---- ===== 同步压缩（原 zlib_sync.c） ===== ---- */


// Synchronous deflate operation
JSValue zlib_deflate_sync(JSContext* ctx, const uint8_t* input, size_t input_len, const ZlibOptions* opts, int format) {
  ZlibContext* zctx = zlib_context_new(ctx);
  if (!zctx) {
    return JS_ThrowOutOfMemory(ctx);
  }

  // Initialize deflate stream
  int ret = zlib_init_deflate(zctx, opts, format);
  if (ret != Z_OK) {
    zlib_context_free(zctx);
    return zlib_throw_error(ctx, ret, "Failed to initialize deflate");
  }

  // Estimate output size (worst case: slightly larger than input)
  size_t output_capacity = deflateBound(&zctx->strm, input_len);
  uint8_t* output_buffer = (uint8_t*)malloc(output_capacity);
  if (!output_buffer) {
    zlib_context_free(zctx);
    return JS_ThrowOutOfMemory(ctx);
  }

  // Set up input and output
  zctx->strm.next_in = (Bytef*)input;
  zctx->strm.avail_in = input_len;
  zctx->strm.next_out = output_buffer;
  zctx->strm.avail_out = output_capacity;

  // Perform compression
  ret = deflate(&zctx->strm, Z_FINISH);
  if (ret != Z_STREAM_END) {
    free(output_buffer);
    zlib_context_free(zctx);
    return zlib_throw_error(ctx, ret, "Deflate failed");
  }

  size_t output_size = zctx->strm.total_out;

  // Create result buffer - just return the ArrayBuffer wrapped as Buffer
  JSValue array_buffer = JS_NewArrayBufferCopy(ctx, output_buffer, output_size);
  free(output_buffer);
  zlib_context_free(zctx);

  if (JS_IsException(array_buffer)) {
    return array_buffer;
  }

  // Wrap in Uint8Array to make it Buffer-like
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JSValue args[1] = {array_buffer};
  JSValue result = JS_CallConstructor(ctx, uint8_ctor, 1, args);

  JS_FreeValue(ctx, array_buffer);
  JS_FreeValue(ctx, uint8_ctor);
  JS_FreeValue(ctx, global);

  return result;
}

// Synchronous inflate operation
JSValue zlib_inflate_sync(JSContext* ctx, const uint8_t* input, size_t input_len, const ZlibOptions* opts, int format) {
  ZlibContext* zctx = zlib_context_new(ctx);
  if (!zctx) {
    return JS_ThrowOutOfMemory(ctx);
  }

  // Initialize inflate stream
  int ret = zlib_init_inflate(zctx, opts, format);
  if (ret != Z_OK) {
    zlib_context_free(zctx);
    return zlib_throw_error(ctx, ret, "Failed to initialize inflate");
  }

  // Use chunk-based decompression since we don't know output size
  size_t chunk_size = opts ? opts->chunkSize : 16 * 1024;
  size_t output_capacity = chunk_size;
  uint8_t* output_buffer = (uint8_t*)malloc(output_capacity);
  if (!output_buffer) {
    zlib_context_free(zctx);
    return JS_ThrowOutOfMemory(ctx);
  }

  size_t output_size = 0;

  // Set up input
  zctx->strm.next_in = (Bytef*)input;
  zctx->strm.avail_in = input_len;

  // Decompress in chunks
  do {
    // Ensure we have space
    if (output_size + chunk_size > output_capacity) {
      output_capacity = output_capacity * 2;
      uint8_t* new_buffer = (uint8_t*)realloc(output_buffer, output_capacity);
      if (!new_buffer) {
        free(output_buffer);
        zlib_context_free(zctx);
        return JS_ThrowOutOfMemory(ctx);
      }
      output_buffer = new_buffer;
    }

    zctx->strm.next_out = output_buffer + output_size;
    zctx->strm.avail_out = chunk_size;

    ret = inflate(&zctx->strm, Z_NO_FLUSH);

    if (ret != Z_OK && ret != Z_STREAM_END) {
      free(output_buffer);
      zlib_context_free(zctx);
      return zlib_throw_error(ctx, ret, "Inflate failed");
    }

    output_size += (chunk_size - zctx->strm.avail_out);

  } while (ret != Z_STREAM_END);

  // Create result buffer - just return the ArrayBuffer wrapped as Uint8Array
  JSValue array_buffer = JS_NewArrayBufferCopy(ctx, output_buffer, output_size);
  free(output_buffer);
  zlib_context_free(zctx);

  if (JS_IsException(array_buffer)) {
    return array_buffer;
  }

  // Wrap in Uint8Array to make it Buffer-like
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue uint8_ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
  JSValue args[1] = {array_buffer};
  JSValue result = JS_CallConstructor(ctx, uint8_ctor, 1, args);

  JS_FreeValue(ctx, array_buffer);
  JS_FreeValue(ctx, uint8_ctor);
  JS_FreeValue(ctx, global);

  return result;
}


/* ---- ===== 常量与工具（原 zlib_constants.c） ===== ---- */


// Export zlib constants to JavaScript
void zlib_export_constants(JSContext* ctx, JSValue exports) {
  JSValue constants = JS_NewObject(ctx);

  // Compression levels
  JS_SetPropertyStr(ctx, constants, "Z_NO_COMPRESSION", JS_NewInt32(ctx, Z_NO_COMPRESSION));
  JS_SetPropertyStr(ctx, constants, "Z_BEST_SPEED", JS_NewInt32(ctx, Z_BEST_SPEED));
  JS_SetPropertyStr(ctx, constants, "Z_BEST_COMPRESSION", JS_NewInt32(ctx, Z_BEST_COMPRESSION));
  JS_SetPropertyStr(ctx, constants, "Z_DEFAULT_COMPRESSION", JS_NewInt32(ctx, Z_DEFAULT_COMPRESSION));

  // Compression strategy
  JS_SetPropertyStr(ctx, constants, "Z_FILTERED", JS_NewInt32(ctx, Z_FILTERED));
  JS_SetPropertyStr(ctx, constants, "Z_HUFFMAN_ONLY", JS_NewInt32(ctx, Z_HUFFMAN_ONLY));
  JS_SetPropertyStr(ctx, constants, "Z_RLE", JS_NewInt32(ctx, Z_RLE));
  JS_SetPropertyStr(ctx, constants, "Z_FIXED", JS_NewInt32(ctx, Z_FIXED));
  JS_SetPropertyStr(ctx, constants, "Z_DEFAULT_STRATEGY", JS_NewInt32(ctx, Z_DEFAULT_STRATEGY));

  // Flush values
  JS_SetPropertyStr(ctx, constants, "Z_NO_FLUSH", JS_NewInt32(ctx, Z_NO_FLUSH));
  JS_SetPropertyStr(ctx, constants, "Z_PARTIAL_FLUSH", JS_NewInt32(ctx, Z_PARTIAL_FLUSH));
  JS_SetPropertyStr(ctx, constants, "Z_SYNC_FLUSH", JS_NewInt32(ctx, Z_SYNC_FLUSH));
  JS_SetPropertyStr(ctx, constants, "Z_FULL_FLUSH", JS_NewInt32(ctx, Z_FULL_FLUSH));
  JS_SetPropertyStr(ctx, constants, "Z_FINISH", JS_NewInt32(ctx, Z_FINISH));
  JS_SetPropertyStr(ctx, constants, "Z_BLOCK", JS_NewInt32(ctx, Z_BLOCK));
  JS_SetPropertyStr(ctx, constants, "Z_TREES", JS_NewInt32(ctx, Z_TREES));

  // Return codes
  JS_SetPropertyStr(ctx, constants, "Z_OK", JS_NewInt32(ctx, Z_OK));
  JS_SetPropertyStr(ctx, constants, "Z_STREAM_END", JS_NewInt32(ctx, Z_STREAM_END));
  JS_SetPropertyStr(ctx, constants, "Z_NEED_DICT", JS_NewInt32(ctx, Z_NEED_DICT));
  JS_SetPropertyStr(ctx, constants, "Z_ERRNO", JS_NewInt32(ctx, Z_ERRNO));
  JS_SetPropertyStr(ctx, constants, "Z_STREAM_ERROR", JS_NewInt32(ctx, Z_STREAM_ERROR));
  JS_SetPropertyStr(ctx, constants, "Z_DATA_ERROR", JS_NewInt32(ctx, Z_DATA_ERROR));
  JS_SetPropertyStr(ctx, constants, "Z_MEM_ERROR", JS_NewInt32(ctx, Z_MEM_ERROR));
  JS_SetPropertyStr(ctx, constants, "Z_BUF_ERROR", JS_NewInt32(ctx, Z_BUF_ERROR));
  JS_SetPropertyStr(ctx, constants, "Z_VERSION_ERROR", JS_NewInt32(ctx, Z_VERSION_ERROR));

  // Add zlib version
  JSValue versions = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, versions, "zlib", JS_NewString(ctx, ZLIB_VERSION));
  JS_SetPropertyStr(ctx, exports, "versions", versions);

  // Add constants to exports (both as direct properties and as constants object)
  // Direct properties for compatibility
  JSPropertyEnum* props;
  uint32_t prop_count;
  if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, constants, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
    for (uint32_t i = 0; i < prop_count; i++) {
      JSValue key = JS_AtomToString(ctx, props[i].atom);
      const char* key_str = JS_ToCString(ctx, key);
      if (key_str) {
        JSValue val = JS_GetProperty(ctx, constants, props[i].atom);
        JS_SetPropertyStr(ctx, exports, key_str, JS_DupValue(ctx, val));
        JS_FreeValue(ctx, val);
        JS_FreeCString(ctx, key_str);
      }
      JS_FreeValue(ctx, key);
      JS_FreeAtom(ctx, props[i].atom);
    }
    js_free(ctx, props);
  }

  // Also export as constants object
  JS_SetPropertyStr(ctx, exports, "constants", constants);
}

// Implement crc32 utility
static JSValue js_zlib_crc32(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "crc32 requires at least 1 argument");
  }

  // Get buffer data
  size_t data_len;
  uint8_t* data = NULL;

  // Try to get as ArrayBuffer
  data = JS_GetArrayBuffer(ctx, &data_len, argv[0]);
  if (!data) {
    // Try to get as typed array
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, NULL, NULL);
    if (!JS_IsException(buffer)) {
      data = JS_GetArrayBuffer(ctx, &data_len, buffer);
      JS_FreeValue(ctx, buffer);
    }
  }

  if (!data) {
    return JS_ThrowTypeError(ctx, "argument must be a Buffer or Uint8Array");
  }

  // Get initial value if provided
  uint32_t initial = 0;
  if (argc > 1 && !JS_IsUndefined(argv[1])) {
    if (JS_ToUint32(ctx, &initial, argv[1]) < 0) {
      return JS_EXCEPTION;
    }
  }

  // Calculate CRC32
  uint32_t result = crc32(initial, data, data_len);

  return JS_NewUint32(ctx, result);
}

// Implement adler32 utility
static JSValue js_zlib_adler32(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "adler32 requires at least 1 argument");
  }

  // Get buffer data
  size_t data_len;
  uint8_t* data = NULL;

  // Try to get as ArrayBuffer
  data = JS_GetArrayBuffer(ctx, &data_len, argv[0]);
  if (!data) {
    // Try to get as typed array
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, argv[0], NULL, NULL, NULL);
    if (!JS_IsException(buffer)) {
      data = JS_GetArrayBuffer(ctx, &data_len, buffer);
      JS_FreeValue(ctx, buffer);
    }
  }

  if (!data) {
    return JS_ThrowTypeError(ctx, "argument must be a Buffer or Uint8Array");
  }

  // Get initial value if provided
  uint32_t initial = 1;  // adler32 starts with 1
  if (argc > 1 && !JS_IsUndefined(argv[1])) {
    if (JS_ToUint32(ctx, &initial, argv[1]) < 0) {
      return JS_EXCEPTION;
    }
  }

  // Calculate Adler-32
  uint32_t result = adler32(initial, data, data_len);

  return JS_NewUint32(ctx, result);
}

// Export utilities
static const JSCFunctionListEntry js_zlib_utils[] = {
    JS_CFUNC_DEF("crc32", 1, js_zlib_crc32),
    JS_CFUNC_DEF("adler32", 1, js_zlib_adler32),
};

void zlib_export_utilities(JSContext* ctx, JSValue exports) {
  JS_SetPropertyFunctionList(ctx, exports, js_zlib_utils, qjs_nitems(js_zlib_utils));
}


/* ---- ===== 模块注册（原 zlib_module.c） ===== ---- */


// Helper to get buffer data from JSValue
static int get_buffer_data(JSContext* ctx, JSValueConst val, const uint8_t** data, size_t* len) {
  size_t size;
  uint8_t* buf = JS_GetArrayBuffer(ctx, &size, val);

  if (buf) {
    *data = buf;
    *len = size;
    return 0;
  }

  // Try to get as typed array
  JSValue buffer = JS_GetTypedArrayBuffer(ctx, val, NULL, NULL, NULL);
  if (!JS_IsException(buffer)) {
    buf = JS_GetArrayBuffer(ctx, &size, buffer);
    JS_FreeValue(ctx, buffer);
    if (buf) {
      *data = buf;
      *len = size;
      return 0;
    }
  }
  /* 回退到字符串 */
  buf = JS_ToCStringLen(ctx, &size, val);
  if (buf) {
    // 注意：当从字符串获取数据时，buf需要在使用后通过JS_FreeCString释放
    // 但由于我们返回的是const uint8_t*，调用者需要负责释放
    *data = buf;
    *len = size;
    return 1; // 返回1表示需要释放data
  }

  JS_ThrowTypeError(ctx, "argument must be a Buffer or Uint8Array");
  return -1;
}

// gzipSync implementation
static JSValue js_zlib_gzip_sync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "gzipSync requires at least 1 argument");
  }

  const uint8_t* input;
  size_t input_len;
  int need_free = get_buffer_data(ctx, argv[0], &input, &input_len);
  if (need_free < 0) {
    return JS_EXCEPTION;
  }

  ZlibOptions opts;
  if (argc > 1 && zlib_parse_options(ctx, argv[1], &opts) < 0) {
    if (need_free == 1) {
      JS_FreeCString(ctx, (const char*)input);
    }
    return JS_EXCEPTION;
  } else if (argc <= 1) {
    zlib_options_init_defaults(&opts);
  }

  JSValue result = zlib_deflate_sync(ctx, input, input_len, &opts, ZLIB_FORMAT_GZIP);

  if (need_free == 1) {
    JS_FreeCString(ctx, (const char*)input);
  }

  if (argc > 1) {
    zlib_options_cleanup(&opts);
  }

  return result;
}

// gunzipSync implementation
static JSValue js_zlib_gunzip_sync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "gunzipSync requires at least 1 argument");
  }

  const uint8_t* input;
  size_t input_len;
  int need_free = get_buffer_data(ctx, argv[0], &input, &input_len);
  if (need_free < 0) {
    return JS_EXCEPTION;
  }

  ZlibOptions opts;
  if (argc > 1 && zlib_parse_options(ctx, argv[1], &opts) < 0) {
    if (need_free == 1) {
      JS_FreeCString(ctx, (const char*)input);
    }
    return JS_EXCEPTION;
  } else if (argc <= 1) {
    zlib_options_init_defaults(&opts);
  }

  JSValue result = zlib_inflate_sync(ctx, input, input_len, &opts, ZLIB_FORMAT_GZIP);

  if (need_free == 1) {
    JS_FreeCString(ctx, (const char*)input);
  }

  if (argc > 1) {
    zlib_options_cleanup(&opts);
  }

  return result;
}

// deflateSync implementation
static JSValue js_zlib_deflate_sync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "deflateSync requires at least 1 argument");
  }

  const uint8_t* input;
  size_t input_len;
  int need_free = get_buffer_data(ctx, argv[0], &input, &input_len);
  if (need_free < 0) {
    return JS_EXCEPTION;
  }

  ZlibOptions opts;
  if (argc > 1 && zlib_parse_options(ctx, argv[1], &opts) < 0) {
    if (need_free == 1) {
      JS_FreeCString(ctx, (const char*)input);
    }
    return JS_EXCEPTION;
  } else if (argc <= 1) {
    zlib_options_init_defaults(&opts);
  }

  JSValue result = zlib_deflate_sync(ctx, input, input_len, &opts, ZLIB_FORMAT_DEFLATE);

  if (need_free == 1) {
    JS_FreeCString(ctx, (const char*)input);
  }

  if (argc > 1) {
    zlib_options_cleanup(&opts);
  }

  return result;
}

// inflateSync implementation
static JSValue js_zlib_inflate_sync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "inflateSync requires at least 1 argument");
  }

  const uint8_t* input;
  size_t input_len;
  int need_free = get_buffer_data(ctx, argv[0], &input, &input_len);
  if (need_free < 0) {
    return JS_EXCEPTION;
  }

  ZlibOptions opts;
  if (argc > 1 && zlib_parse_options(ctx, argv[1], &opts) < 0) {
    if (need_free == 1) {
      JS_FreeCString(ctx, (const char*)input);
    }
    return JS_EXCEPTION;
  } else if (argc <= 1) {
    zlib_options_init_defaults(&opts);
  }

  JSValue result = zlib_inflate_sync(ctx, input, input_len, &opts, ZLIB_FORMAT_DEFLATE);

  if (need_free == 1) {
    JS_FreeCString(ctx, (const char*)input);
  }

  if (argc > 1) {
    zlib_options_cleanup(&opts);
  }

  return result;
}

// unzipSync implementation (auto-detect gzip or deflate)
static JSValue js_zlib_unzip_sync(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "unzipSync requires at least 1 argument");
  }

  const uint8_t* input;
  size_t input_len;
  int need_free = get_buffer_data(ctx, argv[0], &input, &input_len);
  if (need_free < 0) {
    return JS_EXCEPTION;
  }

  ZlibOptions opts;
  if (argc > 1 && zlib_parse_options(ctx, argv[1], &opts) < 0) {
    if (need_free == 1) {
      JS_FreeCString(ctx, (const char*)input);
    }
    return JS_EXCEPTION;
  } else if (argc <= 1) {
    zlib_options_init_defaults(&opts);
  }

  // Use windowBits + 32 to auto-detect gzip or deflate format
  // This is a special zlib feature
  opts.windowBits = 15 + 32;  // Enable auto-detection

  JSValue result = zlib_inflate_sync(ctx, input, input_len, &opts, ZLIB_FORMAT_DEFLATE);

  if (need_free == 1) {
    JS_FreeCString(ctx, (const char*)input);
  }

  if (argc > 1) {
    zlib_options_cleanup(&opts);
  }

  return result;
}

static const JSCFunctionListEntry js_zlib_funcs[] = {
    JS_CFUNC_DEF("gzip", 1, js_zlib_gzip_sync),
    JS_CFUNC_DEF("gunzip", 1, js_zlib_gunzip_sync),
    JS_CFUNC_DEF("deflate", 1, js_zlib_deflate_sync),
    JS_CFUNC_DEF("inflate", 1, js_zlib_inflate_sync),
    JS_CFUNC_DEF("unzip", 1, js_zlib_unzip_sync),
};
void qjs_zlib_install_global(JSContext *ctx, JSValueConst global)
{
    JSValue zlib = JS_NewObject(ctx);
    if (JS_IsException(zlib)) {
        return;
    }
    
    JS_SetPropertyFunctionList(ctx, zlib,
                               js_zlib_funcs,
                               qjs_nitems(js_zlib_funcs));
    // Export constants and utilities
    zlib_export_constants(ctx, zlib);
    zlib_export_utilities(ctx, zlib);
    JS_DefinePropertyValueStr(ctx, global, "zlib", zlib, JS_PROP_C_W_E);
}
