# 可选插件注入（编译期开关）

本仓的全局能力由 `native/quickjs/ext/quickjs_ext.cpp` 的
`qjs_ext_install_all()` 在 `JS_NewContext` 末尾统一注册：每个插件是
`native/quickjs/ext/modules/*.c` 里的一个模块，暴露

```c
int qjs_xxx_init(JSContext *ctx, JSValueConst global_obj);
```

内部用 `JS_SetPropertyStr` / `JS_SetPropertyFunctionList` 把能力挂到全局对象。
CMake 用 `file(GLOB modules/*.c)` 收集源文件，所以新增插件**默认参与构建**。

## 让插件变成“编译期可选”（以 JSON5 为例）

1. `modules/qjs_json5.c`：实现 `qjs_json5_init()` 与 `JSON5.parse/stringify`；
2. `include/qjs_native.h`：声明 `int qjs_json5_init(JSContext *, JSValueConst);`
3. `quickjs_ext.cpp`：注册调用包在开关里
   ```cpp
   #ifdef QJS_ENABLE_JSON5
   if (qjs_json5_init(ctx, global_obj) != 0) { ... }
   #endif
   ```
4. `ext/CMakeLists.txt`：
   ```cmake
   option(QJS_ENABLE_JSON5 "..." OFF)
   if(QJS_ENABLE_JSON5)
       add_compile_definitions(QJS_ENABLE_JSON5=1)
   else()
       list(FILTER EXT_MODULES_SRC EXCLUDE REGEX "qjs_json5\\.c$")
   endif()
   ```
   `OFF` 时源文件不参与编译，产物里既没有符号也没有全局对象；
   `ON` 时编译并注册。根构建直接传 `-DQJS_ENABLE_JSON5=ON` 即可。
5. CI：`workflow_dispatch` 的 `json5` 输入会透传
   `-DQJS_ENABLE_JSON5=ON/OFF`（push 触发的构建默认 OFF）。

## JSON5 插件

- 开关：`-DQJS_ENABLE_JSON5=ON`（默认 OFF，适合做“按需裁剪二进制”的实验）；
- 全局对象：`JSON5`，与官方 [json5](https://json5.org/) 包语义对齐：
  - `JSON5.parse(text[, reviver])` — 完整 JSON5 语法 + reviver（自底向上，
    返回 `undefined` 删除属性）；
  - `JSON5.stringify(value[, replacer[, space]])` — 官方风格输出（裸键、
    单/双引号自动选择、尾逗号、`space` 缩进、`quote` 选项、replacer
    函数/数组、`toJSON5`/`toJSON`、循环检测、BigInt 抛 TypeError）；
  - `JSON5.version` = `2.2.3`。
- 实现：
  - **parse**：vendored [libpdjson5](https://github.com/boris-kolpackov/libpdjson5)
    （Unlicense，事件流解析器；完整 JSON5 语法、UTF-8/代理对/内嵌 NUL），
    本仓 `qjs_json5.c` 把事件流组装成 JSValue；
  - **stringify**：官方 json5 的 stringify 算法（MIT，Aseem Kishore 等），
    作为插件注入的 JS 助手（`qjs_json5_stringify.js`），仅把 Unicode
    标识符判断换成 `\p{ID_Start}`/`\p{ID_Continue}`。
- 覆盖语法：行/块注释、裸键、单/双引号、尾逗号、十六进制、`+`/`-`、
  `Infinity`/`NaN`、前导/尾随小数点、指数、续行与跨行字符串、
  `\x`/`\u` 转义与代理对、NBSP/BOM/U+2028/U+2029 空白；
- 与官方的已知差异：Unicode 空白识别使用上游 libpdjson5 的“常见 Zs 子集”
  （NBSP/BOM/U+2028/U+2029 已覆盖）；`stringify` 对 BigInt 抛错、不输出
  JSON5 未定义的扩展类型，与官方一致；
- 测试：`test/engine_test.dart` 的 “JSON5 插件（可选全局能力）” 组；
  未启用时自动 skip，启用后跑完整用例（语法矩阵、reviver、stringify
  选项、循环/BigInt、`__proto__` 不污染原型）。

## 已验证的两种产物

同一个 CMake 构建目录切换开关即可：

```sh
cmake -S . -B build/x -DQJS_ENABLE_JSON5=OFF   # 默认：无 JSON5，66 测试 7 skip
cmake -S . -B build/x -DQJS_ENABLE_JSON5=ON    # 注入 JSON5，73 测试全跑
cmake --build build/x
```

## 一条命令看完整能力矩阵

```sh
dart run tool/capability_check.dart [动态库路径]
```

脚本会真实加载 so/dll，逐项验证 §23 全部全局能力（cheerio/URL/TextCodec/
Buffer/crypto/fs/zlib/SQLite/wasm/timers/JSON5…），输出 OK/FAIL/SKIP 表格；
JSON5 未编译时显示 SKIP，有 FAIL 时退出码 1。CI 的 dart-check job 也会跑它。

新增可选插件时复制这套四步即可；已注册的必选插件（cheerio/URL/TextCodec 等）
不受影响。
