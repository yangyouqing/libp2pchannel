#!/usr/bin/env bash
#
# run_peer_on_k3s.sh -- Run p2p_peer (subscriber) against K3s-deployed
#                       signaling server and coturn.
#
# Prerequisites:
#   - k3s running with the p2p-av namespace deployed
#   - p2p_peer built (in build/src/p2p/)
#   - Display (DISPLAY=:0) for video rendering
#
# Usage:
#   ./scripts/run_peer_on_k3s.sh [--duration SECONDS] [--room ROOM] [--peer-id ID] [--skip-build]
#
# Example (run client in one terminal, peer in another):
#   Terminal 1: ./scripts/run_client_on_k3s.sh --room myroom
#   Terminal 2: ./scripts/run_peer_on_k3s.sh --room myroom

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DURATION=30
ROOM_ID="k3s-test-room"
PEER_ID="sub1"
SKIP_BUILD=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[K3S-PEER]${NC} $1"; }
pass()  { echo -e "${GREEN}[  OK  ]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ WARN ]${NC} $1"; }
fail()  { echo -e "${RED}[ FAIL ]${NC} $1"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)   DURATION="$2"; shift 2 ;;
        --room)       ROOM_ID="$2"; shift 2 ;;
        --peer-id)    PEER_ID="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --help|-h)
            echo "Usage: $0 [--duration SEC] [--room ROOM] [--peer-id ID] [--skip-build]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo ""
echo "============================================"
echo "  P2P Peer (Subscriber) on K3s"
echo "============================================"
echo ""

# --- Step 0: Build if needed ---
if [[ "$SKIP_BUILD" == "false" ]] && [[ ! -f "$BUILD_DIR/src/p2p/p2p_peer" ]]; then
    info "Building project..."
    cd "$PROJECT_DIR"
    mkdir -p build && cd build
    cmake .. -DBUILD_XQUIC=OFF -DBUILD_COTURN=OFF -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug > /dev/null 2>&1
    make -j$(nproc) > /dev/null 2>&1
    pass "Build complete"
fi

# --- Step 1: Verify K3s cluster ---
info "Checking K3s cluster..."

if ! kubectl cluster-info > /dev/null 2>&1; then
    fail "K3s cluster not reachable. Run: sudo systemctl start k3s"
    exit 1
fi

POD_COUNT=$(kubectl -n p2p-av get pods --field-selector=status.phase=Running --no-headers 2>/dev/null | wc -l)
if [[ "$POD_COUNT" -lt 3 ]]; then
    fail "Expected at least 3 running pods in p2p-av namespace (got $POD_COUNT)"
    echo "  Deploy first: kubectl apply -f deploy/k8s/"
    exit 1
fi
pass "K3s cluster OK ($POD_COUNT pods running)"

# --- Step 2: Get node IP ---
NODE_IP=$(kubectl get nodes -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}')
SIGNALING_ADDR="${NODE_IP}:30443"
STUN_ADDR="${NODE_IP}:3478"

info "Node IP:    $NODE_IP"
info "Signaling:  $SIGNALING_ADDR"
info "STUN/TURN:  $STUN_ADDR"

# --- Step 3: Verify signaling health ---
info "Checking signaling server health..."
HEALTH=$(curl -sk "https://${SIGNALING_ADDR}/health" 2>/dev/null || echo "{}")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling healthy: $HEALTH"
else
    fail "Signaling health check failed: $HEALTH"
    exit 1
fi

# --- Step 4: Get admin secret from K3s ---
ADMIN_SECRET=$(kubectl -n p2p-av get secret p2p-secrets -o jsonpath='{.data.admin-secret}' | base64 -d)
info "Admin secret retrieved from K3s"

