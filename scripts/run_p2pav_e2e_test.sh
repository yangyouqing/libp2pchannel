#!/usr/bin/env bash
#
# run_p2pav_e2e_test.sh -- Build and run the libp2pav E2E test suite.
#
# This script:
#   1. Builds the project (C libs + signaling server)
#   2. Generates a self-signed TLS certificate (if needed)
#   3. Starts the signaling server in background
#   4. Obtains JWT tokens via the admin API
#   5. Runs test_p2pav_e2e with the tokens
#   6. Cleans up background processes on exit
#
# Usage:
#   ./scripts/run_p2pav_e2e_test.sh [--clean] [--skip-build]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

SIGNALING_PORT="${SIGNALING_PORT:-8443}"
SIGNALING_ADDR="127.0.0.1:${SIGNALING_PORT}"
ADMIN_SECRET="${ADMIN_SECRET:-p2p-admin-secret}"

SKIP_BUILD=false
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --clean)      CLEAN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--clean] [--skip-build]"
            echo "  --clean       Clean build directory before building"
            echo "  --skip-build  Skip the build step (use existing binaries)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

PIDS=()
cleanup() {
    echo ""
    info "Stopping background processes..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    info "Cleanup done."
}
trap cleanup EXIT INT TERM

echo "============================================"
echo "  libp2pav E2E Test Runner"
echo "============================================"
echo ""

# Step 1: Build
if ! $SKIP_BUILD; then
    info "Step 1: Building project..."
    BUILD_ARGS=()
    if $CLEAN; then BUILD_ARGS+=(--clean); fi
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}"
    pass "Project built successfully"
else
    info "Step 1: Skipping build (--skip-build)"
fi

# Verify the test binary exists
if [[ ! -f "$BUILD_DIR/tests/test_p2pav_e2e" ]]; then
    fail "test_p2pav_e2e binary not found at $BUILD_DIR/tests/test_p2pav_e2e"
    exit 1
fi

# Step 2: Generate TLS certificate
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Step 2: Generating self-signed TLS certificate..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-test" 2>/dev/null
    pass "TLS certificate generated"
else
    info "Step 2: TLS certificate already exists, reusing"
fi

# Step 3: Start signaling server
info "Step 3: Starting signaling server on $SIGNALING_ADDR..."
LISTEN_ADDR=":${SIGNALING_PORT}" \
TLS_CERT_FILE="$CERT_DIR/server.crt" \
TLS_KEY_FILE="$CERT_DIR/server.key" \
ADMIN_SECRET="$ADMIN_SECRET" \
    "$BUILD_DIR/signaling-server" &
PIDS+=($!)
sleep 2

# Verify signaling server is running
if ! kill -0 "${PIDS[0]}" 2>/dev/null; then
    fail "Signaling server failed to start"
    exit 1
fi
pass "Signaling server started (pid=${PIDS[0]})"

# Step 4: Obtain JWT tokens
info "Step 4: Obtaining JWT tokens..."
TOKEN_PUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=pub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_SUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=sub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_SUB2=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=sub2" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

if [[ -z "$TOKEN_PUB" || -z "$TOKEN_SUB" ]]; then
    fail "Failed to obtain JWT tokens from signaling server"
    exit 1
fi
pass "JWT tokens obtained (pub1, sub1, sub2)"

# Step 5: Run E2E tests
info "Step 5: Running test_p2pav_e2e..."
echo ""

cd "$BUILD_DIR"
set +e
./tests/test_p2pav_e2e \
    --signaling "$SIGNALING_ADDR" \
    --cert "$CERT_DIR/server.crt" \
    --key "$CERT_DIR/server.key" \
    --token "$TOKEN_PUB" \
    --token-sub "$TOKEN_SUB" \
    --token-sub2 "$TOKEN_SUB2"
TEST_EXIT=$?
set -e

echo ""
if [[ $TEST_EXIT -eq 0 ]]; then
    echo "============================================"
    echo -e "  ${GREEN}All E2E tests passed!${NC}"
    echo "============================================"
else
    echo "============================================"
    echo -e "  ${RED}Some E2E tests failed (exit=$TEST_EXIT)${NC}"
    echo "============================================"
fi

exit $TEST_EXIT
