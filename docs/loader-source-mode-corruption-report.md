# 源码模式 module loader 随机损坏报告（DSPlayer dr3 实测取证）

> 状态：**已修复（2026-09-30，根因见下节）** ｜ 报告日期：2026-09-30 ｜ 提交方：DsPlayer（drpy3 源运行时宿主）
> 关联版本：libquickjs_bridge.dll（Windows 本机构建，2026-09-30）｜ 复现测试：`test/loader_source_corruption_test.dart`

## 根因（修复时确认，非本报告原判断）

**quickjs 引擎契约：`JS_Eval` 的 input 必须满足 `input[input_len] == '\0'`**
（`native/quickjs/quickjs.c` `__JS_EvalInternal` 头注释；lexer `next_token`
顶端无条件 `c = *p` 解引用，EOF 判定是 `*p == 0 && p >= buf_end`）。
`module_loader_trampoline` 源码模式的 buf 按回调契约只含 len 字节、**无终结符**，
lexer 读穿 buf 把相邻堆字节当源码继续解析 → 随机假语法错误。

原文「堆越界写」推断不成立，实为**越界读**：无写操作。全部指纹由此统一解释——

| 指纹 | 解释 |
|---|---|
| bytecode 模式零挂 | `JS_ReadObject` 是长度界读，不依赖终结符 |
| 直调 `evaluateModule` 零挂 | Dart `toNativeUtf8()` 自带 `\0`，契约恰好满足 |
| 5.4KB 小源全挂 / 42KB 大源零挂 | 小 buf 位于堆中部、后邻活跃块（非零垃圾被当 token）；大 buf 在堆顶、后邻新零页，越界首读即 `\0` → 立即 EOF，解析照常成功 |
| 挂率随轮次/布局随机 | 越界读到的垃圾内容随堆邻居变化 |
| heisenbug（加桥调用即消失） | 堆布局整体移位，垃圾邻居换人 |

修复：`bridge/quickjs_bridge.c` `module_loader_trampoline` 源码模式在交引擎前
统一拷贝补 `\0`（回调协议保持「len 字节」不变，任何宿主都安全）。
A/B 验证：仅给 buf 补终结符（Dart 侧临时补丁、引擎未动），旧 dll 哨兵组
15/30 挂 → 连跑 3 次全绿；下述排查方向 1-5 不再需要。

## 摘要（TL;DR）

**源码模式**的 module loader 管线存在随机损坏：loader 回调返回源码文本后，C 侧
`JS_Eval` 编译该文本时以 ~10-50% 概率报词法/引用级**假**语法错误（源码本身合法，
node --check 验证过）。同管线返回 **bytecode**（`JS_ReadObject` 反序列化）则
零故障。对照实验（同引擎 30 次全新模块 import）：**源码 loader 15/30 挂，
bytecode loader 0/30**。

集成层（DSPlayer drpy3）已切 bytecode 装载绕开，**但源码模式分支的 C 层根因
未修**，本报告提供归因数据、复现测试与排查方向。

## 现象

### 错误指纹

编译合法源码时报词法/引用级假错误，**同一份源码不同轮次报不同错误**：

```
JsEvalException: SyntaxError: unexpected end of string
JsEvalException: SyntaxError: unexpected character
JsEvalException: SyntaxError: invalid number literal
JsEvalException: SyntaxError: expecting ';'
JsEvalException: SyntaxError: unexpected token in expression: ''
JsEvalException: ReferenceError: 'c' is not defined   ← 残缺标识符（执行期形态）
```

### 关键特征

| 特征 | 数据 |
|---|---|
| 挂率（源码 loader，全新模块名逐个 import） | ~14%（DSPlayer 完整链路实测）/ 50%（对照实验，bm1 源 15/15 全挂） |
| bytecode loader 同场景 | **0 挂** |
| 直调 `evaluateModule`（不经 loader 回调） | 数十次零挂 |
| 与栈大小无关 | 4MB / 64MB / 512MB 三档同挂 |
| 与跨源复用无关 | 每轮全新引擎只载一源同挂 |
| 与源大小正相关**反转** | 5.4KB 的源比 42KB 的挂率显著更高（见下） |
| heisenbug | 回调路径插入 console.log 桥调用即消失（堆布局/时序改变） |
| 挂率与具体源强相关 | 同实验内小源 15/15 全挂、大源 0/15——每轮 `free(buf)` 后 `malloc` 同尺寸复用同块地址，呈现**地址复用相关的稳定模式**（指向该块内存在两次分配之间被越界写破坏） |

### 影响面

- DSPlayer drpy3 源每次装载 ~14% 假错（手机端「源加载失败」高频反馈）；
- DSPlayer 质量门三个「深水区」测试用例自 2026-09-28 起随机挂，bytecode 化后
  全部转绿（`benchmark_sources_test`、`drpy3_acceptance_test`×2）；
- drpy2 宿主不受影响——它走 bytecode loader + 依赖树拍平，不走源码模式。

## 排除清单（已实证排除，勿重走）

| 假设 | 排除证据 |
|---|---|
| 源码本身语法问题 | `node --check` 通过；同源码 `evaluateModule` 直调稳定编译 |
| NUL 截断（Dart `toDartString()` 无长度读） | 挂的源里 NUL 字节数为 0（`python data.count(b'\x00')` = 0） |
| 跨堆 free（Windows DLL CRT 与 Dart malloc 不同堆） | `qjs_alloc_buffer` 与 C 侧 `free` 同在 quickjs_bridge.c/同一 malloc 实例；且 bytecode 模式走同一对 alloc/free 零故障 |
| 栈溢出/栈检查误报 | 4/64/512MB 三档 `setMaxStackSize` 同挂 |
| 字符串边界转换截断 | Dart→JS、JS→Dart、往返 echo 45KB/3MB 全部逐字节相等 |
| bundle/宿主 JS 逻辑（正则 lastIndex、模式判定分叉） | 剥离 bundle 的纯引擎复现同样挂（见复现测试） |
| 引擎实例复用/跨源状态 | 每轮新建引擎单源装载同挂 |
| isolateLocal 回调本身 | bytecode 模式走同一回调（同一 `module_loader_trampoline`）零故障 |

