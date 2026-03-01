#!/usr/bin/env bash
#
# deploy_client.sh -- Build and deploy the P2P AV Publisher (p2p_client)
#
# Usage:
#   ./scripts/deploy_client.sh [OPTIONS]
#
# Options:
#   --install-deps     Install system build dependencies via apt
#   --deploy-dir DIR   Custom deployment directory (default: deploy/client)
#   --start            Build, deploy, and start all services
#   --stop             Stop running services
#   --restart          Stop then start services
#   --skip-build       Skip build step (use existing binaries)
#   -h, --help         Show this help
#
# Environment variables (override defaults):
#   SIGNALING_ADDR   Signaling server address       (default: 127.0.0.1:8080)
#   STUN_SERVER      STUN/TURN server address        (default: TURN_HOST:TURN_PORT)
#   ROOM_ID          Room identifier                 (default: test-room)
#   PEER_ID          Publisher peer ID                (default: pub1)
#   VIDEO_DEV        V4L2 video device               (default: /dev/video0)
#   AUDIO_DEV        ALSA audio device               (default: default)
#   TURN_HOST        TURN server host                 (default: 127.0.0.1)
#   TURN_PORT        TURN server port                 (default: 3478)
#   TURN_SECRET      TURN shared secret               (default: p2p-turn-secret)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/client}"
INSTALL_DEPS=false
START_SERVICES=false
STOP_SERVICES=false
SKIP_BUILD=false

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8080}"
TURN_HOST="${TURN_HOST:-127.0.0.1}"
TURN_PORT="${TURN_PORT:-3478}"
STUN_SERVER="${STUN_SERVER:-${TURN_HOST}:${TURN_PORT}}"
ROOM_ID="${ROOM_ID:-test-room}"
PEER_ID="${PEER_ID:-pub1}"
VIDEO_DEV="${VIDEO_DEV:-/dev/video0}"
AUDIO_DEV="${AUDIO_DEV:-default}"
TURN_SECRET="${TURN_SECRET:-p2p-turn-secret}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --deploy-dir)   DEPLOY_DIR="$2"; shift 2 ;;
        --start)        START_SERVICES=true; shift ;;
        --stop)         STOP_SERVICES=true; shift ;;
        --restart)      STOP_SERVICES=true; START_SERVICES=true; shift ;;
        --skip-build)   SKIP_BUILD=true; shift ;;
        -h|--help)
            head -30 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;32m[deploy_client]\033[0m $*"; }

# ============================================================
# Stop running services
# ============================================================
stop_services() {
    log "Stopping running services..."
    local pids
    pids=$(pgrep -f "$DEPLOY_DIR/bin/p2p_client" 2>/dev/null || true)
    [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  p2p_client stopped" || true

    pids=$(pgrep -f "$DEPLOY_DIR/bin/signaling-server" 2>/dev/null || true)
    [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  signaling-server stopped" || true

    sleep 1
    log "Services stopped."
}

if $STOP_SERVICES && ! $START_SERVICES; then
    stop_services
    exit 0
fi

# ============================================================
# Step 1: Build
# ============================================================
if ! $SKIP_BUILD; then
    BUILD_ARGS=()
    $INSTALL_DEPS && BUILD_ARGS+=(--install-deps)

    log "Building project..."
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}"
fi

# Verify binaries exist
for bin in "$BUILD_DIR/p2p/p2p_client" "$BUILD_DIR/signaling-server"; do
    if [[ ! -f "$bin" ]]; then
        log "ERROR: $bin not found. Run without --skip-build first."
        exit 1
    fi
done

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
# Step 3: Stop old processes before copying (avoid "text file busy")
# ============================================================
if [[ -d "$DEPLOY_DIR/bin" ]]; then
    stop_services
fi

# ============================================================
# Step 4: Assemble deployment directory
# ============================================================
log "Assembling deployment directory: $DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"/{bin,certs,conf,logs}

cp -f "$BUILD_DIR/p2p/p2p_client"   "$DEPLOY_DIR/bin/"
cp -f "$BUILD_DIR/signaling-server"  "$DEPLOY_DIR/bin/"
cp -f "$CERT_DIR/server.crt"        "$DEPLOY_DIR/certs/"
cp -f "$CERT_DIR/server.key"        "$DEPLOY_DIR/certs/"

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
LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR"

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
"$SCRIPT_DIR/bin/signaling-server" 2>&1 | tee "$LOG_DIR/signaling.log" &
PIDS+=($!)
sleep 1

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

echo "[launcher] Starting p2p_client (publisher)..."
echo "[launcher]   Signaling: $SIGNALING_ADDR  Room: $ROOM_ID  STUN: $STUN_HOST:$STUN_PORT"
echo "[launcher]   Video: $VIDEO_DEV  Audio: $AUDIO_DEV"
"$SCRIPT_DIR/bin/p2p_client" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --video-dev "$VIDEO_DEV" --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_HOST:$STUN_PORT" \
    --ssl-cert "$SCRIPT_DIR/certs/server.crt" \
    --ssl-key "$SCRIPT_DIR/certs/server.key" 2>&1 | tee "$LOG_DIR/p2p_client.log" &
PIDS+=($!)

echo "[launcher] Publisher running. Press Ctrl+C to stop."
echo "[launcher] Logs: $LOG_DIR/"
wait
LAUNCHER
chmod +x "$DEPLOY_DIR/start.sh"

cat > "$DEPLOY_DIR/stop.sh" <<'STOPPER'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "[stop] Stopping p2p_client..."
pkill -f "$SCRIPT_DIR/bin/p2p_client" 2>/dev/null || true
echo "[stop] Stopping signaling-server..."
pkill -f "$SCRIPT_DIR/bin/signaling-server" 2>/dev/null || true
sleep 1
echo "[stop] Done."
STOPPER
chmod +x "$DEPLOY_DIR/stop.sh"

log "Deployment ready: $DEPLOY_DIR/"
log "  bin/p2p_client        Publisher binary"
log "  bin/signaling-server  Signaling server binary"
log "  certs/                TLS certificates"
log "  conf/client.env       Configuration"
log "  logs/                 Runtime logs"
log "  start.sh              Start all services"
log "  stop.sh               Stop all services"

# ============================================================
# Step 5: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting services..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
