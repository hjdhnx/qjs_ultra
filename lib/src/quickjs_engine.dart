import 'dart:async';
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
  QuickjsEngine._(this._bridge, this._config) {
    _rt = _bridge.newRuntime();
    if (_rt == nullptr) throw StateError('JS_NewRuntime 失败');
    // 中断/超时处理器 + 未处理 rejection 管道（ctl 由 C 层 qjs_free_runtime
    // 自动回收，无需 Dart 侧清理）
    _bridge.installRuntimeCtl(_rt);
    if (_config.memoryLimit > 0) {
      _bridge.setMemoryLimit(_rt, _config.memoryLimit);
    }
    if (_config.gcThreshold != null && _config.gcThreshold! > 0) {
      _bridge.setGcThreshold(_rt, _config.gcThreshold!);
    }
    if (_config.stackSize > 0) {
      _bridge.setMaxStackSize(_rt, _config.stackSize);
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
  final JsEngineConfig _config;
  late final Pointer<Void> _rt;
  late final Pointer<Void> _ctxPointer;

  /// 当前是否处于墙钟截止期内（_runGuarded 设置）。用于把引擎的
  /// "interrupted" 异常归类为 timeout。
  bool _deadlineActive = false;

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

  Object? _eval(String script, String fileName, int flags) => _runGuarded(() {
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
          if (rc != 0) throw _takeException();
          final result = _resolveResult(out);
          _drainJobs();
          return result;
        } finally {
          // rejected promise 会从 _resolveResult 抛出——free 必须在 finally，
          // 否则 promise 对象引用泄漏
          _bridge.freeValue(_ctxPointer, out);
          _bridge.freeSlot(out);
          malloc
            ..free(scriptPtr)
            ..free(namePtr);
        }
      });

  /// 单次 JS 执行的墙钟截止期（fjs shutdown 模型的同步宿主适配版）：
  /// Dart 侧 Timer 在同步 eval 期间不会触发，超时必须由 C 侧中断处理器
  /// 查墙钟。离开作用域即清除，一次调用超时不影响后续调用。
  T _runGuarded<T>(T Function() body) {
    final timeout = _config.timeoutMs;
    final useDeadline = timeout != null && timeout > 0;
    if (useDeadline) {
      _deadlineActive = true;
      _bridge.setDeadline(
          _rt, DateTime.now().millisecondsSinceEpoch + timeout);
    }
    try {
      return body();
    } finally {
      if (useDeadline) {
        _deadlineActive = false;
        _bridge.setDeadline(_rt, 0);
      }
    }
  }

  /// 求值结果统一出口（fjs async eval 语义的同步泵版）：
  /// 结果是 promise 时泵微任务直至 settle 并取 settle 值；rejected 抛
  /// [JsEvalException]；队空仍 pending（在等宿主异步，本契约不支持）时
  /// 返回 null——promise 无 JSON-like Dart 表示。
  Object? _resolveResult(Pointer<QjsValue> slot) {
    var state = _bridge.getPromiseState(_ctxPointer, slot);
    if (state < 0) return _jsToDart(slot, _Conv());
    while (state == 0) {
      final rc = _bridge.executePendingJob(_rt);
      if (rc < 0) throw _takeException();
      if (rc == 0) break;
      state = _bridge.getPromiseState(_ctxPointer, slot);
    }
    if (state == 0) return null;
    final result = _newSlot();
    try {
      _bridge.getPromiseResult(_ctxPointer, slot, result);
      if (state == 2) throw _exceptionFromValue(result);
      return _jsToDart(result, _Conv());
    } finally {
      _bridge.freeValue(_ctxPointer, result);
      _bridge.freeSlot(result);
    }
  }

  @override
  Uint8List compileModule(String source, {String? fileName}) {
    _checkDisposed();
    return _compile(source, fileName ?? 'module.js', true);
  }

  Uint8List _compile(String source, String fileName, bool isModule) =>
      _runGuarded(() {
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
            throw _takeException();
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
      });

  @override
  Object? executeBytecode(Uint8List bytecode, {String? fileName}) {
    _checkDisposed();
    return _runGuarded(() {
      final buf = malloc<Uint8>(bytecode.length);
      buf.asTypedList(bytecode.length).setAll(0, bytecode);
      final out = _newSlot();
      try {
        final rc = _bridge.evalBytecode(_ctxPointer, buf, bytecode.length, out);
        if (rc != 0) {
          throw _takeException();
        }
        final result = _resolveResult(out);
        _drainJobs();
        return result;
      } finally {
        _bridge.freeValue(_ctxPointer, out);
        _bridge.freeSlot(out);
        malloc.free(buf);
      }
    });
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
      final result = _jsToDart(value, _Conv());
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

  /// 同步调用 JS 函数，返回**结果槽**（调用方负责 freeValue/freeSlot）。
  /// rc != 0 时抛结构化异常。供 [_callSlot] / [_callSlotAsync] 复用。
  Pointer<QjsValue> _callRaw(Pointer<QjsValue> funcSlot, List<Object?> args) {
    final argv = calloc<QjsValue>(args.isEmpty ? 1 : args.length);
    var out = Pointer<QjsValue>.fromAddress(0);
    try {
      for (var i = 0; i < args.length; i++) {
        _dartToJs(args[i], argv + i);
      }
      out = _newSlot();
      final rc = _bridge.call(
        _ctxPointer,
        funcSlot,
        nullptr,
        args.length,
        argv,
        out,
      );
      if (rc != 0) {
        _bridge.freeSlot(out);
        out = Pointer<QjsValue>.fromAddress(0);
        throw _takeException();
      }
      final result = out;
      out = Pointer<QjsValue>.fromAddress(0);
      return result;
    } finally {
      if (out.address != 0) _bridge.freeSlot(out);
      for (var i = 0; i < args.length; i++) {
        _bridge.freeValue(_ctxPointer, argv + i);
      }
      malloc.free(argv);
    }
  }

  Object? _callSlot(Pointer<QjsValue> funcSlot, List<Object?> args) =>
      _runGuarded(() {
        final out = _callRaw(funcSlot, args);
        try {
          final result = _resolveResult(out);
          _drainJobs();
          return result;
        } finally {
          _bridge.freeValue(_ctxPointer, out);
          _bridge.freeSlot(out);
        }
      });

  @override
  Object? parseJson(String json) {
    _checkDisposed();
    final ptr = json.toNativeUtf8();
    final out = _newSlot();
    try {
      final rc = _bridge.parseJson(_ctxPointer, ptr, _utf8Len(json), out);
      if (rc != 0) {
        throw _takeException();
      }
      final result = _jsToDart(out, _Conv());
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
    return _runGuarded(() {
      final slot = _newSlot();
      try {
        _dartToJs(value, slot);
        final outPtr = malloc<Pointer<Utf8>>();
        final lenPtr = malloc<Int32>();
        try {
          final rc = _bridge.stringify(_ctxPointer, slot, outPtr, lenPtr);
          if (rc != 0) {
            throw _takeException();
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
    });
  }

  @override
  bool executePendingJobs() {
    _checkDisposed();
    return _runGuarded(() {
      for (;;) {
        final rc = _bridge.executePendingJob(_rt);
        if (rc == 0) return true; // 队列已空
        if (rc < 0) throw _takeException(); // job 内异常必须浮出，吞掉会让
        // 失败表现为「静默不执行」（对齐 _drainJobs 的契约）
      }
    });
  }

  /// 排空微任务队列。job 内抛出的 JS 异常不能吞掉，
  /// 否则模块顶层求值失败会表现为"静默不执行"。
  void _drainJobs() {
    for (;;) {
      final rc = _bridge.executePendingJob(_rt);
      if (rc == 0) return;
      if (rc < 0) throw _takeException();
    }
  }

  /// 排空后台未处理 promise rejection 队列（fjs drainUnhandledJobErrors
  /// 语义，C 侧 rejection tracker 收集）。这是 QuickjsEngine 的具体能力，
  /// 不在 [JsEngine] 抽象里。返回格式化文本与队满（32 条）累计丢弃数。
  ///
  /// 队列语义是「**曾**处于未处理状态」的 rejection：被 Dart 侧
  /// `_resolveResult` 转成异常抛出的 rejected promise，入队记录同样保留
  /// （本层同步模型不做 fjs 的身份撤单/checkpoint 差集——无重放问题，
  /// 诊断场景下宁多勿丢）。
  ({List<String> errors, int dropped}) drainUnhandledRejections() {
    _checkDisposed();
    final errors = <String>[];
    final out = malloc<Pointer<Utf8>>();
    final dropped = malloc<Uint64>();
    try {
      while (_bridge.pollRejection(_rt, out, dropped) == 1) {
        final text = _optNativeText(out.value);
        if (text != null) errors.add(text);
      }
      return (errors: errors, dropped: dropped.value);
    } finally {
      malloc
        ..free(out)
        ..free(dropped);
    }
  }

  // ---------- 异步桥（fjs bridge_call 语义的同步宿主移植） ----------
  //
  // JS 语义：`const r = await fn(args...)`。宿主函数被 JS 调用时同步创建
  // pending promise 返回（JS await 让出，引擎不被阻塞），Dart 侧 Future
  // 完成后经 qjs_settle_pending 唤醒并泵微任务推进 await 链。in-flight
  // 调用期间 evaluateAsync/callAsync 的前台泵循环挂起等待（fjs 前台等待
  // + 后台兜底两层模型的单 isolate 版）。

  final _asyncInFlight = <int>{};
  Completer<void>? _activitySignal;
  final _backgroundErrors = <String>[];
  bool _pumping = false;
  var _nextTimerId = 1;
  final _timers = <int, Timer>{};

  /// 注册异步宿主函数为 JS 全局函数。返回值走 JSON-like 契约
  /// （Map/List/String/num/bool/null/Uint8List/DateTime）。
  void registerAsyncFunction(
      String name, Future<Object?> Function(List<Object?> args) fn) {
    registerFunction(name, (args) {
      final promise = _newSlot();
      final idPtr = malloc<Int32>();
      try {
        final rc = _bridge.newPendingPromise(_ctxPointer, promise, idPtr);
        if (rc != 0) throw _takeException();
        final pid = idPtr.value;
        _asyncInFlight.add(pid);
        unawaited(_runAsyncBridge(fn, pid, args));
        return _PendingPromiseHandle(promise);
      } finally {
        malloc.free(idPtr);
      }
    });
  }

  Future<void> _runAsyncBridge(
      Future<Object?> Function(List<Object?> args) fn, int pid,
      List<Object?> args) async {
    var ok = 1;
    final value = _newSlot();
    try {
      try {
        _dartToJs(await fn(args), value);
      } catch (e) {
        ok = 0;
        final msgPtr = e.toString().toNativeUtf8();
        try {
          _bridge.newString(_ctxPointer, msgPtr, _utf8Len(e.toString()), value);
        } finally {
          malloc.free(msgPtr);
        }
      }
      if (_bridge.settlePending(_ctxPointer, pid, ok, value) != 0) {
        // 表已清（engine 已 dispose）：引用自释放，静默收场
        _bridge.freeValue(_ctxPointer, value);
      }
    } finally {
      _bridge.freeSlot(value);
    }
    _pumpUntilIdle(); // settle 后泵微任务，推进 JS await 链
    _asyncInFlight.remove(pid);
    _signalActivity();
  }

  /// 后台泵：job 异常吞进 [drainBackgroundErrors]（无前台调用方可抛）。
  /// 重入保护：泵中触发的新 job 由外层循环继续消费（单 isolate 无并发）。
  void _pumpUntilIdle() {
    if (_pumping) return;
    _pumping = true;
    try {
      for (;;) {
        final rc = _bridge.executePendingJob(_rt);
        if (rc == 0) return;
        if (rc < 0) _backgroundErrors.add(_takeException().toString());
      }
    } finally {
      _pumping = false;
    }
  }

  Future<void> _waitForActivity() =>
      (_activitySignal ??= Completer<void>()).future;

  void _signalActivity() {
    final c = _activitySignal;
    _activitySignal = null;
    c?.complete();
  }

  /// 异步求值（fjs 前台等待语义）：结果为 promise 时泵微任务并等待
  /// in-flight 异步宿主调用直至 settle；rejected 抛 [JsEvalException]。
  /// 队空仍 pending（等的是未注册的异步源）返回 null。
  Future<Object?> evaluateAsync(String script,
      {String? fileName, int flags = QjsEvalFlags.global}) {
    _checkDisposed();
    return _runGuardedAsync(() async {
      final scriptPtr = script.toNativeUtf8();
      final namePtr = (fileName ?? 'script.js').toNativeUtf8();
      final out = _newSlot();
      try {
        final rc = _bridge.eval(
            _ctxPointer, scriptPtr, _utf8Len(script), namePtr, flags, out);
        if (rc != 0) throw _takeException();
        final result = await _resolveResultAsync(out);
        _pumpUntilIdle();
        return result;
      } finally {
        _bridge.freeValue(_ctxPointer, out);
        _bridge.freeSlot(out);
        malloc
          ..free(scriptPtr)
          ..free(namePtr);
      }
    });
  }

  /// 异步调用 JS 函数（[callFunction] 的 async 版，promise 语义同
  /// [evaluateAsync]）。drpy3 六环节调度入口。
  Future<Object?> callAsync(Object? fn, List<Object?> args) {
    _checkDisposed();
    return _runGuardedAsync(() async {
      switch (fn) {
        case JsFunctionRef ref:
          return _callSlotAsync(ref.slot, args);
        case String name:
          final target = getGlobalProperty(name);
          if (target is JsFunctionRef) {
            try {
              return await _callSlotAsync(target.slot, args);
            } finally {
              target.close();
              _openFunctionRefs.remove(target);
            }
          }
          throw StateError('全局属性 $name 不是函数');
        default:
          throw StateError('callAsync 只接受 JsFunctionRef 或全局函数名');
      }
    });
  }

  Future<Object?> _callSlotAsync(
      Pointer<QjsValue> funcSlot, List<Object?> args) async {
    final out = _callRaw(funcSlot, args);
    try {
      final result = await _resolveResultAsync(out);
      _pumpUntilIdle();
      return result;
    } finally {
      _bridge.freeValue(_ctxPointer, out);
      _bridge.freeSlot(out);
    }
  }

  Future<Object?> _resolveResultAsync(Pointer<QjsValue> slot) async {
    var state = _bridge.getPromiseState(_ctxPointer, slot);
    if (state < 0) return _jsToDart(slot, _Conv());
    while (state == 0) {
      _pumpForeground();
      state = _bridge.getPromiseState(_ctxPointer, slot);
      if (state != 0) break;
      if (_asyncInFlight.isNotEmpty) {
        await _waitForActivity();
        continue;
      }
      break;
    }
    if (state == 0) return null;
    final result = _newSlot();
    try {
      _bridge.getPromiseResult(_ctxPointer, slot, result);
      if (state == 2) throw _exceptionFromValue(result);
      return _jsToDart(result, _Conv());
    } finally {
      _bridge.freeValue(_ctxPointer, result);
      _bridge.freeSlot(result);
    }
  }

  /// 前台泵：job 异常抛给当前调用方（与 [_pumpUntilIdle] 的差异点）。
  void _pumpForeground() {
    for (;;) {
      final rc = _bridge.executePendingJob(_rt);
      if (rc == 0) return;
      if (rc < 0) throw _takeException();
    }
  }

  Future<T> _runGuardedAsync<T>(Future<T> Function() body) async {
    final timeout = _config.timeoutMs;
    final useDeadline = timeout != null && timeout > 0;
    if (useDeadline) {
      _deadlineActive = true;
      _bridge.setDeadline(
          _rt, DateTime.now().millisecondsSinceEpoch + timeout);
    }
    try {
      return await body();
    } finally {
      if (useDeadline) {
        _deadlineActive = false;
        _bridge.setDeadline(_rt, 0);
      }
    }
  }

  /// 排空后台错误（后台泵吞掉的 job 异常 + timer 回调异常）。
  List<String> drainBackgroundErrors() {
    final out = List<String>.of(_backgroundErrors);
    _backgroundErrors.clear();
    return out;
  }

  /// 安装 setTimeout/clearTimeout 宿主实现（Dart Timer 驱动）。回调经
  /// [callAsync] 执行：回调内 await 异步宿主函数可正常驱动；回调异常进
  /// 后台错误（对齐 fjs guarded timers 语义）。setInterval 未提供
  /// （drpy3 bundle 无引用；需要时再加）。
  void installTimers() {
    registerFunction('__qjs_setTimeout', (args) {
      final fn = args.isNotEmpty ? args[0] : null;
      final ms = args.length > 1 ? (args[1] as num?)?.toInt() ?? 0 : 0;
      if (fn is! JsFunctionRef) return 0;
      final id = _nextTimerId++;
      _timers[id] = Timer(Duration(milliseconds: ms), () {
        _timers.remove(id);
        unawaited(_fireTimer(fn));
      });
      return id;
    });
    registerFunction('__qjs_clearTimeout', (args) {
      final id = args.isNotEmpty ? args[0] as num? : null;
      _timers.remove(id?.toInt() ?? 0)?.cancel();
      return null;
    });
    evaluate(
      'globalThis.setTimeout = (fn, ms) => __qjs_setTimeout(fn, ms ?? 0);'
      'globalThis.clearTimeout = (id) => __qjs_clearTimeout(id);',
    );
  }

  Future<void> _fireTimer(JsFunctionRef fn) async {
    try {
      await callAsync(fn, const []);
    } catch (e) {
      _backgroundErrors.add('timer: $e');
    }
  }

  /// drpy3 bundle 装载前置垫片：globalThis.fjs 桥（bundle **原样复用，
  /// 零改包**）、atob/btoa 兜底、timers。[bridge] 收到 bundle 发来的
  /// `{action, ...}` 消息（req/loadAsset/getProxy/evalModule 协议与
  /// DsPlayer Drpy3BridgeHandlers 完全一致）。须在装载 bundle 之前调用。
  void installDrpy3Shim(Future<Object?> Function(Object? message) bridge) {
    registerAsyncFunction('__qjs_bridge_call', (args) => bridge(args.first));
    installTimers();
    evaluate('''
globalThis.fjs = { bridge_call: (msg) => globalThis.__qjs_bridge_call(msg) };
if (typeof globalThis.atob === 'undefined') {
  globalThis.atob = (s) => {
    const bytes = [];
    for (let i = 0; i < s.length; i++) bytes.push(s.charCodeAt(i) & 0xff);
    return Buffer.from(bytes).toString('base64');
  };
}
if (typeof globalThis.btoa === 'undefined') {
  globalThis.btoa = (s) => {
    const bin = Buffer.from(String(s), 'base64');
    let out = '';
    for (let i = 0; i < bin.length; i++) out += String.fromCharCode(bin[i]);
    return out;
  };
}
''');
  }

  /// 装载 ESM 模块源码（经内置模块表）并把 [exports] 导出挂为
  /// `__<导出名>` 全局函数——qjs_ultra 没有 rquickjs 的
  /// `engine.call(module, method)` 调度面，drpy3 六环节经此暴露
  /// （callAsync('__drpy3Call', ...)）。含顶层 await 的模块会等到装载完成。
  Future<void> loadModuleToGlobals(
    String moduleName,
    String source, {
    List<String> exports = const [
      'drpy3Setup',
      'drpy3Load',
      'drpy3Call',
      'drpy3Capabilities',
      'drpy3Sweep',
      'drpy3StoreExport',
      'drpy3StoreImport',
    ],
  }) {
    final existing = _moduleLoader;
    if (existing == null) {
      _moduleLoader = _InlineModuleLoader({moduleName: source});
    } else if (existing is _InlineModuleLoader) {
      existing.modules[moduleName] = source;
    } else {
      throw StateError('loadModuleToGlobals 与自定义 JsModuleLoader 冲突');
    }
    final attach = exports.map((e) => 'g.__$e = m.$e;').join('\n  ');
    return evaluateAsync('''
(async () => {
  const m = await import('$moduleName');
  const g = globalThis;
  $attach
})()
''');
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
    final cx = _Conv();
    final args = <Object?>[
      for (var i = 0; i < argc; i++) _jsToDart(argv + i, cx),
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

  Object? _jsToDart(Pointer<QjsValue> slot, _Conv cx) {
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
        // core 契约：Symbol 转 String；BigInt 保任意精度（十进制文本），
        // 不得静默丢弃或降 double。JS_ToCStringLen 对 symbol 产出
        // description、bigInt 产出十进制文本。
        final s = _readCString(slot);
        return s ?? slot.ref.u.u64.toString();
      case QjsTag.null_:
      case QjsTag.undefined:
        return null;
      case QjsTag.object:
        return _objectToDart(slot, cx);
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

  Object? _objectToDart(Pointer<QjsValue> slot, _Conv cx) {
    // 护栏（fjs 同款上限）：超限/循环处降级 null——宁可丢值不可打爆宿主栈
    if (++cx.nodes > _Conv.maxNodes || ++cx.depth > _Conv.maxDepth) {
      return null;
    }
    // JSObject* 在一次转换遍历中稳定，可作循环身份（仅 64 位，见文件头约定）
    final identity = slot.ref.u.u64;
    if (!cx.active.add(identity)) return null;
    try {
      // TypedArray / ArrayBuffer？直接回读为 Uint8List。
      // 必须放在数组判定之前（TypedArray 也是 object tag）。
      final bytes = _tryReadBytes(slot);
      if (bytes != null) return bytes;

      // Date？借道 getTime()（fjs 同款，不碰引擎内部表示）。
      // Invalid Date（NaN）不算命中，继续走对象路径。
      final ms = malloc<Double>();
      try {
        if (_bridge.getDateMs(_ctxPointer, slot, ms) == 0 &&
            !ms.value.isNaN) {
          return DateTime.fromMillisecondsSinceEpoch(ms.value.round());
        }
      } finally {
        malloc.free(ms);
      }

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
              list.add(_jsToDart(item, cx));
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
          map[name] = _jsToDart(value, cx);
        } finally {
          _bridge.freeValue(_ctxPointer, value);
          _bridge.freeSlot(value);
        }
      }
      return map;
    } finally {
      cx.active.remove(identity);
      cx.depth--;
    }
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
      case _PendingPromiseHandle h:
        // 异步桥返回通道：promise 引用移交 out，载体槽立即释放
        _bridge.valueMove(out, h.slot);
        _bridge.freeSlot(h.slot);
      case DateTime d:
        _bridge.newDate(_ctxPointer, d.millisecondsSinceEpoch.toDouble(), out);
      case int i:
        if (i >= -2147483648 && i <= 2147483647) {
          _bridge.makeInt32(_ctxPointer, out, i);
        } else if (i > 9007199254740991 || i < -9007199254740991) {
          // 超出 ±2^53（JSON 安全整数）：转 BigInt 保精度，降 double 丢位
          // （fjs 同款策略）
          final text = i.toString();
          final ptr = text.toNativeUtf8();
          try {
            _bridge.newBigInt(_ctxPointer, ptr, _utf8Len(text), out);
          } finally {
            malloc.free(ptr);
          }
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

  /// 取走 pending 异常并拆解为结构化 [JsEvalException]（fjs error.rs
  /// from_exception 的移植：name/message/stack 一次取全，按 name 归类，
  /// syntax 错误从 stack 解析行号/列号）。
  JsEvalException _takeException() {
    final namePtr = malloc<Pointer<Utf8>>();
    final msgPtr = malloc<Pointer<Utf8>>();
    final stackPtr = malloc<Pointer<Utf8>>();
    try {
      _bridge.getExceptionDetails(_ctxPointer, namePtr, msgPtr, stackPtr);
      return _buildException(
        _optNativeText(namePtr.value),
        _optNativeText(msgPtr.value),
        _optNativeText(stackPtr.value),
      );
    } finally {
      malloc
        ..free(namePtr)
        ..free(msgPtr)
        ..free(stackPtr);
    }
  }

  /// 从 promise rejection reason 值构造结构化异常。
  JsEvalException _exceptionFromValue(Pointer<QjsValue> reason) {
    final text = _readCString(reason);
    return _buildException(
      _readPropText(reason, 'name'),
      _readPropText(reason, 'message'),
      _readPropText(reason, 'stack'),
      fallbackText: text,
    );
  }

  String? _readPropText(Pointer<QjsValue> obj, String prop) {
    final slot = _newSlot();
    try {
      final namePtr = prop.toNativeUtf8();
      try {
        _bridge.getProp(_ctxPointer, obj, namePtr, slot);
      } finally {
        malloc.free(namePtr);
      }
      if (_bridge.getTag(slot) == QjsTag.undefined) return null;
      return _readCString(slot);
    } finally {
      _bridge.freeValue(_ctxPointer, slot);
      _bridge.freeSlot(slot);
    }
  }

  String? _optNativeText(Pointer<Utf8> p) {
    if (p == nullptr) return null;
    final s = p.toDartString();
    _bridge.freeBuffer(p.cast());
    return s;
  }

  JsEvalException _buildException(
    String? name,
    String? message,
    String? stack, {
    String? fallbackText,
  }) {
    final kind = _classifyError(name, message, _deadlineActive);
    final pos =
        kind == JsErrorKind.syntax && stack != null ? _parseStackPosition(stack) : null;
    final text = (message != null && message.isNotEmpty)
        ? (name != null ? '$name: $message' : message)
        : (fallbackText != null && fallbackText.isNotEmpty
            ? fallbackText
            : 'JS exception');
    return JsEvalException(
      text,
      name: name,
      stack: stack,
      line: pos?.$1,
      column: pos?.$2,
      errorKind: kind,
    );
  }

  /// 按 Error.name 归类（fjs 分类表的最小移植）。本内核栈溢出/中断/OOM
  /// 都走 InternalError（JS 侧无 InternalError 构造器，用户抛不出，
  /// 不需要 fjs 那套防伪造消息匹配）；TypeError/ReferenceError 等用户可
  /// throw 伪造，但对诊断只影响分类字符串，不影响行为。
  static JsErrorKind _classifyError(
      String? name, String? message, bool inDeadline) {
    switch (name) {
      case 'SyntaxError':
        return JsErrorKind.syntax;
      case 'TypeError':
        return JsErrorKind.type;
      case 'ReferenceError':
        return JsErrorKind.reference;
      case 'RangeError':
        return JsErrorKind.range;
      case 'InternalError':
        if (message != null) {
          if (message.contains('stack overflow')) {
            return JsErrorKind.stackOverflow;
          }
          if (message.contains('out of memory')) {
            return JsErrorKind.memoryLimit;
          }
          if (message.contains('interrupted')) {
            return inDeadline ? JsErrorKind.timeout : JsErrorKind.interrupted;
          }
        }
        return JsErrorKind.generic;
      default:
        return JsErrorKind.generic;
    }
  }

  /// 从 stack 首帧解析位置，支持 `at fn (file:line:col)` 与 `at file:line:col`
  /// 两种形态（fjs parse_stack_position 同款）。
  static (int, int)? _parseStackPosition(String stack) {
    for (final line in stack.split('\n')) {
      final m = RegExp(r'\(([^()]+):(\d+):(\d+)\)').firstMatch(line) ??
          RegExp(r'at\s+([^(\s]+):(\d+):(\d+)').firstMatch(line);
      if (m != null) {
        return (int.parse(m.group(2)!), int.parse(m.group(3)!));
      }
    }
    return null;
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

/// evaluate / compile / call 失败时抛出。
///
/// [message] 为 "Name: message"（或兜底 coerce 文本）；[name] / [stack] 来自
/// JS Error 对象；[line] / [column] 由 stack 首帧解析（syntax 错误）；
/// [errorKind] 为结构化分类（fjs JsError 的最小移植）。
class JsEvalException implements Exception {
  JsEvalException(
    this.message, {
    this.name,
    this.stack,
    this.line,
    this.column,
    this.errorKind = JsErrorKind.generic,
  });

  final String message;
  final String? name;
  final String? stack;
  final int? line;
  final int? column;
  final JsErrorKind errorKind;

  @override
  String toString() => 'JsEvalException: $message';
}

/// JS 异常的结构化分类。
enum JsErrorKind {
  /// 墙钟超时被打断（JsEngineConfig.timeoutMs）。
  timeout,

  /// 手动中断（非超时场景的 interrupted）。
  interrupted,

  /// 引擎栈溢出（InternalError: stack overflow）。
  stackOverflow,

  /// 内存水位触顶（InternalError: out of memory）。
  memoryLimit,

  /// 语法错误（可从 stack 取行号/列号）。
  syntax,

  /// TypeError。
  type,

  /// ReferenceError。
  reference,

  /// RangeError（用户主动抛出等）。
  range,

  /// 其余（用户 Error/字符串抛出等）。
  generic,
}

/// 值转换护栏状态（fjs ConversionState 同款）：循环引用检测 +
/// 深度/节点上限。按次新建、逐层传参——宿主回调里可再嵌套 evaluate
/// （重入），不能放引擎字段。
class _Conv {
  static const maxDepth = 128;
  static const maxNodes = 100000;

  int depth = 0;
  int nodes = 0;
  final active = <int>{};
}

/// 异步桥的 pending promise 载体：宿主函数返回值通道中把 promise 引用
/// 移交给 JS（[_dartToJs] 消费后槽内存即释放）。
class _PendingPromiseHandle {
  _PendingPromiseHandle(this.slot);
  final Pointer<QjsValue> slot;
}

/// loadModuleToGlobals 的内置模块表（名字 → 源码）。bundle 已被 esbuild
/// 打平，动态 import 只命中精确模块名，无需相对解析。
class _InlineModuleLoader implements JsModuleLoader {
  _InlineModuleLoader(this.modules);
  final Map<String, String> modules;

  @override
  Uint8List? getModuleBytecode(String moduleName) => null;

  @override
  String? getModuleSource(String moduleName) => modules[moduleName];

  @override
  String normalizeName(String moduleBaseName, String moduleName) =>
      moduleName;
}
