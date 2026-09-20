# qjs_ultra

DsPlayer 的 **QJS 引擎源码与构建仓**：产出 `libquickjs_bridge.so`（Android 各架构），
供 [DsPlayer](https://github.com/hjdhnx/DsPlayer) 的「QJS 引擎插件」跨包 dlopen 使用。

## 这是什么

基于 [AAswordman/Operit](https://github.com/AAswordman/Operit) 的 QuickJS 引擎派生，
在其之上维护 **DsPlayer 专用补丁**（见下）。引擎本体是自包含静态库 `qjs_engine`
（QuickJS 核心 + 扩展模块 + BoringSSL / lexbor / sqlite3 / zlib / wasm3），
`bridge/quickjs_bridge.c` 之上再包一层 `libquickjs_bridge.so` 共享库对外暴露 C API。

DsPlayer 侧用它执行 TVBox/drpy2 生态的 `.js` 爬虫源（`type=3` 的 `.js` 源与
sdcard `spider/js` 本地源）。

### 引擎能力（so 启动即向 JS 注入）

`cheerio`(Lexbor) · `URL`/`URLSearchParams` · `TextEncoder`/`TextDecoder`(含 GBK) ·
`Buffer` · `crypto`(createHash/HMAC + WebCrypto subtle) · `fs` · `zlib` ·
`DataBase`(SQLite) · `path` · `WebAssembly`(wasm3) · `atob`/`btoa` · `performance`

依据：`docs/reference/quickjs-android-api-docs.html`（§23 全局速查表，见 DsPlayer 仓库）。

## DsPlayer 补丁

### per-context 回调注册（根治跨 isolate SIGABRT）

**问题**：上游的 `qjs_set_callbacks` **无 ctx 参数**，`host_call` / `module_load` /
`normalize` 三个回调存在**文件级全局变量**里。这意味着进程内任何新引擎（新
isolate）一创建就**覆盖**旧值 —— 若旧 isolate 仍有 native 任务在跑（drpy2 的同步
`req`、wasm 解密），它回调时命中新 isolate 的 Dart 闭包，Dart VM 判定
`Cannot invoke native callback from a different isolate` → **SIGABRT 闪退**。

**修法**（`bridge/quickjs_bridge.c`）：回调表挂到 `JSContext` 的 opaque
（`JS_GetContextOpaque`），实现 per-engine 注册：

- 新增 `QjsCallbacks` 结构 + `callbacks_of(ctx)` 取用
- `qjs_set_callbacks(ctx, ...)` **新增首参**（⚠️ ABI 变更）
- 新增 `qjs_clear_callbacks(ctx)` 供 Dart 侧 dispose 释放
- 三处 trampoline（host / module / normalize）改为从 ctx 取回调

**ABI 契约（重要）**：导出函数签名与 DsPlayer 的 Dart FFI 绑定严格对应。
改签名 = 改 ABI —— **必须同时升本体版本并出包**，否则旧本体加载新 so 会
`SIGSEGV` 启动崩溃（2026-09-20 实机事故）。DsPlayer 侧有
`tools/qjs_native/check_abi_compat.py` 闸门做符号级校验。

## 构建

### 方式一：CI（推荐）

Actions → `Build quickjs_bridge` → Run workflow：

| 输入 | 说明 |
|---|---|
| `arches` | 架构，空格分隔（缺省 `arm64-v8a armeabi-v7a x86 x86_64`） |
| `release` | 勾选 = 构建完创建 Release 并发布 `.so` |
| `releaseTag` | Release tag（留空 = `build-<日期>-<短提交>`） |

产物同时以 artifact（`so-<abi>`）形式附在 run 页。

**为什么优先 CI**：这套交叉编译链需要 NDK + CMake + Ninja + Go + 整个
BoringSSL/lexbor/sqlite3 源码树，本地搭环境成本高；GitHub Actions 干净可复现，
四架构并行约 10-15 分钟。只有"本机环境本就满足"时才本地编（见 DsPlayer 侧纪律）。

### 方式二：本地构建

前置：`cmake >= 3.15`、`ninja`、`go >= 1.20`（BoringSSL 生成阶段用）、Android NDK r25+。

```sh
export ANDROID_NDK_HOME=/path/to/ndk            # 如 ~/Android/Sdk/ndk/27.0.12077973
cmake -S . -B build/arm64-v8a -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DQJS_USE_BORINGSSL_SOURCE=ON
cmake --build build/arm64-v8a -j$(nproc)
# 产物：build/arm64-v8a/libquickjs_bridge.so
```

`-DQJS_USE_BORINGSSL_SOURCE=ON` 是**必需**的（本仓未附带 prebuilt BoringSSL）。缺 Go 会
自动回退系统 OpenSSL —— Android 交叉编译下那条路走不通，故 CI 与本地都要 Go。

### 产物去向

```
build/<abi>/libquickjs_bridge.so
  → plugin_qjs/app/src/main/jniLibs/<abi>/libquickjs_bridge.so   （DsPlayer 插件）
  → pack/dist/DsPlayer-QJS-plugin-<版本>-<abi>.apk               （出包）
```

## 仓库结构

```
bridge/quickjs_bridge.c            # 对外 C API + DsPlayer 补丁（唯一的自维护文件）
CMakeLists.txt                     # 顶层构建：qjs_engine 静态库 + bridge 共享库
native/cpp/                        # JNI wrapper（供 Android 侧 Java 调用路径）
native/quickjs/                    # QuickJS 核心（cutils/dtoa/…）
native/quickjs/ext/
  ├── CMakeLists.txt               # 引擎静态库：核心 + 全部扩展模块
  ├── modules/                     # 扩展模块（crypto/fs/html/url/textcodec/buffer/…）
  ├── boringssl/                   # 加密（源码编译，需 Go）
  ├── lexbor/                      # HTML 解析（cheerio 的底座）
  ├── sqlite3/ zlib/ wasm3/        # DataBase / zlib / WebAssembly 模块
  └── include/ tests/              # 头与测试宿主（QJS_BUILD_TESTS 控制）
```

### 相对上游的裁剪（2026-09-21）

删除了**编译不需要**的测试数据，292MB → 130MB：

| 删除项 | 体积 | 依据 |
|---|---|---|
| `boringssl/third_party/wycheproof_testvectors/` | 138MB | 仅被 `crypto/**/*_test.cc` 引用（`BUILD_TESTING` 关时不参与构建） |
| `boringssl/fuzz/` | 18MB | `if(FUZZ)` 块内 |
| `boringssl/third_party/googletest/` `benchmark/` | 6MB | `if(BUILD_TESTING)` 块内 |
| `boringssl/rust/` | 1.4MB | `if(RUST_BINDINGS)` |
| `boringssl/docs/` `infra/` | 0.6MB | 文档/CI 基建 |

`gen/` 保留（`include(gen/sources.cmake)` 与 ASM include 路径必需），`crypto/` `ssl/`
`pki/` 保留（BoringSSL 构建目标互相引用）。

## 许可

本仓的第三方组件各自遵循其原始许可：QuickJS（MIT）、BoringSSL（OpenSSL License +
ISC）、lexbor（Apache-2.0）、sqlite3（Public Domain）、zlib（zlib License）、
wasm3（MIT）。仓根 `LICENSE` 为 GPL-3.0（继承自上游 Operit）。

## 相关文档（DsPlayer 仓库）

- `docs/plugin/QJS-PLUGIN-DESIGN.md` — QJS 引擎插件设计
- `docs/plugin/QJS-PROCESS-ISOLATION-DESIGN.md` — per-context 改造的完整背景（§六·五）
- `docs/reference/quickjs-android-api-docs.html` — so 注入的全局能力清单（§23）
- `tools/qjs_native/` — 本地构建与 ABI 校验脚本（`build_so.py` / `check_abi_compat.py`）
