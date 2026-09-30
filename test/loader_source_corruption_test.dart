// 源码模式 module loader 随机损坏复现（docs/loader-source-mode-corruption-report.md）。
//
// 现象：loader 回调返回源码文本 → C 侧 JS_Eval 文本编译，合法源码以
// ~10-50% 概率报词法/引用级假语法错误；bytecode 模式（同回调返回
// compileModule 产物 → JS_ReadObject）零故障。对照数据见报告。
//
// 用法：QJS_ULTRA_LIB=<dll/so 路径> dart test test/loader_source_corruption_test.dart
// 期望：修复前 sourceMode 组挂（本测试红即 bug 在场）；修复后两组全绿。
// ⚠️ 同名模块 import 走 quickjs 模块缓存不会重新编译——每轮必须唯一名。
import 'dart:io';
import 'dart:typed_data';

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

const _candidates = [
  'native/windows/x64/quickjs_bridge.dll',
  'build/windows/x64/runner/Release/quickjs_bridge.dll',
];

String? _resolveLib() {
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty) return env;
  for (final p in _candidates) {
    if (File(p).existsSync()) return p;
  }
  return null;
}

/// 源码/字节码可配置的模块表 loader
class _MapLoader implements JsModuleLoader {
  final Map<String, Uint8List> bytecode;
  final Map<String, String> sources;
  _MapLoader({this.bytecode = const {}, this.sources = const {}});

  @override
  Uint8List? getModuleBytecode(String name) => bytecode[name];

  @override
  String? getModuleSource(String name) => sources[name];

  @override
  String normalizeName(String base, String name) => name;
}

void main() {
  final libPath = _resolveLib();
  if (libPath == null) {
    test('（跳过）缺 quickjs_bridge 库：设 QJS_ULTRA_LIB 或放默认路径', () {},
        skip: 'QJS_ULTRA_LIB 未设置且默认路径无库');
    return;
  }

  final small = File('test/fixtures/corrupt_probe_small.js');
  final large = File('test/fixtures/corrupt_probe_large.js');
  if (!small.existsSync() || !large.existsSync()) {
    test('（跳过）缺探针源 fixtures', () {}, skip: 'test/fixtures 探针源缺失');
    return;
  }
  final codes = [small.readAsStringSync(), large.readAsStringSync()];

  Future<int> runGroup(
      _MapLoader loader, QuickjsEngine engine, int rounds) async {
    var failCnt = 0;
    for (var i = 0; i < rounds; i++) {
      final name = 'mod_$i';
      try {
        await engine.evaluateAsync('import("$name")');
      } catch (e) {
        // ignore: avoid_print
        print('  $name: FAIL → ${e.toString().split('\n').first}');
        failCnt++;
      }
    }
    return failCnt;
  }

  test('[基线] bytecode loader：30 个全新模块 import 零故障', () async {
    final engine = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 4 * 1024 * 1024),
      libPath: libPath,
    );
    final bytecodes = <String, Uint8List>{};
    for (var i = 0; i < 30; i++) {
      bytecodes['mod_$i'] =
          engine.compileModule(codes[i % codes.length], fileName: 'b$i.js');
    }
    engine.setModuleLoader(_MapLoader(bytecode: bytecodes));
    try {
      final fails = await runGroup(_MapLoader(bytecode: bytecodes), engine, 30);
      expect(fails, 0, reason: 'bytecode 模式应恒稳定（对照基线）');
    } finally {
      engine.dispose();
    }
  }, timeout: const Timeout(Duration(minutes: 10)));

  test('[损坏哨兵] 源码 loader：30 个全新模块 import 应零故障', () async {
    final engine = QuickjsEngine.createWith(
      const JsEngineConfig(stackSize: 4 * 1024 * 1024),
      libPath: libPath,
    );
    final sources = <String, String>{};
    for (var i = 0; i < 30; i++) {
      sources['mod_$i'] = codes[i % codes.length];
    }
    engine.setModuleLoader(_MapLoader(sources: sources));
    try {
      final fails = await runGroup(_MapLoader(sources: sources), engine, 30);
      expect(fails, 0, reason:
          '源码 loader 随机损坏在场（2026-09-30 实测 15/30）——'
          '排查方向见 docs/loader-source-mode-corruption-report.md');
    } finally {
      engine.dispose();
    }
  }, timeout: const Timeout(Duration(minutes: 10)));
}
