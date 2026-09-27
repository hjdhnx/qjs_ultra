// drpy3 × qjs_ultra 冒烟验收：本仓引擎（quickjs + C 扩展核心 + 异步桥）
// 跑通 drpy3-fjs bundle 的六环节全链路。bundle **一字不改**（原为 fjs 构建，
// 经 installDrpy3Shim 提供 globalThis.fjs 桥 + timers + atob/btoa 兜底）。
//
// 与上游 drpy3 仓 hosts/fjs/integration_test/smoke_test.dart 同源（同一
// bundle、同一演示稿 bm1、同一 mock 数据形状）；上游为 flutter
// integration_test，此处为纯 dart test + 真实 so。
//
// fixture 同步要求：bundle 来自 DsPlayer assets/drpy3/（drpy3 仓
// hosts/fjs/tools/build.mjs 产物），bm1 来自 hosts/fjs/assets/test/。
// 运行：dart test test/drpy3_smoke_test.dart（需 QJS_ULTRA_LIB 或仓内约定路径有 so）
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data' show BytesBuilder;

import 'package:qjs_ultra/qjs_ultra.dart';
import 'package:test/test.dart';

import 'engine_test.dart' show resolveLibPath;

const detailHtml = '<!doctype html><html><head><title>详情页</title></head><body>\n'
    '<div class="m-details"><h1>测试影片</h1><p>类型：国产动漫</p><p>地区：中国大陆</p>'
    '<p>上映：2023-01-01</p><p>导演：测试导演</p><p>主演：张三 李四 王五</p></div>\n'
    '<div class="video-img"><img src="//img.example.com/pic.jpg"></div>\n'
    '<div class="desc">简介：这是一个用于冒烟测试的影片简介。</div>\n'
    '<div class="vt-txt">测试影片全名</div>\n</body></html>';

Future<HttpServer> _startMock() async {
  final server = await HttpServer.bind('127.0.0.1', 0);
  server.listen((req) async {
    final p = req.uri.path;
    final port = server.port;
    void reply(Object obj, {String type = 'application/json; charset=utf-8'}) {
      req.response.headers.set('Content-Type', type);
      req.response.write(jsonEncode(obj));
      req.response.close();
    }

    if (p.startsWith('/rider/list')) {
      final docs = [1, 2, 3].map((i) => {
            'title': '影片$i',
            'img': 'http://img.example.com/$i.jpg',
            'updateInfo': '更新至第$i集',
            'rightCorner': {'text': 'HD'},
            'playPartId': 'vid$i',
          }).toList();
      reply({'data': {'hitDocs': docs}});
    } else if (p.startsWith('/msite/search')) {
      final q = req.uri.queryParameters['q'];
      reply({'data': {'contents': [
        {'type': 'media', 'data': [{
          'desc': ['2023', '动漫'],
          'source': 'imgo',
          'img': 'http://img.example.com/s.jpg',
          'rpt': 'idx=50&other=1',
          'title': '<B>$q</B>',
          'url': 'http://127.0.0.1:$port/b/999/vid9.html',
        }]},
      ]}});
    } else if (p.startsWith('/episode/list')) {
      final eps = [1, 2].map((i) => {
            'url': '/b/999/vid$i.html',
            't4': '第$i集',
            't2': '${24 * i}分钟',
            'img': 'http://img.example.com/e.jpg',
            'isIntact': '1',
          }).toList();
      reply({'data': {'list': eps, 'total': 2, 'total_page': 1, 'series': []}});
    } else if (p.endsWith('.html')) {
      req.response.headers.set('Content-Type', 'text/html; charset=utf-8');
      req.response.write(detailHtml);
      await req.response.close();
    } else {
      reply(const {});
    }
  });
  return server;
}

