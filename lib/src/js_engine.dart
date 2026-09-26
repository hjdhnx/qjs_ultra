import 'dart:typed_data';

/// 注册到 JS 全局的宿主函数。
///
/// **参数是 Dart 值，返回值也是 Dart 值**——这是两条绑定路线必须共同遵守的
/// 契约：跨 FFI 边界的值统一为 JSON-like（Map/List/String/num/bool/null）
/// 加 `Uint8List`（映射为 JS TypedArray）。
///
/// 这类函数是**同步执行**的。JS 爬虫里的 `req(url)` 就是同步调用，
/// 因此实现层必须能做到「JS 调 Dart 时阻塞等待结果」，不能退化成异步消息。
typedef JsHostFunction = Object? Function(List<Object?> args);

/// 跨边界值的完整契约（所有实现必须一致）：
///
/// - 基础类型：`null` / `bool` / `num` / `String` / `Map<String, Object?>` /
///   `List<Object?>` / `Uint8List`
/// - **JS 函数**：从 JS 侧传到 Dart 时保持为引擎定义的句柄对象
///   （例如 `Map` 里的 `complete` 回调）。句柄**只能**回传给
///   [JsEngine.callFunction]，在 Dart 侧直接调用它是未定义行为。
///   `getGlobalProperty` 返回的对象里的函数同理。
/// - **Date**：JS `Date` ↔ Dart `DateTime`（毫秒精度快照）。
/// - **BigInt**：JS `BigInt` → Dart 十进制数字符串（任意精度）；
///   Dart `int` 超出 ±2^53（JSON 安全整数）→ JS `BigInt`，保精度不降 double。
/// - 未在列表内的 JS 值（Symbol、Proxy 等）应转换为 String 或抛错，
///   不得静默丢弃。
/// - JS 侧 resolve 的 Promise 会被自动等待（排空微任务至 settle），
///   Dart 侧拿到的是 settle 后的值；被 reject 时抛 `JsEvalException`。

/// 模块加载器，对应原版 `QuickJSContext.BytecodeModuleLoader`。
///
/// TVBox 生态的模块多为预编译 bytecode（`//bb` / `//DRPY` 前缀的 base64），
/// 但也有纯源码模块（模板.js、net.js）。引擎实现应**优先**使用
/// [getModuleBytecode]，取不到时回退 [getModuleSource] 自己编译——
/// 这样既兼容 flutter_qjs_next 的源码型 moduleHandler，
/// 也保住了 bytecode 缓存路径。
abstract class JsModuleLoader {
  /// 返回模块字节码；null 表示没有缓存，继续尝试 [getModuleSource]。
  Uint8List? getModuleBytecode(String moduleName);

  /// 返回模块源码；与 [getModuleBytecode] 同时为 null 表示模块不存在。
  String? getModuleSource(String moduleName);

  /// 相对路径解析，对应原版 `UriUtil.resolve`。
  String normalizeName(String moduleBaseName, String moduleName);
}

/// 引擎配置。字段对齐原版 `QuickJSContext` 的可调项。
class JsEngineConfig {
  const JsEngineConfig({
    this.stackSize = 1024 * 1024,
    this.memoryLimit = 64 * 1024 * 1024,
    this.gcThreshold,
    this.timeoutMs,
  });

  /// JS 栈大小，字节。
  final int stackSize;

  /// 堆上限，字节；0 表示不限制。
  final int memoryLimit;

  /// GC 触发阈值（已分配字节数），null 表示用引擎默认。
  final int? gcThreshold;

  /// 单次求值/调用的墙钟超时，毫秒；null 表示不限制。
  ///
  /// 由引擎中断处理器实现（对齐 fjs 的 shutdown 模型）：到期后 JS 执行在
  /// 下一个中断检查点被打断，抛出 `errorKind == timeout` 的
  /// [JsEvalException]。同步调用模型下 Dart 侧 Timer 不会触发
  /// （eval 阻塞 isolate），超时必须由 C 侧查墙钟——`while(true){}`
  /// 这类死循环也能被回收成可捕获异常，而非卡死线程。
  final int? timeoutMs;
}

class JsMemoryUsage {
  const JsMemoryUsage({this.mallocSize = 0, this.memoryUsedSize = 0});
  final int mallocSize;
  final int memoryUsedSize;
}

/// JS 引擎抽象。
///
/// 存在的原因：绑定路线并行（自建 dart:ffi 与 flutter_qjs_next），
/// 业务层只依赖本接口，切换实现不需要改上层代码，
/// 并且可以用同一套 conformance test 对比两条路线的行为差异。
abstract class JsEngine {
  /// 工厂注册表：app 启动前注册其中一个实现。
  static JsEngineFactory? _factory;

  static void registerFactory(JsEngineFactory factory) => _factory = factory;

  static JsEngineFactory get factory => _factory ??= (throw StateError(
        '未注册 JsEngineFactory。请在 app 入口调用 '
        'JsEngine.registerFactory(...)。',
      ));

  static JsEngine create([JsEngineConfig? config]) =>
      factory.create(config ?? const JsEngineConfig());

  /// 实现名称，用于日志与测试报告区分。
  String get implementationName;

  bool get isDisposed;

  /// 执行脚本（非模块）。
  Object? evaluate(String script, {String? fileName});

  /// 执行 ES module。爬虫源码与 `模板.js` 都走这条路径。
  Object? evaluateModule(String script, {String? fileName});

  /// 执行预编译字节码。对应原版 `ctx.execute(bytecode, name)`。
  Object? executeBytecode(Uint8List bytecode, {String? fileName});

  /// 编译模块为字节码，供缓存复用。
  Uint8List compileModule(String source, {String? fileName});

  void setModuleLoader(JsModuleLoader? loader);

  /// 把 Dart 函数注册为 JS 全局函数。
  ///
  /// 这是整个宿主能力层的入口：`pdfh` / `pd` / `_http` / `local.get` 等
  /// 全局函数全部通过它注入。
  void registerFunction(String name, JsHostFunction fn);

  /// 取一个可写入对象属性的函数句柄。
  ///
  /// 用于 `local.get` / `local.set` 这类挂在子对象上的函数，
  /// 以及需要把回调塞进 options 的场景。
  Object? registeredFunctionFor(JsHostFunction fn);

  /// console.log 等输出。
  void setConsoleSink(void Function(String message)? sink);

  /// 读取全局属性；返回 Dart 值。
  Object? getGlobalProperty(String name);

  /// 写入全局属性。
  void setGlobalProperty(String name, Object? value);

  /// 调用 JS 函数。
  Object? callFunction(Object? fn, List<Object?> args);

  /// 把 JSON 字符串解析为 JS 对象句柄。
  Object? parseJson(String json);

  /// 把 JS 值序列化为 JSON 字符串。对应原版 `stringify`。
  String stringify(Object? value);

  /// 排空 Promise 微任务队列。
  bool executePendingJobs();

  void runGC();

  JsMemoryUsage? getMemoryUsage();

  void dispose();
}

/// 引擎工厂，两条路线各提供一个实现。
abstract class JsEngineFactory {
  String get name;

  JsEngine create(JsEngineConfig config);
}
