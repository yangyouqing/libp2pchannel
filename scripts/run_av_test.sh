#!/usr/bin/env bash
#
# run_av_test.sh -- Quick local test: signaling + publisher + subscriber
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8080}"
ROOM_ID="${ROOM_ID:-test-room}"
VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-default}"
STUN_SERVER="${STUN_SERVER:-127.0.0.1:3478}"

# Build if needed
if [[ ! -f "$BUILD_DIR/p2p/p2p_client" || ! -f "$BUILD_DIR/signaling-server" ]]; then
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

SIG_PORT="${SIGNALING_ADDR##*:}"

echo "[test] === P2P AV Local Test ==="
echo "[test] Signaling: $SIGNALING_ADDR"
echo "[test] Room:      $ROOM_ID"
echo "[test] STUN:      $STUN_SERVER"
echo ""

# Start signaling server
LISTEN_ADDR=":$SIG_PORT" "$BUILD_DIR/signaling-server" &
PIDS+=($!)
sleep 1

# Start publisher
"$BUILD_DIR/p2p/p2p_client" \
    --signaling "$SIGNALING_ADDR" --room "$ROOM_ID" --peer-id pub1 \
    --video-dev "$VIDEO_DEV" --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_SERVER" \
    --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" &
PIDS+=($!)
sleep 2

# Start subscriber
"$BUILD_DIR/p2p/p2p_peer" \
    --signaling "$SIGNALING_ADDR" --room "$ROOM_ID" --peer-id sub1 \
    --stun "$STUN_SERVER" &
PIDS+=($!)

echo "[test] All services running. Press Ctrl+C to stop."
wait
