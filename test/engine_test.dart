// ignore_for_file: cascade_invocations
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

/// qjs_ultra Dart 绑定的真实引擎测试。
///
/// **为什么要用真实 so**：绑定层的错误（函数签名写错、结构体布局不符、
/// 内存管理漏 free）在被调用的那一刻才会暴露成崩溃或错误结果，mock 测不出来。
///
/// **库来源**：优先环境变量 `QJS_ULTRA_LIB`（CI 或本地指定任意架构产物）；
/// 未设时回落 `native/<平台>/<arch>/`（本仓放置测试用二进制的约定路径）。
/// 找不到库则**整个文件跳过**（`skip`）—— 不让缺二进制阻塞 lint/单测，
/// 但真跑时必须有。
void main() {
  final libPath = _resolveLib();
  if (libPath == null) {
    test('（跳过）未找到 quickjs_bridge 动态库', () {
      // 显式占位，让「0 tests run」不出现（更易在 CI 日志里发现被跳过）
    }, skip: '未找到动态库：设置 QJS_ULTRA_LIB 或放置 native/<平台>/<arch>/');
    return;
  }

  // 预检：per-context 补丁（qjs_clear_callbacks）是当前 ABI 的一部分。
  // 缺该符号 = 改造前的旧 so（如仓库里遗留的 windows dll），绑定会因
  // 符号解析失败而完全不可用 —— 此时跳过并给出明确原因，而不是让
  // 12 个用例齐刷刷报 "Failed to lookup symbol"（那种噪音会掩盖真问题）。
  final outdated = _missingSymbol(libPath, 'qjs_clear_callbacks');
  if (outdated) {
    test('（跳过）动态库缺少当前 ABI 符号 qjs_clear_callbacks', () {
      // 占位，让跳过在日志里可见
    }, skip: '库为 per-context 改造前产物（$libPath）：'
        '请用本仓 CI Release 的 so，或用 QJS_ULTRA_LIB 指定新构建产物');
    return;
  }

  late QuickjsEngine engine;

  setUp(() {
    engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
  });

  tearDown(() {
    engine.dispose();
  });

  group('引擎基础（真实 so）', () {
    test('实现名与初始状态', () {
      expect(engine.implementationName, isNotEmpty);
      expect(engine.isDisposed, isFalse);
    });

    test('求值基础表达式', () {
      expect(engine.evaluate('1 + 2'), 3);
      expect(engine.evaluate('"a" + "b"'), 'ab');
      expect(engine.evaluate('true'), isTrue);
      expect(engine.evaluate('null'), isNull);
      expect(engine.evaluate('undefined'), isNull);
    });

    test('浮点（验证 JSValue 的 double 读取路径）', () {
      expect(engine.evaluate('3.14'), closeTo(3.14, 1e-9));
      expect(engine.evaluate('0.1 + 0.2'), closeTo(0.3, 1e-9));
    });

    test('负整数不符号回绕（JSValue u64 是二补码容器）', () {
      // -42 曾被读成 4294967254（漏 int32 有符号转换）——源里返回负数很常见
      // （-1 哨兵、偏移量、坐标），静默变巨大正数极难排查。
      expect(engine.evaluate('-42'), -42);
      expect(engine.evaluate('-1'), -1);
      expect(engine.evaluate('0 - 2147483648'), -2147483648);
      expect(engine.evaluate('2147483647'), 2147483647);
      expect(engine.evaluate('({n: -5})'), {'n': -5});
      expect(engine.evaluate('[-1, -2, 3]'), [-1, -2, 3]);
    });

    test('对象与数组（走 JSON 编组路径）', () {
      final obj = engine.evaluate('({a: 1, b: "x"})');
      expect(obj, isA<Map>());
      expect((obj as Map)['a'], 1);
      expect(obj['b'], 'x');

      final arr = engine.evaluate('[1, 2, 3]');
      expect(arr, isA<List>());
      expect(arr, [1, 2, 3]);
    });

    test('宿主函数注入（registerFunction → JS 调用 → 回 Dart）', () {
      engine.registerFunction('hostAdd', (args) {
        final a = args[0] as int;
        final b = args[1] as int;
        return a + b;
      });
      expect(engine.evaluate('hostAdd(20, 22)'), 42);
    });

    test('宿主函数收到的参数经 JSON 契约编组', () {
      Object? seen;
      engine.registerFunction('capture', (args) {
        seen = args;
        return args.length;
      });
      engine.evaluate('capture(1, "two", [3], {four: 4}, null, true)');
      expect(seen, isA<List>());
      final list = seen! as List;
      expect(list[0], 1);
      expect(list[1], 'two');
      expect(list[2], [3]);
      expect(list[3], {'four': 4});
      expect(list[4], isNull);
      expect(list[5], isTrue);
    });

    test('全局属性读写', () {
      engine.setGlobalProperty('injected', {'k': 'v'});
      expect(engine.getGlobalProperty('injected'), {'k': 'v'});
      expect(engine.evaluate('injected.k'), 'v');
    });

    test('JSON 解析与序列化', () {
      final parsed = engine.parseJson('{"n":1,"s":"x"}');
      expect(parsed, isA<Map>());
      expect(engine.stringify({'a': [1, 2]}), contains('"a"'));
    });

    test('异常不导致进程崩溃，且可读回', () {
      // 抛异常时求值应返回 null 或抛 Dart 异常，两种都可接受；
      // **不可接受的是崩溃或静默返回错误值**
      Object? result;
      try {
        result = engine.evaluate('throw new Error("boom")');
      } catch (_) {
        result = 'threw';
      }
      expect(result, anyOf(isNull, 'threw'));
      // 引擎仍可用（异常不应破坏上下文）
      expect(engine.evaluate('1 + 1'), 2);
    });

    test('内存用量可读（指针编组正确）', () {
      final usage = engine.getMemoryUsage();
      expect(usage, isNotNull);
      expect(usage!.memoryUsedSize, greaterThanOrEqualTo(0));
    });

    test('dispose 幂等且状态正确', () {
      final e2 = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
      e2.dispose();
      expect(e2.isDisposed, isTrue);
      e2.dispose(); // 重复 dispose 不应抛
    });
  });

  group('模块求值', () {
    // evaluateModule 返回的是**模块 evaluation promise 的 settle 值**
    // （内核为支持顶层 await，模块求值走 promise 路径，settle 值为
    // undefined → Dart null）。爬虫源的正确用法是靠副作用注册全局
    // （见 DsPlayer 的 js_spider.js：evaluateModule(src) 忽略返回值）。
    // 注意：不要断言「返回模块命名空间对象」——导出的 namespace 无法
    // 跨 FFI 枚举（JS_GPN_ENUM_ONLY 对导出为空），旧版返回空 Map 只是
    // promise 被误当普通对象转换的侥幸结果。
    test('执行模块体（副作用注册全局）', () {
      final out = engine.evaluateModule(
        'globalThis.moduleRan = true;',
        fileName: 'side_effect.js',
      );
      expect(engine.getGlobalProperty('moduleRan'), isTrue,
          reason: '模块体应已执行');
      expect(out, isNull, reason: '模块 promise settle 值为 undefined');
    });

    test('模块执行完成且异常会浮出', () {
      engine.evaluateModule(
        'export const answer = 6 * 7;',
        fileName: 'export_const.js',
      );
      // 模块体执行完毕（含导出声明），无异常抛出即为成功；
      // 结果落在全局的通路单独验证：
      final out = engine.evaluateModule(
        'globalThis.exported = 42;',
        fileName: 'export_to_global.js',
      );
      expect(out, isNull);
      expect(engine.getGlobalProperty('exported'), 42,
          reason: '模块体副作用应生效');
    });

  test('模块内可调用宿主函数（爬虫源核心路径）', () {
    engine.registerFunction('hostEcho', (args) => {'echo': args.first});
    engine.evaluateModule(
      'globalThis.got = hostEcho("hi");',
      fileName: 'host_call.js',
    );
    expect(engine.getGlobalProperty('got'), {'echo': 'hi'});
  });
});

group('结构化错误（fjs JsError 对齐）', () {
  late QuickjsEngine engine;
  setUp(() {
    engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: libPath);
  });
  tearDown(() => engine.dispose());

  test('TypeError 按名归类', () {
    try {
      engine.evaluate('null.foo');
      fail('应抛 JsEvalException');
    } on JsEvalException catch (e) {
      expect(e.name, 'TypeError');
      expect(e.errorKind, JsErrorKind.type);
      expect(e.message, contains('TypeError'));
    }
  });

  test('语法错误带行号列号', () {
    try {
      engine.evaluate('function {', fileName: 'broken.js');
      fail('应抛 JsEvalException');
    } on JsEvalException catch (e) {
      expect(e.errorKind, JsErrorKind.syntax);
      expect(e.line, isNotNull, reason: 'stack: ${e.stack}');
      expect(e.column, isNotNull);
    }
  });

  test('栈溢出归类 stackOverflow 且引擎可复用', () {
    try {
      engine.evaluate('(function f(){ f() })()');
      fail('应抛 JsEvalException');
    } on JsEvalException catch (e) {
      expect(e.errorKind, JsErrorKind.stackOverflow);
    }
    expect(engine.evaluate('1 + 1'), 2, reason: '溢出后上下文应仍可用');
  });
});

