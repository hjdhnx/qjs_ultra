// L2 复现/回归：前台泵并发挂死（DaoBox M3-2 遗留，PROMPT-M3-REST §一）。
//
// L2 现象：webview 会话活跃（高频 onEvent callAsync）时，长解析链
// callAsync（JS 内多次 await 异步宿主函数）偶发永不落定。
//
// 本文件用「解析链 + 事件风暴交错」的形状打真实引擎，超时探针把
// 「不落定」转成可断言的失败。
import 'dart:async';
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

void main() {
  final libPath = _resolveLib();
  if (libPath == null) {
    test('（跳过）未找到 quickjs_bridge 动态库', () {},
        skip: '未找到动态库：设置 QJS_ULTRA_LIB 或放置 native/<平台>/<arch>/');
    return;
  }

  Future<QuickjsEngine> make() async {
    final engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
    // 模拟 host.httpGet：可变延迟（含 0ms——settle 与泵的强交错源）
    var n = 0;
    engine.registerAsyncFunction('host_httpGet', (args) async {
      final delay = args.isNotEmpty ? (args.first as num).toInt() : 0;
      if (delay > 0) await Future<void>.delayed(Duration(milliseconds: delay));
      n++;
      return <String, Object?>{'status': 200, 'n': n};
    });
    // 解析链：串行多次异步宿主调用（DaoBox resolveViaParsers 形状）
    engine.evaluate('''
globalThis.host = { httpGet: host_httpGet };
globalThis.resolveChain = async function (delays) {
  const out = [];
  for (let i = 0; i < delays.length; i++) {
    const r = await host.httpGet(delays[i]);
    out.push(r.status);
  }
  return out.join(',');
};
// 事件处理：短链（DaoBox onEvent 形状）
globalThis.onEvent = async function (i) {
  const r = await host.httpGet(i % 3);
  return { handled: i, n: r.n };
};
''');
    return engine;
  }

  // 超时探针：不落定 → 返回标记（断言失败），而非挂死测试进程
  Future<Object?> withProbe(Future<Object?> f) =>
      f.timeout(const Duration(seconds: 20), onTimeout: () async => '__TIMEOUT__');

  test('解析链 + 事件风暴交错（L2 形状）', () async {
    final engine = await make();
    addTearDown(engine.dispose);

    final chain = withProbe(engine
        .callAsync('resolveChain', const [
          [2, 0, 5, 1, 0, 8, 3, 0, 1, 2]
        ]));
    // 事件风暴：高频短 callAsync 交错（webview progress 已节流，仍有残留频率）
    final events = <Future<Object?>>[];
    for (var i = 0; i < 60; i++) {
      events.add(withProbe(engine.callAsync('onEvent', [i])));
      await Future<void>.delayed(const Duration(milliseconds: 2));
    }
    final results = await Future.wait([chain, ...events]);
    for (final r in results) {
      expect(r, isNot('__TIMEOUT__'), reason: 'L2 复现：并发 callAsync 不落定');
    }
    expect(results.first, '200,200,200,200,200,200,200,200,200,200');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('多前台泵并发等不同 promise（交错 settle 顺序）', () async {
    final engine = await make();
    addTearDown(engine.dispose);

    // 8 个并发 callAsync，各自的链长不同 → settle 与前台泵唤醒高度交错
    final futures = <Future<Object?>>[];
    for (var i = 1; i <= 8; i++) {
      futures.add(withProbe(engine.callAsync('resolveChain', [
        List<int>.generate(i, (j) => (j % 4) == 0 ? 0 : 1)
      ])));
    }
    final results = await Future.wait(futures);
    for (var i = 0; i < results.length; i++) {
      final expectLen = List.filled(i + 1, 200).join(',');
      expect(results[i], expectLen, reason: '链 #$i 结果错误或超时');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('事件风暴期间长链被中途唤醒后继续等新 in-flight', () async {
    final engine = await make();
    addTearDown(engine.dispose);

    // 长链跑一半时风暴进入；链的后续 httpGet 由风暴 settle 后的泵推进注册
    final chain = withProbe(engine
        .callAsync('resolveChain', const [
          [30, 30, 30, 30, 0, 0, 0, 0]
        ]));
    final sw = Stopwatch()..start();
    while (sw.elapsedMilliseconds < 40) {
      await withProbe(engine.callAsync('onEvent', [1]));
    }
    final r = await chain;
    expect(r, isNot('__TIMEOUT__'), reason: '风暴中启动的长链不落定');
    expect(r, '200,200,200,200,200,200,200,200');
  }, timeout: const Timeout(Duration(seconds: 60)));
}

String? _resolveLib() {
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty && File(env).existsSync()) return env;
  final root = Directory.current.path;
  final candidates = <String>[
    '$root/native/windows/x64/quickjs_bridge.dll',
    '$root/native/windows/x64/quickjs_bridge-windows-x64.dll',
    '$root/build/win-dbg/libquickjs_bridge.dll',
  ];
  for (final c in candidates) {
    if (File(c).existsSync()) return c;
  }
  return null;
}
