/// qjs_ultra：QuickJS 引擎的 Dart/Flutter 绑定。
///
/// 与 C 源码同仓（`bridge/` + `native/`），so 由本仓 CI 出四架构产物
/// （见 Releases）。纯 Dart + `dart:ffi`，不依赖 Flutter —— 可用于 Flutter
/// app、纯 Dart CLI、服务端。
///
/// 快速开始：
/// ```dart
/// import 'package:qjs_ultra/qjs_ultra.dart';
///
/// final engine = QjsUltraEngine.create(libPath: '/path/to/libquickjs_bridge.so');
/// engine.registerFunction('req', (args) => ...);
/// final result = engine.evaluateModule(source);
/// engine.dispose();
/// ```
library;

export 'src/js_engine.dart'
    show JsEngine, JsEngineConfig, JsEngineFactory, JsHostFunction, JsMemoryUsage, JsModuleLoader;
export 'src/qjs_bindings.dart' show QjsBridge, QjsTag, QjsEvalFlags, QjsValue;
export 'src/quickjs_engine.dart' show QuickjsEngine;
