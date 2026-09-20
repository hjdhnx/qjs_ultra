# ============================================================================
# ext/CMakeLists.txt - 引擎级扩展模块统一静态库构建
#
# 目标：libqjs_engine（STATIC）
#   QuickJS 引擎核心（quickjs.c 等）+ 全部扩展模块（Buffer/zlib/fs/crypto/
#   webcrypto/sqlite/wasm3/path/HTML/URL/TextCodec）+ wasm3 + sqlite3 + lexbor
#
# 所有模块已集中到 native/quickjs/ext/ 目录，无 JNI / Android 依赖，
# 由 quickjs.c 的 JS_NewContext（qjs_ext_install_all）自动完成注册。
#
# 用法（任一平台）：
#   cmake -S native/quickjs/ext -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# 产物：build/libqjs_engine.a（可直接链接的纯静态库）
#
# 头文件使用方 include 路径：native/quickjs/ext/include（qjs_native.h 等）
# ============================================================================

cmake_minimum_required(VERSION 3.10.2)
project(qjs_engine C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 版本号（与根目录构建一致） ----
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION.txt" CONFIG_VERSION)
add_compile_definitions(CONFIG_VERSION="${CONFIG_VERSION}")

# ---- 平台定义（与原 Android 构建保持一致的字符串格式） ----
string(TOLOWER "${CMAKE_SYSTEM_NAME}" JS_PLATFORM)
add_compile_definitions(JS__PLATFORM="${JS_PLATFORM}")

set(EXT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================================
# 1. 引擎核心源码（quickjs 本体）
# ============================================================================
set(QJS_CORE_SRC
    ../cutils.c
    ../dtoa.c
    ../libregexp.c
    ../libunicode.c
    ../quickjs.c          # 内部调用 qjs_ext_install_all / qjs_ext_free
)

# ============================================================================
# 2. 扩展模块源码（ext/modules/，已剥离 JNI 文件）
# ============================================================================
file(GLOB EXT_MODULES_SRC
    ${EXT_DIR}/modules/*.c
)

# ============================================================================
# 3. wasm3 源码（与原 wrapper-android 构建一致的源列表）
# ============================================================================
set(WASM3_SRC
    ${EXT_DIR}/wasm3/m3_api_libc.c
    ${EXT_DIR}/wasm3/m3_bind.c
    ${EXT_DIR}/wasm3/m3_code.c
    ${EXT_DIR}/wasm3/m3_compile.c
    ${EXT_DIR}/wasm3/m3_core.c
    ${EXT_DIR}/wasm3/m3_env.c
    ${EXT_DIR}/wasm3/m3_exec.c
    ${EXT_DIR}/wasm3/m3_function.c
    ${EXT_DIR}/wasm3/m3_info.c
    ${EXT_DIR}/wasm3/m3_module.c
    ${EXT_DIR}/wasm3/m3_parse.c
)

# ============================================================================
# 4. 引擎级扩展注册入口（quickjs_ext.cpp，C 链接导出）
# ============================================================================
set(EXT_ENTRY_SRC ${EXT_DIR}/quickjs_ext.cpp)

# ============================================================================
# 5. lexbor 源码收集（递归收集 source/ 下全部 .c）
# ============================================================================
file(GLOB_RECURSE LEXBOR_SRC ${EXT_DIR}/lexbor/source/*.c)
# 按平台选择端口实现：POSIX（Linux/macOS/Android）与 Windows（windows_nt）互斥
list(FILTER LEXBOR_SRC EXCLUDE REGEX "/ports/")
if(WIN32)
    file(GLOB LEXBOR_WIN_SRC ${EXT_DIR}/lexbor/source/lexbor/ports/windows_nt/lexbor/core/*.c)
    list(APPEND LEXBOR_SRC ${LEXBOR_WIN_SRC})
else()
    file(GLOB LEXBOR_POSIX_SRC ${EXT_DIR}/lexbor/source/lexbor/ports/posix/lexbor/core/*.c)
    list(APPEND LEXBOR_SRC ${LEXBOR_POSIX_SRC})
endif()

# ============================================================================
# 6. zlib 1.3.1 源码（ext/zlib/，随引擎编译，无需系统 zlib）
# ============================================================================
file(GLOB ZLIB_SRC
    ${EXT_DIR}/zlib/adler32.c
    ${EXT_DIR}/zlib/compress.c
    ${EXT_DIR}/zlib/crc32.c
    ${EXT_DIR}/zlib/deflate.c
    ${EXT_DIR}/zlib/gzclose.c
    ${EXT_DIR}/zlib/gzlib.c
    ${EXT_DIR}/zlib/gzread.c
    ${EXT_DIR}/zlib/gzwrite.c
    ${EXT_DIR}/zlib/infback.c
    ${EXT_DIR}/zlib/inffast.c
    ${EXT_DIR}/zlib/inflate.c
    ${EXT_DIR}/zlib/inftrees.c
    ${EXT_DIR}/zlib/trees.c
    ${EXT_DIR}/zlib/uncompr.c
    ${EXT_DIR}/zlib/zutil.c
)

# ============================================================================
# libqjs_engine 静态库
# ============================================================================
add_library(qjs_engine STATIC
    ${QJS_CORE_SRC}
    ${EXT_MODULES_SRC}
    ${EXT_ENTRY_SRC}
    ${WASM3_SRC}
    ${EXT_DIR}/sqlite3/sqlite3.c
    ${ZLIB_SRC}
    ${LEXBOR_SRC}
)

# ---- 性能优化：LTO + O2（Release 默认开启，可用 QJS_ENABLE_LTO=OFF 关闭） ----
option(QJS_ENABLE_LTO "Enable Link Time Optimization (Release)" ON)
include(CheckIPOSupported)
if(QJS_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    check_ipo_supported(RESULT _qjs_ipo_supported OUTPUT _qjs_ipo_msg)
    if(_qjs_ipo_supported)
        set_target_properties(qjs_engine PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "qjs_engine: LTO enabled (O2 + -flto)")
    else()
        message(STATUS "qjs_engine: LTO not supported by toolchain, fallback to plain O2 (${_qjs_ipo_msg})")
    endif()
endif()

# zlib：启用 unistd.h（gzlib 等用到 lseek，clang 下隐式声明为 error）
target_compile_definitions(qjs_engine PRIVATE HAVE_UNISTD_H=1)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qjs_engine PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-strict-aliasing>   # __js_rc 指针类型转换要求
        -ffunction-sections
        -fdata-sections
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(qjs_engine PRIVATE -O2)
    endif()
endif()

target_include_directories(qjs_engine PUBLIC
    ${EXT_DIR}/include                       # qjs_native.h 等公共头
    ${EXT_DIR}/boringssl/include             # BoringSSL/OpenSSL 头（crypto/webcrypto 模块编译需要）
    ${EXT_DIR}/wasm3                         # m3_env.h 等
    ${EXT_DIR}/lexbor/source                 # lexbor
    ${EXT_DIR}/sqlite3                       # sqlite3.h
    ${EXT_DIR}/modules                       # qjs_index.h / zlib_internal.h / qjs_openssl.h 等
    ${EXT_DIR}/zlib                          # zlib.h / zconf.h
    ${CMAKE_CURRENT_SOURCE_DIR}/..           # quickjs.h / cutils.h（引擎头）
)

target_compile_definitions(qjs_engine PRIVATE
    LEXBOR_STATIC
    # 排除 qjs_fs 等模块对 JNI 的依赖（如有需要可按模块补充宏）
)

# ---- BoringSSL（谷歌版）：源码编译 > 系统 OpenSSL，两级回退 ----
option(QJS_USE_BORINGSSL_SOURCE "Build BoringSSL from source in ext/boringssl" ON)

if(DEFINED CMAKE_ANDROID_ARCH_ABI)
    set(QJS_ABI ${CMAKE_ANDROID_ARCH_ABI})
elseif(ANDROID_ABI)
    set(QJS_ABI ${ANDROID_ABI})
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(QJS_ABI arm64-v8a)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(QJS_ABI armeabi-v7a)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set(QJS_ABI x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i686|x86")
    set(QJS_ABI x86)
else()
    set(QJS_ABI arm64-v8a)
endif()


if(QJS_USE_BORINGSSL_SOURCE AND EXISTS ${EXT_DIR}/boringssl/CMakeLists.txt)
    # 方式一：源码随引擎编译（需要 Go >= 1.18 用于生成汇编/表格）
    find_program(GO_EXECUTABLE go)
    if(NOT GO_EXECUTABLE)
        message(WARNING "qjs_engine: 找到 boringssl 源码但缺少 Go 工具链，无法源码编译 BoringSSL，回退到系统 OpenSSL")
    else()
        set(BUILD_TESTING OFF CACHE INTERNAL "" FORCE)
        set(BORINGSSL_BUILD_SOURCE OFF)   # 不做 FIPS 等附加构建
        add_subdirectory(${EXT_DIR}/boringssl ${CMAKE_BINARY_DIR}/boringssl EXCLUDE_FROM_ALL)
        target_link_libraries(qjs_engine INTERFACE ssl crypto)
        target_include_directories(qjs_engine INTERFACE ${EXT_DIR}/boringssl/include)
        message(STATUS "qjs_engine: BoringSSL built from source (ext/boringssl, Go=${GO_EXECUTABLE})")
        set(QJS_SSL_RESOLVED ON)
    endif()
endif()

if(NOT QJS_SSL_RESOLVED)
    # 方式二：系统 OpenSSL
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        target_link_libraries(qjs_engine INTERFACE OpenSSL::Crypto OpenSSL::SSL)
        message(STATUS "qjs_engine: system OpenSSL")
        set(QJS_SSL_RESOLVED ON)
    endif()
endif()

if(NOT QJS_SSL_RESOLVED)
    message(WARNING
        "qjs_engine: 未找到 BoringSSL 源码/系统 OpenSSL。\n"
        "crypto/webcrypto 模块将无法链接。可选：\n"
        "  1. 放置 BoringSSL 源码到 ext/boringssl/（源码编译，需 Go）\n"
        "  2. 安装系统 OpenSSL（libssl-dev）")
endif()

# ---- 系统库 ----
find_package(Threads REQUIRED)
target_link_libraries(qjs_engine INTERFACE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(qjs_engine INTERFACE m dl)
elseif(APPLE)
    target_link_libraries(qjs_engine INTERFACE m)
elseif(WIN32)
    target_link_libraries(qjs_engine INTERFACE ws2_32 bcrypt)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(qjs_engine INTERFACE log)   # zlib 已源码内置，无需 -lz
endif()

# ---- 编译选项（与原 Android 构建保持一致的优化策略，见上方 LTO + O2 段） ----

# ---- 测试宿主（可执行文件） ----
option(QJS_BUILD_TESTS "Build and run qjs_engine test runner" OFF)
if(QJS_BUILD_TESTS)
    add_executable(qjs_test_runner tests/qjs_test_runner.c)
    target_link_libraries(qjs_test_runner PRIVATE qjs_engine)
    # 注意：crypto/webcrypto 模块需要 BoringSSL/OpenSSL 符号。
    # CI 交叉构建使用 QJS_USE_BORINGSSL_SOURCE=OFF（静态库不做最终链接），
    # 运行测试的平台须由 CI 脚本提供系统 OpenSSL（如 libssl-dev）或开启源码 BoringSSL。
endif()

message(STATUS "qjs_engine: engine + all extension modules -> libqjs_engine.a (JS_PLATFORM=${JS_PLATFORM})")
