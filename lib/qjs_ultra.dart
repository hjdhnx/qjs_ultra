/// qjs_ultra：Flutter / Dart 的 QuickJS 引擎包。
///
/// 自研魔改 QuickJS + 高性能 C 扩展核心（cheerio/URL/GBK/Buffer/crypto/
/// WebCrypto/fs/zlib/SQLite/wasm3 启动即注入 JS 全局），纯 Dart + `dart:ffi`
/// 封装，不依赖 Flutter。
///
/// ## 接入三步
///
/// ```dart
/// import 'package:qjs_ultra/qjs_ultra.dart';
///
/// final engine = QuickjsEngine.createWith(
///   const JsEngineConfig(
///     stackSize: 1 * 1024 * 1024,     // 必须 ≤ 宿主线程栈 3/4
///     memoryLimit: 64 * 1024 * 1024,
///     timeoutMs: 10000,               // 单次求值/调用墙钟超时
///   ),
///   libPath: '/path/to/libquickjs_bridge.so',
/// );
///
/// // 同步宿主函数：JS 调用时阻塞等 Dart 返回
/// engine.registerFunction('hostAdd', (args) => args[0] + args[1]);
///
/// // 异步宿主函数：JS 侧 await 让出，Dart Future 完成后 settle
/// engine.registerAsyncFunction('httpGet', (args) async => {...});
///
/// final out = await engine.evaluateAsync('(async () => await hostAdd(1, 2))()');
/// engine.dispose();
/// ```
///
/// ## 两种宿主模式
///
/// - **同步**（[QuickjsEngine.registerFunction]）：JS 阻塞等 Dart 返回，适合
///   worker isolate 里跑（drpy2 的同步 req 模型）；
/// - **异步**（[QuickjsEngine.registerAsyncFunction]）：JS 调用同步拿到 Promise
///   即让出，Dart Future 完成后 settle 并自动泵微任务——`await` 期间 UI 正常，
///   同一引擎实例可与同步函数混用（drpy3 异步桥模型）。
///
/// ## Promise / 模块
///
/// [QuickjsEngine.evaluateAsync] / [QuickjsEngine.callAsync] 的结果为 promise 时
/// 自动泵微任务至 settle（含顶层 await 模块、`Promise.all` 链）；rejected 抛
/// [JsEvalException]（[JsErrorKind] 结构化分类，syntax 错误带行列号）。
/// 模块用 [QuickjsEngine.registerModule] 注册、[QuickjsEngine.executeBytecode]
/// 跑预编译字节码；纯 JS 定时器用 [QuickjsEngine.installTimers]。
///
/// ## 值契约与护栏
///
/// 跨边界值为 JSON-like（null/bool/num/String/Map/List）加 `Uint8List`（→
/// TypedArray）、`DateTime`（→ Date）、超 ±2^53 int（→ BigInt）；JS 函数跨边界
/// 为 [JsFunctionRef]（只能传回 callFunction/callAsync）。转换带循环引用检测与
/// 深度/节点上限，恶意结构打不爆宿主栈。
///
/// ## 平台前提
///
/// 只支持 64 位目标（arm64-v8a / x86_64）。引擎动态库从本仓
/// [Releases](https://github.com/hjdhnx/qjs_ultra/releases) 按平台下载；
/// [QuickjsEngine.createWith] 的 `libPath` 指向它，未传时按环境变量
/// `QJS_ULTRA_LIB` 与平台默认名查找。
///
/// 完整文档见仓库 README；JS 侧注入的全局能力清单见
/// `docs/quickjs-android-api-docs.html`（§23 速查表）。
library;

export 'src/js_engine.dart'
    show JsEngine, JsEngineConfig, JsEngineFactory, JsHostFunction, JsMemoryUsage, JsModuleLoader;
export 'src/qjs_bindings.dart' show QjsBridge, QjsTag, QjsEvalFlags, QjsValue;
export 'src/quickjs_engine.dart'
    show QuickjsEngine, JsEvalException, JsErrorKind, JsFunctionRef;
