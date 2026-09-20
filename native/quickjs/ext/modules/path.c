/*
 * path.c - 路径处理模块实现
 *
 * 提供 Node.js 风格的 path API（join/resolve/normalize/parse 等）。
基于 POSIX 路径规范实现。
 *
 * Copyright (C) QuickJS Native 项目 contributors
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "qjs_native.h"

#ifdef _WIN32
#define PATH_SEPARATOR '\\'
#define PATH_SEPARATOR_STR "\\"
#define PATH_DELIMITER ';'
#define IS_PATH_SEPARATOR(c) ((c) == '\\' || (c) == '/')
#else
#define PATH_SEPARATOR '/'
#define PATH_SEPARATOR_STR "/"
#define PATH_DELIMITER ':'
#define IS_PATH_SEPARATOR(c) ((c) == '/')
#endif

// Helper: Check if path is absolute
static bool is_absolute_path(const char* path) {
  if (!path || !*path)
    return false;

#ifdef _WIN32
  // Windows: C:\ or \\ (UNC) or /
  if ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) {
    return path[1] == ':';
  }
  return IS_PATH_SEPARATOR(path[0]);
#else
  // Unix: starts with /
  return path[0] == '/';
#endif
}

// Normalize slashes in path
static char* normalize_separators(const char* path) {
  if (!path)
    return NULL;

  char* result = strdup(path);
  if (!result)
    return NULL;

  for (char* p = result; *p; p++) {
#ifdef _WIN32
    if (*p == '/')
      *p = '\\';
#else
    if (*p == '\\')
      *p = '/';
#endif
  }

  return result;
}

// Normalize path by resolving . and .. segments
static char* normalize_path(const char* path) {
  if (!path || !*path)
    return strdup(".");

  // First normalize separators
  char* normalized = normalize_separators(path);
  if (!normalized)
    return NULL;

  bool is_absolute = is_absolute_path(normalized);
  size_t len = strlen(normalized);
  char* result = malloc(len + 2);  // +2 for potential leading / and null terminator
  if (!result) {
    free(normalized);
    return NULL;
  }

  char* segments[256];  // Max 256 path segments
  int segment_count = 0;

  // Split path into segments
  char* token = strtok(normalized, PATH_SEPARATOR_STR);
  while (token && segment_count < 256) {
    if (strcmp(token, ".") == 0) {
      // Skip . segments
      token = strtok(NULL, PATH_SEPARATOR_STR);
      continue;
    }

    if (strcmp(token, "..") == 0) {
      // .. segment - go up one directory
      if (segment_count > 0 && strcmp(segments[segment_count - 1], "..") != 0) {
        // Remove previous segment (unless it's also ..)
        segment_count--;
      } else if (!is_absolute) {
        // Keep .. for relative paths
        segments[segment_count++] = "..";
      }
      // For absolute paths, .. at root is ignored
    } else {
      // Regular segment
      segments[segment_count++] = token;
    }

    token = strtok(NULL, PATH_SEPARATOR_STR);
  }

  // Rebuild path
  result[0] = '\0';

  if (is_absolute) {
    strcpy(result, PATH_SEPARATOR_STR);
  }

  for (int i = 0; i < segment_count; i++) {
    if (i > 0) {
      strcat(result, PATH_SEPARATOR_STR);
    }
    strcat(result, segments[i]);
  }

  // Handle empty result
  if (result[0] == '\0') {
    strcpy(result, ".");
  }

  free(normalized);
  return result;
}

// node:path.join implementation
static JSValue js_path_join(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc == 0) {
    return JS_NewString(ctx, ".");
  }

  // Calculate total length needed
  size_t total_len = 0;
  for (int i = 0; i < argc; i++) {
    size_t len;
    const char* str = JS_ToCStringLen(ctx, &len, argv[i]);
    if (!str)
      return JS_EXCEPTION;
    total_len += len + 1;  // +1 for separator
    JS_FreeCString(ctx, str);
  }

  // Allocate result buffer
  char* result = malloc(total_len + 1);
  if (!result) {
    JS_ThrowOutOfMemory(ctx);
    return JS_EXCEPTION;
  }

  result[0] = '\0';
  bool first = true;

  // Join all paths
  for (int i = 0; i < argc; i++) {
    const char* part = JS_ToCString(ctx, argv[i]);
    if (!part) {
      free(result);
      return JS_EXCEPTION;
    }

    if (*part) {  // Skip empty strings
      // Remove leading separators from part if not first.
      // 注意：必须用局部副本偏移，JS_FreeCString 需要原始指针
      //（QuickJS 按原始指针定位字符串单元引用计数，释放偏移指针会破坏堆）。
      const char* part_p = part;
      if (!first) {
        while (IS_PATH_SEPARATOR(*part_p)) {
          part_p++;
        }
      }

      if (!first && strlen(result) > 0 && !IS_PATH_SEPARATOR(result[strlen(result) - 1])) {
        strcat(result, PATH_SEPARATOR_STR);
      }
      strcat(result, part_p);
      first = false;
    }

    JS_FreeCString(ctx, part);
  }

  if (result[0] == '\0') {
    strcpy(result, ".");
  }

  // Normalize separators and resolve . and .. segments
  char* temp_normalized = normalize_separators(result);
  free(result);

  // Then apply full normalization to handle . and .. segments
  char* fully_normalized = normalize_path(temp_normalized);
  free(temp_normalized);

  JSValue ret = JS_NewString(ctx, fully_normalized);
  free(fully_normalized);
  return ret;
}

// node:path.resolve implementation
static JSValue js_path_resolve(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  char resolved[4096] = "";
  bool absolute = false;

  // Process arguments from right to left
  for (int i = argc - 1; i >= 0 && !absolute; i--) {
    const char* path = JS_ToCString(ctx, argv[i]);
    if (!path)
      return JS_EXCEPTION;

    if (*path) {
      if (resolved[0]) {
        // Prepend path separator if needed
        char temp[4096];
        snprintf(temp, sizeof(temp), "%s%s%s", path,
                 IS_PATH_SEPARATOR(path[strlen(path) - 1]) ? "" : PATH_SEPARATOR_STR, resolved);
        strcpy(resolved, temp);
      } else {
        strcpy(resolved, path);
      }

      absolute = is_absolute_path(path);
    }

    JS_FreeCString(ctx, path);
  }

  // If still not absolute, prepend current working directory
  if (!absolute) {
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd))) {
      char temp[4096];
      snprintf(temp, sizeof(temp), "%s%s%s", cwd, PATH_SEPARATOR_STR, resolved);
      strcpy(resolved, temp);
    }
  }

  // Normalize the result (resolves . and .. segments)
  char* normalized = normalize_path(resolved);
  JSValue ret = JS_NewString(ctx, normalized);
  free(normalized);
  return ret;
}

// node:path.normalize implementation
static JSValue js_path_normalize(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "path.normalize requires a path argument");
  }

  //NODE_ARG_REQUIRE_STRING(ctx, argv[0], "path");

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  // Use the normalize_path function which handles . and .. segments
  char* normalized = normalize_path(path);

  JS_FreeCString(ctx, path);

  if (!normalized) {
    JS_ThrowOutOfMemory(ctx);
    return JS_EXCEPTION;
  }

  JSValue ret = JS_NewString(ctx, normalized);
  free(normalized);
  return ret;
}

// node:path.isAbsolute implementation
static JSValue js_path_is_absolute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_FALSE;
  }

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  bool is_abs = is_absolute_path(path);
  JS_FreeCString(ctx, path);

  return JS_NewBool(ctx, is_abs);
}

// node:path.dirname implementation
static JSValue js_path_dirname(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "path.dirname requires a path argument");
  }

  //NODE_ARG_REQUIRE_STRING(ctx, argv[0], "path");

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  size_t len = strlen(path);

  // Handle empty path
  if (len == 0) {
    JS_FreeCString(ctx, path);
    return JS_NewString(ctx, ".");
  }

  // Find last separator
  int last_sep = -1;
  for (int i = len - 1; i >= 0; i--) {
    if (IS_PATH_SEPARATOR(path[i])) {
      last_sep = i;
      break;
    }
  }

  JSValue result;
  if (last_sep == -1) {
    // No separator found
    result = JS_NewString(ctx, ".");
  } else if (last_sep == 0) {
    // Root directory
    result = JS_NewString(ctx, PATH_SEPARATOR_STR);
  } else {
    // Return everything before the last separator
    char* dir = malloc(last_sep + 1);
    memcpy(dir, path, last_sep);
    dir[last_sep] = '\0';
    result = JS_NewString(ctx, dir);
    free(dir);
  }

  JS_FreeCString(ctx, path);
  return result;
}

// node:path.basename implementation
static JSValue js_path_basename(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "path.basename requires a path argument");
  }

  //NODE_ARG_REQUIRE_STRING(ctx, argv[0], "path");

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  size_t len = strlen(path);

  // Skip trailing separators
  while (len > 0 && IS_PATH_SEPARATOR(path[len - 1])) {
    len--;
  }

  // Find last separator
  int start = 0;
  for (int i = len - 1; i >= 0; i--) {
    if (IS_PATH_SEPARATOR(path[i])) {
      start = i + 1;
      break;
    }
  }

  // Extract basename
  size_t basename_len = len - start;
  char* basename = malloc(basename_len + 1);
  memcpy(basename, path + start, basename_len);
  basename[basename_len] = '\0';

  // Handle optional extension parameter
  if (argc >= 2 && JS_IsString(argv[1])) {
    const char* ext = JS_ToCString(ctx, argv[1]);
    if (ext) {
      size_t ext_len = strlen(ext);
      if (basename_len >= ext_len && strcmp(basename + basename_len - ext_len, ext) == 0) {
        basename[basename_len - ext_len] = '\0';
      }
      JS_FreeCString(ctx, ext);
    }
  }

  JSValue result = JS_NewString(ctx, basename);
  free(basename);
  JS_FreeCString(ctx, path);
  return result;
}

// node:path.extname implementation
static JSValue js_path_extname(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "path.extname requires a path argument");
  }

  //NODE_ARG_REQUIRE_STRING(ctx, argv[0], "path");

  const char* path = JS_ToCString(ctx, argv[0]);
  if (!path)
    return JS_EXCEPTION;

  size_t len = strlen(path);

  // Find last dot after last separator
  int last_dot = -1;
  int last_sep = -1;

  for (int i = len - 1; i >= 0; i--) {
    if (path[i] == '.' && last_dot == -1) {
      last_dot = i;
    } else if (IS_PATH_SEPARATOR(path[i])) {
      last_sep = i;
      break;
    }
  }

  JSValue result;
  if (last_dot > last_sep && last_dot > 0) {
    result = JS_NewString(ctx, path + last_dot);
  } else {
    result = JS_NewString(ctx, "");
  }

  JS_FreeCString(ctx, path);
  return result;
}

// node:path.relative implementation
static JSValue js_path_relative(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "path.relative requires from and to arguments");
  }

  //NODE_ARG_REQUIRE_STRING(ctx, argv[0], "from");
  //NODE_ARG_REQUIRE_STRING(ctx, argv[1], "to");

  const char* from = JS_ToCString(ctx, argv[0]);
  const char* to = JS_ToCString(ctx, argv[1]);

  if (!from || !to) {
    if (from)
      JS_FreeCString(ctx, from);
    if (to)
      JS_FreeCString(ctx, to);
    return JS_EXCEPTION;
  }

  // Normalize paths to resolve . and .. segments
  char* from_normalized = normalize_path(from);
  char* to_normalized = normalize_path(to);

  JS_FreeCString(ctx, from);
  JS_FreeCString(ctx, to);

  if (!from_normalized || !to_normalized) {
    if (from_normalized)
      free(from_normalized);
    if (to_normalized)
      free(to_normalized);
    JS_ThrowOutOfMemory(ctx);
    return JS_EXCEPTION;
  }

  // Convert relative paths to absolute using current working directory
  char from_abs[4096], to_abs[4096];

  if (!is_absolute_path(from_normalized)) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      snprintf(from_abs, sizeof(from_abs), "%s%s%s", cwd, PATH_SEPARATOR_STR, from_normalized);
    } else {
      strcpy(from_abs, from_normalized);
    }
  } else {
    strcpy(from_abs, from_normalized);
  }

  if (!is_absolute_path(to_normalized)) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      snprintf(to_abs, sizeof(to_abs), "%s%s%s", cwd, PATH_SEPARATOR_STR, to_normalized);
    } else {
      strcpy(to_abs, to_normalized);
    }
  } else {
    strcpy(to_abs, to_normalized);
  }

  free(from_normalized);
  free(to_normalized);

  // Split paths into segments
  char from_copy[4096], to_copy[4096];
  strcpy(from_copy, from_abs);
  strcpy(to_copy, to_abs);

  char* from_segments[256];
  char* to_segments[256];
  int from_count = 0, to_count = 0;

  // Split from path
  char* token = strtok(from_copy, PATH_SEPARATOR_STR);
  while (token && from_count < 256) {
    if (*token) {  // Skip empty segments
      from_segments[from_count++] = strdup(token);
    }
    token = strtok(NULL, PATH_SEPARATOR_STR);
  }

  // Split to path
  token = strtok(to_copy, PATH_SEPARATOR_STR);
  while (token && to_count < 256) {
    if (*token) {  // Skip empty segments
      to_segments[to_count++] = strdup(token);
    }
    token = strtok(NULL, PATH_SEPARATOR_STR);
  }

  // Find common prefix
  int common = 0;
  while (common < from_count && common < to_count && strcmp(from_segments[common], to_segments[common]) == 0) {
    common++;
  }

  // Build relative path
  char result[4096] = "";

  // Add .. for each remaining segment in from path
  for (int i = common; i < from_count; i++) {
    if (strlen(result) > 0) {
      strcat(result, PATH_SEPARATOR_STR);
    }
    strcat(result, "..");
  }

  // Add remaining segments from to path
  for (int i = common; i < to_count; i++) {
    if (strlen(result) > 0) {
      strcat(result, PATH_SEPARATOR_STR);
    }
    strcat(result, to_segments[i]);
  }

  // Clean up allocated segments
  for (int i = 0; i < from_count; i++) {
    free(from_segments[i]);
  }
  for (int i = 0; i < to_count; i++) {
    free(to_segments[i]);
  }

  // Handle empty result
  if (result[0] == '\0') {
    strcpy(result, ".");
  }

  return JS_NewString(ctx, result);
}

void qjs_setup_path(JSContext *ctx, JSValueConst global) {
  JSValue path_obj = JS_NewObject(ctx);
  if (JS_IsException(path_obj)) {
    return;
  }

  // Add methods
  static const JSCFunctionListEntry js_path_funcs[] = {
    JS_CFUNC_DEF("join", 0, js_path_join),
    JS_CFUNC_DEF("resolve", 0, js_path_resolve),
    JS_CFUNC_DEF("normalize", 1, js_path_normalize),
    JS_CFUNC_DEF("isAbsolute", 1, js_path_is_absolute),
    JS_CFUNC_DEF("dirname", 1, js_path_dirname),
    JS_CFUNC_DEF("basename", 2, js_path_basename),
    JS_CFUNC_DEF("extname", 1, js_path_extname),
    JS_CFUNC_DEF("relative", 2, js_path_relative),
  };
  JS_SetPropertyFunctionList(ctx, path_obj, js_path_funcs, 
                           qjs_nitems(js_path_funcs));

  // Add properties
  JS_SetPropertyStr(ctx, path_obj, "sep", JS_NewString(ctx, PATH_SEPARATOR_STR));
  char delimiter[2] = {PATH_DELIMITER, '\0'};
  JS_SetPropertyStr(ctx, path_obj, "delimiter", JS_NewString(ctx, delimiter));

  JS_SetPropertyStr(ctx, global, "path", path_obj);
}

