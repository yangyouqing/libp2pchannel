#!/usr/bin/env bash
#
# build.sh -- Unified build script for libp2pchannel (Linux)
#
# All build artifacts go into the top-level build/ directory:
#   build/
#     mbedtls/            mbedTLS static libraries
#     xquic/              xquic static library + generated headers
#     libjuice/           libjuice static library (via cmake subdirectory)
#     p2p/                libp2pav.so, p2p_client, p2p_peer
#     tests/              test binaries
#     signaling-server    Go signaling server binary
#
# Usage:
#   ./scripts/build.sh [--install-deps] [--clean] [--jobs N]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

INSTALL_DEPS=false
CLEAN=false
EXE_EXT=""
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --clean)        CLEAN=true; shift ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--install-deps] [--clean] [--jobs N]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;32m[build]\033[0m $*"; }

# ============================================================
# Step 0: Clean
# ============================================================
if $CLEAN; then
    log "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

# ============================================================
# Step 1: Install system dependencies
# ============================================================
if $INSTALL_DEPS; then
    log "Installing system dependencies (apt)..."
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        build-essential cmake pkg-config git golang \
        libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        libsdl2-dev libasound2-dev v4l-utils
    log "Dependencies installed."
fi

# ============================================================
# Step 2: Build mbedTLS -> build/mbedtls/
# ============================================================
MBEDTLS_SRC="$PROJECT_DIR/third_party/mbedtls"
MBEDTLS_BUILD="$BUILD_DIR/mbedtls"

MBEDTLS_LIB="$MBEDTLS_BUILD/library/libmbedcrypto.a"

if [[ ! -f "$MBEDTLS_LIB" ]]; then
    log "Building mbedTLS..."
    cmake -B "$MBEDTLS_BUILD" -S "$MBEDTLS_SRC" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_C_FLAGS="-Os -fPIC" \
        -DENABLE_TESTING=OFF \
        -DENABLE_PROGRAMS=OFF
    cmake --build "$MBEDTLS_BUILD" -j"$JOBS"
    log "mbedTLS built -> $MBEDTLS_BUILD/"
else
    log "mbedTLS already built, skipping."
fi

# ============================================================
# Step 3: Build xquic -> build/xquic/
# ============================================================
XQUIC_SRC="$PROJECT_DIR/third_party/xquic"
XQUIC_BUILD="$BUILD_DIR/xquic"

XQUIC_LIB="$XQUIC_BUILD/libxquic-static.a"

if [[ ! -f "$XQUIC_LIB" ]]; then
    log "Building xquic (with mbedTLS)..."
    cmake -B "$XQUIC_BUILD" -S "$XQUIC_SRC" \
        -DSSL_TYPE=mbedtls \
        -DMBEDTLS_PATH="$MBEDTLS_SRC" \
        -DMBEDTLS_BUILD_PATH="$MBEDTLS_BUILD" \
        -DSSL_INC_PATH="$MBEDTLS_SRC/include" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DXQC_ENABLE_TESTING=OFF
    cmake --build "$XQUIC_BUILD" --target xquic-static -j"$JOBS"
    log "xquic built -> $XQUIC_BUILD/"
else
    log "xquic already built, skipping."
fi

# ============================================================
# Step 4: Build main project -> build/
# ============================================================
log "Building libp2pchannel..."
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMBEDTLS_BUILD_DIR="$MBEDTLS_BUILD" \
    -DMBEDTLS_SRC_DIR="$MBEDTLS_SRC" \
    -DXQUIC_BUILD_DIR="$XQUIC_BUILD" \
    -DXQUIC_SRC_DIR="$XQUIC_SRC"
cmake --build "$BUILD_DIR" -j"$JOBS"
log "Main project built."

# ============================================================
# Step 5: Build Go signaling server -> build/signaling-server
# ============================================================
SIGNALING_DIR="$PROJECT_DIR/signaling-server"
SIGNALING_BIN="$BUILD_DIR/signaling-server"
if [[ -f "$SIGNALING_DIR/main.go" ]]; then
    log "Building signaling server..."
    (cd "$SIGNALING_DIR" && go build -o "$SIGNALING_BIN" .)
    log "Signaling server built -> $SIGNALING_BIN"
else
    log "Signaling server source not found, skipping."
fi

# ============================================================
# Summary
# ============================================================
log ""
log "Build complete. Output:"
log "  $BUILD_DIR/"
log "    mbedtls/library/          mbedTLS (libmbedtls.a, libmbedx509.a, libmbedcrypto.a)"
log "    xquic/libxquic-static.a   xquic"
log "    p2p/libp2pav.so           Shared transport library"
log "    p2p/p2p_client            Publisher"
log "    p2p/p2p_peer              Subscriber"
[[ -f "$BUILD_DIR/signaling-server" ]] && \
log "    signaling-server           Signaling server"
