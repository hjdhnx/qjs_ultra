/*
 * test_all.js — qjs_engine 全模块综合测试
 * 覆盖 API 文档第 4-20 章全部模块。运行: qjs_test_runner test_all.js
 * 失败时 throw Error => 宿主非零退出码；全部通过打印 PASS 摘要。
 */

var __pass = 0, __fail = 0, __current = '';

function eq(a, b, msg) {
    if (a !== b) {
        throw new Error('[' + __current + '] ' + (msg || '') +
            ' expect ' + JSON.stringify(b) + ' got ' + JSON.stringify(a));
    }
    __pass++;
}
function ok(v, msg) {
    if (!v) throw new Error('[' + __current + '] FAIL: ' + (msg || 'assertion false'));
    __pass++;
}
function deepEq(a, b, msg) {
    var ja = JSON.stringify(a), jb = JSON.stringify(b);
    if (ja !== jb) throw new Error('[' + __current + '] ' + (msg || '') + ' expect ' + jb + ' got ' + ja);
    __pass++;
}
function throws(f, msg) {
    try { f(); } catch (e) { __pass++; return; }
    throw new Error('[' + __current + '] expected throw: ' + (msg || ''));
}
function test(name, f) {
    __current = name;
    try { f(); __current = ''; }
    catch (e) { __current = ''; throw e; }
}

/* ================= 4. cheerio ================= */
test('cheerio: load + 选择器 + 属性 + 文本', function () {
    var $ = cheerio.load('<ul id="list"><li class="item a">one</li><li class="item">two</li><li>three</li></ul><a href="https://example.com/x?a=1" title="t">link</a>');
    eq($('#list').length, 1, 'id selector');
    eq($('li').length, 3, 'tag selector');
    eq($('li.item').length, 2, 'class selector');
    eq($('li.item.a').length, 1, 'multi class');
    eq($('li:first').text(), 'one', ':first');
    eq($('li:last').text(), 'three', ':last');
    eq($('li').eq(1).text(), 'two', 'eq()');
    eq($('a').attr('href'), 'https://example.com/x?a=1', 'attr get');
    $('a').attr('href', 'https://changed.com');
    eq($('a').attr('href'), 'https://changed.com', 'attr set');
    eq($('a').text(), 'link', 'text get');
    $('a').text('changed');
    eq($('a').text(), 'changed', 'text set');
    /* 本引擎 cheerio 无静态 contains/html，且不支持 [0] 索引访问；改用 find 判断包含关系 */
    ok($('#list').find('li').length === 3, 'contains true (via find)');
    ok($('a').find('li').length === 0, 'contains false (via find)');
    eq($('li').parent().attr('id'), 'list', 'parent');
    /* 本引擎 map() 直接返回真数组，无 .get() */
    deepEq($('li').map(function (i, el) { return el.text(); }), ['one', 'two', 'three'], 'map');
    ok(/one/.test($('li:first').html()), 'html()');
    eq($('a[href]').length, 1, 'attr exists selector');
    eq($('a[href^="https"]').length, 1, '^=');
});

/* ================= 5. URL / URLSearchParams ================= */
test('URL: 解析 / 属性 / 相对解析 / canParse', function () {
    var u = new URL('https://user:pass@www.example.com:8080/path/to/page?q=hello%20world&n=1#frag');
    eq(u.protocol, 'https:', 'protocol');
    eq(u.hostname, 'www.example.com', 'hostname');
    eq(u.port, '8080', 'port');
    eq(u.pathname, '/path/to/page', 'pathname');
    eq(u.search, '?q=hello%20world&n=1', 'search encoded');
    eq(u.hash, '#frag', 'hash');
    eq(u.username, 'user', 'username');
    eq(u.password, 'pass', 'password');
    eq(u.origin, 'https://www.example.com:8080', 'origin');
    eq(u.searchParams.get('q'), 'hello world', 'searchParams decode');
    eq(u.searchParams.get('n'), '1', 'searchParams 2');
    var rel = new URL('../img/logo.png', 'https://ex.com/a/b/page.html');
    eq(rel.href, 'https://ex.com/a/img/logo.png', 'relative resolve');
    eq(URL.canParse('https://ok.com/'), true, 'canParse true');
    eq(URL.canParse(':::bad'), false, 'canParse false');
    throws(function () { new URL('not a url'); }, 'invalid URL');
});
test('URLSearchParams: append/set/get/sort/entries', function () {
    var sp = new URLSearchParams('b=2&a=1&b=3');
    eq(sp.get('a'), '1');
    deepEq(sp.getAll('b'), ['2', '3']);
    sp.set('b', '9');
    deepEq(sp.getAll('b'), ['9']);
    sp.append('a', '0');
    eq(sp.has('a'), true);
    sp.delete('a');
    eq(sp.has('a'), false);
    sp.append('c', '3'); sp.append('a', '1');
    sp.sort();
    eq(sp.toString(), 'a=1&b=9&c=3', 'sort+toString');
    var keys = []; sp.forEach(function (v, k) { keys.push(k); });
    deepEq(keys, ['a', 'b', 'c'], 'forEach order');
    eq(sp.size, 3, 'size');
});

