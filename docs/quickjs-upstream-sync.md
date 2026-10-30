# QuickJS 上游同步记录

> 目的：记录本仓 vendored QuickJS 的基线、本地补丁、与官方的差异，以及已验证的
> 同步路径。官方来源：<https://bellard.org/quickjs/> / <https://github.com/bellard/quickjs>。

## 当前基线

- **官方版本：2026-06-04**（当前最新正式 release，与 `native/quickjs/VERSION.txt` 一致）
- 该版本相对 2025-09-13 **bench-v8 快 42%**（小对象 custom malloc + 微优化），
  这部分性能已经在本仓生效。
- 本地补丁只改 3 个文件，共约 720 行（`diff` 统计 +607/−2）：
  - `quickjs.c`：`JSRuntime.ext_data` / `JSContext` 回调槽、`JS_NewContext` 末尾
    `qjs_ext_install_all()`、`JS_FreeRuntime` 的 `qjs_ext_free()`、atob/btoa /
    performance intrinsic、`JS_NewUint8ArrayCopy` 等；
  - `quickjs.h`：上述 API 声明；
  - `cutils.h`：少量跨平台辅助声明。
- 其余核心文件（quickjs-libc、cutils.c、dtoa、libregexp、libunicode 等）与
  官方 2026-06-04 逐字节一致；`ext/` 为本仓扩展与第三方 vendored 代码。

## 上游 2026-06-04 之后的现状（2026-10-30 检查）

官方 master 领先 release **81 个提交**，核心 7 个文件 +5041/−784（`quickjs.c`
+5612 行）。主要内容：

- **新特性（未发版，不建议整体跟）**：Temporal + Intl（新增 `intl/` 1.8MB、
  `temporal/` 1.2MB，默认 `CONFIG_TEMPORAL_INTL=y`，32 位主机禁用）、explicit
  resource management（`using`）、`Atomics.waitAsync`、`Array.fromAsync`、
  `Iterator.zip/join/includes`、`Promise.allKeyed` 等。
- **性能提交（6 个）**：`445624b` `8f0c037` `088cf5e` `762e6fc`（比较/相等运算
  内联）、`42d08be`（`Array.prototype.slice/splice`）、`888481d`
  （`String.prototype.concat`）。
- **修复/稳健性提交**：UAF（`cd97c2b` build_backtrace OOM）、内存泄漏
  （`79f58c1` BigInt、`c8ef889` Atomics.store）、死循环（`817509a`
  unicode_script）、`Array.prototype.sort` 写回（`9a0197f`）、模板字符串替换
  （`f8199dc`）、Proxy ownKeys（`1134912`）、ArrayBuffer.resize（`5b6356a`）、
  TypedArray.at（`8ac9d9b`）等。

## 已实测（2026-10-30）

在官方 release 树上叠加本仓本地补丁后：

1. **本地补丁可干净应用到官方 master**：`git apply --3way` 全部成功，无冲突；
2. **6 个性能提交全部可 clean cherry-pick**，编译通过（MinGW gcc，
   `-O2 -c quickjs.c`）；
3. **14 个修复提交可 clean cherry-pick**：`cd97c2b` `79f58c1` `817509a`
   `9a0197f` `f8199dc` `1134912` `5b6356a` `8ac9d9b` `13876a2` `55c5f0b`
   `a479eff` `c17f7fc` `abf571a` `19d6bd8`；
4. 合计 20 个提交的树**编译通过**；
5. 以下 10 个修复 cherry-pick 有冲突，需逐个手工评估：`1ec58aa` `4984249`
   `cfc9464` `1aa7429` `fce112d` `efc48a3` `ce0dbb7` `535a7c2` `d88b1ed`
   `9d01a96`。

## 同步策略（建议）

- **生产**：继续锁 2026-06-04 正式 release（master 未发版且含实验性 Temporal/Intl）。
- **跟进**：开独立分支 `sync/quickjs-upstream`，先 backport 上面 20 个已
  验证的 perf + fix 提交；冲突的 10 个逐个处理。验证通过后再合入 main。
- **整体升级**：等官方下一次 release，再按“本地补丁 → 新 release”的方式
  rebase；届时若接 Temporal/Intl，需要 CMake 增加 `intl/`、`temporal/` 源文件
  与数据，并处理 32 位（armeabi-v7a/x86）禁用分支。

## 复现命令

```sh
# 1. 取官方基线
git clone https://github.com/bellard/quickjs.git /tmp/quickjs-upstream
git -C /tmp/quickjs-upstream worktree add /tmp/qjs-release 3d5e064

# 2. 生成“本仓本地补丁”（在 release 树上覆盖 3 个文件后）
cp native/quickjs/{quickjs.c,quickjs.h,cutils.h} /tmp/qjs-release/
git -C /tmp/qjs-release diff > /tmp/local_qjs.patch

# 3. 验证补丁可应用到官方 master
git -C /tmp/quickjs-upstream worktree add -b master-test /tmp/qjs-master master
git -C /tmp/qjs-master apply --3way /tmp/local_qjs.patch

# 4. cherry-pick 已列出的 perf/fix 提交后，至少跑
#    - native/quickjs/ext/tests/test_all.js（宿主 runner）
#    - dart test（真实 so/dll）
#    - CI 四架构构建
```

## 风险点

- master 的 Temporal/Intl 被官方标注为 experimental，代码量约 3MB（含 locale/
  tzdata），对 so 体积与内存有实际影响。
- 部分修复依赖 master 的新 API（如 `21cbd86` 引入 `JS_ToInt64SatF`），
  cherry-pick 顺序与依赖需逐个确认。
- 任何对 `native/quickjs/` 的改动都应在 commit message 中注明来源 commit 与
  验证方式（本仓约定）。
