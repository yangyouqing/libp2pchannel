#!/usr/bin/env bash
#
# run_client_remote.sh -- Run p2p_client (publisher) against a remote k3s server.
#
# Usage:
#   ./scripts/run_client_remote.sh [OPTIONS]
#
# Options:
#   --server IP            Remote server public IP   (default: 106.54.30.119)
#   --room ROOM            Room ID                   (default: remote-test)
#   --peer-id ID           Publisher peer ID          (default: pub1)
#   --video-dev DEV        V4L2 video device          (default: auto-detect)
#   --audio-dev DEV        ALSA audio device          (default: default)
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
ROOM_ID="remote-test"
PEER_ID="pub1"
ADMIN_SECRET="MOTMaqfVspj7DQvWKlTCIdih"
DURATION=0
AUDIO_DEV="default"
VIDEO_DEV=""

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[client]${NC} $1"; }
pass()  { echo -e "${GREEN}[  OK  ]${NC} $1"; }
fail()  { echo -e "${RED}[ FAIL ]${NC} $1"; exit 1; }

# Auto-detect video device
detect_video() {
    for vdev in /dev/video*; do
        if [[ -c "$vdev" ]] && v4l2-ctl -d "$vdev" --info 2>/dev/null | grep -q "uvcvideo"; then
            echo "$vdev"; return
        fi
    done
    echo "/dev/video0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)       SERVER_IP="$2"; shift 2 ;;
        --room)         ROOM_ID="$2"; shift 2 ;;
        --peer-id)      PEER_ID="$2"; shift 2 ;;
        --video-dev)    VIDEO_DEV="$2"; shift 2 ;;
        --audio-dev)    AUDIO_DEV="$2"; shift 2 ;;
        --admin-secret) ADMIN_SECRET="$2"; shift 2 ;;
        --duration)     DURATION="$2"; shift 2 ;;
        -h|--help)
            head -18 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

[[ -z "$VIDEO_DEV" ]] && VIDEO_DEV=$(detect_video)

SIGNALING="${SERVER_IP}:${SIGNALING_PORT}"
STUN="${SERVER_IP}:${STUN_PORT}"
LOG_DIR="$BUILD_DIR/k8s_test_logs"
CERT_DIR="$BUILD_DIR/certs"

CLIENT_BIN="$BUILD_DIR/src/p2p/p2p_client"
[[ -f "$CLIENT_BIN" ]] || fail "Binary not found: $CLIENT_BIN (run build.sh first)"

mkdir -p "$LOG_DIR" "$CERT_DIR"

if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating QUIC TLS certificates..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-quic" 2>/dev/null
fi

echo ""
echo "========================================"
echo "  P2P Client (Publisher) -> Remote K8s"
echo "========================================"
info "Server:    $SERVER_IP"
info "Signaling: $SIGNALING"
info "STUN/TURN: $STUN"
info "Room:      $ROOM_ID"
info "Peer ID:   $PEER_ID"
info "Video:     $VIDEO_DEV"
info "Audio:     $AUDIO_DEV"
echo ""

info "Checking signaling health..."
HEALTH=$(curl -sk --connect-timeout 5 "https://${SIGNALING}/health" 2>/dev/null || echo "{}")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling healthy: $HEALTH"
else
    fail "Signaling server unreachable at ${SIGNALING}: $HEALTH"
fi

info "Requesting JWT token..."
TOKEN_JSON=$(curl -sk --connect-timeout 10 "https://${SIGNALING}/v1/token?peer_id=${PEER_ID}" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" 2>/dev/null)
TOKEN=$(echo "$TOKEN_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || true)
[[ -n "$TOKEN" ]] || fail "Failed to get JWT token: $TOKEN_JSON"
pass "Token obtained for ${PEER_ID}"

export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

LOG_FILE="$LOG_DIR/p2p_client.log"
rm -f "$LOG_FILE"

info "Starting p2p_client... (log: $LOG_FILE)"
echo ""

cleanup() {
    echo ""
    info "Stopping p2p_client..."
    kill $CLIENT_PID 2>/dev/null || true
    wait $CLIENT_PID 2>/dev/null || true
    echo ""
    info "Last 10 lines of log:"
    tail -10 "$LOG_FILE" 2>/dev/null | sed 's/^/  /'
    info "Full log: $LOG_FILE"
}

"$CLIENT_BIN" \
    --signaling "$SIGNALING" \
    --room "$ROOM_ID" \
    --peer-id "$PEER_ID" \
    --token "$TOKEN" \
    --video-dev "$VIDEO_DEV" \
    --audio-dev "$AUDIO_DEV" \
    --stun "$STUN" \
    --ssl-cert "$CERT_DIR/server.crt" \
    --ssl-key "$CERT_DIR/server.key" \
    2>&1 | tee "$LOG_FILE" &
CLIENT_PID=$!
trap cleanup EXIT INT TERM

if [[ "$DURATION" -gt 0 ]]; then
    info "Running for ${DURATION}s..."
    sleep "$DURATION"
else
    info "Running indefinitely. Press Ctrl+C to stop."
    wait $CLIENT_PID
fi