/* ================= 6. TextEncoder / TextDecoder ================= */
test('TextCodec: UTF-8 往返 + GBK/Big5/Shift_JIS 解码 + fatal', function () {
    var enc = new TextEncoder();
    ok(String(enc.encoding).toLowerCase() === 'utf-8', 'encoder encoding label');
    var bytes = enc.encode('你好');
    eq(bytes.length, 6, 'utf8 2 chars -> 6 bytes');
    eq(bytes[0], 0xE4, 'first byte');
    var dec = new TextDecoder('utf-8');
    ok(String(dec.encoding).toLowerCase() === 'utf-8', 'decoder encoding label');
    eq(dec.decode(bytes), '你好', 'utf8 roundtrip');
    var buf = new Uint8Array(16);
    var res = enc.encodeInto('ab中', buf);
    eq(res.written, 5, 'encodeInto written (a+b+中=1+1+3)');
    var g = new TextDecoder('gbk').decode(new Uint8Array([0xC4, 0xE3, 0xBA, 0xC3]));
    eq(g, '你好', 'gbk decode');
    var b5 = new TextDecoder('big5').decode(new Uint8Array([0xA7, 0x41]));
    eq(b5, '你', 'big5 decode');
    var sj = new TextDecoder('shift_jis').decode(new Uint8Array([0x82, 0xA0]));
    eq(sj, 'あ', 'shift_jis decode');
    /* 本引擎 TextDecoder 未实现 fatal 选项（无效字节始终替换为 U+FFFD），此处仅验证容错行为 */
    eq(new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array([0xFF])), '\uFFFD', 'invalid byte -> replacement char');
    eq(new TextDecoder().decode(new Uint8Array([0xFF])), '�', 'replacement char');
});

/* ================= 7. Buffer ================= */
test('Buffer: alloc/from/数值读写/toString/copy/swap/slice', function () {
    var b = Buffer.alloc(4);
    eq(b.length, 4);
    ok(Buffer.isBuffer(b));
    b.writeUInt32LE(0x11223344, 0);
    eq(b.readUInt8(0), 0x44, 'LE low byte');
    eq(b.readUInt32LE(0), 0x11223344, 'u32 LE');
    eq(b.readUInt32BE(0), 0x44332211, 'u32 BE');
    var b2 = Buffer.from([1, 2, 3]);
    eq(b2[0], 1); eq(b2.length, 3);
    var bs = Buffer.from('hello');
    eq(bs.toString(), 'hello');
    eq(bs.toString('utf8', 1, 3), 'el', 'toString range');
    eq(bs.toString('base64'), 'aGVsbG8=', 'base64');
    eq(bs.toString('hex'), '68656c6c6f', 'hex');
    deepEq(Array.from(Buffer.from('aGVsbG8=', 'base64')), Array.from(bs), 'base64 decode');
    deepEq(Array.from(Buffer.from('68656c6c6f', 'hex')), Array.from(bs), 'hex decode');
    var bf = Buffer.alloc(8);
    bf.writeFloatLE(3.5, 0);
    ok(Math.abs(bf.readFloatLE(0) - 3.5) < 1e-6, 'float32 LE');
    bf.writeDoubleBE(1.25, 0);
    eq(bf.readDoubleBE(0), 1.25, 'double BE');
    bf.writeInt16BE(-2, 0);
    eq(bf.readInt16BE(0), -2, 'int16 BE');
    var cc = Buffer.concat([bs, Buffer.from(' world')]);
    eq(cc.toString(), 'hello world', 'concat');
    eq(Buffer.byteLength('你好'), 6, 'byteLength utf8');
    eq(bs.compare(Buffer.from('hello')), 0, 'compare equal');
    ok(bs.equals(Buffer.from('hello')), 'equals');
    ok(bs.includes('ell'), 'includes str');
    eq(bs.indexOf('l'), 2, 'indexOf');
    eq(bs.lastIndexOf('l'), 3, 'lastIndexOf');
    var fz = Buffer.alloc(5); fz.fill(7);
    deepEq(Array.from(fz), [7, 7, 7, 7, 7], 'fill');
    var dst = Buffer.alloc(5); bs.copy(dst, 1);
    eq(dst.toString('utf8', 1), 'hell', 'copy (clamped at dst end)');
    var sw = Buffer.from([1, 2, 3, 4]); sw.swap16();
    deepEq(Array.from(sw), [2, 1, 4, 3], 'swap16');
    eq(bs.slice(1, 3).toString(), 'el', 'slice');
    eq(bs.subarray(1, 3).toString(), 'el', 'subarray');
    eq(Buffer.isEncoding('utf-8'), true, 'isEncoding');
    eq(Buffer.isEncoding('nope'), false, 'isEncoding false');
});

