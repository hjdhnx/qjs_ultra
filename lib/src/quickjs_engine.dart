import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'js_engine.dart';
import 'qjs_bindings.dart';

/// 由 Dart 实现的宿主函数句柄（尚未绑定到 JS 值）。
///
/// `registeredFunctionFor` 返回它；在编组进 JS 对象时由
/// [_dartToJs] 转成真正的 JS 函数值。
class _HostFunctionHandle {
  _HostFunctionHandle(this.id);
  final int id;
}

/// 一个已 dup 保活的 JS 函数值引用。
///
/// 从 JS 传到 Dart 的函数（如 `options.complete` 回调）都包装成它；
/// 由引擎统一持有生命周期，引擎 dispose 时一并释放。
class JsFunctionRef {
  JsFunctionRef._(this._engine, this._slot);
  final QuickjsEngine _engine;
  final Pointer<QjsValue> _slot;
  bool _closed = false;

  Pointer<QjsValue> get slot => _slot;

  void close() {
    if (_closed) return;
    _closed = true;
    _engine._bridge
      ..freeValue(_engine._ctxPointer, _slot)
      ..freeSlot(_slot);
  }
}

/// 自建 dart:ffi 的 QuickJS 引擎实现（quickjs-ng 2026-06-04）。
///
/// 原生侧由 `native/quickjs_bridge.c` 提供稳定 ABI：
/// 所有 JSValue 走堆指针，Dart 回调全部 void 签名，
/// 宿主函数异常转成 JS Error（JS 侧可 try/catch）。
class QuickjsEngine implements JsEngine {
  QuickjsEngine._(this._bridge, JsEngineConfig config) {
    _rt = _bridge.newRuntime();
    if (_rt == nullptr) throw StateError('JS_NewRuntime 失败');
    if (config.memoryLimit > 0) {
      _bridge.setMemoryLimit(_rt, config.memoryLimit);
    }
    if (config.stackSize > 0) {
      _bridge.setMaxStackSize(_rt, config.stackSize);
    }

    _hostCallCb = NativeCallable<
        Void Function(Int32, Pointer<Void>, Int32, Pointer<QjsValue>,
            Pointer<QjsValue>)>.isolateLocal(_hostCallImpl);
    _moduleLoadCb = NativeCallable<
        Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Pointer<Uint8>>,
            Pointer<Int32>)>.isolateLocal(_moduleLoadImpl, exceptionalReturn: 0);
    _normalizeCb = NativeCallable<
        Int32 Function(Pointer<Utf8>, Pointer<Utf8>,
            Pointer<Pointer<Utf8>>)>.isolateLocal(_normalizeImpl,
        exceptionalReturn: 0);
    // 先建 ctx 再注册回调（per-context 注册，2026-09-20 ABI：根治
    // 「新引擎覆盖旧引擎回调 → 旧 native 线程回调命中新 isolate 闭包 →
    // SIGABRT Cannot invoke native callback from a different isolate」）
    _ctxPointer = _bridge.newContext(_rt);
    if (_ctxPointer == nullptr) throw StateError('JS_NewContext 失败');
    _bridge.setCallbacks(
      _ctxPointer,
      _hostCallCb!.nativeFunction.cast(),
      _moduleLoadCb!.nativeFunction.cast(),
      _normalizeCb!.nativeFunction.cast(),
    );

