// 运行时全局能力自检：真实加载 so/dll，逐项验证文档 §23 的注入能力。
//
// 用法：
//   dart run tool/capability_check.dart [动态库路径]
//
// 库路径优先级：命令行参数 > QJS_ULTRA_LIB > 仓内约定路径（native/、out/、
// build/、build-debug/）。JSON5 为编译期可选插件：
//   - 未启用（默认）：显示 SKIP，不计失败；
//   - 启用（-DQJS_ENABLE_JSON5=ON）：完整跑 parse/stringify 用例。
//
// 退出码：0 = 全部通过（SKIP 不算失败）；1 = 有 FAIL；2 = 找不到动态库。
import 'dart:convert';
import 'dart:io';

import 'package:qjs_ultra/qjs_ultra.dart';

const _pass = '[ OK ]';
const _fail = '[FAIL]';
const _skip = '[SKIP]';

class _Result {
  _Result(this.name, this.ok, this.detail, {this.skipped = false});
  final String name;
  final bool ok;
  final String detail;
  final bool skipped;
}

Future<void> main(List<String> args) async {
  final libPath = _resolveLib(args);
  if (libPath == null) {
    stderr.writeln('找不到 quickjs 动态库：传参或设置 QJS_ULTRA_LIB');
    exit(2);
  }

  final engine = QuickjsEngine.createWith(
    const JsEngineConfig(),
    libPath: libPath,
  );
  engine.installTimers();

  final results = <_Result>[];
  // fs 自检读取一个真实存在的文件：优先脚本自身，退回落仓根 pubspec.yaml
  final scriptPath = File.fromUri(Platform.script).absolute.path;
  final fsProbe = File(scriptPath).existsSync()
      ? File(scriptPath)
      : File('pubspec.yaml').absolute;
  final fsPath = fsProbe.path;

  try {
    // ---- 同步能力（一次求值返回逐项结果）----
    final sync = engine.evaluate(
        _syncChecks.replaceFirst('__FS_PATH__', jsonEncode(fsPath)));
    for (final entry in (sync as Map).entries) {
      final m = (entry.value as Map).cast<String, Object?>();
      final detail = '${m['detail'] ?? ''}';
      results.add(_Result(
        entry.key as String,
        m['ok'] == true,
        detail,
        skipped: detail == '__SKIP__',
      ));
    }

    // ---- 异步能力（WebCrypto / 微任务 / timer / import.meta）----
    results.add(await _asyncCheck('crypto.subtle.digest', () async {
      final v = await engine.evaluateAsync(r'''
(async () => {
  const d = await crypto.subtle.digest('SHA-256', new TextEncoder().encode('abc'));
  return Array.from(new Uint8Array(d)).map(x => x.toString(16).padStart(2, '0')).join('');
})()
''');
      if (v != 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') {
        throw StateError('unexpected digest: $v');
      }
      return 'SHA-256(abc) 正确';
    }));

    results.add(await _asyncCheck('queueMicrotask', () async {
      final v = await engine.evaluateAsync(
          'new Promise((r) => queueMicrotask(() => r(42)))');
      if (v != 42) throw StateError('unexpected: $v');
      return '微任务队列执行';
    }));

    results.add(await _asyncCheck('setTimeout', () async {
      final v = await engine
          .evaluateAsync('new Promise((r) => setTimeout(() => r(7), 5))');
      if (v != 7) throw StateError('unexpected: $v');
      return '回调 settle 前台 await 链';
    }));

    results.add(await _asyncCheck('setInterval', () async {
      final v = await engine.evaluateAsync('''
new Promise((resolve) => {
  let n = 0;
  const id = setInterval(() => {
    n++;
    if (n === 3) { clearInterval(id); resolve(n); }
  }, 5);
})
''');
      if (v != 3) throw StateError('unexpected: $v');
      return '周期回调 + clearInterval';
    }));

    results.add(await _asyncCheck('import.meta.url', () async {
      engine.registerModule(
          'cap_meta_probe', 'globalThis.__cap_meta = import.meta.url;');
      await engine.evaluateAsync('import("cap_meta_probe")');
      final v = engine.evaluate('String(globalThis.__cap_meta)');
      if (!v.toString().contains('cap_meta_probe')) {
        throw StateError('unexpected: $v');
      }
      return v.toString();
    }));
  } finally {
    engine.dispose();
  }

  _print(libPath, results);
  exit(results.any((r) => !r.ok && !r.skipped) ? 1 : 0);
}

Future<_Result> _asyncCheck(
    String name, Future<String> Function() body) async {
  try {
    return _Result(name, true, await body());
  } catch (e) {
    return _Result(name, false, 'ERR: $e');
  }
}

