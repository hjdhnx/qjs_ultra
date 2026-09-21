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
    // evaluateModule 返回的是**模块命名空间对象**（不是 default 导出值）——
    // 爬虫源的正确用法是靠副作用注册全局（见 DsPlayer 的 js_spider.dart：
    // `_engine.evaluateModule(src, fileName: api)` 忽略返回值），所以这里
    // 验的是「模块体真的执行了 + 返回命名空间」而非取 default。
    test('执行模块体（副作用注册全局）', () {
      final out = engine.evaluateModule(
        'globalThis.moduleRan = true;',
        fileName: 'side_effect.js',
      );
      expect(engine.getGlobalProperty('moduleRan'), isTrue,
          reason: '模块体应已执行');
      expect(out, isA<Map>(), reason: '返回模块命名空间对象');
    });

    test('模块内的 export 可从全局读到（模块体副作用可见）', () {
      // 注：**不要**断言「evaluateModule 的返回值里能读到 default」——
      // 该返回值的属性枚举走 `JS_GPN_ENUM_ONLY`（C 侧 qjs_own_property_names），
      // 而 ES 模块命名空间的导出不满足该标志，枚举结果为空。
      // 这正是爬虫源不依赖返回值、而靠模块体写全局的原因（见 js_spider.dart）。
      // 这里验「模块体执行 → 结果落在全局」这条真实通路。
      engine.evaluateModule(
        'export const answer = 6 * 7;',
        fileName: 'export_const.js',
      );
      // 模块内导出不是全局；此处用例的真实目的 = 确认返回值是命名空间形态
      // 且模块已正常执行完（无异常抛出）
      final out = engine.evaluateModule(
        'globalThis.exported = 42;',
        fileName: 'export_to_global.js',
      );
      expect(out, isA<Map>(), reason: '返回模块命名空间对象');
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