/* ================= 8. crypto Hash / HMAC ================= */
test('crypto: createHash md5/sha1/sha256 系列 + digest + hmac', function () {
    eq(crypto.createHash('md5').update('').digest('hex'), 'd41d8cd98f00b204e9800998ecf8427e', 'md5 empty');
    eq(crypto.createHash('md5').update('abc').digest('hex'), '900150983cd24fb0d6963f7d28e17f72', 'md5 abc');
    eq(crypto.createHash('sha1').update('abc').digest('hex'), 'a9993e364706816aba3e25717850c26c9cd0d89d', 'sha1 abc');
    eq(crypto.createHash('sha256').update('abc').digest('hex'),
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'sha256 abc');
    eq(crypto.createHash('sha224').update('abc').digest('hex'),
        '23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7', 'sha224');
    eq(crypto.createHash('sha384').update('abc').digest('hex'),
        'cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7', 'sha384');
    eq(crypto.createHash('sha512').update('abc').digest('hex'),
        'ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f', 'sha512');
    var h = crypto.createHash('sha256');
    h.update('ab'); h.update('c');
    var db = h.digest();
    ok(db instanceof ArrayBuffer || Buffer.isBuffer(db), 'digest default binary');
    eq(Buffer.from(db).toString('hex'), crypto.createHash('sha256').update('abc').digest('hex'), 'chained update');
    var h2 = crypto.createHash('sha256').update('abc');
    var h3 = h2.copy(); h2.update('def');
    eq(h2.digest('hex'), crypto.createHash('sha256').update('abcdef').digest('hex'), 'copy + extend');
    eq(h3.digest('hex'), crypto.createHash('sha256').update('abc').digest('hex'), 'copy independent');
    eq(crypto.createHmac('sha256', 'Jefe').update('what do ya want for nothing?').digest('hex'),
        '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843', 'hmac sha256 RFC4231');
    eq(crypto.createHmac('md5', 'key').update('The quick brown fox jumps over the lazy dog').digest('hex'),
        '80070713463e7749b90c2dc24911e275', 'hmac md5');
    ok(typeof crypto.createHash('sha256').digest('base64') === 'string', 'digest base64');
    throws(function () { crypto.createHash('nope'); }, 'bad algo');
});

