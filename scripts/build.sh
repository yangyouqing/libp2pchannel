#!/usr/bin/env bash
#
# build.sh -- Unified build script for libp2pchannel
#
# All build artifacts go into the top-level build/ directory:
#   build/
#     boringssl/          BoringSSL static libraries
#     xquic/              xquic static library + generated headers
#     libjuice/           libjuice static library (via cmake subdirectory)
#     p2p/                p2p_av library, p2p_client, p2p_peer
#     tests/              test binaries
#     signaling-server    Go signaling server binary
#
# Usage:
#   ./scripts/build.sh [--install-deps] [--clean] [--jobs N]
#
# Steps:
#   1. (Optional) Install system build dependencies
#   2. Clone BoringSSL if missing, build in build/boringssl/
#   3. Build xquic in build/xquic/
#   4. Build main project (libjuice + p2p + tests) in build/
#   5. Build Go signaling server -> build/signaling-server
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

INSTALL_DEPS=false
CLEAN=false
JOBS="$(nproc 2>/dev/null || echo 4)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --clean)        CLEAN=true; shift ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--install-deps] [--clean] [--jobs N]"
            echo ""
            echo "  --install-deps   Install system dependencies via apt"
            echo "  --clean          Remove build/ and rebuild from scratch"
            echo "  --jobs N         Parallel build jobs (default: $(nproc))"
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
    log "Installing system dependencies..."
    sudo apt-get update -qq
    sudo apt-get install -y -qq \
        build-essential cmake pkg-config git golang \
        libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        libsdl2-dev libasound2-dev v4l-utils
    log "Dependencies installed."
fi

# ============================================================
# Step 2: Build BoringSSL -> build/boringssl/
# ============================================================
BORINGSSL_SRC="$PROJECT_DIR/xquic/third_party/boringssl"
BORINGSSL_BUILD="$BUILD_DIR/boringssl"

if [[ ! -f "$BORINGSSL_BUILD/libssl.a" ]]; then
    log "Building BoringSSL..."

    if [[ ! -d "$BORINGSSL_SRC" ]]; then
        log "Cloning BoringSSL..."
        mkdir -p "$PROJECT_DIR/xquic/third_party"
        git clone --depth 1 https://boringssl.googlesource.com/boringssl "$BORINGSSL_SRC"
    fi

    cmake -B "$BORINGSSL_BUILD" -S "$BORINGSSL_SRC" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BORINGSSL_BUILD" --target ssl crypto -j"$JOBS"
    log "BoringSSL built -> $BORINGSSL_BUILD/"
else
    log "BoringSSL already built, skipping."
fi

# ============================================================
# Step 3: Build xquic -> build/xquic/
# ============================================================
XQUIC_SRC="$PROJECT_DIR/xquic"
XQUIC_BUILD="$BUILD_DIR/xquic"

if [[ ! -f "$XQUIC_BUILD/libxquic-static.a" ]]; then
    log "Building xquic..."
    cmake -B "$XQUIC_BUILD" -S "$XQUIC_SRC" \
        -DSSL_TYPE=boringssl \
        -DSSL_PATH="$BORINGSSL_SRC" \
        -DSSL_INC_PATH="$BORINGSSL_SRC/include" \
        -DSSL_LIB_PATH="$BORINGSSL_BUILD/libssl.a;$BORINGSSL_BUILD/libcrypto.a" \
        -DCMAKE_BUILD_TYPE=Release \
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
    -DBORINGSSL_BUILD_DIR="$BORINGSSL_BUILD" \
    -DBORINGSSL_SRC_DIR="$BORINGSSL_SRC" \
    -DXQUIC_BUILD_DIR="$XQUIC_BUILD" \
    -DXQUIC_SRC_DIR="$XQUIC_SRC"
cmake --build "$BUILD_DIR" -j"$JOBS"
log "Main project built."

# ============================================================
# Step 5: Build Go signaling server -> build/signaling-server
# ============================================================
SIGNALING_DIR="$PROJECT_DIR/signaling-server"
if [[ -f "$SIGNALING_DIR/main.go" ]]; then
    log "Building signaling server..."
    (cd "$SIGNALING_DIR" && go build -o "$BUILD_DIR/signaling-server" .)
    log "Signaling server built -> $BUILD_DIR/signaling-server"
else
    log "Signaling server source not found, skipping."
fi

# ============================================================
# Summary
# ============================================================
log ""
log "Build complete. Output:"
log "  $BUILD_DIR/"
log "    boringssl/libssl.a          BoringSSL"
log "    boringssl/libcrypto.a"
log "    xquic/libxquic-static.a     xquic"
log "    p2p/p2p_client              Publisher"
log "    p2p/p2p_peer                Subscriber"
log "    signaling-server            Signaling server"
[[ -f "$BUILD_DIR/tests/test_signaling" ]] && \
log "    tests/test_signaling        Test"
[[ -f "$BUILD_DIR/tests/test_ice_connectivity" ]] && \
log "    tests/test_ice_connectivity Test"
