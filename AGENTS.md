<!-- 由 .githooks/pre-commit 从 CLAUDE.md 自动生成，请勿直接编辑 -->

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 仓库定位

产出 Android 四架构的 `libquickjs_bridge.so`，供 DsPlayer 的「QJS 引擎插件」跨包 dlopen，执行 TVBox/drpy2 生态的 `.js` 爬虫源。引擎派生自 [AAswordman/Operit](https://github.com/AAswordman/Operit) 的 QuickJS。

**改动范围纪律**：本仓唯一自维护文件是 `bridge/quickjs_bridge.c`（对外 C API + DsPlayer 补丁）。`native/` 下全部是引擎与第三方 vendored 源码（quickjs 核心、boringssl、lexbor、sqlite3、zlib、wasm3）——改它们等于改上游，需格外谨慎并在 commit message 说明原因。

## 常用命令

### 构建（优先 CI）

Actions → `Build quickjs_bridge` → Run workflow（`.github/workflows/build.yaml`）：输入 `arches`（默认四架构）、`release` + `releaseTag`（可选发布 Release）。四架构并行约 10-15 分钟，产物以 artifact（`so-<abi>`）附加。

本地交叉编译前置：CMake ≥ 3.15、Ninja、**Go ≥ 1.20（BoringSSL 源码编译生成阶段必需，缺了会静默回退系统 OpenSSL，在 Android 交叉编译下走不通）**、NDK r25+：

```sh
export ANDROID_NDK_HOME=/path/to/ndk
cmake -S . -B build/arm64-v8a -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DQJS_USE_BORINGSSL_SOURCE=ON
cmake --build build/arm64-v8a -j$(nproc)
# 产物：build/arm64-v8a/libquickjs_bridge.so
```

`-DQJS_USE_BORINGSSL_SOURCE=ON` 是必需的（仓内无 prebuilt BoringSSL）。

### 引擎测试（宿主可执行，非交叉编译）

`ext/CMakeLists.txt` 可独立构建 `qjs_engine` 静态库 + 测试宿主：

```sh
cmake -S native/quickjs/ext -B build-host -DCMAKE_BUILD_TYPE=Release -DQJS_BUILD_TESTS=ON
cmake --build build-host -j$(nproc)
./build-host/qjs_test_runner native/quickjs/ext/tests/test_all.js   # 全量套件
./build-host/qjs_test_runner path/to/case.js                        # 单个测试 = 传任意 JS 文件
```

测试即 JS 脚本：`test(name, f)` 注册用例，失败时 throw 即非零退出。runner 会泵空微任务队列，async/await（WebCrypto 等）可正常完成。非 Android 平台链接 crypto 模块需系统 OpenSSL（或 Go + 源码 BoringSSL）。

## 架构

两层结构（根 `CMakeLists.txt`）：

1. **`qjs_engine` 静态库**（`native/quickjs/ext/CMakeLists.txt`）：QuickJS 核心 + 全部扩展模块 + wasm3/sqlite3/lexbor/zlib/BoringSSL，自包含、无 JNI 依赖。
2. **`quickjs_bridge` 共享库**（`bridge/quickjs_bridge.c`）：Dart FFI 的稳定 ABI 层。所有 `JSValue`（16 字节 struct）改为堆指针传递、所有回调一律 void 签名——这是本层存在的理由，新增 API 必须维持该模式。

**扩展注册机制**：`quickjs.c` 被补丁过，`JS_NewContext` 末尾自动调 `qjs_ext_install_all()`（`ext/quickjs_ext.cpp`），一次性注册全部 C 模块（Buffer/zlib/fs/crypto/webcrypto/sqlite/wasm/path/HTML/URL/TextCodec）并注入 JS 封装层（Date polyfill/console/wasm.js/sqlite3.js）。所以：**新模块只要加进 `ext/modules/*.c` 并在此函数注册，JS 侧启动即可用**；runtime 级资源经 `rt->ext_data` 挂靠，销毁走 `JS_FreeRuntime → qjs_ext_free`。

**DsPlayer 补丁（per-context 回调）**：`host_call`/`module_load`/`normalize` 三个回调表存在 `JSContext` opaque（`QjsCallbacks` 结构），而非文件级全局——根治多引擎共存时跨 isolate SIGABRT。`qjs_set_callbacks` 首参是 ctx，`qjs_clear_callbacks` 供 Dart dispose。

**`native/cpp/`（JNI wrapper）不参与本仓构建**，是上游遗留的参考代码，根/ext 两级 CMake 均未引用。

## ABI 契约（最高优先级约束）

- `abi/exports.txt` 是 ABI 唯一事实源（51 个导出符号，从现役生产 so 导出）。CI 每次构建比对：**缺符号 = 失败；多符号 = 告警**（需评估同步 Dart 绑定）。
- 导出函数签名与 DsPlayer 的 Dart FFI 绑定严格对应。**改签名 = 改 ABI**，必须同时升 DsPlayer 本体版本并出包，否则旧本体加载新 so 直接 SIGSEGV（2026-09-20 实机事故）。
- 新增导出函数三步缺一不可：① 函数标 `QJS_API`（全库 `-fvisibility=hidden`，不标不导出）；② 同步 `abi/exports.txt`；③ 通知 DsPlayer 侧升版本。
- DsPlayer 侧闸门：`tools/qjs_native/check_abi_compat.py`。

## 关键陷阱

- **行尾**：`.gitattributes` 强制全仓 LF。历史教训：CRLF 的 `\r` 混进 `abi/exports.txt` 导致 CI 符号比对整表判不同。编辑导出清单等基线文件后确认无 CRLF。
- **32 位架构**：`JSValue` 字段直写曾导致 armeabi-v7a 构建失败，已改为跨架构宏（commit abf2209）。写 bridge 层操作 JSValue 内存的代码时沿用现有宏，不要直接按 64 位布局写字段。
- **严格别名**：全库 `-fno-strict-aliasing`（`__js_rc` 指针类型转换所需），不要移除。
- **页对齐**：Android 链接带 `-Wl,-z,max-page-size=16384`（Android 15+ 16KB 页要求）。
- **Windows/MinGW 本地构建**：根 CMakeLists 的 WIN32 分支注入 strndup/sized-delete 补丁与静态 libstdc++，改动链接顺序相关逻辑时注意注释中的符号解析顺序说明。

## 约定

- `AGENTS.md`（供其他 AI agent 的协作入口）是 `CLAUDE.md` 的生成镜像：由 pre-commit 钩子（`.githooks/pre-commit`，启用：`git config core.hooksPath .githooks`）在每次提交前自动重生成。**两份都不要手改 `AGENTS.md`，改动一律落到 `CLAUDE.md`**。
- Commit message：约定式前缀 + 中文描述（`feat:` / `fix:` / `fix(ci):` / `chore:` / `docs:`）。
- 引擎能力权威清单：`docs/quickjs-android-api-docs.html`（**重点 §23 全局速查表**）。写 JS 源或改桥接时的原则：**so 已有的 C 能力（md5/base64/GBK/HTML 解析等）不要用 JS 重造**。
- DsPlayer 侧背景文档（另一仓库）：`docs/plugin/QJS-PLUGIN-DESIGN.md`、`docs/plugin/QJS-PROCESS-ISOLATION-DESIGN.md`（per-context 改造完整背景）、`tools/qjs_native/`（本地构建与 ABI 校验脚本）。