group('墙钟超时（interrupt handler）', () {
  test('死循环被超时打断，错误归类 timeout，引擎可复用', () {
    final engine = QuickjsEngine.createWith(
      const JsEngineConfig(timeoutMs: 100),
      libPath: libPath,
    );
    try {
      final sw = Stopwatch()..start();
      try {
        engine.evaluate('while (true) {}');
        fail('应抛超时异常');
      } on JsEvalException catch (e) {
        expect(e.errorKind, JsErrorKind.timeout, reason: 'message: ${e.message}');
      }
      sw.stop();
      expect(sw.elapsedMilliseconds, lessThan(10000), reason: '应远早于无限等待');
      // 打断后 deadline 已清除，引擎必须可复用
      expect(engine.evaluate('40 + 2'), 42);
    } finally {
      engine.dispose();
    }
  });

  test('未配置超时的引擎不受影响', () {
    expect(engine.evaluate('7 * 6'), 42);
  });
});

group('值转换护栏（fjs ConversionState 对齐）', () {
  test('自引用对象不炸宿主，非循环字段保留', () {
    final out = engine.evaluate('var a = {ok: 1}; a.self = a; a') as Map;
    expect(out['ok'], 1);
    expect(out['self'], isNull, reason: '循环引用处降级 null');
  });

  test('深层嵌套对象不栈溢出', () {
    engine.evaluate(
      'var deep = 0; for (var i = 0; i < 200; i++) deep = {v: deep};',
    );
    // 128 层护栏截断后是 null，无论如何不应崩
    expect(() => engine.getGlobalProperty('deep'), returnsNormally);
  });
});