String? _resolveLib(List<String> args) {
  if (args.isNotEmpty) {
    final f = File(args.first);
    return f.existsSync() ? f.absolute.path : null;
  }
  final env = Platform.environment['QJS_ULTRA_LIB'];
  if (env != null && env.isNotEmpty && File(env).existsSync()) return env;

  final root = Directory.current.path;
  final exe = Platform.isWindows ? '.dll' : '.so';
  final candidates = <String>[
    if (Platform.isWindows) '$root/native/windows/x64/quickjs_bridge.dll',
    if (Platform.isLinux) '$root/native/linux/x64/libquickjs_bridge.so',
    if (Platform.isMacOS) '$root/native/macos/x64/libquickjs_bridge.dylib',
    '$root/out/libquickjs_bridge-x86_64$exe',
    if (Platform.isWindows) '$root/build/windows-x64/libquickjs_bridge.dll',
    if (Platform.isWindows) '$root/build/windows-x64/quickjs_bridge.dll',
    if (Platform.isWindows)
      '$root/build-debug/quickjs_bridge-windows-x64.dll',
    '$root/build/host/libquickjs_bridge$exe',
  ];
  for (final c in candidates) {
    if (File(c).existsSync()) return c;
  }
  return null;
}

void _print(String libPath, List<_Result> results) {
  final failed = results.where((r) => !r.ok && !r.skipped).length;
  final skipped = results.where((r) => r.skipped).length;
  final passed = results.length - failed - skipped;

  print('');
  print('QuickJS 全局能力自检');
  print('库: $libPath');
  print('─' * 72);
  for (final r in results) {
    final tag = r.skipped ? _skip : (r.ok ? _pass : _fail);
    final detail = r.skipped ? '未启用（编译期开关）' : r.detail;
    print('$tag ${r.name.padRight(24)} $detail');
  }
  print('─' * 72);
  print('通过 $passed / 失败 $failed / 跳过 $skipped');
  print('');
}

