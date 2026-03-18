#!/usr/bin/env bash
#
# run_peer_remote.sh -- Run p2p_peer (subscriber) against a remote k3s server.
#
# Usage:
#   ./scripts/run_peer_remote.sh [OPTIONS]
#
# Options:
#   --server IP            Remote server public IP   (default: 106.54.30.119)
#   --room ROOM            Room ID                   (default: prompt for input)
#   --peer-id ID           Subscriber peer ID         (default: sub1)
#   --admin-secret SECRET  Admin secret for JWT token
#   --duration SEC         Run duration in seconds    (default: 0 = infinite)
#   -h, --help             Show this help
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

SERVER_IP="106.54.30.119"
SIGNALING_PORT="30443"
STUN_PORT="3478"
ROOM_ID=""
PEER_ID="sub1"
ADMIN_SECRET="MOTMaqfVspj7DQvWKlTCIdih"
DURATION=0

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[peer]${NC} $1"; }
pass()  { echo -e "${GREEN}[ OK ]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)       SERVER_IP="$2"; shift 2 ;;
        --room)         ROOM_ID="$2"; shift 2 ;;
        --peer-id)      PEER_ID="$2"; shift 2 ;;
        --admin-secret) ADMIN_SECRET="$2"; shift 2 ;;
        --duration)     DURATION="$2"; shift 2 ;;
        -h|--help)
            head -16 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Prompt for room number if not specified
if [[ -z "$ROOM_ID" ]]; then
    echo ""
    if [[ -t 0 ]]; then
        read -p "Enter room number (6 digits, from publisher): " ROOM_ID
    fi
    ROOM_ID=$(echo "$ROOM_ID" | tr -d '[:space:]')
    if [[ -z "$ROOM_ID" ]]; then
        fail "Room number required. Use --room ROOM or run interactively."
    fi
fi

SIGNALING="${SERVER_IP}:${SIGNALING_PORT}"
STUN="${SERVER_IP}:${STUN_PORT}"
LOG_DIR="$BUILD_DIR/k8s_test_logs"

PEER_BIN="$BUILD_DIR/src/p2p/p2p_peer"
[[ -f "$PEER_BIN" ]] || fail "Binary not found: $PEER_BIN (run build.sh first)"

mkdir -p "$LOG_DIR"

echo ""
echo "========================================="
echo "  P2P Peer (Subscriber) -> Remote K8s"
echo "========================================="
info "Server:    $SERVER_IP"
info "Signaling: $SIGNALING"
info "STUN/TURN: $STUN"
info "Room:      $ROOM_ID"
info "Peer ID:   $PEER_ID"
echo ""

info "Checking signaling health & requesting JWT token (parallel)..."
HEALTH_TMP=$(mktemp)
curl -sk --connect-timeout 15 --max-time 20 "https://${SIGNALING}/health" >"$HEALTH_TMP" 2>/dev/null &
HEALTH_PID=$!

TOKEN_JSON=$(curl -sk --connect-timeout 10 "https://${SIGNALING}/v1/token?peer_id=${PEER_ID}" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" 2>/dev/null)
TOKEN=$(echo "$TOKEN_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || true)
[[ -n "$TOKEN" ]] || fail "Failed to get JWT token: $TOKEN_JSON"
pass "Token obtained for ${PEER_ID}"

wait $HEALTH_PID || true
HEALTH=$(cat "$HEALTH_TMP"); rm -f "$HEALTH_TMP"
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling healthy: $HEALTH"
else
    fail "Signaling server unreachable at ${SIGNALING}: $HEALTH"
fi

export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

LOG_FILE="$LOG_DIR/p2p_peer.log"
rm -f "$LOG_FILE"

info "Starting p2p_peer... (log: $LOG_FILE)"
echo ""

cleanup() {
    echo ""
    info "Stopping p2p_peer..."
    kill $PEER_PID 2>/dev/null || true
    wait $PEER_PID 2>/dev/null || true
    echo ""
    info "Last 10 lines of log:"
    tail -10 "$LOG_FILE" 2>/dev/null | sed 's/^/  /'
    info "Full log: $LOG_FILE"
}

"$PEER_BIN" \
    --signaling "$SIGNALING" \
    --room "$ROOM_ID" \
    --peer-id "$PEER_ID" \
    --token "$TOKEN" \
    --stun "$STUN" \
    2>&1 | tee "$LOG_FILE" &
PEER_PID=$!
trap cleanup EXIT INT TERM

if [[ "$DURATION" -gt 0 ]]; then
    info "Running for ${DURATION}s..."
    sleep "$DURATION"
else
    info "Running indefinitely. Press Ctrl+C to stop."
    wait $PEER_PID
fi
