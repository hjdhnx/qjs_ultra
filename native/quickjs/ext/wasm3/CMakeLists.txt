# Copyright © 2019 Volodymyr Shymanskyy. All rights reserved.
# Copyright 2024 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.1)

project(wasm3 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 2. 安卓专用优化选项（瘦身+性能）
if(ANDROID)
    add_compile_options(
        -O2                          # 解释器调度循环在 -O2 下指令更紧凑，L1I 缓存命中率更高
        -flto=thin
        -fno-strict-aliasing
        -fvisibility=hidden          # 隐藏符号，减小 so 体积
        -ffunction-sections          # 拆分段，配合链接器瘦身
        -fdata-sections
    )
    add_link_options(
        -O2
        -flto=thin
        -Wl,--gc-sections            # 移除未使用代码
        -Wl,--strip-all              # 剥离符号，极致瘦身
        -fuse-ld=lld                 # 用 lld 链接，速度更快
    )
endif()

set(sources
    "extensions/m3_extensions.c"
    "m3_api_libc.c"
    "m3_api_wasi.c"
    "m3_api_uvwasi.c"
    "m3_api_meta_wasi.c"
    "m3_api_tracer.c"
    "m3_bind.c"
    "m3_code.c"
    "m3_compile.c"
    "m3_core.c"
    "m3_env.c"
    "m3_exec.c"
    "m3_function.c"
    "m3_info.c"
    "m3_module.c"
    "m3_parse.c"
)

# 3. 编译为静态库（安卓推荐用静态库，由 Gradle 链接）
add_library(m3 STATIC ${sources})

target_include_directories(m3 PUBLIC .)

# 4. 安卓场景默认关闭 WASI（按需开启）
target_compile_definitions(m3 PRIVATE -Dd_m3HasWASI)

if(BUILD_WASI MATCHES "simple")
    target_compile_definitions(m3 PUBLIC d_m3HasWASI)
elseif(BUILD_WASI MATCHES "metawasi")
    target_compile_definitions(m3 PUBLIC d_m3HasMetaWASI)
elseif(BUILD_WASI MATCHES "uvwasi")
    target_compile_definitions(m3 PUBLIC d_m3HasUVWASI)
    include_directories("${libuv_SOURCE_DIR}/include")
endif()
