#!/usr/bin/env bash
#
# deploy_client.sh -- Build and deploy the P2P AV Publisher (p2p_client)
#
# Usage:
#   ./scripts/deploy_client.sh [--install-deps] [--deploy-dir DIR] [--start]
#
# Environment variables (override defaults):
#   SIGNALING_ADDR, STUN_SERVER, ROOM_ID, PEER_ID,
#   VIDEO_DEV, AUDIO_DEV, TURN_HOST, TURN_PORT, TURN_SECRET
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/client}"
INSTALL_DEPS=false
START_SERVICES=false

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8080}"
STUN_SERVER="${STUN_SERVER:-${TURN_HOST:-127.0.0.1}:${TURN_PORT:-3478}}"
ROOM_ID="${ROOM_ID:-test-room}"
PEER_ID="${PEER_ID:-pub1}"
VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-default}"
TURN_HOST="${TURN_HOST:-127.0.0.1}"
TURN_PORT="${TURN_PORT:-3478}"
TURN_SECRET="${TURN_SECRET:-p2p-turn-secret}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --deploy-dir)   DEPLOY_DIR="$2"; shift 2 ;;
        --start)        START_SERVICES=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--install-deps] [--deploy-dir DIR] [--start]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;32m[deploy_client]\033[0m $*"; }

# ============================================================
# Step 1: Build everything via scripts/build.sh
# ============================================================
BUILD_ARGS=()
$INSTALL_DEPS && BUILD_ARGS+=(--install-deps)

log "Building project..."
"$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}"

# ============================================================
# Step 2: Generate self-signed TLS certificate
# ============================================================
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" || ! -f "$CERT_DIR/server.key" ]]; then
    log "Generating self-signed TLS certificate..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-av-publisher" 2>/dev/null
    log "Certificate generated."
else
    log "TLS certificate already exists, skipping."
fi

# ============================================================
# Step 3: Assemble deployment directory
# ============================================================
log "Assembling deployment directory: $DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"/{bin,certs,conf}

cp "$BUILD_DIR/p2p/p2p_client"      "$DEPLOY_DIR/bin/"
cp "$BUILD_DIR/signaling-server"     "$DEPLOY_DIR/bin/"
cp "$CERT_DIR/server.crt"           "$DEPLOY_DIR/certs/"
cp "$CERT_DIR/server.key"           "$DEPLOY_DIR/certs/"

cat > "$DEPLOY_DIR/conf/client.env" <<EOF
# P2P Client (Publisher) Configuration
SIGNALING_ADDR=${SIGNALING_ADDR}
STUN_SERVER=${STUN_SERVER}
ROOM_ID=${ROOM_ID}
PEER_ID=${PEER_ID}
VIDEO_DEV=${VIDEO_DEV}
AUDIO_DEV=${AUDIO_DEV}

# Signaling server settings
LISTEN_ADDR=:${SIGNALING_ADDR##*:}
TURN_HOST=${TURN_HOST}
TURN_PORT=${TURN_PORT}
TURN_SECRET=${TURN_SECRET}
MAX_SUBSCRIBERS=5
EOF

cat > "$DEPLOY_DIR/start.sh" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/conf/client.env"

PIDS=()
cleanup() {
    echo "[launcher] Stopping services..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null
    echo "[launcher] All services stopped."
}
trap cleanup EXIT INT TERM

echo "[launcher] Starting signaling server on $LISTEN_ADDR ..."
LISTEN_ADDR="$LISTEN_ADDR" \
TURN_HOST="$TURN_HOST" TURN_PORT="$TURN_PORT" \
TURN_SECRET="$TURN_SECRET" MAX_SUBSCRIBERS="${MAX_SUBSCRIBERS:-5}" \
"$SCRIPT_DIR/bin/signaling-server" &
PIDS+=($!)
sleep 1

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

echo "[launcher] Starting p2p_client (publisher)..."
"$SCRIPT_DIR/bin/p2p_client" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --video-dev "$VIDEO_DEV" --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_HOST:$STUN_PORT" \
    --ssl-cert "$SCRIPT_DIR/certs/server.crt" \
    --ssl-key "$SCRIPT_DIR/certs/server.key" &
PIDS+=($!)

echo "[launcher] Publisher running. Press Ctrl+C to stop."
wait
LAUNCHER
chmod +x "$DEPLOY_DIR/start.sh"

log "Deployment ready: $DEPLOY_DIR/"
log "  bin/p2p_client, bin/signaling-server, certs/, conf/client.env, start.sh"

# ============================================================
# Step 4: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting services..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