/// bridge 处理器（对齐 DsPlayer Drpy3IoHandlers 的 req 契约）：
/// 返回 {content, headers}，出错不抛（drpy2/3 契约）。
Future<Map<String, dynamic>> _handleReq(
    String url, Map<String, dynamic> options) async {
  final client = HttpClient()..connectionTimeout = const Duration(seconds: 10);
  try {
    final method = (options['method'] ?? 'GET').toString().toUpperCase();
    final headers = <String, String>{};
    for (final e in ((options['headers'] ?? <String, dynamic>{}) as Map).entries) {
      headers[e.key.toString()] = e.value.toString();
    }
    var uri = Uri.parse(url);
    var body = options['body']?.toString();
    final data = options['data'];
    if (data is Map && data.isNotEmpty && method == 'GET') {
      final q = Map<String, String>.from(uri.queryParameters);
      data.forEach((k, v) => q[k.toString()] = v.toString());
      uri = uri.replace(queryParameters: q);
    }
    final req = await client.openUrl(method, uri);
    headers.forEach(req.headers.set);
    if (body != null && method != 'GET' && method != 'HEAD') {
      req.add(utf8.encode(body));
    }
    final timeoutMs = (options['timeout'] as num?)?.toInt() ?? 8000;
    final res = await req.close().timeout(Duration(milliseconds: timeoutMs));
    final builder = BytesBuilder(copy: false);
    await for (final chunk in res) {
      builder.add(chunk);
    }
    final bytes = builder.takeBytes();
    final outHeaders = <String, dynamic>{'status': 'HTTP/1.1 ${res.statusCode}'};
    res.headers.forEach((name, values) => outHeaders[name] = values.join(', '));
    final buffer = (options['buffer'] as num?)?.toInt() ?? 0;
    Object content;
    if (buffer == 1) {
      content = bytes;
    } else if (buffer == 2) {
      content = base64Encode(bytes);
    } else {
      content = utf8.decode(bytes, allowMalformed: true);
    }
    return {'content': content, 'headers': outHeaders};
  } catch (e) {
    return {'content': '', 'headers': {'error': e.toString()}};
  } finally {
    client.close(force: true);
  }
}

Future<Map<String, dynamic>> _call(
    QuickjsEngine engine, String fn, List<dynamic> args) async {
  final raw = await engine.callAsync('__$fn', [
    for (final a in args)
      a is String || a is num || a is bool || a == null ? a : jsonEncode(a),
  ]);
  final decoded = jsonDecode(raw as String);
  if (decoded is Map && decoded['__drpy3_error'] != null) {
    throw StateError('$fn: ${jsonEncode(decoded['__drpy3_error'])}');
  }
  return (decoded ?? <String, dynamic>{}) as Map<String, dynamic>;
}

