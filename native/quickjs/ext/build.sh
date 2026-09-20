#!/usr/bin/env bash
# ============================================================================
# build.sh - QuickJS 扩展引擎多平台多架构一键编译脚本
#
# 用法:
#   ./build.sh <target> [jobs]
#
# Targets:
#   android-arm64-v8a | android-armeabi-v7a | android-x86_64 | android-x86
#   linux-x86_64 | linux-aarch64
#   ios-device-arm64 | ios-simulator-arm64 | ios-simulator-x86_64
#   macos-arm64 | macos-x86_64
#   windows-msvc-x64 | windows-msvc-arm64   (需在 Windows/MSVC 环境执行)
#   windows-mingw-x64
#   all-android      (依次编 Android 四个 ABI)
#
# 环境要求:
#   Android : ANDROID_NDK_HOME 指向 NDK (r25+)，cmake >= 3.22，ninja
#   Linux   : 本机 gcc/clang；aarch64 交叉编译需 g++-aarch64-linux-gnu
#   iOS/macOS: macOS + Xcode，cmake
#   Windows : MSVC (VS2022) 或 MinGW-w64
#   BoringSSL 源码编译额外需要 Go >= 1.20 (可 QJS_USE_BORINGSSL_SOURCE=OFF 关闭，
#             Android 下会自动回退到 ext/prebuilt/<abi>/ 预编译库)
# ============================================================================
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_DIR="$SCRIPT_DIR"
BUILD_ROOT="$EXT_DIR/build"
JOBS="${2:-$(nproc 2>/dev/null || echo 4)}"

