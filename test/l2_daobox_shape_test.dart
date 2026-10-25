// L2 复现（DaoBox 全形状）：timeoutMs=10000 的 deadline + 解析链超预算 +
// 事件风暴并发（DaoBox lib/js_engine.dart 的真实 config）。
import 'dart:async';
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

void main() {
  final libPath = _resolveLib();
  if (libPath == null) {
    test('（跳过）未找到动态库', () {}, skip: '设置 QJS_ULTRA_LIB');
    return;
  }

  Future<QuickjsEngine> make() async {
    // DaoBox 同款 config（lib/js_engine.dart createEngine）
    final engine = QuickjsEngine.createWith(
        const JsEngineConfig(
          stackSize: 512 * 1024,
          memoryLimit: 64 * 1024 * 1024,
          timeoutMs: 10000,
        ),
        libPath: libPath);
    var n = 0;
    engine.registerAsyncFunction('host_httpGet', (args) async {
      final delay = args.isNotEmpty ? (args.first as num).toInt() : 0;
      if (delay > 0) await Future<void>.delayed(Duration(milliseconds: delay));
      n++;
      return <String, Object?>{'status': 200, 'n': n};
    });
    engine.registerFunction('host_requestRefresh', (args) => true);
    engine.evaluate('''
globalThis.host = { httpGet: host_httpGet, requestRefresh: host_requestRefresh };
globalThis.resolveChain = async function (delays) {
  const out = [];
  for (let i = 0; i < delays.length; i++) {
    const r = await host.httpGet(delays[i]);
    out.push(r.status);
  }
  return out.join(',');
};
globalThis.onEvent = async function (i) {
  const r = await host.httpGet(i % 3);
  return { handled: i, n: r.n };
};
''');
    return engine;
  }

  Future<Object?> withProbe(Future<Object?> f) =>
      f.timeout(const Duration(seconds: 20), onTimeout: () async => '__TIMEOUT__');

  test('DaoBox 形状：10s deadline + 慢解析链 + 事件风暴', () async {
    final engine = await make();
    addTearDown(engine.dispose);

    // 解析链总时长 > 10s（DaoBox 解析预算 40s 的等比缩小：8 x 1.8s）
    final chain = withProbe(engine.callAsync('resolveChain', const [
      [1800, 1800, 1800, 1800, 1800, 1800, 1800, 1800]
    ]));
    // 风暴贯穿全程（2ms 间隔 x 300 次 ≈ 600ms+调用本身）
    final events = <Future<Object?>>[];
    final sw = Stopwatch()..start();
    while (sw.elapsedMilliseconds < 2000) {
      events.add(withProbe(engine.callAsync('onEvent', [events.length])));
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
    final r = await chain;
    expect(r, isNot('__TIMEOUT__'), reason: 'L2：慢解析链不落定');
    expect(r, '200,200,200,200,200,200,200,200');
    final ev = await Future.wait(events);
    for (final e in ev) {
      expect(e, isNot('__TIMEOUT__'), reason: 'L2：事件处理不落定');
    }
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('L2 回归：慢异步链（总时长 > timeoutMs）不被 deadline 打断', () async {
    // 修复前：deadline 挂在 async body 上，首个同步段之后持续激活——
    // settle 后的泵恢复 JS 时撞上过期 deadline → interrupted → await 链
    // 中断（表现为不落定/静默 null）。修复后 deadline 按同步段计。
    final engine = QuickjsEngine.createWith(
        const JsEngineConfig(timeoutMs: 200), libPath: libPath);
    addTearDown(engine.dispose);
    var n = 0;
    engine.registerAsyncFunction('host_httpGet', (args) async {
      await Future<void>.delayed(const Duration(milliseconds: 150));
      n++;
      return <String, Object?>{'status': 200, 'n': n};
    });
    engine.evaluate('''
globalThis.host = { httpGet: host_httpGet };
globalThis.slowChain = async function () {
  const out = [];
  for (let i = 0; i < 5; i++) out.push((await host.httpGet(0)).status);
  return out.join(',');
};
''');
    final r = await engine.callAsync('slowChain', const []).timeout(
        const Duration(seconds: 10),
        onTimeout: () async => '__TIMEOUT__');
    expect(r, '200,200,200,200,200',
        reason: '每段同步 JS 远小于 200ms，总链 750ms 不应被打断');
  }, timeout: const Timeout(Duration(seconds: 30)));
}

String? _resolveLib() {
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty && File(env).existsSync()) return env;
  final root = Directory.current.path;
  for (final c in [
    '$root/native/windows/x64/quickjs_bridge.dll',
    '$root/native/windows/x64/quickjs_bridge-windows-x64.dll',
    '$root/build/win-dbg/libquickjs_bridge.dll',
  ]) {
    if (File(c).existsSync()) return c;
  }
  return null;
}
