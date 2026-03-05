#!/usr/bin/env bash
#
# run_av_test.sh -- Quick local test: signaling + publisher + subscriber
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

SIGNALING_PORT="${SIGNALING_PORT:-8443}"
SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:${SIGNALING_PORT}}"
ROOM_ID="${ROOM_ID:-test-room}"
VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-default}"
STUN_SERVER="${STUN_SERVER:-127.0.0.1:3478}"
ADMIN_SECRET="${ADMIN_SECRET:-p2p-admin-secret}"
JWT_SECRET="${JWT_SECRET:-p2p-jwt-secret}"

# Build if needed
if [[ ! -f "$BUILD_DIR/src/p2p/p2p_client" || ! -f "$BUILD_DIR/signaling-server" ]]; then
    echo "[test] Building project..."
    "$SCRIPT_DIR/build.sh"
fi

# Generate cert if needed
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-test" 2>/dev/null
fi

PIDS=()
cleanup() {
    echo ""
    echo "[test] Stopping..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null
    echo "[test] Done."
}
trap cleanup EXIT INT TERM

echo "[test] === P2P AV Local Test ==="
echo "[test] Signaling: $SIGNALING_ADDR"
echo "[test] Room:      $ROOM_ID"
echo "[test] STUN:      $STUN_SERVER"
echo ""

# Start signaling server (HTTPS)
LISTEN_ADDR=":${SIGNALING_PORT}" \
TLS_CERT_FILE="$CERT_DIR/server.crt" \
TLS_KEY_FILE="$CERT_DIR/server.key" \
JWT_SECRET="$JWT_SECRET" \
ADMIN_SECRET="$ADMIN_SECRET" \
    "$BUILD_DIR/signaling-server" &
PIDS+=($!)
sleep 2

# Generate JWT tokens
TOKEN_PUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=pub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_SUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=sub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_PUB_ARG=""
TOKEN_SUB_ARG=""
[[ -n "${TOKEN_PUB:-}" ]] && TOKEN_PUB_ARG="--token $TOKEN_PUB"
[[ -n "${TOKEN_SUB:-}" ]] && TOKEN_SUB_ARG="--token $TOKEN_SUB"

export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p:${LD_LIBRARY_PATH:-}"

# Start publisher
"$BUILD_DIR/src/p2p/p2p_client" \
    --signaling "$SIGNALING_ADDR" --room "$ROOM_ID" --peer-id pub1 \
    --video-dev "$VIDEO_DEV" --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_SERVER" \
    --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
    $TOKEN_PUB_ARG &
PIDS+=($!)
sleep 2

# Start subscriber
"$BUILD_DIR/src/p2p/p2p_peer" \
    --signaling "$SIGNALING_ADDR" --room "$ROOM_ID" --peer-id sub1 \
    --stun "$STUN_SERVER" \
    $TOKEN_SUB_ARG &
PIDS+=($!)

echo "[test] All services running. Press Ctrl+C to stop."
wait
