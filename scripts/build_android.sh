#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export NDK="${ANDROID_HOME}/ndk/26.1.10909125"
export TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
export NDK_TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

ABI="x86_64"
API=24
JOBS=$(nproc)
PREFIX="$PROJECT_DIR/android_deps/$ABI"

mkdir -p "$PREFIX"

echo "=== Android cross-compile: ABI=$ABI API=$API ==="
echo "NDK=$NDK"
echo "PREFIX=$PREFIX"

# ---- 1. mbedTLS ----
echo ""
echo "=== Building mbedTLS ==="
MBEDTLS_SRC="$PROJECT_DIR/third_party/mbedtls"
MBEDTLS_BUILD="$PROJECT_DIR/build_android/mbedtls"
rm -rf "$MBEDTLS_BUILD"
mkdir -p "$MBEDTLS_BUILD"
cmake -S "$MBEDTLS_SRC" -B "$MBEDTLS_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build "$MBEDTLS_BUILD" -j "$JOBS"
cmake --install "$MBEDTLS_BUILD"
# Ensure libs are also in $PREFIX/lib (some cmake versions install to lib64)
for f in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
    [ -f "$PREFIX/lib/$f" ] || cp "$MBEDTLS_BUILD/library/$f" "$PREFIX/lib/" 2>/dev/null || true
done
echo "mbedTLS installed to $PREFIX"

# ---- 2. xquic ----
echo ""
echo "=== Building xquic ==="
XQUIC_SRC="$PROJECT_DIR/third_party/xquic"
XQUIC_BUILD="$PROJECT_DIR/build_android/xquic"
MBEDTLS_SRC="$PROJECT_DIR/third_party/mbedtls"
rm -rf "$XQUIC_BUILD"
mkdir -p "$XQUIC_BUILD"
cmake -S "$XQUIC_SRC" -B "$XQUIC_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSSL_TYPE=mbedtls \
    -DMBEDTLS_PATH="$MBEDTLS_SRC" \
    -DMBEDTLS_BUILD_PATH="$MBEDTLS_BUILD" \
    -DSSL_INC_PATH="$MBEDTLS_SRC/include" \
    -DXQC_ENABLE_TESTING=OFF
cmake --build "$XQUIC_BUILD" --target xquic-static -j "$JOBS"
cp "$XQUIC_BUILD"/libxquic-static.a "$PREFIX/lib/" 2>/dev/null || true
mkdir -p "$PREFIX/include"
cp -r "$XQUIC_SRC/include/xquic" "$PREFIX/include/" 2>/dev/null || true
# Also copy generated config header if it exists
if [ -d "$XQUIC_BUILD/include" ]; then
    cp -r "$XQUIC_BUILD/include/"* "$PREFIX/include/" 2>/dev/null || true
fi
echo "xquic installed to $PREFIX"

# ---- 3. libjuice ----
echo ""
echo "=== Building libjuice ==="
JUICE_SRC="$PROJECT_DIR/third_party/libjuice"
JUICE_BUILD="$PROJECT_DIR/build_android/libjuice"
rm -rf "$JUICE_BUILD"
mkdir -p "$JUICE_BUILD"
cmake -S "$JUICE_SRC" -B "$JUICE_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DNO_SERVER=ON \
    -DJUICE_BUILD_TESTS=OFF \
    -DJUICE_BUILD_UDPMUX=OFF
cmake --build "$JUICE_BUILD" -j "$JOBS"
cmake --install "$JUICE_BUILD"
echo "libjuice installed to $PREFIX"

# ---- 4. FFmpeg ----
echo ""
echo "=== Building FFmpeg ==="
FFMPEG_SRC="$PROJECT_DIR/build_android/ffmpeg_src"
FFMPEG_BUILD="$PROJECT_DIR/build_android/ffmpeg_build"

if [ ! -d "$FFMPEG_SRC" ]; then
    echo "Downloading FFmpeg 6.1.2..."
    mkdir -p "$FFMPEG_SRC"
    cd /tmp
    wget -q "https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz" -O ffmpeg.tar.xz
    tar xf ffmpeg.tar.xz -C "$FFMPEG_SRC" --strip-components=1
    rm -f ffmpeg.tar.xz
fi

CC="$NDK_TOOLCHAIN/bin/x86_64-linux-android${API}-clang"
CXX="$NDK_TOOLCHAIN/bin/x86_64-linux-android${API}-clang++"
SYSROOT="$NDK_TOOLCHAIN/sysroot"

rm -rf "$FFMPEG_BUILD"
mkdir -p "$FFMPEG_BUILD"
cd "$FFMPEG_SRC"

./configure \
    --prefix="$PREFIX" \
    --target-os=android \
    --arch=x86_64 \
    --cc="$CC" \
    --cxx="$CXX" \
    --sysroot="$SYSROOT" \
    --enable-cross-compile \
    --enable-static \
    --disable-shared \
    --disable-programs \
    --disable-doc \
    --disable-avdevice \
    --disable-avformat \
    --disable-network \
    --disable-postproc \
    --disable-avfilter \
    --enable-pic \
    --enable-small \
    --disable-debug \
    --disable-encoders \
    --enable-decoder=h264 \
    --enable-decoder=opus \
    --enable-parser=h264 \
    --enable-parser=opus \
    --enable-swscale \
    --enable-swresample \
    --disable-asm \
    --extra-cflags="-fPIC" \
    --extra-ldflags="-fPIC"

make -j "$JOBS"
make install
echo "FFmpeg installed to $PREFIX"

# ---- 5. Download SDL2 source ----
echo ""
echo "=== Downloading SDL2 source ==="
SDL2_SRC="$PROJECT_DIR/build_android/SDL2"
if [ ! -d "$SDL2_SRC" ]; then
    cd /tmp
    wget -q "https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-2.30.3.tar.gz" -O sdl2.tar.gz
    mkdir -p "$SDL2_SRC"
    tar xf sdl2.tar.gz -C "$SDL2_SRC" --strip-components=1
    rm -f sdl2.tar.gz
fi
echo "SDL2 source at $SDL2_SRC"

echo ""
echo "=== All Android dependencies built successfully ==="
echo "Prefix: $PREFIX"
ls -la "$PREFIX/lib/"