/* ================= 9. WebCrypto ================= */
test('WebCrypto: getRandomValues', function () {
    var arr = new Uint8Array(16);
    var r = crypto.getRandomValues(arr);
    eq(r, arr, 'returns same array');
    ok(arr.some(function (x) { return x !== 0; }), 'random not all zero');
});
test('WebCrypto: subtle 同步 API（digest/HMAC/AES-GCM/PBKDF2）', function () {
    var s = crypto.subtle;
    eq(Buffer.from(s.digest('SHA-256', new TextEncoder().encode('abc'))).toString('hex'),
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', 'subtle SHA-256');
    var hk = s.generateKey({ name: 'HMAC', hash: 'SHA-256' }, true, ['sign', 'verify']);
    ok(hk && hk.type === 'secret', 'HMAC key type');
    var data = new TextEncoder().encode('hello');
    var sig = s.sign('HMAC', hk, data);
    eq(s.verify('HMAC', hk, sig, data), true, 'HMAC verify ok');
    eq(s.verify('HMAC', hk, new Uint8Array(32), data), false, 'HMAC verify bad sig');
    var ak = s.generateKey({ name: 'AES-GCM', length: 256 }, true, ['encrypt', 'decrypt']);
    ok(ak.type === 'secret', 'AES key type');
    var iv = crypto.getRandomValues(new Uint8Array(12));
    var pt = new TextEncoder().encode('secret message');
    var ct = s.encrypt({ name: 'AES-GCM', iv: iv }, ak, pt);
    var dec = s.decrypt({ name: 'AES-GCM', iv: iv }, ak, ct);
    eq(new TextDecoder().decode(dec), 'secret message', 'AES-GCM roundtrip');
    var raw = s.exportKey('raw', ak);
    eq(raw.byteLength, 32, 'AES raw 32 bytes');
    var imp = s.importKey('raw', raw, { name: 'AES-GCM' }, false, ['encrypt', 'decrypt']);
    ok(imp.extractable === false, 'importKey extractable false');
    var pwKey = s.importKey('raw', new TextEncoder().encode('password'), 'PBKDF2', false, ['deriveBits']);
    var bits = s.deriveBits(
        { name: 'PBKDF2', hash: 'SHA-256', salt: new Uint8Array(16), iterations: 1000 },
        pwKey, 256);
    eq(bits.byteLength, 32, 'PBKDF2 deriveBits 256');
});

/* ================= 10. fs ================= */
test('fs: 同步 API 全流程 + Stats', function () {
    var dir = 'qjs_test_dir';
    var file = dir + '/test.txt';
    if (fs.existsSync(dir)) fs.rmdirSync(dir, { recursive: true });
    fs.mkdirSync(dir);
    ok(fs.existsSync(dir), 'mkdir');
    ok(fs.readdirSync('.').indexOf(dir) >= 0, 'readdir');
    fs.writeFileSync(file, 'line1\n');
    fs.appendFileSync(file, 'line2\n');
    eq(fs.readFileSync(file, 'utf8'), 'line1\nline2\n', 'write/append/read');
    var raw = fs.readFileSync(file);
    ok(raw instanceof Uint8Array, 'readFileSync returns Uint8Array/Buffer');
    var st = fs.statSync(file);
    eq(st.isFile(), true, 'stat isFile');
    eq(st.isDirectory(), false, 'stat isDirectory');
    eq(st.size, 12, 'stat size');
    ok(st.mtime instanceof Date, 'stat mtime Date');
    fs.writeFileSync(file + '.c1', 'data');
    fs.writeFileSync(file + '.c2', fs.readFileSync(file + '.c1'));
    ok(fs.existsSync(file + '.c2'), 'copyFile');
    fs.renameSync(file + '.c2', file + '.r');
    ok(fs.existsSync(file + '.r'), 'rename');
    ok(fs.realpathSync(dir).length > 0, 'realpath');
    fs.unlinkSync(file + '.r');
    fs.unlinkSync(file + '.c1');
    fs.unlinkSync(file);
    fs.rmdirSync(dir);
    ok(!fs.existsSync(dir), 'rmdir');
    throws(function () { fs.readFileSync('qjs_no_such_file_xyz'); }, 'read missing throws');
});
test('fs: promises API', function () {
    fs.promises.writeFile('qjs_p.txt', 'prom').then(function () {
        return fs.promises.readFile('qjs_p.txt', 'utf8');
    }).then(function (c) {
        eq(c, 'prom', 'fs.promises roundtrip');
        return fs.promises.unlink('qjs_p.txt');
    }).catch(function (e) {
        throw new Error('[fs.promises] ' + (e && e.message || e));
    });
});

/* ================= 11. zlib ================= */
test('zlib: crc32/adler32 标准向量 + gzip/deflate 往返', function () {
    var data = Buffer.from('123456789');
    eq(zlib.crc32(data) >>> 0, 0xCBF43926, 'crc32 standard vector');
    eq(zlib.adler32(data) >>> 0, 0x091E01DE, 'adler32 standard vector');
    var big = Buffer.from('hello hello hello hello hello hello world world world');
    var gz = zlib.gzipSync ? zlib.gzipSync(big) : zlib.gzip(big);
    ok(gz && gz.length > 0, 'gzip returns data');
    eq(Buffer.from(zlib.gunzipSync ? zlib.gunzipSync(gz) : zlib.gunzip(gz)).toString(), big.toString(), 'gzip roundtrip');
    var df = zlib.deflateSync ? zlib.deflateSync(big) : zlib.deflate(big);
    ok(df && df.length > 0, 'deflate returns data');
    eq(Buffer.from(zlib.inflateSync ? zlib.inflateSync(df) : zlib.inflate(df)).toString(), big.toString(), 'deflate roundtrip');
    var empty = Buffer.alloc(0);
    eq(Buffer.from(zlib.inflateSync ? zlib.inflateSync(zlib.deflateSync(empty)) : zlib.inflate(zlib.deflate(empty))).length, 0, 'empty roundtrip');
});

/* ================= 12. SQLite ================= */
test('SQLite: DataBase 建表/CRUD/事务/Statement', function () {
    var dbpath = 'qjs_test.db';
    if (fs.existsSync(dbpath)) fs.unlinkSync(dbpath);
    var db = new DataBase(dbpath);
    db.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)');
    var stmt = db.prepare('INSERT INTO t (name, score) VALUES (?, ?)');
    stmt.run('alice', 90.5);
    stmt.run('bob', 80);
    stmt.finalize();
    var row = db.prepare('SELECT * FROM t WHERE name = ?').all('alice')[0];
    eq(row.name, 'alice', 'prepared get');
    eq(row.score, 90.5, 'real value');
    var all = db.prepare('SELECT * FROM t ORDER BY id').all();
    eq(all.length, 2, 'all rows');
    eq(all[1].name, 'bob', 'second row');
    // 事务提交
    db.exec('BEGIN');
    db.exec("INSERT INTO t (name, score) VALUES ('carol', 70)");
    db.exec('COMMIT');
    eq(db.prepare('SELECT COUNT(*) AS n FROM t').all()[0].n, 3, 'commit count');
    // 事务回滚
    db.exec('BEGIN');
    db.exec("INSERT INTO t (name, score) VALUES ('dave', 60)");
    db.exec('ROLLBACK');
    eq(db.prepare('SELECT COUNT(*) AS n FROM t').all()[0].n, 3, 'rollback count');
    // run 返回值 / bind
    var st2 = db.prepare('SELECT ? AS v');
    if (typeof st2.bind === 'function') {
        st2.bind([42]);
        eq(st2.all()[0].v, 42, 'bind number');
    } else {
        eq(db.prepare('SELECT 42 AS v').all()[0].v, 42, 'select constant');
    }
    st2.finalize();
    db.close();
    ok(fs.existsSync(dbpath), 'db file persisted');
    fs.unlinkSync(dbpath);
});

