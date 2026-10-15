// ignore_for_file: cascade_invocations
import 'dart:ffi' show DynamicLibrary;
import 'dart:isolate' show Isolate;
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

/// unwind 重入崩溃复现（DsPlayer 源检测闪退根修配套，2026-10-15）。
///
/// 真机 SIGABRT「Cannot invoke native callback while unwind error
/// propagates」的假设机制：C（quickjs）同步回调 Dart 宿主函数期间 Dart
/// 异常以 unwind 状态穿透 FFI 边界，quickjs 不感知并继续执行 JS，再次
/// 进入任一 native callback 即触发 VM 保护性 abort（检测高频连续执行
/// 把该窗口打爆）。本文件用最小用例钉死该机制：
///
/// - 修复前：进程在「异常 → 后续回调」的用例上直接崩（dart test 报
///   crash / 非零退出码）；
/// - 修复后：异常全部转为 JS 可 catch 的错误，后续回调照常执行。
///
/// **为什么要用真实 so**：unwind/trampoline 是 VM + C 的行为，mock 无法
/// 复现。库解析与 engine_test 同款（QJS_ULTRA_LIB 或 native 约定路径）。
void main() {
  final libPath = _resolveLib();
  if (libPath == null) {
    test('（跳过）未找到动态库', () {},
        skip: '设置 QJS_ULTRA_LIB 或放置 native/<平台>/<arch>/');
    return;
  }
  if (_missingSymbol(libPath, 'qjs_clear_callbacks')) {
    test('（跳过）动态库缺少当前 ABI 符号', () {},
        skip: '库为 per-context 改造前产物（$libPath）');
    return;
  }

  QuickjsEngine newEngine() =>
      QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);

  group('unwind 重入（宿主回调异常后 C 侧继续执行）', () {
    test('同步 hostCall 抛异常 + JS catch 后继续 console', () {
      final engine = newEngine();
      addTearDown(engine.dispose);
      final logs = <String>[];
      engine.setConsoleSink(logs.add);
      engine.registerFunction('boom', (args) => throw StateError('kaboom'));
      final r = engine.evaluate('''
let out = '';
try { boom(); } catch (e) { out += 'caught:' + e.message; }
console.log('after:' + out);
out
''');
      expect(r, contains('kaboom'));
      expect(logs.single, contains('after:caught'));
    });

    test('hostCall 异常未捕获 → evaluate 抛出 → 后续调用正常', () {
      final engine = newEngine();
      addTearDown(engine.dispose);
      engine.registerFunction('boom', (args) => throw StateError('first'));
      expect(() => engine.evaluate('boom()'), throwsA(anything));
      // unwind 残留若在，第二轮任何回调（console）进入 trampoline 即崩
      final logs = <String>[];
      engine.setConsoleSink(logs.add);
      expect(engine.evaluate("'alive' + '!'"), 'alive!');
      engine.evaluate("console.log('second round')");
      expect(logs.single, contains('second round'));
    });

    test('异步桥异常转 rejection，await 链恢复后继续执行', () async {
      final engine = newEngine();
      addTearDown(engine.dispose);
      final logs = <String>[];
      engine.setConsoleSink(logs.add);
      engine.registerAsyncFunction(
          'areq', (args) async => throw StateError('net-down'));
      // rejection 的微任务消费可能晚于前台泵收尾（evaluateAsync 对队空
      // pending 返回 null）——本用例钉的是「进程不崩 + 引擎可继续用」，
      // 返回值形态不做强断言
      final r = await engine.evaluateAsync('''
(async () => {
  let e = '';
  try { await areq(); } catch (err) { e = err.message; }
  console.log('recovered:' + e);
  return e;
})()
''');
      expect(r == null || (r as String).contains('net-down'), isTrue);
      // 第二轮同步求值必须照常（unwind 残留若在，任何回调进入即崩）
      expect(engine.evaluate('40 + 2'), 42);
    });

    test('循环内交替异常与正常回调（打爆 unwind 窗口）', () {
      final engine = newEngine();
      addTearDown(engine.dispose);
      engine.registerFunction('flaky', (args) {
        final i = (args.first as num?)?.toInt() ?? 0;
        if (i.isEven) throw StateError('fail$i');
        return i;
      });
      final r = engine.evaluate('''
let caught = 0, passed = 0;
for (let i = 0; i < 300; i++) {
  try { flaky(i); passed++; } catch (e) { caught++; }
}
caught + ':' + passed
''');
      expect(r, '150:150');
    });

    test('console sink 自身抛异常不穿透（引擎层硬保证）', () {
      final engine = newEngine();
      addTearDown(engine.dispose);
      engine.setConsoleSink((m) => throw StateError('sink-broken'));
      // 引擎层加固目标语义：console 是日志面，sink 异常被吞掉，绝不能
      // 影响 JS 执行（更不能崩进程）——console.log 调用点照常返回
      engine.evaluate("console.log('boom')");
      expect(engine.evaluate('1+1'), 2);
    });

    test('多 isolate 并发引擎 + 混合异常回调（真机并发崩溃复现）', () async {
      // 真机 qjsstress 并发≥4 时 SIGABRT（串行/并发2 存活）——把核心
      // 要素搬到 PC：多 isolate 各建引擎并发跑「hostCall 异常×正常×
      // console」混合脚本。PC 崩 = 本地可迭代；不崩 = Android 线程池特有
      final N = 8;
      final results = await Future.wait([
        for (var i = 0; i < N; i++)
          Isolate.run(() => _concurrentWorker(libPath, i)),
      ]);
      expect(results.every((e) => e == 'done'), isTrue);
    });
  });
}

/// 并发 worker：独立 isolate 建引擎，注册会抛/正常的宿主函数，跑混合
/// 脚本（异常路径 + console + 正常调用交替，模拟检测期的坏源负载）
String _concurrentWorker(String libPath, int seed) {
  final engine = QuickjsEngine.createWith(
    const JsEngineConfig(),
    libPath: libPath,
  );
  try {
    final logs = <String>[];
    engine.setConsoleSink(logs.add);
    engine.registerFunction('flaky', (args) {
      final i = (args.first as num?)?.toInt() ?? 0;
      if ((i + seed) % 3 == 0) throw StateError('flaky-$seed-$i');
      return i * seed;
    });
    engine.registerFunction('fetchy', (args) {
      final i = (args.first as num?)?.toInt() ?? 0;
      if (i % 2 == 0) throw StateError('fetch-fail-$i');
      return 'ok:$i';
    });
    final r = engine.evaluate('''
let caught = 0;
for (let i = 0; i < 500; i++) {
  try { flaky(i); } catch (e) { caught++; }
  try { fetchy(i); } catch (e) { caught++; }
  if (i % 50 == 0) console.log('tick:' + i);
}
'alive-' + caught
''');
    if (r is! String || !r.startsWith('alive-')) {
      throw StateError('unexpected result: $r');
    }
    return 'done';
  } finally {
    engine.dispose();
  }
}

String? _resolveLib() {
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty && File(env).existsSync()) return env;
  final root = Directory.current.path;
  final candidates = <String>[
    '$root/native/windows/x64/quickjs_bridge.dll',
    '$root/build/win-dbg/libquickjs_bridge.dll',
  ];
  for (final c in candidates) {
    if (File(c).existsSync()) return c;
  }
  return null;
}

bool _missingSymbol(String libPath, String symbol) {
  try {
    DynamicLibrary.open(libPath).lookup(symbol);
    return false;
  } catch (_) {
    return true;
  }
}
