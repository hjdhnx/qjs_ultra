import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

/// JSValue 的 16 字节内存视图（与 quickjs.h 的 `struct JSValue` 对齐）：
/// `union { uint64; double; ptr; } u; int64_t tag;`
final class QjsValueUnion extends Union {
  @Uint64()
  external int u64;

  @Double()
  external double d;
}

final class QjsValue extends Struct {
  external QjsValueUnion u;

  @Int64()
  external int tag;
}

/// quickjs.h 里的 tag 常量。
abstract class QjsTag {
  static const object = -1;
  static const functionBytecode = -2;
  static const module = -3;
  static const string = -7;
  static const stringRope = -6;
  static const symbol = -8;
  static const bigInt = -9;
  static const int_ = 0;
  static const bool_ = 1;
  static const null_ = 2;
  static const undefined = 3;
  static const exception = 6;
  static const float64 = 8;
}

/// JS_EVAL_* flags。
abstract class QjsEvalFlags {
  static const global = 0;
  static const module = 1;
  static const compileOnly = 1 << 5;
}

/// quickjs_bridge.c 的 FFI 绑定（本仓 so 的 Dart 侧）。
///
/// 动态库构成：QuickJS + 扩展模块（BoringSSL/lexbor/sqlite3/zlib/wasm3）静态
/// 链入，加 `bridge/quickjs_bridge.c` 的 C API。本机开发用环境变量
/// `QJS_ULTRA_LIB` 指定路径；分发时随应用携带。
///
/// **ABI 契约**：本文件的 NativeFunction 签名与 `abi/exports.txt` 的导出函数
/// 严格对应。改 C 侧签名 = 改 ABI —— 必须同步本文件与调用方，否则加载新 so
/// 会崩溃（2026-09-20 实机事故：新 so 期望 4 参、旧绑定传 3 参 → SIGSEGV）。
///
/// **平台前提**：`QjsValue` 按 `struct { union u; int64 tag; }`（16 字节）布局
/// 读取，这成立的前提是引擎编为 **64 位**（`JS_PTR64`，见 quickjs.h 的
/// `INTPTR_MAX >= INT64_MAX` 判定）。32 位下 JS_NAN_BOXING 会把 JSValue 压成
/// 单个 uint64，本文件的 `.u.u64` / `.tag` 读取即失效 —— 故本包只支持
/// 64 位目标（arm64-v8a / x86_64）。
class QjsBridge {
  QjsBridge._(this._lib) {
    Pointer<T> _lk<T extends NativeType>(String n) => _lib.lookup<T>(n);

    newRuntime =
        _lk<NativeFunction<Pointer<Void> Function()>>('qjs_new_runtime')
            .asFunction();
    freeRuntime =
        _lk<NativeFunction<Void Function(Pointer<Void>)>>('qjs_free_runtime')
            .asFunction();
    newContext = _lk<NativeFunction<Pointer<Void> Function(Pointer<Void>)>>(
            'qjs_new_context')
        .asFunction();
    freeContext =
        _lk<NativeFunction<Void Function(Pointer<Void>)>>('qjs_free_context')
            .asFunction();
    setMemoryLimit = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, UintPtr)>>('qjs_set_memory_limit')
        .asFunction();
    setMaxStackSize = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, UintPtr)>>('qjs_set_max_stack_size')
        .asFunction();
    runGc = _lk<NativeFunction<Void Function(Pointer<Void>)>>('qjs_run_gc')
        .asFunction();

    eval = _lk<
            NativeFunction<
                Int32 Function(
                    Pointer<Void>,
                    Pointer<Utf8>,
                    Int32,
                    Pointer<Utf8>,
                    Int32,
                    Pointer<QjsValue>)>>('qjs_eval')
        .asFunction();
    compile = _lk<
            NativeFunction<
                Int32 Function(
                    Pointer<Void>,
                    Pointer<Utf8>,
                    Int32,
                    Pointer<Utf8>,
                    Int32,
                    Pointer<Pointer<Uint8>>,
                    Pointer<Int32>)>>('qjs_compile')
        .asFunction();
    evalBytecode = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32,
                    Pointer<QjsValue>)>>('qjs_eval_bytecode')
        .asFunction();
    executePendingJob = _lk<
            NativeFunction<Int32 Function(Pointer<Void>)>>(
            'qjs_execute_pending_job')
        .asFunction();
    installModuleLoader =
        _lk<NativeFunction<Void Function(Pointer<Void>)>>(
                'qjs_install_module_loader')
            .asFunction();
    // ABI：首参为 ctx（per-context 注册，根治跨 isolate SIGABRT；
    // so 由 tools/qjs_native/build_so.py 重编，配套补丁见同目录）
    setCallbacks = _lk<
            NativeFunction<
                Void Function(Pointer<Void>,
                    Pointer<NativeFunction<Void Function()>>,
                    Pointer<NativeFunction<Void Function()>>,
                    Pointer<NativeFunction<Void Function()>>)>>(
            'qjs_set_callbacks')
        .asFunction();
    clearCallbacks =
        _lk<NativeFunction<Void Function(Pointer<Void>)>>(
                'qjs_clear_callbacks')
            .asFunction();

    freeValue = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_free_value')
        .asFunction();
    dupValue = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_dup_value')
        .asFunction();
    valueMove = _lk<
            NativeFunction<
                Void Function(
                    Pointer<QjsValue>, Pointer<QjsValue>)>>('qjs_value_move')
        .asFunction();
    makeUndefined =
        _lk<NativeFunction<Void Function(Pointer<QjsValue>)>>(
                'qjs_make_undefined')
            .asFunction();
    makeNull = _lk<NativeFunction<Void Function(Pointer<QjsValue>)>>(
            'qjs_make_null')
        .asFunction();
    makeBool = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, Pointer<QjsValue>, Int32)>>(
            'qjs_make_bool')
        .asFunction();
    makeInt32 = _lk<
            NativeFunction<
                Void Function(Pointer<Void>, Pointer<QjsValue>, Int32)>>(
            'qjs_make_int32')
        .asFunction();
    makeFloat64 = _lk<
            NativeFunction<
                Void Function(
                    Pointer<Void>, Pointer<QjsValue>, Double)>>('qjs_make_float64')
        .asFunction();
    getTag = _lk<NativeFunction<Int32 Function(Pointer<QjsValue>)>>(
            'qjs_get_tag')
        .asFunction();
    getInt32 = _lk<
            NativeFunction<
                Int32 Function(Pointer<QjsValue>, Pointer<Int32>)>>(
            'qjs_get_int32')
        .asFunction();
    getFloat64 = _lk<
            NativeFunction<
                Int32 Function(Pointer<QjsValue>, Pointer<Double>)>>(
            'qjs_get_float64')
        .asFunction();
    toCString = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>,
                    Pointer<Pointer<Utf8>>, Pointer<Int32>)>>('qjs_to_cstring')
        .asFunction();
    newString = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<Utf8>, Int32,
                    Pointer<QjsValue>)>>('qjs_new_string')
        .asFunction();
    newObject = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_new_object')
        .asFunction();
    newArray = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_new_array')
        .asFunction();
    stringify = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>,
                    Pointer<Pointer<Utf8>>, Pointer<Int32>)>>('qjs_stringify')
        .asFunction();
    parseJson = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<Utf8>, Int32,
                    Pointer<QjsValue>)>>('qjs_parse_json')
        .asFunction();
    getGlobal = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_get_global')
        .asFunction();
    getProp = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>, Pointer<Utf8>,
                    Pointer<QjsValue>)>>('qjs_get_prop')
        .asFunction();
    setProp = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>, Pointer<Utf8>,
                    Pointer<QjsValue>)>>('qjs_set_prop')
        .asFunction();
    getPropU32 = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>, Uint32,
                    Pointer<QjsValue>)>>('qjs_get_prop_u32')
        .asFunction();
    arrayLength = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>,
                    Pointer<Uint32>)>>('qjs_array_length')
        .asFunction();
    setPropU32 = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>, Uint32,
                    Pointer<QjsValue>)>>('qjs_set_prop_u32')
        .asFunction();
    ownPropertyNames = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>,
                    Pointer<Pointer<Pointer<Utf8>>>,
                    Pointer<Uint32>)>>('qjs_own_property_names')
        .asFunction();
    freeStringArray = _lk<
            NativeFunction<
                Void Function(Pointer<Pointer<Utf8>>, Uint32)>>(
            'qjs_free_string_array')
        .asFunction();
    call = _lk<
            NativeFunction<
                Int32 Function(
                    Pointer<Void>,
                    Pointer<QjsValue>,
                    Pointer<QjsValue>,
                    Int32,
                    Pointer<QjsValue>,
                    Pointer<QjsValue>)>>('qjs_call')
        .asFunction();
    newHostFunction = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Int32, Pointer<QjsValue>)>>(
            'qjs_new_host_function')
        .asFunction();
    registerFunction = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<Utf8>, Int32)>>(
            'qjs_register_function')
        .asFunction();
    throwNativeError =
        _lk<NativeFunction<Int32 Function(Pointer<Void>, Pointer<Utf8>)>>(
                'qjs_throw_error')
            .asFunction();
    isException = _lk<NativeFunction<Int32 Function(Pointer<QjsValue>)>>(
            'qjs_is_exception')
        .asFunction();
    getException = _lk<
            NativeFunction<
                Int32 Function(Pointer<Void>, Pointer<QjsValue>)>>(
            'qjs_get_exception')
        .asFunction();
    freeBuffer = _lk<NativeFunction<Void Function(Pointer<Void>)>>(
            'qjs_free_buffer')
        .asFunction();
    isFunction = _lk<NativeFunction<Int32 Function(Pointer<Void>,
            Pointer<QjsValue>)>>('qjs_is_function')
        .asFunction();
    memoryUsage = _lk<NativeFunction<Int32 Function(Pointer<Void>,
            Pointer<Int64>, Pointer<Int64>)>>('qjs_memory_usage')
        .asFunction();
    isArray = _lk<NativeFunction<Int32 Function(Pointer<Void>,
            Pointer<QjsValue>)>>('qjs_is_array')
        .asFunction();
    getBytes = _lk<NativeFunction<Int32 Function(Pointer<Void>,
            Pointer<QjsValue>, Pointer<Pointer<Uint8>>,
            Pointer<Int32>)>>('qjs_get_bytes')
        .asFunction();
    newUint8Array = _lk<NativeFunction<Int32 Function(Pointer<Void>,
            Pointer<Uint8>, Int32, Pointer<QjsValue>)>>('qjs_bridge_new_uint8_array')
        .asFunction();
  }

  /// 释放 Dart 侧的 JSValue 槽内存。
  void freeSlot(Pointer<QjsValue> p) => malloc.free(p);

  final DynamicLibrary _lib;

  late final Pointer<Void> Function() newRuntime;
  late final void Function(Pointer<Void>) freeRuntime;
  late final Pointer<Void> Function(Pointer<Void>) newContext;
  late final void Function(Pointer<Void>) freeContext;
  late final void Function(Pointer<Void>, int) setMemoryLimit;
  late final void Function(Pointer<Void>, int) setMaxStackSize;
  late final void Function(Pointer<Void>) runGc;

  late final int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Utf8>,
      int, Pointer<QjsValue>) eval;
  late final int Function(Pointer<Void>, Pointer<Utf8>, int, Pointer<Utf8>,
      int, Pointer<Pointer<Uint8>>, Pointer<Int32>) compile;
  late final int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<QjsValue>)
      evalBytecode;
  late final int Function(Pointer<Void>) executePendingJob;
  late final void Function(Pointer<Void>) installModuleLoader;
  late final void Function(
          Pointer<Void>,
          Pointer<NativeFunction<Void Function()>>,
          Pointer<NativeFunction<Void Function()>>,
          Pointer<NativeFunction<Void Function()>>)
      setCallbacks;
  late final void Function(Pointer<Void>) clearCallbacks;

  late final void Function(Pointer<Void>, Pointer<QjsValue>) freeValue;
  late final void Function(Pointer<Void>, Pointer<QjsValue>) dupValue;
  late final void Function(Pointer<QjsValue>, Pointer<QjsValue>) valueMove;
  late final void Function(Pointer<QjsValue>) makeUndefined;
  late final void Function(Pointer<QjsValue>) makeNull;
  late final void Function(Pointer<Void>, Pointer<QjsValue>, int) makeBool;
  late final void Function(Pointer<Void>, Pointer<QjsValue>, int) makeInt32;
  late final void Function(Pointer<Void>, Pointer<QjsValue>, double)
      makeFloat64;
  late final int Function(Pointer<QjsValue>) getTag;
  late final int Function(Pointer<QjsValue>, Pointer<Int32>) getInt32;
  late final int Function(Pointer<QjsValue>, Pointer<Double>) getFloat64;
  late final int Function(Pointer<Void>, Pointer<QjsValue>,
      Pointer<Pointer<Utf8>>, Pointer<Int32>) toCString;
  late final int Function(
      Pointer<Void>, Pointer<Utf8>, int, Pointer<QjsValue>) newString;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) newObject;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) newArray;
  late final int Function(Pointer<Void>, Pointer<QjsValue>,
      Pointer<Pointer<Utf8>>, Pointer<Int32>) stringify;
  late final int Function(
      Pointer<Void>, Pointer<Utf8>, int, Pointer<QjsValue>) parseJson;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) getGlobal;
  late final int Function(Pointer<Void>, Pointer<QjsValue>, Pointer<Utf8>,
      Pointer<QjsValue>) getProp;
  late final int Function(Pointer<Void>, Pointer<QjsValue>, Pointer<Utf8>,
      Pointer<QjsValue>) setProp;
  late final int Function(
      Pointer<Void>, Pointer<QjsValue>, int, Pointer<QjsValue>) getPropU32;
  late final int Function(
      Pointer<Void>, Pointer<QjsValue>, Pointer<Uint32>) arrayLength;
  late final int Function(
      Pointer<Void>, Pointer<QjsValue>, int, Pointer<QjsValue>) setPropU32;
  late final int Function(Pointer<Void>, Pointer<QjsValue>,
      Pointer<Pointer<Pointer<Utf8>>>, Pointer<Uint32>) ownPropertyNames;
  late final void Function(Pointer<Pointer<Utf8>>, int) freeStringArray;
  late final int Function(Pointer<Void>, Pointer<QjsValue>, Pointer<QjsValue>,
      int, Pointer<QjsValue>, Pointer<QjsValue>) call;
  late final int Function(Pointer<Void>, int, Pointer<QjsValue>)
      newHostFunction;
  late final int Function(Pointer<Void>, Pointer<Utf8>, int) registerFunction;
  late final int Function(Pointer<Void>, Pointer<Utf8>) throwNativeError;
  late final int Function(Pointer<QjsValue>) isException;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) getException;
  late final void Function(Pointer<Void>) freeBuffer;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) isFunction;
  late final int Function(Pointer<Void>, Pointer<Int64>, Pointer<Int64>)
      memoryUsage;
  late final int Function(Pointer<Void>, Pointer<QjsValue>) isArray;
  late final int Function(Pointer<Void>, Pointer<QjsValue>,
      Pointer<Pointer<Uint8>>, Pointer<Int32>) getBytes;
  late final int Function(
      Pointer<Void>, Pointer<Uint8>, int, Pointer<QjsValue>) newUint8Array;

  static QjsBridge? _instance;

  /// 打开 quickjs 动态库。
  ///
  /// [path] 支持 `path/to/lib.dll|so|dylib`；仅传目录时按平台默认名拼。
  static QjsBridge open({String? path}) {
    return _instance ??= QjsBridge._(_open(path));
  }

  static DynamicLibrary _open(String? path) {
    if (path != null) {
      return DynamicLibrary.open(path);
    }
    final env = Platform.environment['QJS_ULTRA_LIB'];
    if (env != null && env.isNotEmpty) {
      return DynamicLibrary.open(env);
    }
    if (Platform.isWindows) {
      return DynamicLibrary.open('quickjs_bridge.dll');
    }
    if (Platform.isMacOS) {
      return DynamicLibrary.open('libquickjs_bridge.dylib');
    }
    return DynamicLibrary.open('libquickjs_bridge.so');
  }
}

/// qjs_compile / 模块加载产出的 buffer 拷回 Dart。
Uint8List copyNativeBuffer(Pointer<Uint8> ptr, int len) =>
    Uint8List.fromList(ptr.asTypedList(len));
