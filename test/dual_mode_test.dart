// 双模式共存交叉验证：dr2 式（同步宿主函数，qjs_worker/tvbox_native 同款
// 用法）与 dr3 式（异步桥，registerAsyncFunction + evaluateAsync）在同一个
// so 上并发使用——模拟 DsPlayer 真实拓扑（worker isolate 跑 dr2、主
// isolate 跑 dr3），确认互不干扰。
import 'dart:async';
import 'dart:isolate';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

import 'engine_test.dart' show resolveLibPath;

/// worker isolate 入口（dr2 式同步引擎；不得捕获主 isolate 对象，
/// 只经消息参数收发——对齐 qjs_worker 的消息驱动模式）
void _workerEntry(_WorkerBootstrap boot) {
  final eng = QuickjsEngine.createWith(
    const JsEngineConfig(),
    libPath: boot.libPath,
  );
  eng.registerFunction('dr2Sync', (args) {
    final a = args[0] as int;
    return a * 3;
  });
  // 同步引擎反复求值（模拟 dr2 源的同步 req 流量）
  for (var i = 0; i < 5; i++) {
    boot.port.send('${eng.evaluate('dr2Sync($i)')}');
  }
  eng.dispose();
  boot.port.send('__done__');
}

class _WorkerBootstrap {
  _WorkerBootstrap(this.libPath, this.port);
  final String libPath;
  final SendPort port;
}

void main() {
  final libPath = resolveLibPath();
  if (libPath == null) {
    test('（跳过）缺 so', () {}, skip: '需要 QJS_ULTRA_LIB');
    return;
  }

  test('同 isolate：dr2 式同步引擎与 dr3 式异步引擎交错互不干扰', () async {
    // dr2 式：同步宿主函数 + 同步求值（qjs_worker 用法）
    final dr2 = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 1024 * 1024, memoryLimit: 64 * 1024 * 1024),
      libPath: libPath,
    );
    // dr3 式：异步桥（Drpy3Host 用法）
    final dr3 = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 4 * 1024 * 1024, memoryLimit: 512 * 1024 * 1024),
      libPath: libPath,
    );
    try {
      dr2.registerFunction('dr2Sync', (args) {
        final a = args[0] as int;
        return a * 2;
      });
      var dr3Calls = 0;
      dr3.registerAsyncFunction('dr3Async', (args) async {
        dr3Calls++;
        await Future<void>.delayed(const Duration(milliseconds: 20));
        return {'call': dr3Calls};
      });
      // dr3 引擎内的同步宿主函数（同实例双模式混用：dr2 语义 + dr3 语义共存）
      dr3.registerFunction('dr3SyncViaHost', (args) => 10);

      // 交错：dr3 先发起（挂起等 Dart 异步），期间 dr2 同步求值多次
      final dr3Future = dr3.evaluateAsync(
        '(async () => { const r = await dr3Async({action:"req"});'
        ' return r.call + dr3SyncViaHost(); })()',
      );
      expect(dr2.evaluate('dr2Sync(21)'), 42,
          reason: 'dr3 异步挂起期间 dr2 同步求值不受阻');
      expect(dr2.evaluate('[1,2,3].map(x => dr2Sync(x))'), [2, 4, 6]);

      final dr3Result = await dr3Future;
      expect(dr3Result, 11, reason: 'dr3 异步桥 settle 后正常推进（1 + 10）');
      expect(dr3Calls, 1);

      // 双引擎继续正常，dr3 后台无污染
      expect(dr2.evaluate('dr2Sync(-7)'), -14);
      expect(dr3.drainBackgroundErrors(), isEmpty);
    } finally {
      dr2.dispose();
      dr3.dispose();
    }
  });

  test('跨 isolate：dr2 式引擎在独立 isolate（真实 qjs_worker 拓扑）与主 isolate dr3 并发',
      () async {
    final workerResults = ReceivePort();
    final done = Completer<List<String>>();
    final results = <String>[];
    late final StreamSubscription<dynamic> sub;
    sub = workerResults.listen((m) {
      results.add('$m');
      if (m == '__done__' && !done.isCompleted) {
        sub.cancel();
        workerResults.close();
        done.complete(results);
      }
    });

    await Isolate.spawn(_workerEntry, _WorkerBootstrap(libPath, workerResults.sendPort));

    // 并发：主 isolate 的 dr3 异步桥挂起等 Dart 定时，worker isolate 同步跑
    final dr3 = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 4 * 1024 * 1024, memoryLimit: 512 * 1024 * 1024),
      libPath: libPath,
    );
    try {
      dr3.registerAsyncFunction('dr3Async', (args) async {
        await Future<void>.delayed(const Duration(milliseconds: 50));
        return 'dr3-ok';
      });
      final dr3Future = dr3.evaluateAsync('(async () => await dr3Async())()');

      expect(await done.future.timeout(const Duration(seconds: 10)),
          ['0', '3', '6', '9', '12', '__done__'],
          reason: 'worker isolate 同步引擎（dr2 模式）全链路正常');

      expect(await dr3Future.timeout(const Duration(seconds: 5)), 'dr3-ok',
          reason: '主 isolate 异步桥（dr3 模式）正常 settle');
      expect(dr3.drainBackgroundErrors(), isEmpty);
    } finally {
      dr3.dispose();
    }
  });
}
