#!/bin/bash
# End-to-end integration test.
#
# Tests performed:
#   1. Build libjuice + p2p adapter library
#   2. Build Go signaling server
#   3. Run signaling protocol unit test (C)
#   4. Run signaling server integration tests (Go)
#   5. Start signaling server, run ICE connectivity test (if xquic available)
#
# Usage: ./scripts/run_e2e_test.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

echo "============================================"
echo "  P2P A/V Framework End-to-End Tests"
echo "============================================"
echo ""

# Step 1: Build C components
info "Step 1: Building C components (libjuice + p2p adapter)..."
cd "$PROJECT_DIR"
mkdir -p build && cd build
cmake .. -DBUILD_XQUIC=OFF -DBUILD_COTURN=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
make -j$(nproc) > /dev/null 2>&1
pass "C components built successfully"

# Step 2: Build Go signaling server
info "Step 2: Building Go signaling server..."
cd "$PROJECT_DIR/src/signaling-server"
go build -o signaling-server . > /dev/null 2>&1
pass "Go signaling server built successfully"

# Step 3: Run signaling protocol test (C)
info "Step 3: Running signaling protocol unit test (C)..."
cd "$PROJECT_DIR/build"
if ./tests/test_signaling > /dev/null 2>&1; then
    pass "Signaling protocol unit test"
else
    fail "Signaling protocol unit test"
fi

# Step 4: Run Go integration tests
info "Step 4: Running Go signaling server integration tests..."
cd "$PROJECT_DIR/src/signaling-server"
if go test -v -count=1 ./... 2>&1 | grep -q "^ok"; then
    pass "Go signaling server integration tests"
else
    fail "Go signaling server integration tests"
fi

# Step 5: Check if full ICE test is possible
info "Step 5: Checking ICE connectivity test availability..."
if [ -f "$PROJECT_DIR/build/tests/test_ice_connectivity" ]; then
    info "Running ICE connectivity test..."
    if timeout 30 "$PROJECT_DIR/build/tests/test_ice_connectivity" 2>&1 | grep -q "TEST PASSED"; then
        pass "ICE connectivity test"
    else
        fail "ICE connectivity test"
    fi
else
    info "ICE connectivity test skipped (xquic library not available)"
    info "To enable: build xquic first, then cmake with -DBUILD_XQUIC=ON"
fi

echo ""
echo "============================================"
echo -e "  ${GREEN}All available tests passed!${NC}"
echo "============================================"
echo ""
echo "Project structure:"
echo "  src/p2p/include/ - Adapter + Publisher/Subscriber headers"
echo "  src/p2p/src/     - C implementation"
echo "  src/signaling-server/ - Go signaling server"
echo "  conf/           - coturn configuration"
echo "  scripts/        - Startup and test scripts"
echo ""
echo "Next steps:"
echo "  1. Build xquic with SSL: cmake -DBUILD_XQUIC=ON -DSSL_TYPE=boringssl"
echo "  2. Start coturn:  ./scripts/start_coturn.sh --build"
echo "  3. Start signaling: ./scripts/start_signaling.sh"
echo "  4. Run full E2E with ICE + QUIC connectivity"
