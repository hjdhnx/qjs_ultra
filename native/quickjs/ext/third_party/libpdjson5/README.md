# libpdjson5（vendored）

- 上游：<https://github.com/boris-kolpackov/libpdjson5>
- 版本：1.0.1（commit `ed602821f2929930e5179cd9240d468abf210d60`，2026-08-06）
- 许可证：Unlicense（public domain），见 `UNLICENSE`
- 文件：`pdjson5.c` / `pdjson5.h`（未做任何修改）

用途：`native/quickjs/ext/modules/qjs_json5.c` 的 JSON5 **parser** 后端。
选择理由：

- 完整 JSON5 语法（注释、裸键、单/双引号、尾逗号、十六进制、±Infinity/
  NaN、前导/尾随小数点、续行/跨行字符串、`\x`/`\u` 与代理对、完整 UTF-8、
  字符串内嵌 NUL）；
- 纯 C99、无外部依赖、流式拉取解析（事件 API），内存占用可控；
- 正确性/健壮性优先（上游有 fuzzing 与测试套件），Unlicense 与 MIT 兼容。

同步方式：替换 `pdjson5.c` / `pdjson5.h` / `UNLICENSE` 三个文件即可；本仓
没有对上游源码做本地修改。`qjs_json5.c` 只依赖公开 API，不触碰内部结构。
