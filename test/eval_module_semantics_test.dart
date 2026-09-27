// 复现实验：strict module 内 direct eval + IIFE 裸赋值外层 let
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

void main() {
  final libPath = Platform.environment['QJS_ULTRA_LIB'] ??
      'native/windows/x64/quickjs_bridge.dll';
  late QuickjsEngine engine;
  setUp(() {
    engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
  });
  tearDown(() => engine.dispose());

  test('A. 单行副作用（engine_test 原样）', () {
    engine.evaluateModule('globalThis.moduleRan = true;', fileName: 'a.js');
    expect(engine.getGlobalProperty('moduleRan'), isTrue);
  });

  test('B. 多行 module + let + 函数赋值', () {
    const src = '''
let rule = {};
globalThis.__init = function (ruleSrc) {
  eval("(function(){" + ruleSrc.replace("var rule", "rule") + "})()");
};
globalThis.__getRule = function () { return JSON.stringify(rule); };
globalThis.__moduleRan = true;
''';
    engine.evaluateModule(src, fileName: 'b.js');
    expect(engine.getGlobalProperty('__moduleRan'), isTrue, reason: '模块体应执行');
  });

  test('C. 完整链：init direct-eval 装载规则源', () {
    const src = '''
let rule = {};
globalThis.__init = function (ruleSrc) {
  eval("(function(){" + ruleSrc.replace("var rule", "rule") + "})()");
};
globalThis.__getRule = function () { return JSON.stringify(rule); };
''';
    engine.evaluateModule(src, fileName: 'c.js');
    final initFn = engine.getGlobalProperty('__init');
    expect(initFn, isNotNull, reason: '__init 应已注册');
    engine.callFunction(initFn as dynamic, [
      "var rule = { title: 'bm', host: 'https://x.example' };",
    ]);
    final getFn = engine.getGlobalProperty('__getRule') as dynamic;
    final out = engine.callFunction(getFn, []);
    print('RULE => $out');
    expect('$out', contains('bm'), reason: 'eval IIFE 裸赋值应命中外层 let rule');
  });
}