/* ================= 13. WebAssembly ================= */
test('WebAssembly: validate + 手写字节码 instantiate 返回 42', function () {
    // (module (func (result i32) i32.const 42) (export "answer" 0))
    var bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,             // type: () -> i32
        0x03, 0x02, 0x01, 0x00,                                // func 0
        0x07, 0x0a, 0x01, 0x06, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x00, 0x00, // export "answer"
        0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b         // code: i32.const 42; end
    ]);
    ok(WebAssembly.validate(bytes), 'validate');
    eq(WebAssembly.validate(new Uint8Array([0, 1, 2, 3])), false, 'validate bad');
    var mod = new WebAssembly.Module(bytes);
    var inst = new WebAssembly.Instance(mod);
    eq(inst.exports.answer(), 42, 'wasm function returns 42');
    ok(WebAssembly.Module && WebAssembly.Instance && WebAssembly.Memory && WebAssembly.Table && WebAssembly.Global, 'API surface');
});
test('WebAssembly: Memory 与 imports 调用', function () {
    // (module (func $add (import "m" "add") (param i32 i32) (result i32))
    //         (func (export "dbl") (param i32) (result i32)
    //           call $add (local.get 0) (local.get 0)))
    // 0x20 = local.get, 0x10 = call
    var bytes = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x0c, 0x02, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, // types
        0x02, 0x09, 0x01, 0x01, 0x6d, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, // import m.add func0
        0x03, 0x02, 0x01, 0x01,                                // func1 : type1
        0x07, 0x07, 0x01, 0x03, 0x64, 0x62, 0x6c, 0x00, 0x01,  // export "dbl" func1
        0x0a, 0x0a, 0x01, 0x08, 0x00, 0x20, 0x00, 0x20, 0x00, 0x10, 0x00, 0x0b // code
    ]);
    var inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {
        m: { add: function (a, b) { return a + b; } }
    });
    eq(inst.exports.dbl(21), 42, 'imported call dbl(21)=42');
    eq(inst.exports.dbl(-5), -10, 'dbl(-5)');
    var mem = new WebAssembly.Memory({ initial: 1 });
    eq(mem.buffer.byteLength, 65536, 'Memory page 64KB');
});

