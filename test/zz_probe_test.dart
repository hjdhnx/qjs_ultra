// 临时探针:二分定位 smoke 崩溃步骤(不提交)
import 'dart:convert';
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

import 'engine_test.dart' show resolveLibPath;

void main() {
  final libPath = resolveLibPath();
  late QuickjsEngine engine;

  setUp(() {
    engine = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 4 * 1024 * 1024, memoryLimit: 512 * 1024 * 1024),
      libPath: libPath,
    );
  });
  tearDown(() => engine.dispose());

  test('S1 异步桥最小往返', () async {
    engine.registerAsyncFunction('fn', (args) async => 42);
    final v = await engine.evaluateAsync('(async () => await fn())()');
    expect(v, 42);
  });

  test('S2 Map 往返 + 两次连续调用', () async {
    var n = 0;
    engine.registerAsyncFunction('fetchMap', (args) async {
      n++;
      return {'content': 'x$n', 'headers': {'status': '200'}};
    });
    final a = await engine.evaluateAsync(
        '(async () => await fetchMap({action:"req"}))()');
    expect(a, {'content': 'x1', 'headers': {'status': '200'}});
    final b = await engine.evaluateAsync(
        '(async () => await fetchMap({action:"req"}))()');
    expect(b, {'content': 'x2', 'headers': {'status': '200'}});
  });

  test('S3 嵌套 await(桥内再调桥)', () async {
    engine.registerAsyncFunction('a1', (args) async => 1);
    engine.registerAsyncFunction('a2', (args) async {
      final v = await engine.callAsync('__a1', const []);
      return (v as int) + 1;
    });
    final v = await engine.evaluateAsync('(async () => await a2())()');
    expect(v, 2);
  });

  test('S4 timers', () async {
    engine.installTimers();
    final v = await engine.evaluateAsync('''
      (async () => {
        globalThis.__t = 0;
        await new Promise((res) => { setTimeout(() => { globalThis.__t = 7; res(); }, 30); });
        return globalThis.__t;
      })()
    ''');
    expect(v, 7);
  });

  test('S5 shim + import bundle', () async {
    engine.installDrpy3Shim((message) async => null);
    await engine.loadModuleToGlobals(
        'drpy3', File('test/fixtures/drpy3-fjs.bundle.js').readAsStringSync());
    final ok = await engine.evaluateAsync('typeof __drpy3Setup');
    expect(ok, 'function');
  });

  test('S6 setup + capabilities', () async {
    engine.installDrpy3Shim((message) async => null);
    await engine.loadModuleToGlobals(
        'drpy3', File('test/fixtures/drpy3-fjs.bundle.js').readAsStringSync());
    final setupRaw = await engine.callAsync('__drpy3Setup',
        [jsonEncode({'fjsVersion': 'probe'})]);
    final rt = jsonDecode(setupRaw as String);
    expect(rt, isA<Object>());
    final caps = jsonDecode(
        await engine.callAsync('__drpy3Capabilities', const []) as String)
        as Map;
    print('CAPS = $caps');
    expect(caps['req'], 'host');
  });
}
