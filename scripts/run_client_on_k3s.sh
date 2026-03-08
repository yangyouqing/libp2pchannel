#!/usr/bin/env bash
#
# run_client_on_k3s.sh -- Run p2p_client (publisher) against K3s-deployed
#                         signaling server and coturn.
#
# Prerequisites:
#   - k3s running with the p2p-av namespace deployed
#   - p2p_client built (in build/src/p2p/)
#   - Camera (/dev/video0), audio (default)
#
# Usage:
#   ./scripts/run_client_on_k3s.sh [--duration SECONDS] [--room ROOM] [--peer-id ID] [--skip-build]
#
# Example (run in one terminal, peer in another):
#   Terminal 1: ./scripts/run_client_on_k3s.sh --room myroom
#   Terminal 2: ./scripts/run_peer_on_k3s.sh --room myroom

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DURATION=30
ROOM_ID="k3s-test-room"
PEER_ID="pub1"
SKIP_BUILD=false
VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-default}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[K3S-CLIENT]${NC} $1"; }
pass()  { echo -e "${GREEN}[  OK  ]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ WARN ]${NC} $1"; }
fail()  { echo -e "${RED}[ FAIL ]${NC} $1"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration)   DURATION="$2"; shift 2 ;;
        --room)       ROOM_ID="$2"; shift 2 ;;
        --peer-id)    PEER_ID="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --video-dev)  VIDEO_DEV="$2"; shift 2 ;;
        --audio-dev)  AUDIO_DEV="$2"; shift 2 ;;
        --help|-h)
            echo "Usage: $0 [--duration SEC] [--room ROOM] [--peer-id ID] [--skip-build] [--video-dev DEV] [--audio-dev DEV]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo ""
echo "============================================"
echo "  P2P Client (Publisher) on K3s"
echo "============================================"
echo ""

# --- Step 0: Build if needed ---
if [[ "$SKIP_BUILD" == "false" ]] && [[ ! -f "$BUILD_DIR/src/p2p/p2p_client" ]]; then
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

# --- Step 5: Generate JWT token for publisher ---
info "Generating JWT token for publisher ($PEER_ID)..."
PUB_TOKEN_JSON=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=${PEER_ID}" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" 2>/dev/null)
PUB_TOKEN=$(echo "$PUB_TOKEN_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null)

if [[ -z "$PUB_TOKEN" ]]; then
    fail "Failed to get publisher token: $PUB_TOKEN_JSON"
    exit 1
fi
pass "Publisher token obtained ($PEER_ID)"

# --- Step 6: Generate QUIC SSL certs ---
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating QUIC TLS certificates..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-quic" 2>/dev/null
fi

# --- Step 7: Check video/audio devices ---
info "Checking devices..."
if [[ -e "$VIDEO_DEV" ]]; then
    pass "Video: $VIDEO_DEV"
else
    warn "Video device $VIDEO_DEV not found, p2p_client may fail"
fi

# --- Step 8: Run p2p_client ---
LOG_DIR="$BUILD_DIR/k3s_test_logs"
mkdir -p "$LOG_DIR"

cleanup() {
    echo ""
    info "Stopping p2p_client..."
    kill "$CLIENT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
    info "Done. Logs in: $LOG_DIR/"
}
trap cleanup EXIT INT TERM

echo ""
echo "============================================"
echo "  Starting P2P Client (Publisher)"
echo "  Room: $ROOM_ID  |  Peer: $PEER_ID  |  Duration: ${DURATION}s"
echo "============================================"
echo ""

export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p:${LD_LIBRARY_PATH:-}"

info "Starting p2p_client (publisher)..."
"$BUILD_DIR/src/p2p/p2p_client" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" \
    --peer-id "$PEER_ID" \
    --token "$PUB_TOKEN" \
    --video-dev "$VIDEO_DEV" \
    --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_ADDR" \
    --ssl-cert "$CERT_DIR/server.crt" \
    --ssl-key "$CERT_DIR/server.key" \
    > "$LOG_DIR/p2p_client.log" 2>&1 &
CLIENT_PID=$!
info "  PID=$CLIENT_PID  log=$LOG_DIR/p2p_client.log"

echo ""
info "Publisher running for ${DURATION}s..."
echo ""

# --- Step 9: Monitor for the test duration ---
ELAPSED=0
CHECK_INTERVAL=5
CLIENT_OK=false

while [[ $ELAPSED -lt $DURATION ]]; do
    sleep $CHECK_INTERVAL
    ELAPSED=$((ELAPSED + CHECK_INTERVAL))

    if ! kill -0 $CLIENT_PID 2>/dev/null; then
        fail "p2p_client exited prematurely at ${ELAPSED}s"
        echo "  Last 10 lines of log:"
        tail -10 "$LOG_DIR/p2p_client.log" 2>/dev/null | sed 's/^/    /'
        exit 1
    fi

    if ! $CLIENT_OK && grep -q "signaling connected" "$LOG_DIR/p2p_client.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Publisher connected to signaling"
        CLIENT_OK=true
    fi
done

echo ""
echo "============================================"
echo "  Client Test Results (${DURATION}s)"
echo "============================================"

CLIENT_ALIVE=true
kill -0 $CLIENT_PID 2>/dev/null || CLIENT_ALIVE=false

if $CLIENT_OK; then
    pass "Publisher signaling connected"
else
    fail "Publisher signaling connected"
fi

if $CLIENT_ALIVE; then
    pass "Publisher still running"
else
    fail "Publisher still running"
fi

echo ""
if $CLIENT_OK && $CLIENT_ALIVE; then
    echo -e "${GREEN}Publisher OK.${NC}"
else
    echo -e "${RED}Check failed. Log tail:${NC}"
    tail -20 "$LOG_DIR/p2p_client.log" 2>/dev/null | sed 's/^/  /'
fi
echo ""
