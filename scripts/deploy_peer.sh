#!/usr/bin/env bash
#
# deploy_peer.sh -- Build and deploy the P2P AV Subscriber (p2p_peer)
#
# Usage:
#   ./scripts/deploy_peer.sh [OPTIONS]
#
# Options:
#   --install-deps     Install system build dependencies via apt
#   --deploy-dir DIR   Custom deployment directory (default: deploy/peer)
#   --start            Build, deploy, and start the subscriber
#   --stop             Stop running subscriber
#   --restart          Stop then start subscriber
#   --skip-build       Skip build step (use existing binaries)
#   -h, --help         Show this help
#
# Environment variables (override defaults):
#   SIGNALING_ADDR   Signaling server address       (default: 127.0.0.1:8080)
#   STUN_SERVER      STUN/TURN server address        (default: TURN_HOST:TURN_PORT)
#   ROOM_ID          Room identifier                 (default: test-room)
#   PEER_ID          Subscriber peer ID               (default: sub1)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/peer}"
INSTALL_DEPS=false
START_SERVICES=false
STOP_SERVICES=false
SKIP_BUILD=false

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8080}"
TURN_HOST="${TURN_HOST:-127.0.0.1}"
TURN_PORT="${TURN_PORT:-3478}"
STUN_SERVER="${STUN_SERVER:-${TURN_HOST}:${TURN_PORT}}"
ROOM_ID="${ROOM_ID:-test-room}"
PEER_ID="${PEER_ID:-sub1}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps) INSTALL_DEPS=true; shift ;;
        --deploy-dir)   DEPLOY_DIR="$2"; shift 2 ;;
        --start)        START_SERVICES=true; shift ;;
        --stop)         STOP_SERVICES=true; shift ;;
        --restart)      STOP_SERVICES=true; START_SERVICES=true; shift ;;
        --skip-build)   SKIP_BUILD=true; shift ;;
        -h|--help)
            head -25 "$0" | grep '^#' | sed 's/^# \?//'
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;36m[deploy_peer]\033[0m $*"; }

# ============================================================
# Stop running services
# ============================================================
stop_services() {
    log "Stopping running services..."
    local pids
    pids=$(pgrep -f "$DEPLOY_DIR/bin/p2p_peer" 2>/dev/null || true)
    [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  p2p_peer stopped" || true
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

# Verify binary exists
if [[ ! -f "$BUILD_DIR/p2p/p2p_peer" ]]; then
    log "ERROR: $BUILD_DIR/p2p/p2p_peer not found. Run without --skip-build first."
    exit 1
fi

# ============================================================
# Step 2: Stop old processes before copying (avoid "text file busy")
# ============================================================
if [[ -d "$DEPLOY_DIR/bin" ]]; then
    stop_services
fi

# ============================================================
# Step 3: Assemble deployment directory
# ============================================================
log "Assembling deployment directory: $DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"/{bin,conf,logs}

cp -f "$BUILD_DIR/p2p/p2p_peer" "$DEPLOY_DIR/bin/"

cat > "$DEPLOY_DIR/conf/peer.env" <<EOF
# P2P Peer (Subscriber) Configuration
SIGNALING_ADDR=${SIGNALING_ADDR}
STUN_SERVER=${STUN_SERVER}
ROOM_ID=${ROOM_ID}
PEER_ID=${PEER_ID}
EOF

cat > "$DEPLOY_DIR/start.sh" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/conf/peer.env"
LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR"

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

echo "[launcher] Starting p2p_peer (subscriber)..."
echo "[launcher]   Signaling: $SIGNALING_ADDR  Room: $ROOM_ID  STUN: $STUN_HOST:$STUN_PORT"
echo "[launcher]   Log: $LOG_DIR/p2p_peer.log"

exec "$SCRIPT_DIR/bin/p2p_peer" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --stun "$STUN_HOST:$STUN_PORT" 2>&1 | tee "$LOG_DIR/p2p_peer.log"
LAUNCHER
chmod +x "$DEPLOY_DIR/start.sh"

cat > "$DEPLOY_DIR/stop.sh" <<'STOPPER'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "[stop] Stopping p2p_peer..."
pkill -f "$SCRIPT_DIR/bin/p2p_peer" 2>/dev/null || true
sleep 1
echo "[stop] Done."
STOPPER
chmod +x "$DEPLOY_DIR/stop.sh"

log "Deployment ready: $DEPLOY_DIR/"
log "  bin/p2p_peer    Subscriber binary"
log "  conf/peer.env   Configuration"
log "  logs/           Runtime logs"
log "  start.sh        Start subscriber"
log "  stop.sh         Stop subscriber"

# ============================================================
# Step 4: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting peer..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
