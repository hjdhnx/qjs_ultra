# ============================================================================
# 根 CMakeLists.txt — quickjs_bridge 共享库
#   - 引擎统一来自 native/quickjs/ext 的自包含静态库 qjs_engine
#     （QuickJS 核心 + 扩展模块 + wasm3 / sqlite3 / zlib / lexbor / BoringSSL）
#   - bridge 层：bridge/quickjs_bridge.c
#   - libc 层：native/quickjs/quickjs-libc.c（js_module_set_import_meta 等支撑）
# 注意：BoringSSL 源码编译需要 Go 工具链（>= 1.18）；
#       可用 -DQJS_USE_BORINGSSL_SOURCE=OFF 切换为系统 OpenSSL/预置头。
# ============================================================================
cmake_minimum_required(VERSION 3.15)
# BoringSSL 为 C++ 编译，纯 C 的 bridge 链接需要 C++ runtime（NDK: c++_static）
set(ANDROID_STL c++_static)
project(quickjs_bridge C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PROJ_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(EXT_DIR ${PROJ_ROOT}/native/quickjs/ext)
set(QJS_DIR ${PROJ_ROOT}/native/quickjs)

# ---- 引擎自包含静态库（含全部扩展与第三方库）----
add_subdirectory(${EXT_DIR} ${CMAKE_BINARY_DIR}/qjs_engine EXCLUDE_FROM_ALL)

# ---- bridge 共享库 ----
add_library(quickjs_bridge SHARED
        bridge/quickjs_bridge.c
        )
# 引擎头（quickjs.h / cutils.h / quickjs-libc.h）与 ext 公共头由
# qjs_engine 的 PUBLIC include 提供，这里补充 quickjs 根目录供 libc 头解析
target_include_directories(quickjs_bridge PRIVATE ${QJS_DIR})
target_compile_definitions(quickjs_bridge PRIVATE _GNU_SOURCE)
target_link_libraries(quickjs_bridge PRIVATE qjs_engine)
# 纯 C 目标不会自动附加 NDK STL，而 BoringSSL(C++) 需要 C++ runtime，显式补齐
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(quickjs_bridge PRIVATE c++_static c++abi)
endif()

# ============================================================================
# 各平台/架构生产优化（编译 + 链接两阶段）
# 通用策略：
#   - O2（解释器循环下比 O3 指令更紧凑、I-Cache 命中更高）+ LTO
#   - -fno-strict-aliasing：__js_rc 指针类型转换违反严格别名规则
#   - -fvisibility=hidden + --exclude-libs,ALL：只导出 bridge API，减小体积
#   - --gc-sections + -ffunction/-fdata-sections：裁剪未使用代码段
# ============================================================================
set(_QJS_LTO_FLAVOR thin)   # clang/NDK 用 thin LTO
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(_QJS_LTO_FLAVOR auto) # gcc 用 auto（等价默认全程序 LTO）
endif()

set(_QJS_OPT_FLAGS
    -O2
    -flto=${_QJS_LTO_FLAVOR}
    -Wno-format-truncation
    -Wno-stringop-overflow
    -fno-strict-aliasing
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)
set(_QJS_LINK_FLAGS
    -flto=${_QJS_LTO_FLAVOR}
    -O2
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
    -Wl,--strip-all
)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    # 16KB 内存页对齐（Android 15+ 要求）
    list(APPEND _QJS_LINK_FLAGS -Wl,-z,max-page-size=16384)
elseif(WIN32)
    # MinGW：无 -fvisibility 生效面（Windows 默认不导出），去掉 gc-sections 不兼容项
    list(REMOVE_ITEM _QJS_LINK_FLAGS -Wl,--exclude-libs,ALL)
    list(APPEND _QJS_LINK_FLAGS -Wl,--kill-at)
    # 需要 C++ runtime（BoringSSL）：静态链接避免 mingw 依赖 dll。
    # 注意：必须挂在 qjs_engine 的 INTERFACE（展开在 boringssl 之后），
    # 否则 libstdc++.a 先于 libcrypto.a 扫描，operator delete 无法回填。
    target_link_libraries(qjs_engine INTERFACE -static-libgcc -static-libstdc++)

    # MinGW CRT 缺 strndup（qjs_url.c 等使用）：注入补丁实现
    file(WRITE ${CMAKE_BINARY_DIR}/mingw_compat_strndup.c
"/* MinGW compat: provide strndup (POSIX) missing in msvcrt */
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\\0';
    return p;
}
")
    add_library(mingw_compat STATIC ${CMAKE_BINARY_DIR}/mingw_compat_strndup.c)
    target_link_libraries(quickjs_bridge PRIVATE mingw_compat)
    # 该工具链 libstdc++.a 缺 sized delete (_ZdlPvm) 定义，BoringSSL C++14 需要。
    # 以目标文件（非静态库）形式挂到 quickjs_bridge：目标文件无条件参与符号
    # 解析，不受归档扫描顺序影响，可直接回填 libcrypto.a 的未定义引用。
    file(WRITE ${CMAKE_BINARY_DIR}/mingw_compat_sized_delete.cpp
"// MinGW compat: this libstdc++ lacks sized deallocation (_ZdlPvm)
"
"void operator delete(void* p, unsigned long long) noexcept { operator delete(p); }
")
    target_sources(quickjs_bridge PRIVATE ${CMAKE_BINARY_DIR}/mingw_compat_sized_delete.cpp)
endif()

if(_QJS_OPT_FLAGS)
    target_compile_options(quickjs_bridge PRIVATE ${_QJS_OPT_FLAGS})
    target_link_options(quickjs_bridge PRIVATE ${_QJS_LINK_FLAGS})
    # LTO 链接需要引擎静态库也参与，将优化传给 qjs_engine 的编译
    target_compile_options(qjs_engine PRIVATE ${_QJS_OPT_FLAGS})
endif()