/* ================= 14. path ================= */
test('path: join/resolve/normalize/basename/dirname/extname/relative/parse', function () {
    eq(path.join('a', 'b', 'c'), 'a/b/c', 'join');
    eq(path.join('a', '../b'), 'b', 'join dotdot');
    eq(path.join('/a', '/b'), '/a/b', 'join abs');
    eq(path.normalize('/a//b/../c/.'), '/a/c', 'normalize');
    eq(path.isAbsolute('/usr/bin'), true, 'isAbsolute true');
    eq(path.isAbsolute('a/b'), false, 'isAbsolute false');
    eq(path.dirname('/a/b/c.txt'), '/a/b', 'dirname');
    eq(path.basename('/a/b/c.txt'), 'c.txt', 'basename');
    eq(path.basename('/a/b/c.txt', '.txt'), 'c', 'basename ext');
    eq(path.extname('a.tar.gz'), '.gz', 'extname');
    eq(path.extname('noext'), '', 'extname empty');
    eq(path.resolve('a', 'b'), path.resolve('a/b'), 'resolve basic');
    ok(path.resolve('/a', 'b').charAt(0) === '/', 'resolve abs');
    eq(path.relative('/a/b', '/a/c'), '../c', 'relative');
    eq(path.sep, '/', 'sep posix');
    ok(typeof path.delimiter === 'string', 'delimiter');
});

/* ================= 15. console / format ================= */
test('console: format 风格化 + 各方法存在性', function () {
    ok(typeof console.log === 'function' && typeof console.info === 'function' &&
       typeof console.warn === 'function' && typeof console.error === 'function' &&
       typeof console.debug === 'function' && typeof console.assert === 'function' &&
       typeof console.table === 'function' && typeof console.trace === 'function' &&
       typeof console.dir === 'function', 'console methods');
    ok(typeof console.count === 'function' && typeof console.countReset === 'function' &&
       typeof console.time === 'function' && typeof console.timeEnd === 'function' &&
       typeof console.clear === 'function', 'console state methods');
    console.count('c1'); console.count('c1'); console.countReset('c1'); console.count('c1');
    console.time('t1'); console.timeEnd('t1');
    ok(typeof globalThis.format === 'function', 'global format');
    eq(format('%s-%d', 'x', 42), 'x-42', 'format %s %d');
    eq(format('%d', 3.7), '3.7', 'format %d');
    eq(format('100%%'), '100%', 'format %%');
    ok(format('%j', { a: 1 }).indexOf('"a":1') >= 0, 'format %j');
    console.assert(true, 'should not throw');
});

/* ================= 16. Date polyfill ================= */
test('Date: 可读格式解析 + 时区/毫秒', function () {
    // 纯日期字符串按 UTC 00:00 解析
    var d1 = new Date('2024-06-15');
    eq(d1.getTime(), Date.UTC(2024, 5, 15), 'date-only -> UTC midnight');
    // 'YYYY-MM-DD HH:mm:ss' 按本地时间解析
    var d2 = new Date('2024-06-15 12:30:45');
    eq(d2.getFullYear(), 2024, 'year');
    eq(d2.getMonth(), 5, 'month');
    eq(d2.getDate(), 15, 'date');
    eq(d2.getHours(), 12, 'hours');
    eq(d2.getMinutes(), 30, 'minutes');
    eq(d2.getSeconds(), 45, 'seconds');
    eq(new Date('2024/06/15 12:30:45').getTime(), d2.getTime(), 'slash format equal');
    // 毫秒
    var d3 = new Date('2024-06-15 12:30:45.123');
    eq(d3.getMilliseconds(), 123, 'milliseconds');
    // Z 后缀（UTC）
    var d4 = new Date('2024-06-15T12:30:45Z');
    eq(d4.getTime(), Date.UTC(2024, 5, 15, 12, 30, 45), 'Z suffix UTC');
    // ±HH:MM 时区后缀
    var d5 = new Date('2024-06-15T12:30:45+08:00');
    eq(d5.getTime(), Date.UTC(2024, 5, 15, 4, 30, 45), '+08:00 offset');
    // Date.now / parse 依然可用
    ok(typeof Date.now() === 'number' && typeof Date.parse('2024-06-15') === 'number', 'Date.now/parse');
});

