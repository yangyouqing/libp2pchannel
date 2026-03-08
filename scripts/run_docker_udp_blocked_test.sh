#!/usr/bin/env bash
#
# run_docker_udp_blocked_test.sh -- Run the libp2pav UDP-blocked test inside Docker.
#
# Builds the test binary, packages it into a Docker image, then runs
# the container with UDP fully blocked via iptables.  The host network
# is never modified.
#
# Usage:
#   ./scripts/run_docker_udp_blocked_test.sh [OPTIONS]
#
# Options:
#   --server IP            Remote k3s server IP       (default: 106.54.30.119)
#   --signaling-port PORT  Signaling server port      (default: 30443)
#   --stun-port PORT       STUN/TURN server port      (default: 3478)
#   --admin-secret SECRET  Admin secret for JWT       (default: built-in)
#   --duration SEC         Test duration in seconds   (default: 600)
#   --skip-build           Skip C/CMake build step
#   --clean                Clean build before building
#   -h, --help             Show this help
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# ---- Defaults ----
SERVER_IP="106.54.30.119"
SIGNALING_PORT="30443"
STUN_PORT="3478"
ADMIN_SECRET="eLTGSBSmlCZqar7lwkf4GFje"
DURATION=600
SKIP_BUILD=false
CLEAN=false

DOCKER_IMAGE="p2pav-udp-test"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${CYAN}[HOST]${NC} $1"; }
pass()  { echo -e "${GREEN}[ OK ]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }

# ---- Parse arguments ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)           SERVER_IP="$2"; shift 2 ;;
        --signaling-port)   SIGNALING_PORT="$2"; shift 2 ;;
        --stun-port)        STUN_PORT="$2"; shift 2 ;;
        --admin-secret)     ADMIN_SECRET="$2"; shift 2 ;;
        --duration)         DURATION="$2"; shift 2 ;;
        --skip-build)       SKIP_BUILD=true; shift ;;
        --clean)            CLEAN=true; shift ;;
        -h|--help)
            head -24 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

SIGNALING_URL="${SERVER_IP}:${SIGNALING_PORT}"
STUN_SERVER="${SERVER_IP}:${STUN_PORT}"

echo ""
echo "================================================"
echo "  P2P AV UDP-Blocked Test (Docker Runner)"
echo "================================================"
info "Server:      $SERVER_IP"
info "Signaling:   $SIGNALING_URL"
info "STUN/TURN:   $STUN_SERVER"
info "Duration:    ${DURATION}s"
echo ""

# ---- Step 1: Build (optional) ----
if $CLEAN; then
    info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

if ! $SKIP_BUILD; then
    info "Building project..."
    "$SCRIPT_DIR/build.sh" --jobs "$(nproc)"
    pass "Build completed"
else
    info "Skipping build (--skip-build)"
fi

# ---- Step 2: Verify binaries ----
TEST_BIN="$BUILD_DIR/tests/test_p2pav_udp_blocked"
LIB_SO="$BUILD_DIR/src/p2p/libp2pav.so"

if [[ ! -f "$TEST_BIN" ]]; then
    fail "Test binary not found: $TEST_BIN"
    fail "Make sure the project is built with tests enabled."
    exit 1
fi
if [[ ! -f "$LIB_SO" ]]; then
    fail "Shared library not found: $LIB_SO"
    exit 1
fi
pass "Binaries verified"

# ---- Step 3: Generate TLS certs if needed ----
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating TLS certificates..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-udp-test" 2>/dev/null
    pass "TLS certificates generated"
else
    pass "TLS certificates found"
fi

# ---- Step 4: Check signaling server reachability ----
info "Checking signaling server at ${SIGNALING_URL}..."
HEALTH=$(curl -sk --connect-timeout 10 "https://${SIGNALING_URL}/health" 2>/dev/null || echo "{}")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling server healthy"
else
    fail "Signaling server unreachable at ${SIGNALING_URL}: $HEALTH"
    fail "Ensure the k3s services are running on ${SERVER_IP}"
    exit 1
fi

# ---- Step 5: Build Docker image ----
info "Building Docker image: ${DOCKER_IMAGE}..."
docker build \
    -f "$PROJECT_DIR/tests/Dockerfile.udp_blocked" \
    -t "$DOCKER_IMAGE" \
    "$PROJECT_DIR" \
    2>&1 | sed 's/^/  /'

pass "Docker image built"

# ---- Step 6: Run Docker container ----
info "Starting Docker container (duration=${DURATION}s)..."
echo ""

CONTAINER_NAME="p2pav-udp-test-$(date +%s)"

docker run --rm \
    --name "$CONTAINER_NAME" \
    --cap-add=NET_ADMIN \
    -e SIGNALING_URL="$SIGNALING_URL" \
    -e STUN_SERVER="$STUN_SERVER" \
    -e ADMIN_SECRET="$ADMIN_SECRET" \
    -e DURATION="$DURATION" \
    -e ENABLE_TCP="1" \
    "$DOCKER_IMAGE"

EXIT_CODE=$?

echo ""
if [[ $EXIT_CODE -eq 0 ]]; then
    pass "Test PASSED (exit code 0)"
else
    fail "Test FAILED (exit code $EXIT_CODE)"
fi

exit $EXIT_CODE
