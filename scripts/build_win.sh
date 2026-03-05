#!/usr/bin/env bash
#
# build_win.sh -- Cross-compile p2p_client and p2p_peer for Windows x86_64
#                 from a Linux host using MinGW-w64.
#
# All build artifacts go into build_win/:
#   build_win/
#     deps/                 Downloaded cross-compilation dependencies
#       ffmpeg/             Pre-built FFmpeg MinGW-w64 libraries
#       SDL2/               Pre-built SDL2 MinGW-w64 libraries
#     mbedtls/              Cross-compiled mbedTLS
#     xquic/                Cross-compiled xquic
#     p2p/                  p2p_client.exe, p2p_peer.exe
#     signaling-server.exe  Go signaling server
#
# Usage:
#   ./scripts/build_win.sh [--install-deps] [--clean] [--jobs N]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build_win"
DEPS_DIR="$BUILD_DIR/deps"
TOOLCHAIN_FILE="$PROJECT_DIR/cmake/mingw-w64-x86_64.cmake"
COMPAT_DIR="$PROJECT_DIR/cmake/mingw-compat"

INSTALL_DEPS=false
CLEAN=false
JOBS="$(nproc 2>/dev/null || echo 4)"

FFMPEG_VERSION="7.1"
SDL2_VERSION="2.30.10"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --clean)        CLEAN=true; shift ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--install-deps] [--clean] [--jobs N]"
            echo ""
            echo "Cross-compile p2p_client.exe and p2p_peer.exe for Windows x86_64."
            echo ""
            echo "  --install-deps   Install MinGW-w64 toolchain and download dependencies"
            echo "  --clean          Remove build_win/ and rebuild from scratch"
            echo "  --jobs N         Parallel build jobs (default: $JOBS)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log()  { echo -e "\033[1;36m[build_win]\033[0m $*"; }
warn() { echo -e "\033[1;33m[build_win]\033[0m $*"; }
die()  { echo -e "\033[1;31m[build_win]\033[0m $*" >&2; exit 1; }

CROSS=x86_64-w64-mingw32

# ============================================================
# Preflight
# ============================================================
if ! command -v ${CROSS}-gcc &>/dev/null; then
    if $INSTALL_DEPS; then
        log "Installing MinGW-w64 toolchain..."
        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            mingw-w64 mingw-w64-tools cmake pkg-config git golang wget unzip
    else
        die "${CROSS}-gcc not found. Run with --install-deps or install mingw-w64."
    fi
fi

# ============================================================
# Clean
# ============================================================
if $CLEAN; then
    log "Cleaning build_win/ ..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR" "$DEPS_DIR"

# ============================================================
# Step 1: Download FFmpeg MinGW-w64 dev libraries
# ============================================================
FFMPEG_DIR="$DEPS_DIR/ffmpeg"
if [[ ! -d "$FFMPEG_DIR/lib" ]]; then
    log "Downloading FFmpeg ${FFMPEG_VERSION} shared dev libraries (MinGW-w64)..."
    FFMPEG_URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n${FFMPEG_VERSION}-latest-win64-lgpl-shared-${FFMPEG_VERSION}.zip"
    FFMPEG_ZIP="$DEPS_DIR/ffmpeg.zip"

    wget -q -O "$FFMPEG_ZIP" "$FFMPEG_URL" || \
        die "Failed to download FFmpeg. URL: $FFMPEG_URL"

    (cd "$DEPS_DIR" && unzip -qo "$FFMPEG_ZIP")
    FFMPEG_EXTRACTED="$(ls -d "$DEPS_DIR"/ffmpeg-n${FFMPEG_VERSION}*-win64-lgpl-shared* 2>/dev/null | head -1)"
    if [[ -z "$FFMPEG_EXTRACTED" || ! -d "$FFMPEG_EXTRACTED" ]]; then
        die "FFmpeg extraction failed."
    fi
    mv "$FFMPEG_EXTRACTED" "$FFMPEG_DIR"
    rm -f "$FFMPEG_ZIP"

    # Generate pkg-config .pc files for FFmpeg
    mkdir -p "$FFMPEG_DIR/lib/pkgconfig"
    for lib in avcodec avutil swscale swresample avformat avdevice; do
        # Find the .dll.a import lib
        IMPORT_LIB=$(find "$FFMPEG_DIR/lib" -name "lib${lib}*.dll.a" -o -name "${lib}*.lib" 2>/dev/null | head -1)
        if [[ -z "$IMPORT_LIB" ]]; then
            IMPORT_LIB=$(find "$FFMPEG_DIR/lib" -name "lib${lib}*.a" 2>/dev/null | head -1)
        fi
        LINK_FLAG="-l${lib}"

        cat > "$FFMPEG_DIR/lib/pkgconfig/lib${lib}.pc" <<PCEOF
prefix=${FFMPEG_DIR}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: lib${lib}
Description: FFmpeg ${lib} library (cross-compiled)
Version: ${FFMPEG_VERSION}
Libs: -L\${libdir} ${LINK_FLAG}
Cflags: -I\${includedir}
PCEOF
    done
    log "FFmpeg ${FFMPEG_VERSION} ready -> $FFMPEG_DIR/"
else
    log "FFmpeg already present, skipping download."
fi

# ============================================================
# Step 2: Download SDL2 MinGW-w64 dev libraries
# ============================================================
SDL2_DIR="$DEPS_DIR/SDL2"
if [[ ! -d "$SDL2_DIR/lib" ]]; then
    log "Downloading SDL2 ${SDL2_VERSION} MinGW-w64 development libraries..."
    SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
    SDL2_TAR="$DEPS_DIR/SDL2.tar.gz"

    wget -q -O "$SDL2_TAR" "$SDL2_URL" || \
        die "Failed to download SDL2. URL: $SDL2_URL"

    (cd "$DEPS_DIR" && tar -xzf "$SDL2_TAR")
    SDL2_EXTRACTED="$DEPS_DIR/SDL2-${SDL2_VERSION}"
    if [[ ! -d "$SDL2_EXTRACTED" ]]; then
        die "SDL2 extraction failed."
    fi
    # Use the x86_64 variant
    if [[ -d "$SDL2_EXTRACTED/x86_64-w64-mingw32" ]]; then
        mkdir -p "$SDL2_DIR"
        cp -r "$SDL2_EXTRACTED/x86_64-w64-mingw32"/* "$SDL2_DIR"/
        # Fix all paths in .pc file to use our local prefix
        if [[ -f "$SDL2_DIR/lib/pkgconfig/sdl2.pc" ]]; then
            sed -i "s|^prefix=.*|prefix=${SDL2_DIR}|" "$SDL2_DIR/lib/pkgconfig/sdl2.pc"
            sed -i "s|^libdir=.*|libdir=\${prefix}/lib|" "$SDL2_DIR/lib/pkgconfig/sdl2.pc"
            sed -i "s|^includedir=.*|includedir=\${prefix}/include|" "$SDL2_DIR/lib/pkgconfig/sdl2.pc"
        fi
    else
        mv "$SDL2_EXTRACTED" "$SDL2_DIR"
    fi
    rm -f "$SDL2_TAR"
    rm -rf "$DEPS_DIR/SDL2-${SDL2_VERSION}"
    log "SDL2 ${SDL2_VERSION} ready -> $SDL2_DIR/"
else
    log "SDL2 already present, skipping download."
fi

# Cross-compile pkg-config setup
export PKG_CONFIG_PATH="$FFMPEG_DIR/lib/pkgconfig:$SDL2_DIR/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""
export PKG_CONFIG_LIBDIR="$FFMPEG_DIR/lib/pkgconfig:$SDL2_DIR/lib/pkgconfig"

# ============================================================
# Step 3: Cross-compile mbedTLS -> build_win/mbedtls/
# ============================================================
MBEDTLS_SRC="$PROJECT_DIR/third_party/mbedtls"
MBEDTLS_BUILD="$BUILD_DIR/mbedtls"
MBEDTLS_LIB="$MBEDTLS_BUILD/library/libmbedcrypto.a"

if [[ ! -f "$MBEDTLS_LIB" ]]; then
    log "Cross-compiling mbedTLS..."

    if [[ ! -d "$MBEDTLS_SRC/include" ]]; then
        die "mbedTLS source not found at $MBEDTLS_SRC. Run: git clone --depth 1 -b v3.6.2 https://github.com/Mbed-TLS/mbedtls.git $MBEDTLS_SRC"
    fi

    cmake -B "$MBEDTLS_BUILD" -S "$MBEDTLS_SRC" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_C_FLAGS="-Os" \
        -DENABLE_TESTING=OFF \
        -DENABLE_PROGRAMS=OFF
    cmake --build "$MBEDTLS_BUILD" -j"$JOBS"
    log "mbedTLS cross-compiled -> $MBEDTLS_BUILD/"
else
    log "mbedTLS already cross-compiled, skipping."
fi

# ============================================================
# Step 4: Cross-compile xquic -> build_win/xquic/
# ============================================================
XQUIC_SRC="$PROJECT_DIR/third_party/xquic"
XQUIC_BUILD="$BUILD_DIR/xquic"
XQUIC_LIB="$XQUIC_BUILD/libxquic-static.a"

if [[ ! -f "$XQUIC_LIB" ]]; then
    log "Cross-compiling xquic (with mbedTLS)..."

    # Patch xquic CMakeLists.txt for MinGW cross-compilation:
    #   1. Remove -Werror (Windows block that disables it runs too late)
    #   2. Remove add_definitions(-DXQC_SYS_WINDOWS=1) to avoid redefinition
    XQUIC_CMAKE="$XQUIC_SRC/CMakeLists.txt"
    XQUIC_CMAKE_BAK="$XQUIC_CMAKE.bak.cross"
    if [[ ! -f "$XQUIC_CMAKE_BAK" ]]; then
        cp "$XQUIC_CMAKE" "$XQUIC_CMAKE_BAK"
    fi
    sed -i 's/-Werror //g' "$XQUIC_CMAKE"
    sed -i '/add_definitions(-DXQC_SYS_WINDOWS/d' "$XQUIC_CMAKE"

    cmake -B "$XQUIC_BUILD" -S "$XQUIC_SRC" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DSSL_TYPE=mbedtls \
        -DMBEDTLS_PATH="$MBEDTLS_SRC" \
        -DMBEDTLS_BUILD_PATH="$MBEDTLS_BUILD" \
        -DSSL_INC_PATH="$MBEDTLS_SRC/include" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DXQC_ENABLE_TESTING=OFF \
        -DCMAKE_C_FLAGS="-I${COMPAT_DIR}"
    cmake --build "$XQUIC_BUILD" --target xquic-static -j"$JOBS"

    # Restore original xquic CMakeLists.txt
    mv "$XQUIC_CMAKE_BAK" "$XQUIC_CMAKE"
    log "xquic cross-compiled -> $XQUIC_BUILD/"
else
    log "xquic already cross-compiled, skipping."
fi

# ============================================================
# Step 5: Cross-compile main project -> build_win/
# ============================================================
log "Cross-compiling libp2pchannel (p2p_client.exe + p2p_peer.exe)..."

# Tell CMake where to find cross-compiled SDL2 and FFmpeg
CMAKE_EXTRA_ARGS=(
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
    -DCMAKE_BUILD_TYPE=Release
    -DMBEDTLS_BUILD_DIR="$MBEDTLS_BUILD"
    -DMBEDTLS_SRC_DIR="$MBEDTLS_SRC"
    -DXQUIC_BUILD_DIR="$XQUIC_BUILD"
    -DXQUIC_SRC_DIR="$XQUIC_SRC"
    -DXQUIC_LIB_PATH="$XQUIC_BUILD"
    -DBUILD_TESTS=OFF
    -DCMAKE_C_FLAGS="-I${COMPAT_DIR}"
)

# Pass FFmpeg and SDL2 library/include paths for cross-compilation
CMAKE_EXTRA_ARGS+=(
    -DCMAKE_PREFIX_PATH="$FFMPEG_DIR;$SDL2_DIR"
)

cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" "${CMAKE_EXTRA_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$JOBS"
log "Cross-compilation complete."

# ============================================================
# Step 6: Cross-compile Go signaling server
# ============================================================
SIGNALING_DIR="$PROJECT_DIR/src/signaling-server"
SIGNALING_BIN="$BUILD_DIR/signaling-server.exe"
if [[ -f "$SIGNALING_DIR/main.go" ]]; then
    log "Cross-compiling signaling server (Go -> Windows amd64)..."
    (cd "$SIGNALING_DIR" && GOOS=windows GOARCH=amd64 go build -o "$SIGNALING_BIN" .)
    log "Signaling server -> $SIGNALING_BIN"
else
    log "Signaling server source not found, skipping."
fi

# ============================================================
# Step 7: Collect runtime DLLs
# ============================================================
DLL_DIR="$BUILD_DIR/bin"
mkdir -p "$DLL_DIR"

# Copy built executables
for exe in p2p_client.exe p2p_peer.exe; do
    if [[ -f "$BUILD_DIR/src/p2p/$exe" ]]; then
        cp "$BUILD_DIR/src/p2p/$exe" "$DLL_DIR/"
    fi
done
[[ -f "$SIGNALING_BIN" ]] && cp "$SIGNALING_BIN" "$DLL_DIR/"

# Copy FFmpeg DLLs
if [[ -d "$FFMPEG_DIR/bin" ]]; then
    cp "$FFMPEG_DIR"/bin/*.dll "$DLL_DIR/" 2>/dev/null || true
fi

# Copy SDL2 DLL
if [[ -d "$SDL2_DIR/bin" ]]; then
    cp "$SDL2_DIR"/bin/*.dll "$DLL_DIR/" 2>/dev/null || true
fi

# Copy MinGW runtime DLLs (if available in toolchain sysroot)
MINGW_SYSROOT="/usr/${CROSS}"
for dll in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    found=$(find "$MINGW_SYSROOT" /usr/lib/gcc/${CROSS}/ -name "$dll" 2>/dev/null | head -1)
    if [[ -n "$found" ]]; then
        cp "$found" "$DLL_DIR/"
    fi
done

# ============================================================
# Summary
# ============================================================
log ""
log "Cross-compilation complete! Windows x86_64 output:"
log "  $BUILD_DIR/"
log "    bin/p2p_client.exe         Publisher"
log "    bin/p2p_peer.exe           Subscriber"
log "    bin/signaling-server.exe   Signaling server"
log "    bin/*.dll                  Runtime DLLs"
log ""
log "Copy the contents of build_win/bin/ to the Windows target machine."
