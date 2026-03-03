#!/usr/bin/env bash
#
# build.sh -- Unified build script for libp2pchannel (Linux & Windows)
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

# ---- Platform detection ----
detect_platform() {
    case "$(uname -s)" in
        Linux*)         P2P_OS=linux ;;
        MINGW*|MSYS*)   P2P_OS=windows ;;
        CYGWIN*)        P2P_OS=windows ;;
        Darwin*)        P2P_OS=macos ;;
        *)              P2P_OS=linux ;;
    esac
}
detect_platform

if [[ "$P2P_OS" == "windows" ]]; then
    EXE_EXT=".exe"
    LIB_PREFIX=""
    STATIC_LIB_EXT=".lib"
    JOBS="${NUMBER_OF_PROCESSORS:-4}"
else
    EXE_EXT=""
    LIB_PREFIX="lib"
    STATIC_LIB_EXT=".a"
    JOBS="$(nproc 2>/dev/null || echo 4)"
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --clean)        CLEAN=true; shift ;;
        --jobs)         JOBS="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--install-deps] [--clean] [--jobs N]"
            echo ""
            echo "  --install-deps   Install system dependencies (Linux: apt, Windows: manual)"
            echo "  --clean          Remove build/ and rebuild from scratch"
            echo "  --jobs N         Parallel build jobs (default: $JOBS)"
            echo ""
            echo "Detected platform: $P2P_OS"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;32m[build]\033[0m $*"; }

log "Platform: $P2P_OS"

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
    if [[ "$P2P_OS" == "linux" ]]; then
        log "Installing system dependencies (apt)..."
        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            build-essential cmake pkg-config git golang \
            libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
            libsdl2-dev libasound2-dev v4l-utils
        log "Dependencies installed."
    elif [[ "$P2P_OS" == "windows" ]]; then
        log "Windows: auto-install not supported. Please install manually:"
        log "  - Visual Studio / MSYS2 / MinGW toolchain"
        log "  - CMake (cmake.org)"
        log "  - Go (go.dev)"
        log "  - FFmpeg dev libraries (vcpkg or manual)"
        log "  - SDL2 dev libraries (vcpkg or manual)"
        log "  - pkg-config (via MSYS2 or vcpkg)"
    fi
fi

# ============================================================
# Step 2: Build BoringSSL -> build/boringssl/
# ============================================================
BORINGSSL_SRC="$PROJECT_DIR/third_party/xquic/third_party/boringssl"
BORINGSSL_BUILD="$BUILD_DIR/boringssl"

BORINGSSL_SSL_LIB="$BORINGSSL_BUILD/${LIB_PREFIX}ssl${STATIC_LIB_EXT}"

if [[ ! -f "$BORINGSSL_SSL_LIB" ]]; then
    log "Building BoringSSL..."

    if [[ ! -d "$BORINGSSL_SRC" ]]; then
        log "Cloning BoringSSL..."
        mkdir -p "$PROJECT_DIR/third_party/xquic/third_party"
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
XQUIC_SRC="$PROJECT_DIR/third_party/xquic"
XQUIC_BUILD="$BUILD_DIR/xquic"

XQUIC_LIB="$XQUIC_BUILD/${LIB_PREFIX}xquic-static${STATIC_LIB_EXT}"

if [[ ! -f "$XQUIC_LIB" ]]; then
    log "Building xquic..."
    BORINGSSL_CRYPTO_LIB="$BORINGSSL_BUILD/${LIB_PREFIX}crypto${STATIC_LIB_EXT}"
    cmake -B "$XQUIC_BUILD" -S "$XQUIC_SRC" \
        -DSSL_TYPE=boringssl \
        -DSSL_PATH="$BORINGSSL_SRC" \
        -DSSL_INC_PATH="$BORINGSSL_SRC/include" \
        -DSSL_LIB_PATH="$BORINGSSL_SSL_LIB;$BORINGSSL_CRYPTO_LIB" \
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
SIGNALING_BIN="$BUILD_DIR/signaling-server${EXE_EXT}"
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
log "Build complete ($P2P_OS). Output:"
log "  $BUILD_DIR/"
log "    boringssl/${LIB_PREFIX}ssl${STATIC_LIB_EXT}          BoringSSL"
log "    boringssl/${LIB_PREFIX}crypto${STATIC_LIB_EXT}"
log "    xquic/${LIB_PREFIX}xquic-static${STATIC_LIB_EXT}     xquic"
log "    p2p/p2p_client${EXE_EXT}              Publisher"
if [[ "$P2P_OS" == "linux" ]]; then
    log "    p2p/p2p_peer                Subscriber"
fi
log "    signaling-server${EXE_EXT}            Signaling server"
[[ -f "$BUILD_DIR/tests/test_signaling${EXE_EXT}" ]] && \
log "    tests/test_signaling${EXE_EXT}        Test"
[[ -f "$BUILD_DIR/tests/test_ice_connectivity${EXE_EXT}" ]] && \
log "    tests/test_ice_connectivity${EXE_EXT} Test"
