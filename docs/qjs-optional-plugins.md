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
- 全局对象：`JSON5`，API：
  - `JSON5.parse(text)` — 返回解析结果；
  - `JSON5.stringify(value[, replacer[, space]])` — 复用 `JSON.stringify`
    （JSON 输出本身是合法 JSON5，暂不生成裸键/单引号风格）；
- 覆盖语法：行/块注释、裸键、单/双引号、尾逗号、十六进制、`+`/`-`、
  `Infinity`/`NaN`、前导/尾随小数点、指数、续行与跨行字符串、
  `\x`/`\u` 转义与代理对、NBSP/BOM/U+2028/U+2029 空白；
- 限制：`parse` 暂不支持 `reviver`；`stringify` 不复刻 JSON5 风格化输出；
- 测试：`test/engine_test.dart` 的 “JSON5 插件（可选全局能力）” 组；
  未启用时自动 skip，启用后跑完整用例（含 `__proto__` 不污染原型）。

## 已验证的两种产物

同一个 CMake 构建目录切换开关即可：

```sh
cmake -S . -B build/x -DQJS_ENABLE_JSON5=OFF   # 默认：无 JSON5，66 测试 4 skip
cmake -S . -B build/x -DQJS_ENABLE_JSON5=ON    # 注入 JSON5，70 测试全跑
cmake --build build/x
```

新增可选插件时复制这套四步即可；已注册的必选插件（cheerio/URL/TextCodec 等）
不受影响。