group('Date / BigInt 编组（fjs value 对齐）', () {
  test('JS Date → Dart DateTime', () {
    final d = engine.evaluate('new Date(1700000000000)');
    expect(d, isA<DateTime>());
    expect((d as DateTime).millisecondsSinceEpoch, 1700000000000);
  });

  test('Invalid Date 不炸', () {
    final d = engine.evaluate('new Date(NaN)');
    expect(d, isA<Map>(), reason: 'NaN 时间值降级为对象路径');
  });

  test('Dart DateTime → JS Date', () {
    engine.setGlobalProperty('day', DateTime.fromMillisecondsSinceEpoch(86400000));
    expect(engine.evaluate('Number(day)'), 86400000);
  });

  test('JS BigInt → Dart 十进制字符串（任意精度）', () {
    expect(
      engine.evaluate('123456789012345678901234567890n'),
      '123456789012345678901234567890',
    );
  });

  test('Dart 超安全整数 → JS BigInt（保精度不降 double）', () {
    engine.setGlobalProperty('big', 9223372036854775807);
    expect(engine.evaluate('typeof big'), 'bigint');
    expect(engine.evaluate('big.toString()'), '9223372036854775807');
  });
});

group('Promise settle（fjs async eval 同步泵版）', () {
  test('async IIFE 的 promise 被等待并取 settle 值', () {
    final v = engine.evaluate(
      '(async () => { await Promise.resolve(); return 7; })()',
    );
    expect(v, 7);
  });

  test('Promise 链与 all 组合', () {
    final v = engine.evaluate(
      'Promise.all([Promise.resolve(1), Promise.resolve(2)])'
      '.then(xs => xs[0] + xs[1])',
    );
    expect(v, 3);
  });

  test('顶层 await 模块执行完整', () {
    engine.evaluateModule(
      'const x = await Promise.resolve(41); globalThis.topAwait = x + 1;',
      fileName: 'top_await.mjs',
    );
    expect(engine.getGlobalProperty('topAwait'), 42);
  });

  test('rejected promise 抛结构化异常', () {
    try {
      engine.evaluate('Promise.reject(new TypeError("nope"))');
      fail('应抛 JsEvalException');
    } on JsEvalException catch (e) {
      expect(e.errorKind, JsErrorKind.type);
      expect(e.message, contains('nope'));
    }
  });
});