# --- Step 5: Generate JWT token for subscriber ---
info "Generating JWT token for subscriber ($PEER_ID)..."
SUB_TOKEN_JSON=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=${PEER_ID}" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" 2>/dev/null)
SUB_TOKEN=$(echo "$SUB_TOKEN_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null)

if [[ -z "$SUB_TOKEN" ]]; then
    fail "Failed to get subscriber token: $SUB_TOKEN_JSON"
    exit 1
fi
pass "Subscriber token obtained ($PEER_ID)"

# --- Step 6: Generate QUIC SSL certs ---
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating QUIC TLS certificates..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-quic" 2>/dev/null
fi

# --- Step 7: Check display ---
info "Checking display..."
if [[ -n "${DISPLAY:-}" ]]; then
    pass "Display: $DISPLAY"
else
    warn "No DISPLAY set, p2p_peer video rendering will be disabled"
fi

# --- Step 8: Run p2p_peer ---
LOG_DIR="$BUILD_DIR/k3s_test_logs"
mkdir -p "$LOG_DIR"

cleanup() {
    echo ""
    info "Stopping p2p_peer..."
    kill "$PEER_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    info "Done. Logs in: $LOG_DIR/"
}
trap cleanup EXIT INT TERM

echo ""
echo "============================================"
echo "  Starting P2P Peer (Subscriber)"
echo "  Room: $ROOM_ID  |  Peer: $PEER_ID  |  Duration: ${DURATION}s"
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p:${LD_LIBRARY_PATH:-}"

info "Starting p2p_peer (subscriber)..."
"$BUILD_DIR/src/p2p/p2p_peer" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" \
    --peer-id "$PEER_ID" \
    --token "$SUB_TOKEN" \
    --stun "$STUN_ADDR" \
    > "$LOG_DIR/p2p_peer.log" 2>&1 &
PEER_PID=$!
info "  PID=$PEER_PID  log=$LOG_DIR/p2p_peer.log"

echo ""
info "Subscriber running for ${DURATION}s..."
echo ""

# --- Step 9: Monitor for the test duration ---
ELAPSED=0
CHECK_INTERVAL=5
PEER_OK=false
ICE_CONNECTED=false
RX_RECEIVED=false

while [[ $ELAPSED -lt $DURATION ]]; do
    sleep $CHECK_INTERVAL
    ELAPSED=$((ELAPSED + CHECK_INTERVAL))

    if ! kill -0 $PEER_PID 2>/dev/null; then
        fail "p2p_peer exited prematurely at ${ELAPSED}s"
        echo "  Last 10 lines of log:"
        tail -10 "$LOG_DIR/p2p_peer.log" 2>/dev/null | sed 's/^/    /'
        exit 1
    fi

    if ! $PEER_OK && grep -q "signaling connected" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Subscriber connected to signaling"
        PEER_OK=true
    fi

    if ! $ICE_CONNECTED && grep -q "ICE connected" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] ICE connection established"
        ICE_CONNECTED=true
    fi

    if ! $RX_RECEIVED && grep -q "\[RX\]" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Subscriber receiving data"
        RX_RECEIVED=true
    fi

    if $RX_RECEIVED; then
        STAT_LINE=$(grep "\[RX-STAT\]" "$LOG_DIR/p2p_peer.log" 2>/dev/null | tail -1 || true)
        if [[ -n "$STAT_LINE" ]]; then
            info "[${ELAPSED}s] $STAT_LINE"
        fi
    fi
done

echo ""
echo "============================================"
echo "  Peer Test Results (${DURATION}s)"
echo "============================================"

PEER_ALIVE=true
kill -0 $PEER_PID 2>/dev/null || PEER_ALIVE=false

PASS_COUNT=0
FAIL_COUNT=0

check_result() {
    local desc="$1" val="$2"
    if $val; then
        pass "$desc"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        fail "$desc"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

check_result "Subscriber signaling connected"  $PEER_OK
check_result "ICE connection established"     $ICE_CONNECTED
check_result "Subscriber receiving A/V data"   $RX_RECEIVED
check_result "Subscriber still running"       $PEER_ALIVE

echo ""
if [[ $FAIL_COUNT -eq 0 ]]; then
    echo -e "${GREEN}All $PASS_COUNT checks passed!${NC}"
else
    echo -e "${RED}$FAIL_COUNT/$((PASS_COUNT + FAIL_COUNT)) checks failed.${NC}"
    echo ""
    echo "Subscriber log tail:"
    tail -20 "$LOG_DIR/p2p_peer.log" 2>/dev/null | sed 's/^/  /'
fi
echo ""