    _bridge.installModuleLoader(_rt);
  }

  final QjsBridge _bridge;
  late final Pointer<Void> _rt;
  late final Pointer<Void> _ctxPointer;

  NativeCallable<
          Void Function(Int32, Pointer<Void>, Int32, Pointer<QjsValue>,
              Pointer<QjsValue>)>?
      _hostCallCb;
  NativeCallable<
          Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Pointer<Uint8>>,
              Pointer<Int32>)>?
      _moduleLoadCb;
  NativeCallable<
          Int32 Function(
              Pointer<Utf8>, Pointer<Utf8>, Pointer<Pointer<Utf8>>)>?
      _normalizeCb;

  final _hostFunctions = <int, JsHostFunction>{};
  var _nextHostId = 1;

  /// 打开的 JS 函数引用，dispose 时统一释放。
  final _openFunctionRefs = <JsFunctionRef>{};

  JsModuleLoader? _moduleLoader;
  void Function(String)? _consoleSink;
  var _disposed = false;

  @override
  String get implementationName => 'quickjs-ffi';

  @override
  bool get isDisposed => _disposed;

  /// 工厂入口。注册到 [JsEngine.registerFactory] 使用。
  static JsEngineFactory factory({String? libPath}) =>
      _QuickjsFactory(libPath);

  static QuickjsEngine createWith(JsEngineConfig config, {String? libPath}) =>
      QuickjsEngine._(QjsBridge.open(path: libPath), config);

  // ---------- JsEngine ----------

  @override
  Object? evaluate(String script, {String? fileName}) {
    _checkDisposed();
    return _eval(script, fileName ?? 'script.js', QjsEvalFlags.global);
  }

  @override
  Object? evaluateModule(String script, {String? fileName}) {
    _checkDisposed();
    return _eval(script, fileName ?? 'module.js', QjsEvalFlags.module);
  }

  /// 计算 UTF-8 编码后的字节长度（C 桥按字节读取字符串）。
  static int _utf8Len(String s) => utf8.encode(s).length;

  Object? _eval(String script, String fileName, int flags) {
    final scriptPtr = script.toNativeUtf8();
    final namePtr = fileName.toNativeUtf8();
    final out = _newSlot();
    try {
      final rc = _bridge.eval(
        _ctxPointer,
        scriptPtr,
        _utf8Len(script),
        namePtr,
        flags,
        out,
      );
      if (rc != 0) {
        final message = _takeExceptionMessage();
        throw JsEvalException(message);
      }
      final result = _jsToDart(out);
      _bridge.freeValue(_ctxPointer, out);
      _bridge.freeSlot(out);
      _drainJobs();
      return result;
    } finally {
      malloc
        ..free(scriptPtr)
        ..free(namePtr);
    }
  }

  @override
  Uint8List compileModule(String source, {String? fileName}) {
    _checkDisposed();
    return _compile(source, fileName ?? 'module.js', true);
  }

  Uint8List _compile(String source, String fileName, bool isModule) {
    final srcPtr = source.toNativeUtf8();
    final namePtr = fileName.toNativeUtf8();
    final bufPtr = malloc<Pointer<Uint8>>();
    final lenPtr = malloc<Int32>();
    try {
      final rc = _bridge.compile(
        _ctxPointer,
        srcPtr,
        _utf8Len(source),
        namePtr,
        isModule ? 1 : 0,
        bufPtr,
        lenPtr,
      );
      if (rc != 0) {
        throw JsEvalException(_takeExceptionMessage());
      }
      final bytes = copyNativeBuffer(bufPtr.value, lenPtr.value);
      _bridge.freeBuffer(bufPtr.value.cast());
      return bytes;
    } finally {
      malloc
        ..free(srcPtr)
        ..free(namePtr)
        ..free(bufPtr)
        ..free(lenPtr);
    }
  }

  @override
  Object? executeBytecode(Uint8List bytecode, {String? fileName}) {
    _checkDisposed();
    final buf = malloc<Uint8>(bytecode.length);
    buf.asTypedList(bytecode.length).setAll(0, bytecode);
    final out = _newSlot();
    try {
      final rc = _bridge.evalBytecode(_ctxPointer, buf, bytecode.length, out);
      if (rc != 0) {
        throw JsEvalException(_takeExceptionMessage());
      }
      final result = _jsToDart(out);
      _bridge.freeValue(_ctxPointer, out);
      _bridge.freeSlot(out);
      _drainJobs();
      return result;
    } finally {
      malloc.free(buf);
    }
  }

  @override
  void setModuleLoader(JsModuleLoader? loader) => _moduleLoader = loader;

  @override
  void registerFunction(String name, JsHostFunction fn) {
    _checkDisposed();
    final id = _nextHostId++;
    _hostFunctions[id] = fn;
    final namePtr = name.toNativeUtf8();
    try {
      final rc = _bridge.registerFunction(_ctxPointer, namePtr, id);
      if (rc != 0) {
        _hostFunctions.remove(id);
        throw StateError('注册宿主函数 $name 失败');
      }
    } finally {
      malloc.free(namePtr);
    }
  }

  @override
  Object? registeredFunctionFor(JsHostFunction fn) {
    _checkDisposed();
    final handle = _HostFunctionHandle(_nextHostId++);
    _hostFunctions[handle.id] = fn;
    return handle;
  }

  @override
  void setConsoleSink(void Function(String message)? sink) {
    _consoleSink = sink;
    if (sink == null || _consoleInstalled) return;
    _consoleInstalled = true;
    // 对齐 Java 版 QuickJSContext.setConsole()：引擎内置 console 对象，
    // 无需全量注册各方法，只在其上注册一个 stdout(level, message) 回调，
    // 由内置 console 把各方法（log/info/warn/error/debug...）格式化后转发到 Dart 层。
    final stdoutFn = registeredFunctionFor(
      (args) {
        if (args.length >= 2) {
          final level = args[0]?.toString() ?? 'log';
          final message = args[1]?.toString() ?? '';
          switch (level) {
            case 'info':
              _consoleSink?.call('[info] $message');
            case 'warn':
              _consoleSink?.call('[warn] $message');
            case 'error':
              _consoleSink?.call('[error] $message');
            default: // log / debug 及未知等级一律走 log
              _consoleSink?.call('[log] $message');
          }
        }
        return null;
      },
    );
    final v = _newSlot();
    try {
      _dartToJs(stdoutFn, v);
      final global = _newSlot();
      final consoleObj = _newSlot();
      try {
        _bridge.getGlobal(_ctxPointer, global);
        final namePtr = 'console'.toNativeUtf8();
        final stdoutPtr = 'stdout'.toNativeUtf8();
        try {
          _bridge.getProp(_ctxPointer, global, namePtr, consoleObj);
          _bridge.setProp(_ctxPointer, consoleObj, stdoutPtr, v);
        } finally {
          malloc
            ..free(namePtr)
            ..free(stdoutPtr);
        }
        _bridge.freeValue(_ctxPointer, global);
        _bridge.freeValue(_ctxPointer, consoleObj);
      } finally {
        _bridge
          ..freeSlot(global)
          ..freeSlot(consoleObj);
      }
    } finally {
      _bridge.freeSlot(v);
    }
  }

  bool _consoleInstalled = false;

  @override
  Object? getGlobalProperty(String name) {
    _checkDisposed();
    final global = _newSlot();
    final value = _newSlot();
    try {
      _bridge.getGlobal(_ctxPointer, global);
      final namePtr = name.toNativeUtf8();
      try {
        _bridge.getProp(_ctxPointer, global, namePtr, value);
      } finally {
        malloc.free(namePtr);
      }
      final result = _jsToDart(value);
      _bridge.freeValue(_ctxPointer, value);
      _bridge.freeValue(_ctxPointer, global);
      return result;
    } finally {
      _bridge.freeSlot(value);
      _bridge.freeSlot(global);
    }
  }

  @override
  void setGlobalProperty(String name, Object? value) {
    _checkDisposed();
    final v = _newSlot();
    try {
      _dartToJs(value, v);
      final global = _newSlot();
      _bridge.getGlobal(_ctxPointer, global);
      final namePtr = name.toNativeUtf8();
      try {
        _bridge.setProp(_ctxPointer, global, namePtr, v);
      } finally {
        malloc.free(namePtr);
      }
      _bridge.freeValue(_ctxPointer, global);
      _bridge.freeSlot(global);
    } finally {
      _bridge.freeSlot(v);
    }
  }

  @override
  Object? callFunction(Object? fn, List<Object?> args) {
    _checkDisposed();
    switch (fn) {
      case JsFunctionRef ref:
        return _callSlot(ref.slot, args);
      case String name:
        final target = getGlobalProperty(name);
        if (target is JsFunctionRef) {
          try {
            return _callSlot(target.slot, args);
          } finally {
            target.close();
            _openFunctionRefs.remove(target);
          }
        }
        throw StateError('全局属性 $name 不是函数');
      default:
        throw StateError('callFunction 只接受 JsFunctionRef 或全局函数名');
    }
  }

  Object? _callSlot(Pointer<QjsValue> funcSlot, List<Object?> args) {
    final argv = calloc<QjsValue>(args.isEmpty ? 1 : args.length);
    try {
      for (var i = 0; i < args.length; i++) {
        _dartToJs(args[i], argv + i);
      }
      final out = _newSlot();
      try {
        final rc = _bridge.call(
          _ctxPointer,
          funcSlot,
          nullptr,
          args.length,
          argv,
          out,
        );
        if (rc != 0) {
          throw JsEvalException(_takeExceptionMessage());
        }
        final result = _jsToDart(out);
        _bridge.freeValue(_ctxPointer, out);
        _drainJobs();
        return result;
      } finally {
        _bridge.freeSlot(out);
      }
    } finally {
      for (var i = 0; i < args.length; i++) {
        _bridge.freeValue(_ctxPointer, argv + i);
      }
      malloc.free(argv);
    }
  }

  @override
  Object? parseJson(String json) {
    _checkDisposed();
    final ptr = json.toNativeUtf8();
    final out = _newSlot();
    try {
      final rc = _bridge.parseJson(_ctxPointer, ptr, _utf8Len(json), out);
      if (rc != 0) {
        throw JsEvalException(_takeExceptionMessage());
      }
      final result = _jsToDart(out);
      _bridge.freeValue(_ctxPointer, out);
      return result;
    } finally {
      malloc.free(ptr);
      _bridge.freeSlot(out);
    }
  }

  @override
  String stringify(Object? value) {
    _checkDisposed();
    final slot = _newSlot();
    try {
      _dartToJs(value, slot);
      final outPtr = malloc<Pointer<Utf8>>();
      final lenPtr = malloc<Int32>();
      try {
        final rc = _bridge.stringify(_ctxPointer, slot, outPtr, lenPtr);
        if (rc != 0) {
          throw JsEvalException(_takeExceptionMessage());
        }
        final s = outPtr.value.toDartString();
        _bridge.freeBuffer(outPtr.value.cast());
        return s;
      } finally {
        malloc
          ..free(outPtr)
          ..free(lenPtr);
        _bridge.freeValue(_ctxPointer, slot);
      }
    } finally {
      _bridge.freeSlot(slot);
    }
  }

  @override
  bool executePendingJobs() {
    _checkDisposed();
    for (;;) {
      final rc = _bridge.executePendingJob(_rt);
      if (rc == 0) return true; // 队列已空
      if (rc < 0) return true; // job 内异常：清空待处理状态后视为结束
    }
  }

  /// 排空微任务队列。job 内抛出的 JS 异常不能吞掉，
  /// 否则模块顶层求值失败会表现为"静默不执行"。
  void _drainJobs() {
    for (;;) {
      final rc = _bridge.executePendingJob(_rt);
      if (rc == 0) return;
      if (rc < 0) throw JsEvalException(_takeExceptionMessage());
    }
  }

  @override
  void runGC() {
    _checkDisposed();
    _bridge.runGc(_rt);
  }

  @override
  JsMemoryUsage? getMemoryUsage() {
    _checkDisposed();
    final mallocSize = malloc<Int64>();
    final usedSize = malloc<Int64>();
    try {
      _bridge.memoryUsage(_rt, mallocSize, usedSize);
      return JsMemoryUsage(
        mallocSize: mallocSize.value,
        memoryUsedSize: usedSize.value,
      );
    } finally {
      malloc
        ..free(mallocSize)
        ..free(usedSize);
    }
  }

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    for (final ref in List<JsFunctionRef>.of(_openFunctionRefs)) {
      ref.close();
    }
    _openFunctionRefs.clear();
    _hostFunctions.clear();
    _bridge
      ..clearCallbacks(_ctxPointer) // per-ctx 回调表释放（必须先于 free ctx）
      ..freeContext(_ctxPointer)
      ..freeRuntime(_rt);
    // NativeCallable.close() 必须在引擎线程执行（isolateLocal 约束）——
    // dispose 由 worker isolate 内的 destroy 消息驱动，天然满足
    _hostCallCb?.close();
    _moduleLoadCb?.close();
    _normalizeCb?.close();
    _hostCallCb = null;
    _moduleLoadCb = null;
    _normalizeCb = null;
  }

  // ---------- Dart → JS 回调（bridge.c 触发） ----------

  void _hostCallImpl(
    int id,
    Pointer<Void> ctx,
    int argc,
    Pointer<QjsValue> argv,
    Pointer<QjsValue> ret,
  ) {
    // native callback 内禁止任何 Dart 异常穿透：Dart 异常沿 FFI 栈
    // 传播时，quickjs 不感知并继续执行 JS，再次进入任一 native
    // callback 即 SIGABRT「Cannot invoke native callback while unwind
    // error propagates」（真机多源切换闪退实锤）——最外层兜底封死
    try {
      _hostCallInner(id, argc, argv, ret);
    } catch (e) {
      ret.ref
        ..u.u64 = 0
        ..tag = QjsTag.exception;
    }
  }

  void _hostCallInner(
    int id,
    int argc,
    Pointer<QjsValue> argv,
    Pointer<QjsValue> ret,
  ) {
    final fn = _hostFunctions[id];
    if (fn == null) return;
    final args = <Object?>[
      for (var i = 0; i < argc; i++) _jsToDart(argv + i),
    ];
    try {
      final result = fn(args);
      _dartToJs(result, ret);
    } catch (e) {
      // Dart 异常转 JS Error，让 JS 侧可以 try/catch（conformance 覆盖）
      final msgPtr = e.toString().toNativeUtf8();
      try {
        _bridge.throwNativeError(_ctxPointer, msgPtr);
      } finally {
        malloc.free(msgPtr);
      }
      ret.ref
        ..u.u64 = 0
        ..tag = QjsTag.exception;
    }
  }

  int _moduleLoadImpl(
    Pointer<Void> ctx,
    Pointer<Utf8> namePtr,
    Pointer<Pointer<Uint8>> outBuf,
    Pointer<Int32> outLen,
  ) {
    // 异常禁止穿透（同 _hostCallImpl 注释）：返回 0 让 quickjs 走
    // 「模块未找到」的常规 JS 异常路径
    try {
      return _moduleLoadInner(namePtr, outBuf, outLen);
    } catch (e) {
      return 0;
    }
  }

  int _moduleLoadInner(
    Pointer<Utf8> namePtr,
    Pointer<Pointer<Uint8>> outBuf,
    Pointer<Int32> outLen,
  ) {
    final loader = _moduleLoader;
    if (loader == null) return 0;
    final name = namePtr.toDartString();
    // 优先 bytecode；否则回退源码文本，交由 C 桥 loader 直接
    // JS_Eval(COMPILE_ONLY)（对齐 Java 版 ModuleLoader 行为，
    // 避免多余的 WriteObject/ReadObject 往返与 loader 内 ResolveModule）。
    final bytes = loader.getModuleBytecode(name);
    if (bytes != null) {
      final buf = malloc<Uint8>(bytes.length);
      buf.asTypedList(bytes.length).setAll(0, bytes);
      outBuf.value = buf;
      outLen.value = bytes.length;
      return 1; // bytecode
    }
    final source = loader.getModuleSource(name);
    if (source == null) return 0;
    final srcBytes = utf8.encode(source);
    final buf = malloc<Uint8>(srcBytes.length);
    buf.asTypedList(srcBytes.length).setAll(0, srcBytes);
    outBuf.value = buf;
    outLen.value = srcBytes.length;
    return 2; // 源码文本
  }

  int _normalizeImpl(
    Pointer<Utf8> basePtr,
    Pointer<Utf8> namePtr,
    Pointer<Pointer<Utf8>> out,
  ) {
    // 异常禁止穿透（同 _hostCallImpl 注释）
    try {
      return _normalizeInner(basePtr, namePtr, out);
    } catch (e) {
      return 0;
    }
  }

  int _normalizeInner(
    Pointer<Utf8> basePtr,
    Pointer<Utf8> namePtr,
    Pointer<Pointer<Utf8>> out,
  ) {
    final loader = _moduleLoader;
    if (loader == null) return 0;
    final normalized = loader.normalizeName(
      basePtr.toDartString(),
      namePtr.toDartString(),
    );
    final ptr = normalized.toNativeUtf8();
    out.value = ptr.cast();
    return 1;
  }

  // ---------- 值编组 ----------

  Pointer<QjsValue> _newSlot() => malloc<QjsValue>();

  Object? _jsToDart(Pointer<QjsValue> slot) {
    switch (_bridge.getTag(slot)) {
      case QjsTag.int_:
        // 与 C 侧 `JS_VALUE_GET_INT(v) = (int)(v).u.uint64` 对齐：u64 是无符号
        // 容器，负数是二补码，**必须做 int32 有符号转换**。直接返回 u64 会把
        // -42 读成 4294967254（2026-09-21 CI 真实引擎测试抓到）。
        return slot.ref.u.u64.toSigned(32);
      case QjsTag.bool_:
        return slot.ref.u.u64 != 0;
      case QjsTag.float64:
        return slot.ref.u.d;
      case QjsTag.string:
      case QjsTag.stringRope:
        // STRING_ROPE（长字符串拼接的 rope 结构）同样用 JS_ToCStringLen
        // 展开，与普通 string 走同一条读取路径。
        return _readCString(slot);
      case QjsTag.symbol:
      case QjsTag.bigInt:
        // core 契约：Symbol 等未列值转 String，不得静默丢弃。
        // JS_ToCStringLen 对 symbol 产出 description、bigInt 产出十进制文本。
        final s = _readCString(slot);
        return s ?? slot.ref.u.u64.toString();
      case QjsTag.null_:
      case QjsTag.undefined:
        return null;
      case QjsTag.object:
        return _objectToDart(slot);
      default:
        return null;
    }
  }

  String? _readCString(Pointer<QjsValue> slot) {
    final outPtr = malloc<Pointer<Utf8>>();
    final lenPtr = malloc<Int32>();
    try {
      final rc = _bridge.toCString(_ctxPointer, slot, outPtr, lenPtr);
      if (rc != 0) return null;
      final s = outPtr.value.toDartString();
      _bridge.freeBuffer(outPtr.value.cast());
      return s;
    } finally {
      malloc
        ..free(outPtr)
        ..free(lenPtr);
    }
  }

  Object? _objectToDart(Pointer<QjsValue> slot) {
    // TypedArray / ArrayBuffer？直接回读为 Uint8List。
    // 必须放在数组判定之前（TypedArray 也是 object tag）。
    final bytes = _tryReadBytes(slot);
    if (bytes != null) return bytes;

    // 数组？用引擎内置 JS_IsArray 精确判定（替代 length 属性猜测：
    // 旧方案对 {length: 3} 这类普通对象会误判成数组）。
    if (_bridge.isArray(_ctxPointer, slot) != 0) {
      final lenSlot = _newSlot();
      try {
        final namePtr = 'length'.toNativeUtf8();
        try {
          _bridge.getProp(_ctxPointer, slot, namePtr, lenSlot);
        } finally {
          malloc.free(namePtr);
        }
        final n = lenSlot.ref.u.u64;
        final list = <Object?>[];
        for (var i = 0; i < n; i++) {
          final item = _newSlot();
          try {
            _bridge.getPropU32(_ctxPointer, slot, i, item);
            list.add(_jsToDart(item));
          } finally {
            _bridge.freeValue(_ctxPointer, item);
            _bridge.freeSlot(item);
          }
        }
        return list;
      } finally {
        _bridge.freeValue(_ctxPointer, lenSlot);
        _bridge.freeSlot(lenSlot);
      }
    }

    // 函数？
    if (_bridge.isFunction(_ctxPointer, slot) != 0) {
      final refSlot = _newSlot();
      _bridge.dupValue(_ctxPointer, slot);
      _bridge.valueMove(refSlot, slot);
      final ref = JsFunctionRef._(this, refSlot);
      _openFunctionRefs.add(ref);
      return ref;
    }

    // 普通对象 → Map。
    // 属性名枚举用 bridge 的 qjs_own_property_names（JS_GetOwnPropertyNames，
    // 仅枚举可枚举 string key），替代早期「临时挂全局 + evaluate」的 hack。
    final names = _ownPropertyNames(slot);
    if (names == null) return null;

    final map = <String, Object?>{};
    for (final name in names) {
      final value = _newSlot();
      try {
        final namePtr = name.toNativeUtf8();
        try {
          _bridge.getProp(_ctxPointer, slot, namePtr, value);
        } finally {
          malloc.free(namePtr);
        }
        map[name] = _jsToDart(value);
      } finally {
        _bridge.freeValue(_ctxPointer, value);
        _bridge.freeSlot(value);
      }
    }
    return map;
  }

  /// 尝试把 JS 值读成 Uint8List（Uint8Array / ArrayBuffer）。
  /// 非二进制值返回 null。成功时字节是 C 侧 malloc 的副本，
  /// 拷贝进 Dart 后 C 缓冲立即释放。
  Uint8List? _tryReadBytes(Pointer<QjsValue> slot) {
    final outPtr = malloc<Pointer<Uint8>>();
    final lenPtr = malloc<Int32>();
    try {
      final rc = _bridge.getBytes(_ctxPointer, slot, outPtr, lenPtr);
      if (rc != 0) return null;
      final len = lenPtr.value;
      final bytes = Uint8List(len);
      bytes.setAll(0, outPtr.value.asTypedList(len));
      _bridge.freeBuffer(outPtr.value.cast());
      return bytes;
    } finally {
      malloc
        ..free(outPtr)
        ..free(lenPtr);
    }
  }

  /// 枚举对象自身可枚举 string 属性名，失败（含异常挂起）返回 null。
  List<String>? _ownPropertyNames(Pointer<QjsValue> slot) {
    final namesPtr = malloc<Pointer<Pointer<Utf8>>>();
    final countPtr = malloc<Uint32>();
    try {
      final rc = _bridge.ownPropertyNames(_ctxPointer, slot, namesPtr, countPtr);
      if (rc != 0) return null;
      final count = countPtr.value;
      final list = <String>[];
      for (var i = 0; i < count; i++) {
        final p = namesPtr.value[i];
        if (p != nullptr) list.add(p.toDartString());
      }
      _bridge.freeBuffer(namesPtr.value.cast());
      return list;
    } finally {
      malloc
        ..free(namesPtr)
        ..free(countPtr);
    }
  }

  void _dartToJs(Object? value, Pointer<QjsValue> out) {
    switch (value) {
      case null:
        _bridge.makeUndefined(out);
      case bool b:
        _bridge.makeBool(_ctxPointer, out, b ? 1 : 0);
      case int i:
        if (i >= -2147483648 && i <= 2147483647) {
          _bridge.makeInt32(_ctxPointer, out, i);
        } else {
          _bridge.makeFloat64(_ctxPointer, out, i.toDouble());
        }
      case double d:
        _bridge.makeFloat64(_ctxPointer, out, d);
      case String s:
        final ptr = s.toNativeUtf8();
        try {
          _bridge.newString(_ctxPointer, ptr, _utf8Len(s), out);
        } finally {
          malloc.free(ptr);
        }
      case _HostFunctionHandle handle:
        _bridge.newHostFunction(_ctxPointer, handle.id, out);
      case JsFunctionRef ref:
        _bridge.dupValue(_ctxPointer, ref.slot);
        _bridge.valueMove(out, ref.slot);
      case Map m:
        _bridge.newObject(_ctxPointer, out);
        m.forEach((k, v) {
          final child = _newSlot();
          try {
            _dartToJs(v, child);
            final keyPtr = k.toString().toNativeUtf8();
            try {
              _bridge.setProp(_ctxPointer, out, keyPtr, child);
            } finally {
              malloc.free(keyPtr);
            }
          } finally {
            _bridge.freeSlot(child);
          }
        });
      case Uint8List bytes:
        // core 契约：Uint8List 映射为 JS TypedArray（非 JS Array of int）。
        // 必须放在 List 之前（Uint8List 是 List<int> 的子类型）。
        if (bytes.isEmpty) {
          _bridge.newUint8Array(_ctxPointer, nullptr, 0, out);
        } else {
          final dataPtr = malloc<Uint8>(bytes.length);
          try {
            dataPtr.asTypedList(bytes.length).setAll(0, bytes);
            _bridge.newUint8Array(_ctxPointer, dataPtr, bytes.length, out);
          } finally {
            malloc.free(dataPtr);
          }
        }
      case List l:
        _bridge.newArray(_ctxPointer, out);
        for (var i = 0; i < l.length; i++) {
          final child = _newSlot();
          try {
            _dartToJs(l[i], child);
            _bridge.setPropU32(_ctxPointer, out, i, child);
          } finally {
            _bridge.freeSlot(child);
          }
        }
      default:
        _bridge.makeUndefined(out);
    }
  }

  String _takeExceptionMessage() {
    final exc = _newSlot();
    try {
      _bridge.getException(_ctxPointer, exc);
      final direct = _readCString(exc);
      if (direct != null) return direct;
      // JS_ToCString 对部分异常（挂起嵌套等）失败——按名取 message/stack
      // 字段兜底，避免真实错误被 'JS exception' 兜底文本吞掉（真机排障
      // 实锤：category 失败只剩无信息兜底文本）
      final msgSlot = _newSlot();
      try {
        for (final field in const ['message', 'stack']) {
          final namePtr = field.toNativeUtf8();
          try {
            _bridge.getProp(_ctxPointer, exc, namePtr, msgSlot);
          } finally {
            malloc.free(namePtr);
          }
          final v = _readCString(msgSlot);
          if (v != null && v.isNotEmpty) return '$field: $v';
        }
      } finally {
        _bridge.freeValue(_ctxPointer, msgSlot);
      }
      return 'JS exception';
    } finally {
      _bridge.freeValue(_ctxPointer, exc);
      _bridge.freeSlot(exc);
    }
  }

  void _checkDisposed() {
    if (_disposed) throw StateError('QuickjsEngine 已 dispose');
  }
}

class _QuickjsFactory implements JsEngineFactory {
  _QuickjsFactory(this.libPath);
  final String? libPath;

  @override
  String get name => 'quickjs-ffi';

  @override
  JsEngine create(JsEngineConfig config) =>
      QuickjsEngine.createWith(config, libPath: libPath);
}

/// evaluate / compile 失败时抛出，message 为 JS 异常文本。
class JsEvalException implements Exception {
  JsEvalException(this.message);
  final String message;

  @override
  String toString() => 'JsEvalException: $message';
}