group('未处理 rejection 管道（fjs error sink 对齐）', () {
  test(' rejection 被收集且可排空', () {
    // 构造不被 settle 等待的 rejection：放进 pending promise，绕过
    // _resolveResult 的等待路径（其 reject 会被直接抛出）
    engine.evaluate(
      '(async () => { Promise.reject(new Error("bg-boom"));'
      ' await null; })()',
    );
    final drained = engine.drainUnhandledRejections();
    expect(drained.errors.join(' '), contains('bg-boom'));
    // 排空后再取应为空
    expect(engine.drainUnhandledRejections().errors, isEmpty);
  });
});

group('GC 阈值', () {
  test('配置 gcThreshold 不崩且引擎可用', () {
    final e2 = QuickjsEngine.createWith(
      const JsEngineConfig(gcThreshold: 1 << 20),
      libPath: libPath,
    );
    try {
      expect(e2.evaluate('1 + 1'), 2);
      e2.runGC();
    } finally {
      e2.dispose();
    }
  });
});
}

/// 库文件里是否**缺少**指定导出符号名。
///
/// 用原始字节搜索而非 nm/objdump：测试要跨平台跑（Windows 无 nm），
/// 且符号名在 PE/ELF 的字符串表里都是明文，搜索足够可靠。
bool _missingSymbol(String path, String symbol) {
  try {
    final bytes = File(path).readAsBytesSync();
    return !_containsBytes(bytes, symbol.codeUnits);
  } catch (_) {
    return true; // 读不了当缺失处理
  }
}

bool _containsBytes(List<int> haystack, List<int> needle) {
  if (needle.isEmpty || haystack.length < needle.length) return false;
  final first = needle[0];
  for (var i = 0; i <= haystack.length - needle.length; i++) {
    if (haystack[i] != first) continue;
    var ok = true;
    for (var j = 1; j < needle.length; j++) {
      if (haystack[i + j] != needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

/// 定位动态库：环境变量优先，其次仓内约定路径。
String? _resolveLib() {
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty && File(env).existsSync()) return env;

  final root = Directory.current.path;
  final candidates = <String>[
    if (Platform.isWindows) '$root/native/windows/x64/quickjs_bridge.dll',
    if (Platform.isLinux) '$root/native/linux/x64/libquickjs_bridge.so',
    if (Platform.isMacOS) '$root/native/macos/x64/libquickjs_bridge.dylib',
    // 本仓 CI 产物落点（workflow 下载后）
    '$root/out/libquickjs_bridge-${Platform.isWindows ? 'x86_64.dll' : 'x86_64.so'}',
  ];
  for (final c in candidates) {
    if (File(c).existsSync()) return c;
  }
  return null;
}