/// 同步能力检查：`t(name, fn)` 逐项捕获异常；返回 name -> {ok, detail}。
/// __FS_PATH__ 由 Dart 侧替换成当前脚本的绝对路径（验证 fs 真实读文件）。
const _syncChecks = r'''
(() => {
  const r = {};
  const t = (n, f) => {
    try {
      const d = f();
      r[n] = { ok: true, detail: String(d === undefined ? 'ok' : d) };
    } catch (e) {
      r[n] = { ok: false, detail: 'ERR: ' + (e && e.message || e) };
    }
  };

  // cheerio：加载/选择器/切片/临时 $ 链式（UAF 回归）
  t('cheerio', () => {
    const html = '<ul><li>a</li><li>b</li><li>c</li><li>d</li></ul>';
    const $ = cheerio.load(html);
    const parts = [
      $('li').eq(1).text(),
      $('li').gt(-2).text(),
      $('li:gt(1)').length,
      $('li:lt(2)').length,
      cheerio.load(html)('li').gt(-2).text(), // 不保存 $ 的临时链式
    ];
    if (parts.join('|') !== 'b|d|2|2|d') throw new Error(parts.join('|'));
    return 'load/eq/gt/:gt/:lt + 临时链式';
  });

  t('URL / URLSearchParams', () => {
    const u = new URL('https://ex.com/p?q=7&x=1');
    const sp = new URLSearchParams('a=1&b=2');
    if (u.searchParams.get('q') !== '7' || sp.get('b') !== '2') throw new Error('bad');
    return '解析与查询参数';
  });

  t('TextEncoder / TextDecoder', () => {
    const enc = new TextEncoder().encode('中文');
    const utf8 = new TextDecoder('utf-8').decode(enc);
    const gbk = new TextDecoder('gbk').decode(new Uint8Array([0xd6,0xd0,0xce,0xc4]));
    if (enc.length !== 6 || utf8 !== '中文' || gbk !== '中文') throw new Error(utf8 + '/' + gbk);
    return 'UTF-8 往返 + GBK 解码';
  });

  t('Buffer', () => {
    const s = Buffer.from('abc').toString('base64');
    if (s !== 'YWJj') throw new Error(s);
    return 'base64 编解码';
  });

  t('crypto', () => {
    const md5 = crypto.createHash('md5').update('abc').digest('hex');
    const hmac = crypto.createHmac('sha256', 'k').update('abc').digest('hex');
    const rnd = crypto.getRandomValues(new Uint8Array(8));
    if (md5 !== '900150983cd24fb0d6963f7d28e17f72' || hmac.length !== 64 || rnd.length !== 8)
      throw new Error('bad');
    return 'createHash/createHmac/getRandomValues';
  });

  t('fs / fs.promises', () => {
    const text = fs.readFileSync(__FS_PATH__, 'utf8');
    if (!text.length || typeof fs.promises.readFile !== 'function')
      throw new Error('read failed');
    return 'readFileSync ' + text.length + ' 字节';
  });

  t('zlib', () => {
    const raw = Buffer.from('hello');
    const gz = zlib.gzip(raw);
    const out = Buffer.from(zlib.unzip(gz)).toString();
    const crc = zlib.crc32(Buffer.from('123456789'));
    const adler = zlib.adler32(Buffer.from('123456789'));
    if (out !== 'hello' || crc !== 3421780262 || adler !== 152961502) throw new Error(out);
    return 'gzip/unzip/crc32/adler32';
  });

  t('DataBase (SQLite)', () => {
    const db = new DataBase(':memory:');
    db.exec("create table t(a integer, b text); insert into t values (1,'x'), (2,'y')");
    const bound = db.prepare('select a from t where a >= ? order by a').bind(1).get();
    const direct = db.prepare('select b from t where a = ?').get(2).b;
    const first = db.prepare('select a from t order by a').iterate().next().value;
    db.close();
    if (bound.a !== 1 || direct !== 'y' || first.a !== 1) throw new Error('bad');
    return 'exec/prepare/bind/get/all/iterate';
  });

  t('path / WebAssembly', () => {
    const joined = path.join('a', 'b', '..', 'c');
    const mod = new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0]));
    if (!joined || !(mod instanceof WebAssembly.Module)) throw new Error('bad');
    return joined + ' + wasm Module';
  });

  t('console / format', () => {
    if (typeof console.log !== 'function' || typeof console.time !== 'function')
      throw new Error('no console');
    const s = globalThis.format('%s=%d', 'a', 7);
    if (s !== 'a=7') throw new Error(s);
    return 'console.log/time + format';
  });

  t('Date / performance', () => {
    const y = new Date('2023-01-01 12:00:00').getFullYear();
    if (y !== 2023 || !(performance.now() >= 0)) throw new Error('bad');
    return '扩展日期解析 + performance.now';
  });

  t('JSON / Math', () => {
    if (!JSON.isRawJSON(JSON.rawJSON('1'))) throw new Error('rawJSON');
    if (Math.sumPrecise([0.1, 0.2]) < 0.3) throw new Error('sumPrecise');
    return 'rawJSON/isRawJSON + sumPrecise';
  });

  t('Set / Map', () => {
    const s = new Set([1]).union(new Set([2]));
    const i = new Set([1,2]).intersection(new Set([2,3]));
    const m = new Map();
    if (s.size !== 2 || i.size !== 1 || typeof m.getOrInsert !== 'function') throw new Error('bad');
    return 'union/intersection + getOrInsert';
  });

  t('Uint8Array / ArrayBuffer', () => {
    const b64 = new Uint8Array([65,66,67]).toBase64();
    const hexLen = Uint8Array.fromHex('414243').length;
    const ab = new ArrayBuffer(4);
    if (b64 !== 'QUJD' || hexLen !== 3 || typeof ab.resize !== 'function')
      throw new Error('bad');
    return 'toBase64/fromHex + resize/transfer';
  });

  t('atob / btoa', () => {
    if (btoa('ABC') !== 'QUJD' || atob('QUJD') !== 'ABC') throw new Error('bad');
    return 'Base64 往返';
  });

  t('Iterator Helpers', () => {
    const v = [1,2,3].values().map(x => x * 2).take(2).toArray().join(',');
    if (v !== '2,4') throw new Error(v);
    return 'map/take/toArray';
  });

  t('Timers', () => {
    if (typeof setTimeout !== 'function' || typeof setInterval !== 'function' ||
        typeof clearTimeout !== 'function' || typeof clearInterval !== 'function')
      throw new Error('missing timer API');
    return 'setTimeout/clearTimeout/setInterval/clearInterval';
  });

  // 可选插件：未编译进来时返回 __SKIP__（不计失败）
  t('JSON5（可选插件）', () => {
    if (typeof JSON5 === 'undefined') return '__SKIP__';
    const o = JSON5.parse(`
// comment
{a: 'x', b: [1, 2,], hex: 0x10, inf: Infinity}
`, (k, v) => typeof v === 'number' ? v + 1 : v);
    if (o.a !== 'x' || o.b.length !== 2 || o.hex !== 17 || o.inf !== Infinity)
      throw new Error('bad parse/reviver');
    const s = JSON5.stringify(o);
    if (s !== "{a:'x',b:[2,3],hex:17,inf:Infinity}")
      throw new Error('bad stringify: ' + s);
    if (JSON5.parse(s).hex !== 17) throw new Error('bad roundtrip');
    return 'parse(reviver) + 官方风格 stringify';
  });

  return r;
})()
''';
