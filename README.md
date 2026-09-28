# qjs_ultra

**Flutter / Dart 的 QuickJS 引擎包**：自研魔改 QuickJS + 高性能 C 扩展核心，
纯 Dart + `dart:ffi` 封装，不依赖 Flutter（Flutter app / 纯 Dart CLI / 服务端都可用）。
同一份引擎同时服务 [DsPlayer](https://github.com/hjdhnx/DsPlayer) 的
drpy2（TVBox js 源）与 drpy3（声明式源）两类爬虫生态。

```dart
final engine = QuickjsEngine.createWith(const JsEngineConfig(), libPath: dll);
engine.registerFunction('req', (args) => httpGet(args[0]));   // 同步宿主函数
engine.registerAsyncFunction('fetch', (args) async => ...);   // 异步宿主函数（JS 侧 await）
final out = await engine.evaluateAsync('(async () => await fetch(url))()');
```

## 特性

- **双模式宿主**：同步宿主函数（JS 阻塞等 Dart，适合 worker isolate 场景）与
  异步宿主函数（JS `await` 让出，Dart 侧真并发 IO）可在同一引擎实例混用
- **Promise 全支持**：`evaluateAsync/callAsync` 自动泵微任务至 settle，含顶层 await
  模块；rejected promise 抛结构化异常
- **墙钟超时**：`timeoutMs` 由引擎中断处理器实现，`while(true){}` 死循环可被打断成
  可捕获异常，而非卡死线程
- **结构化错误**：`JsErrorKind` 九类归置（syntax 带行号列号 / stackOverflow /
  memoryLimit / timeout / type / reference / range / interrupted / generic）
- **引擎内置 C 能力**（so 启动即注入 JS 全局，比 JS 实现快数倍）：
  `cheerio`(Lexbor) · `URL`/`URLSearchParams` · `TextEncoder`/`TextDecoder`(含 GBK) ·
  `Buffer` · `crypto`(createHash/HMAC + WebCrypto subtle) · `fs` · `zlib` ·
  `DataBase`(SQLite) · `path` · `WebAssembly`(wasm3) · `atob`/`btoa` · `performance`
- **原生 WebAssembly**：wasm3 编入 so，`typeof WebAssembly !== 'undefined'`
- **值转换护栏**：循环引用检测 + 深度 128 / 节点 10 万上限，恶意结构打不爆宿主栈
- **未处理 rejection 管道**：有界队列收集后台 rejection，`drainUnhandledRejections()` 排空
- **多引擎共存安全**：回调按 context 注册（per-context），多 isolate / 多引擎互不干扰

## 安装

```yaml
dependencies:
  qjs_ultra:
    git:
      url: https://github.com/hjdhnx/qjs_ultra
      ref: <commit 或 tag>   # 建议钉 commit
```

包本体是纯 Dart（FFI 声明 + 引擎封装），**还需按平台拿到引擎动态库**：

| 平台 | 库 | 获取方式 |
|---|---|---|
| Android arm64-v8a / armeabi-v7a / x86 / x86_64 | `libquickjs_bridge.so` | [Releases](https://github.com/hjdhnx/qjs_ultra/releases) 下载对应架构，放应用 `jniLibs/<abi>/`，或走「插件 APK 跨包 dlopen」模式（见 DsPlayer plugin_qjs） |
| Windows x64 | `quickjs_bridge.dll` | Releases 下载，放可执行文件目录或任意可寻路径 |
| Linux / macOS | `.so` / `.dylib` | 仓库 CI 手动触发对应构建（Actions → Run workflow），或本地构建（见下文） |

> **只支持 64 位目标**（arm64-v8a / x86_64）：`JSValue` 按 16 字节结构体布局读取，
> 依赖引擎编为 64 位（`JS_PTR64`）；32 位下 NAN_BOXING 会把 JSValue 压成单个
> uint64，Dart 侧读取路径失效。

## 快速开始

```dart
import 'package:qjs_ultra/qjs_ultra.dart';

void main() {
  // 1. 创建引擎（libPath 指向引擎动态库；不传则按平台默认名/环境变量 QJS_ULTRA_LIB 查找）
  final engine = QuickjsEngine.createWith(
    const JsEngineConfig(
      stackSize: 1 * 1024 * 1024,      // JS 栈（必须 ≤ 宿主线程栈 3/4，见下方护栏说明）
      memoryLimit: 64 * 1024 * 1024,   // JS 堆上限
      timeoutMs: 10 * 1000,            // 单次求值/调用墙钟超时（可选）
    ),
    libPath: 'libquickjs_bridge.so',
  );

  // 2. 注入宿主能力（同步宿主函数：JS 调用时阻塞等 Dart 返回）
  engine.registerFunction('hostAdd', (args) => (args[0] as int) + (args[1] as int));

  // 3. 求值
  print(engine.evaluate('hostAdd(20, 22)'));            // 42
  print(engine.evaluate('JSON.stringify({a: 1})'));     // {"a":1}

  // 4. 释放（幂等；回调表随引擎一并释放）
  engine.dispose();
}
```

### 异步宿主函数（JS 侧 `await`）

```dart
// 注册异步宿主函数：JS 调用同步拿到 Promise 并让出，Dart Future 完成后 settle，
// 微任务自动泵进——JS 里就是普通的 async 函数调用。
engine.registerAsyncFunction('httpGet', (args) async {
  final resp = await dio.get(args[0] as String);
  return {'status': resp.statusCode, 'body': resp.data};  // Map → JS object
});

final body = await engine.evaluateAsync(
  '(async () => { const r = await httpGet("https://example.com"); return r.status; })()',
);
print(body); // 200
```

Promise 语义：`evaluateAsync` / `callAsync` 会泵微任务直至结果 settle（含嵌套
`Promise.all`/`then` 链）；rejected promise 抛 `JsEvalException`（结构化，见下）。
纯 JS 定时器需要装 timers 垫片：

```dart
engine.installTimers(); // setTimeout/clearTimeout（Dart Timer 驱动，回调异常进后台错误）
```

### ES Module

```dart
// 执行模块（顶层 await 会等到 settle；爬虫源靠副作用写 globalThis）
await engine.evaluateModule(source, fileName: 'src.js');

// 注册可 import 的模块 + 执行
engine.registerModule('my_mod', 'export const x = 41;');
await engine.evaluateAsync('(async () => { const m = await import("my_mod"); return m.x + 1; })()'); // 42

// 预编译字节码（缓存复用；同引擎版本内有效）
final bytecode = engine.compileModule(source, fileName: 'src.js');
engine.executeBytecode(bytecode);
```

### 调用 JS 函数

```dart
engine.setGlobalProperty('add', {'impl': 'js'}); // 传值
final fn = engine.getGlobalProperty('add');      // JsFunctionRef（活函数引用）
engine.callFunction(fn, [1, 2]);
// 或异步版（返回值是 promise 时等 settle）：
await engine.callAsync(fn, [1, 2]);
```

## 值转换契约（Dart ↔ JS）

| Dart | JS | 备注 |
|---|---|---|
| `null` | `undefined` / `null` | 双向映射 null |
| `bool` / `String` | 同型 | |
| `int`（≤ int32） | number（整型） | |
| `int`（int32 ~ ±2^53） | number（double） | |
| `int`（超 ±2^53） | **BigInt** | 保精度，不降 double |
| `double` | number | |
| `List` | `Array` | 递归 |
| `Map` | 普通对象 | key 转 string，递归 |
| `Uint8List` | `Uint8Array` | 值拷贝 |
| `DateTime` | `Date` | 毫秒精度快照 |
| JS 函数 | `JsFunctionRef` | 活引用；只能传回 `callFunction/callAsync`，引擎 dispose 时统一释放 |
| 超限结构 | 降级 `null` | 深度 128 / 节点 10 万 / 循环引用处 |

## 错误处理

```dart
try {
  engine.evaluate('null.foo');
} on JsEvalException catch (e) {
  e.name;        // 'TypeError'
  e.message;     // "TypeError: Cannot read property 'foo' of null"
  e.errorKind;   // JsErrorKind.type
  e.stack;       // JS stack 文本
}
```

`JsErrorKind`：`syntax`（`line`/`column` 已解析）/ `type` / `reference` / `range` /
`stackOverflow` / `memoryLimit` / `timeout` / `interrupted` / `generic`。

诊断通道：

```dart
engine.drainUnhandledRejections(); // ({errors: List<String>, dropped: int}) 后台 rejection
engine.drainBackgroundErrors();    // 后台泵/timer 吞掉的异常（建议周期排空上报）
engine.getMemoryUsage();           // JsMemoryUsage(mallocSize, memoryUsedSize)
engine.runGC();
```

## API 速览

| 类别 | 同步 | 异步（promise 自动 settle） |
|---|---|---|
| 求值 | `evaluate` / `evaluateModule` / `executeBytecode` | `evaluateAsync` |
| 宿主函数 | `registerFunction` / `registeredFunctionFor` | `registerAsyncFunction` |
| 调用 | `callFunction` | `callAsync` |
| 模块 | `registerModule` / `compileModule` / `setModuleLoader` | `loadModuleToGlobals`（drpy3 bundle 等单模块场景） |
| 全局 | `getGlobalProperty` / `setGlobalProperty` | |
| JSON | `parseJson` / `stringify` | |
| 微任务 | `executePendingJobs`（手动泵） | |
| 垫片 | `installTimers`（setTimeout/clearTimeout） | |
| 诊断 | `getMemoryUsage` / `runGC` / `drainUnhandledRejections` / `drainBackgroundErrors` | |
| 生命周期 | `dispose`（幂等） | |

配置项 `JsEngineConfig`：`stackSize`（护栏：≤ 宿主线程栈 3/4，超限死递归会段错误
而非可捕获异常）/ `memoryLimit` / `gcThreshold` / `timeoutMs`。

## 与同类引擎包对比

| | **qjs_ultra** | [fjs](https://github.com/fluttercandies/fjs) | flutter_js |
|---|---|---|---|
| 核心栈 | C（魔改 QuickJS + 扩展） | Rust（rquickjs + frb） | C（quickjs 分支） |
| 内置 C 能力 | cheerio/URL/GBK/Buffer/crypto/WebCrypto/fs/zlib/SQLite/wasm3 | llrt 模块（fetch/timers 等） | 少量 |
| 原生 WebAssembly | ✅ wasm3 | quickjs-ng 内建 | ❌ |
| 同步宿主函数 | ✅ | ❌（仅异步） | ✅ |
| 异步宿主函数 | ✅（同实例可与同步混用） | ✅ | ❌ |
| 引擎级超时 | ✅ 中断处理器 | ✅ shutdown | 部分 |
| 结构化错误分类 | ✅ 9 类 + 行列号 | ✅ 17 类 | 字符串 |
| 构建依赖 | 无（CI 产物直用） | Rust 工具链（cargokit） | 无 |

## 测试

`dart test` 用**真实动态库**跑全套引擎用例（含 drpy3 bundle 六环节冒烟、
双模式交叉验证）——绑定层的错误只有真实加载才暴露得出来。库路径经环境变量
`QJS_ULTRA_LIB` 指定，未设回落 `native/<平台>/<arch>/`；缺库时整组跳过并说明原因。

## 构建（维护者）

引擎 so 由本仓 CI 出包：push 到 `main`（源码/构建链路径）自动构建四架构并发
Release；手动：Actions → `Build quickjs_bridge` → Run workflow（`arches` /
`release` / `releaseTag`）。本地交叉编译需要 `cmake >= 3.15`、`ninja`、
`go >= 1.20`（BoringSSL 生成必需）、NDK r25+：

```sh
export ANDROID_NDK_HOME=/path/to/ndk
cmake -S . -B build/arm64-v8a -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DQJS_USE_BORINGSSL_SOURCE=ON
cmake --build build/arm64-v8a -j$(nproc)
```

产物：`build/<abi>/libquickjs_bridge.so`（Windows DLL 由 CI 的 `build-windows`
job 产出，MinGW 静态链接零第三方运行时依赖）。

### ABI 契约（维护者必读）

`abi/exports.txt` 是导出符号的唯一事实源，CI 每次构建比对：**缺符号 = 失败**，
多符号 = 告警。改任何导出签名 = 改 ABI，必须同步 `lib/src/qjs_bindings.dart`
并通知下游升版。

## 仓库结构

```
lib/                               # Dart 包（FFI 绑定 + 引擎封装 + 异步桥）
test/                              # 真实引擎测试（engine_test / drpy3_smoke / dual_mode）
bridge/quickjs_bridge.c            # 对外 C API（稳定 ABI 层）
CMakeLists.txt                     # 顶层构建：qjs_engine 静态库 + bridge 共享库
native/quickjs/                    # QuickJS 核心 + 扩展模块 + 第三方库
docs/quickjs-android-api-docs.html # JS 全局能力权威清单（§23 速查表）
abi/exports.txt                    # 导出符号基线（ABI 唯一事实源）
```

## 许可

仓根 `LICENSE` 为 GPL-3.0（继承自上游
[AAswordman/Operit](https://github.com/AAswordman/Operit)）。第三方组件各自遵循
原始许可：QuickJS（MIT）、BoringSSL（OpenSSL + ISC）、lexbor（Apache-2.0）、
sqlite3（Public Domain）、zlib（zlib）、wasm3（MIT）。