void main() {
  final libPath = resolveLibPath();
  final bundleFile = File('test/fixtures/drpy3-fjs.bundle.js');
  final sourceFile = File('test/fixtures/bm1.source.js');
  if (libPath == null || !bundleFile.existsSync() || !sourceFile.existsSync()) {
    test('（跳过）drpy3 冒烟：缺 so 或 fixtures', () {}, skip: '需要 QJS_ULTRA_LIB + test/fixtures/*');
    return;
  }

  test('百忙无果1 六环节在 qjs_ultra 引擎内跑通（bundle 零改包）', () async {
    final mock = await _startMock();
    final b = 'http://127.0.0.1:${mock.port}';

    final engine = QuickjsEngine.createWith(
      // 栈 4MB（≤ 宿主线程栈 3/4 的护栏契约）；wasm 源形态内存给足 512MB
      const JsEngineConfig(stackSize: 4 * 1024 * 1024, memoryLimit: 512 * 1024 * 1024),
      libPath: libPath,
    );
    try {
      var code = sourceFile.readAsStringSync();
      for (final h in [
        'https://pianku.api.%6d%67%74%76.com',
        'https://mobileso.bz.%6d%67%74%76.com',
        'https://pcweb.api.mgtv.com',
        'https://www.mgtv.com',
      ]) {
        code = code.replaceAll(h, b);
      }

      // ① shim（fjs 桥 + timers + atob/btoa 兜底）
      var moduleSeq = 0;
      engine.installDrpy3Shim((message) async {
        final msg = message as Map;
        switch (msg['action']) {
          case 'req':
            return _handleReq(
                msg['url'] as String,
                (msg['options'] as Map?)?.cast<String, dynamic>() ?? const {});
          case 'loadAsset':
            return '';
          case 'getProxy':
            return '';
          case 'evalModule':
            // 对齐 DsPlayer Drpy3Host：注册源模块并回传名字，胶水 import(name)
            final name = 'drpy3_src_${moduleSeq++}';
            engine.registerModule(name, msg['code'] as String);
            return name;
          default:
            return null;
        }
      });

      // ② 装载 bundle（模块名 drpy3，六环节导出挂 __ 前缀全局）
      await engine.loadModuleToGlobals('drpy3', bundleFile.readAsStringSync());

      // ③ 能力表：五件套全 host + 原生 WebAssembly（wasm3，fjs 所无）
      final caps = await _call(engine, 'drpy3Capabilities', []);
      expect(caps['req'], 'host');
      expect(caps['pdfh'], 'host');
      expect(caps['pdfl'], 'host');
      expect(caps['wasm'], 'native', reason: 'qjs_ultra 有原生 wasm3');

      // ④ 装载演示源 + init
      await _call(engine, 'drpy3Load', [code, '_bm1_qjs']);
      await _call(engine, 'drpy3Call', ['_bm1_qjs', 'init', ['']]);

      // ⑤ home：静态分类 + gzip filter 解压
      final home = await _call(engine, 'drpy3Call', ['_bm1_qjs', 'home', ['']]);
      expect((home['class'] as List).length, 7, reason: 'home：静态分类 7 个');
      expect(((home['filters'] as Map?) ?? const {}).isNotEmpty, isTrue,
          reason: 'home：gzip filter 解压');

      // ⑥ category：json: 一级解析
      final cate = await _call(engine, 'drpy3Call',
          ['_bm1_qjs', 'category', ['3', 1, false, {}]]);
      final cateList = (cate['list'] as List).cast<Map>();
      expect(cateList.length, 3, reason: 'category：json:一级 3 条');
      expect(cateList[0]['vod_id'], '3\$vid1', reason: 'category：分类\$id 前缀');
      expect(cateList[0]['vod_remarks'], '更新至第1集');

      // ⑦ search：imgo 过滤 + <B> 清洗
      final search = await _call(engine, 'drpy3Call',
          ['_bm1_qjs', 'search', ['斗罗大陆', false, 1]]);
      final searchList = (search['list'] as List).cast<Map>();
      expect(searchList.length, 1, reason: 'search：imgo 过滤后 1 条');
      expect(searchList[0]['vod_name'], '斗罗大陆', reason: 'search：<B> 清洗');
      expect(searchList[0]['vod_id'], '50\$vid9');

      // ⑧ detail：cheerio pdfh/pd（bundle 内实现）
      final detail =
          await _call(engine, 'drpy3Call', ['_bm1_qjs', 'detail', [searchList[0]['vod_id']]]);
      final vod = (detail['list'] as List).cast<Map>()[0];
      expect(vod['vod_name'], '测试影片全名', reason: 'detail：pdfh .vt-txt');
      expect(vod['vod_pic'], 'http://img.example.com/pic.jpg', reason: 'detail：pd 协议相对补全');
      final eps = (vod['vod_play_url'] as String).split('#');
      expect(eps.length, 2, reason: 'detail：2 集列表');

      // ⑨ play：url 透传
      final firstPlay = eps[0].split('\$').sublist(1).join('\$');
      final play = await _call(engine, 'drpy3Call',
          ['_bm1_qjs', 'play', [vod['vod_play_from'], firstPlay, const []]]);
      expect(play['url'], firstPlay, reason: 'play：url 透传');
      expect(play['parse'], 1);
      expect(play['jx'], 0);

      // ⑩ store 快照通道
      await _call(engine, 'drpy3StoreImport',
          [jsonEncode({'_bm1_qjs': {'visit': 'n1'}})]);
      final dumpRaw = await engine.callAsync('__drpy3StoreExport', const []);
      final dump = jsonDecode(dumpRaw as String) as Map;
      expect(((dump['_bm1_qjs'] as Map)['visit']), 'n1');

      // 后台无错误积压
      expect(engine.drainBackgroundErrors(), isEmpty,
          reason: '后台 job/timer 错误应为空');
      expect(engine.drainUnhandledRejections().errors, isEmpty,
          reason: '未处理 rejection 应为空');
    } finally {
      engine.dispose();
      await mock.close();
    }
  }, timeout: const Timeout(Duration(minutes: 5)));
}