## 决定性对照实验

同一引擎、30 个**唯一名**模块（⚠️ 同名 import 走 quickjs 模块缓存不会重新编译，
必须唯一名）、两个真实爬虫源交替、逐个 `evaluateAsync('import("mod_i")')`：

| loader 模式 | 回调返回 | C 侧消费 | 结果 |
|---|---|---|---|
| 源码模式 | `getModuleSource` → utf8 文本 | `JS_Eval` 文本编译 | **15/30 挂** |
| bytecode 模式 | `getModuleBytecode` → `compileModule` 产物 | `JS_ReadObject` 反序列化 | **0/30** |

两条路径的差异**只有一处**：回调返回物进 C 后是「文本 parser」还是「二进制
反序列化」。回调本身、`allocBuffer`/`free` 配对、isolateLocal 机制均相同——
**损坏不在回调，在「文本 buf → parser 编译」这一段之间或期间**。

复现测试已按本仓测试风格落盘：`test/loader_source_corruption_test.dart`
（`QJS_ULTRA_LIB` 指定库路径，`dart test test/loader_source_corruption_test.dart`
即跑；源文件路径为参数，见文件头说明）。

## 损坏段定位（C 管线引用）

`bridge/quickjs_bridge.c` `module_loader_trampoline` 源码模式分支：

```c
int32_t mode = cb->module_load(ctx, module_name, &buf, &len);   // Dart isolateLocal 回调
...
const char *script = (const char *)buf;
size_t script_len = (size_t)len;
int res = js_module_test_json(ctx, attributes);                 // ← 审查点 ①
...
JSValue func_val = JS_Eval(ctx, script, script_len, module_name,
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);       // ← 损坏表现处
free(buf);                                                       // ← 审查点 ②
```

Dart 侧 `_moduleLoadInner`：`allocBuffer(len)` 分配（**未加 NUL 终结符**）→
`setAll` 拷贝 → 返回。

### 排查方向（按建议优先级）

1. **ASAN 取证（首选，一抓一个准）**：Windows 构建加
   `-fsanitize=address -fno-omit-frame-pointer` 跑复现测试——
   heap-buffer-overflow / use-after-free 会直接指名肇事者。
   挂率的「同尺寸地址复用」模式说明：某处对 malloc 堆的越界写恰好落在
   loader buf 复用的块上；ASAN 的红区会让它在第一次越界就爆。
2. **审查点 ① `js_module_test_json`**（本仓自加的 import attributes 检查，
   非原生 quickjs）：它在 JS_Eval 前执行且每轮都跑，审查其对 `attributes`
   （JSValueConst，可能为 undefined）与字符串操作的边界。
3. **审查点 ② NUL 终结符试验（低成本）**：Dart 侧 `allocBuffer(len + 1)` 补
   `buf[len] = 0`。`JS_Eval` 虽按 len 语义，但 parser 报错路径（错误消息构造、
   标识符 intern）可能存在读穿 buf 的路径——若补 NUL 后挂率显著变化，即锁定
   读穿点。
4. **越界写来源扫描**：挂率随「同尺寸重复分配」上升——怀疑堆内存在
   loader buf 两次分配的间隙被写坏。扫描 so 内其它 `memcpy`/`setAll` 调用点
   的长度计算（chars vs bytes、int16 截断）；重点复核
   `qjs_to_cstring`/`qjs_stringify`/`qjs_get_bytes` 的调用方 free 尺寸。
5. **parser 期间 buf 稳定性**：确认 `JS_Eval` 编译期间没有任何路径会再次进入
   Dart 回调（重入会改变堆布局；本实验为同步 eval，理论无重入，ASAN 可证）。

### 修复验收标准

- 复现测试 30 模块循环 N=50 连跑 3 次全绿；
- 真实源（含 5.4KB 小源）装载挂率归零；
- bytecode 模式行为不变（`JS_ReadObject` 分支回归）。

## 集成层已实施的绕开（供参考，非替代上游修复）

DSPlayer drpy3 宿主已全部切 bytecode：源模块与引擎 bundle 本体均
`compileModule` 直调编译（直调路径实测零挂）后经自定义 `JsModuleLoader`
返回字节码；import 依赖树由模块机制照常承接；源真语法错误在
`compileModule` 处确定性抛出。该绕开同时带来编译缓存收益（对齐 drpy2 的
bytecode 缓存架构），即使上游修复后也会保留——但**源码模式分支仍被
drpy2 的 `getModuleSource` 兜底路径与其他宿主使用，根因仍需修复**。

## 环境信息

- 平台：Windows x64（本机构建 quickjs_bridge.dll）+ Android arm64
  （真机 libquickjs_bridge.so，行为一致）；
- 集成版本：DSPlayer pubspec 钉 `qjs_ultra d5c01609ae160201b9fd20a2497c351a0f5a163c`；
- 报告与复现测试提交自 DSPlayer 诊断会话（完整诊断过程存档于 DSPlayer
  仓库记忆 `dr3-qjs-corruption-diagnosis`）。