log() { printf '\033[1;32m[build]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[warn]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
do_android() {
    local abi="$1"
    command -v cmake >/dev/null || die "需要 cmake"
    [ -n "$ANDROID_NDK_HOME" ] || die "请设置 ANDROID_NDK_HOME 指向 NDK 根目录"
    local preset="android-$abi"
    local bdir="$BUILD_ROOT/$preset"

    log "Android $abi: 配置 (NDK=$ANDROID_NDK_HOME)"
    cmake -S "$EXT_DIR" -B "$bdir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Android \
        -DCMAKE_ANDROID_NDK="$ANDROID_NDK_HOME" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM=android-24 \
        -DQJS_USE_BORINGSSL_SOURCE=OFF   # Android 命中 prebuilt/，无需源码编

    log "Android $abi: 编译 (-j$JOBS)"
    cmake --build "$bdir" -j "$JOBS"

    local out="$BUILD_ROOT/android/$abi"
    mkdir -p "$out"
    find "$bdir" -name 'libqjs_engine*.a' -exec cp {} "$out/" \;
    log "Android $abi: 产物 → $out"
}

# ---------------------------------------------------------------------------
do_linux() {
    local preset="$1"
    local bdir="$BUILD_ROOT/$preset"
    log "Linux: 配置 ($preset)"
    cmake -S "$EXT_DIR" -B "$bdir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DQJS_USE_BORINGSSL_SOURCE="${QJS_USE_BORINGSSL_SOURCE:-ON}"
    log "Linux: 编译 (-j$JOBS)"
    cmake --build "$bdir" -j "$JOBS"
    local out="$BUILD_ROOT/linux/${preset#linux-}"
    mkdir -p "$out"
    find "$bdir" -name 'libqjs_engine*.a' -exec cp {} "$out/" \;
    log "Linux: 产物 → $out"
}

# ---------------------------------------------------------------------------
do_apple() {
    local preset="$1"
    command -v xcodebuild >/dev/null || die "iOS/macOS 构建需要 macOS + Xcode"
    local bdir="$BUILD_ROOT/$preset"
    log "Apple: 配置 ($preset)"
    local sysargs=()
    if [[ "$preset" == ios-* ]]; then
        sysargs=(-DCMAKE_SYSTEM_NAME=iOS)
    fi
    cmake -S "$EXT_DIR" -B "$bdir" -G Xcode \
        -DCMAKE_BUILD_TYPE=Release \
        "${sysargs[@]}" \
        -DCMAKE_OSX_SYSROOT="${CMAKE_OSX_SYSROOT:-iphoneos}" \
        -DCMAKE_OSX_ARCHITECTURES="${CMAKE_OSX_ARCHITECTURES:-arm64}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${CMAKE_OSX_DEPLOYMENT_TARGET:-12.0}" \
        -DQJS_USE_BORINGSSL_SOURCE=ON
    log "Apple: 编译 (-j$JOBS)"
    cmake --build "$bdir" -j "$JOBS" --config Release -- -quiet
    local out="$BUILD_ROOT/apple/$preset"
    mkdir -p "$out"
    find "$bdir" -name 'libqjs_engine*.a' -exec cp {} "$out/" \;
    log "Apple: 产物 → $out"
}

# iOS 模拟器变体
do_ios_sim_arm64() {
    CMAKE_OSX_SYSROOT=iphonesimulator CMAKE_OSX_ARCHITECTURES=arm64 do_apple "ios-simulator-arm64"
}
do_ios_sim_x86_64() {
    CMAKE_OSX_SYSROOT=iphonesimulator CMAKE_OSX_ARCHITECTURES=x86_64 do_apple "ios-simulator-x86_64"
}
do_ios_device() {
    CMAKE_OSX_SYSROOT=iphoneos CMAKE_OSX_ARCHITECTURES=arm64 do_apple "ios-device-arm64"
}
do_macos_arm64() {
    unset CMAKE_SYSTEM_NAME
    CMAKE_OSX_SYSROOT=macosx CMAKE_OSX_ARCHITECTURES=arm64 do_apple "macos-arm64"
}
do_macos_x86_64() {
    unset CMAKE_SYSTEM_NAME
    CMAKE_OSX_SYSROOT=macosx CMAKE_OSX_ARCHITECTURES=x86_64 do_apple "macos-x86_64"
}

# ---------------------------------------------------------------------------
do_windows_msvc() {
    local arch="$1"   # x64 | ARM64
    die "请在 Windows 上用 Developer Command Prompt 执行:
  cmake -S . -B build/windows-msvc-$arch -G \"Visual Studio 17 2022\" -A $arch
  cmake --build build/windows-msvc-$arch --config Release -j
或直接使用 CMakePresets: cmake --preset windows-msvc-$arch"
}

do_windows_mingw() {
    local bdir="$BUILD_ROOT/windows-mingw-x64"
    log "Windows MinGW: 交叉配置"
    cmake -S "$EXT_DIR" -B "$bdir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
        -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
        -DQJS_USE_BORINGSSL_SOURCE=OFF
    log "Windows MinGW: 编译 (-j$JOBS)"
    cmake --build "$bdir" -j "$JOBS"
    local out="$BUILD_ROOT/windows/mingw-x64"
    mkdir -p "$out"
    find "$bdir" -name 'libqjs_engine*.a' -exec cp {} "$out/" \;
    log "Windows MinGW: 产物 → $out"
}

# ---------------------------------------------------------------------------
usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

[ $# -ge 1 ] || usage
TARGET="$1"

case "$TARGET" in
    android-*)           do_android "${TARGET#android-}" ;;
    linux-*)             do_linux "$TARGET" ;;
    ios-device-arm64)    do_ios_device ;;
    ios-simulator-arm64) do_ios_sim_arm64 ;;
    ios-simulator-x86_64) do_ios_sim_x86_64 ;;
    macos-*)             do_"$TARGET" ;;
    windows-msvc-*)      do_windows_msvc "${TARGET#windows-msvc-}" ;;
    windows-mingw-x64)   do_windows_mingw ;;
    all-android)
        for abi in arm64-v8a armeabi-v7a x86_64 x86; do
            do_android "$abi"
        done
        ;;
    *) usage ;;
esac

log "完成: $TARGET"