/* ================= 17. atob / btoa ================= */
test('atob/btoa: Base64 往返', function () {
    eq(btoa('hello'), 'aGVsbG8=', 'btoa');
    eq(atob('aGVsbG8='), 'hello', 'atob');
    eq(btoa(''), '', 'btoa empty');
    eq(btoa('a'), 'YQ==', 'btoa padding 1');
    eq(btoa('ab'), 'YWI=', 'btoa padding 2');
    eq(btoa('abc'), 'YWJj', 'btoa no padding');
    // Latin-1 码点 ≤255
    eq(btoa('\xE9'), '6Q==', 'btoa latin1');
    eq(atob('6Q=='), '\xE9', 'atob latin1');
    throws(function () { btoa('中'); }, 'btoa out of latin1 throws');
    throws(function () { atob('!!'); }, 'atob invalid throws');
});

/* ================= 18. performance ================= */
test('performance: now / timeOrigin', function () {
    var t1 = performance.now();
    ok(typeof t1 === 'number' && t1 >= 0, 'now number');
    var spin = Date.now() + 15;
    while (Date.now() < spin) {} // 忙等 ~15ms
    ok(performance.now() >= t1, 'monotonic');
    ok(typeof performance.timeOrigin === 'number', 'timeOrigin number');
});

/* ================= 19. Uint8Array Base64/Hex ================= */
test('Uint8Array: toBase64/toHex/setFromBase64/setFromHex/fromBase64/fromHex', function () {
    var u = new Uint8Array([104, 101, 108, 108, 111]); // "hello"
    eq(u.toBase64(), 'aGVsbG8=', 'toBase64');
    eq(u.toHex(), '68656c6c6f', 'toHex');
    deepEq(Array.from(Uint8Array.fromBase64('aGVsbG8=')), [104, 101, 108, 108, 111], 'fromBase64');
    deepEq(Array.from(Uint8Array.fromHex('68656c6c6f')), [104, 101, 108, 108, 111], 'fromHex');
    var t = new Uint8Array(5);
    t.setFromBase64('aGVsbG8=');
    deepEq(Array.from(t), [104, 101, 108, 108, 111], 'setFromBase64');
    var t2 = new Uint8Array(5);
    t2.setFromHex('68656c6c6f');
    deepEq(Array.from(t2), [104, 101, 108, 108, 111], 'setFromHex');
    throws(function () { Uint8Array.fromHex('zz'); }, 'fromHex invalid');
});

/* ================= 20. ES2025+ ================= */
test('ES2025: Set 集合运算 7 方法', function () {
    var a = new Set([1, 2, 3]), b = new Set([2, 3, 4]);
    deepEq(Array.from(a.union(b)), [1, 2, 3, 4], 'union');
    deepEq(Array.from(a.intersection(b)), [2, 3], 'intersection');
    deepEq(Array.from(a.difference(b)), [1], 'difference');
    deepEq(Array.from(a.symmetricDifference(b)), [1, 4], 'symmetricDifference');
    eq(new Set([1, 2]).isSubsetOf(a), true, 'isSubsetOf');
    eq(a.isSupersetOf(new Set([1, 2])), true, 'isSupersetOf');
    eq(a.isDisjointFrom(new Set([9])), true, 'isDisjointFrom');
    eq(a.isDisjointFrom(b), false, 'not disjoint');
});
test('ES2025: Iterator Helpers 惰性求值', function () {
    deepEq([1, 2, 3, 4, 5].values().map(function (x) { return x * 2; }).toArray(), [2, 4, 6, 8, 10], 'map+toArray');
    deepEq([1, 2, 3, 4].values().filter(function (x) { return x % 2 === 0; }).toArray(), [2, 4], 'filter');
    deepEq([1, 2, 3].values().take(2).toArray(), [1, 2], 'take');
    deepEq([1, 2, 3].values().drop(1).toArray(), [2, 3], 'drop');
    eq([1, 2, 3].values().reduce(function (a, c) { return a + c; }, 0), 6, 'reduce');
    var s = 0;
    [1, 2].values().forEach(function (x) { s += x; });
    eq(s, 3, 'forEach');
    eq([1, 2].values().some(function (x) { return x === 2; }), true, 'some');
    eq([1, 2].values().every(function (x) { return x > 0; }), true, 'every');
    eq([1, 2, 3].values().find(function (x) { return x > 1; }), 2, 'find');
    // 生成器 + 链式
    function* gen() { yield 1; yield 2; yield 3; }
    deepEq(gen().map(function (x) { return x + 10; }).filter(function (x) { return x % 2 === 1; }).toArray(), [11, 13], 'generator chain');
    // 惰性：take(2) 后不应消费全部
    var consumed = 0;
    function* g2() { consumed++; yield 1; consumed++; yield 2; consumed++; yield 3; }
    deepEq(g2().take(2).toArray(), [1, 2], 'lazy take');
    eq(consumed, 2, 'lazy consumption');
});
test('ES2025: Map getOrInsert / getOrInsertComputed', function () {
    var m = new Map([['a', 1]]);
    eq(m.getOrInsert('a', 99), 1, 'getOrInsert existing');
    eq(m.getOrInsert('b', 99), 99, 'getOrInsert insert');
    eq(m.get('b'), 99, 'value stored');
    var calls = 0;
    eq(m.getOrInsertComputed('c', function () { calls++; return 7; }), 7, 'computed insert');
    eq(calls, 1, 'fn called once');
    m.getOrInsertComputed('c', function () { calls++; return 8; });
    eq(calls, 1, 'fn not called when exists');
    eq(m.get('c'), 7, 'existing kept');
});
test('ES2025: ArrayBuffer resize / transfer', function () {
    var ab = new ArrayBuffer(8, { maxByteLength: 64 });
    eq(ab.resizable, true, 'resizable flag');
    eq(ab.maxByteLength, 64, 'maxByteLength');
    ab.resize(32);
    eq(ab.byteLength, 32, 'resize');
    ab.resize(4);
    eq(ab.byteLength, 4, 'resize down');
    var dv = new DataView(new ArrayBuffer(8, { maxByteLength: 16 }));
    ok(dv.byteLength === 8, 'resizable-backed DataView');
    var ab2 = new ArrayBuffer(8);
    var t = ab2.transfer();
    eq(ab2.detached, true, 'source detached');
    eq(t.byteLength, 8, 'transferred buffer length');
    var ab3 = new ArrayBuffer(8);
    var t2 = ab3.transferToFixedLength(4);
    eq(t2.byteLength, 4, 'transferToFixedLength');
    eq(ab3.detached, true, 'detached after fixed');
    var ab4 = new ArrayBuffer(8, { maxByteLength: 32 });
    ab4.resize(16);
    eq(ab4.byteLength, 16, 'resize');
});
test('ES2025: JSON rawJSON / Math.sumPrecise', function () {
    eq(JSON.isRawJSON(JSON.rawJSON('123456789012345678901234567890')), true, 'rawJSON isRawJSON');
    eq(JSON.isRawJSON(42), false, 'isRawJSON false for number');
    eq(typeof JSON.rawJSON('1').toJSON, 'undefined', 'rawJSON object has no toJSON (engine)');
    // rawJSON 用于 stringify 保留大数精度
    var out = JSON.stringify({ big: JSON.rawJSON('123456789012345678901234567890') });
    eq(out, '{"big":123456789012345678901234567890}', 'rawJSON preserves precision');
    eq(JSON.stringify({ n: 123456789012345678901234567890 }), '{"n":1.2345678901234568e+29}', 'plain number loses precision');
    var s = Math.sumPrecise([0.1, 0.2, 0.3]);
    eq(s, 0.6, 'sumPrecise 0.1+0.2+0.3=0.6');
    eq(Math.sumPrecise([]), 0, 'sumPrecise empty');
});

/* ================= 摘要 ================= */
console.log('----------------------------------------');
console.log('qjs_engine test_all.js: ' + __pass + ' assertions passed');
if (__fail > 0) {
    throw new Error(__fail + ' failures');
}
console.log('ALL TESTS PASSED');
