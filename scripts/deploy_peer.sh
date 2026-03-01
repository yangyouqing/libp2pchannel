#!/usr/bin/env bash
#
# deploy_peer.sh -- Build and deploy the P2P AV Subscriber (p2p_peer)
#
# Usage:
#   ./scripts/deploy_peer.sh [--install-deps] [--deploy-dir DIR] [--start]
#
# Environment variables (override defaults):
#   SIGNALING_ADDR, STUN_SERVER, ROOM_ID, PEER_ID
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/peer}"
INSTALL_DEPS=false
START_SERVICES=false

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8080}"
STUN_SERVER="${STUN_SERVER:-${TURN_HOST:-127.0.0.1}:${TURN_PORT:-3478}}"
ROOM_ID="${ROOM_ID:-test-room}"
PEER_ID="${PEER_ID:-sub1}"

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

log() { echo -e "\033[1;36m[deploy_peer]\033[0m $*"; }

# ============================================================
# Step 1: Build everything via scripts/build.sh
# ============================================================
BUILD_ARGS=()
$INSTALL_DEPS && BUILD_ARGS+=(--install-deps)

log "Building project..."
"$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]+"${BUILD_ARGS[@]}"}"

# ============================================================
# Step 2: Assemble deployment directory
# ============================================================
log "Assembling deployment directory: $DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR"/{bin,conf}

cp "$BUILD_DIR/p2p/p2p_peer" "$DEPLOY_DIR/bin/"

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

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

echo "[launcher] Starting p2p_peer (subscriber)..."
echo "[launcher] Signaling: $SIGNALING_ADDR  Room: $ROOM_ID  STUN: $STUN_HOST:$STUN_PORT"

exec "$SCRIPT_DIR/bin/p2p_peer" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --stun "$STUN_HOST:$STUN_PORT"
LAUNCHER
chmod +x "$DEPLOY_DIR/start.sh"

log "Deployment ready: $DEPLOY_DIR/"
log "  bin/p2p_peer, conf/peer.env, start.sh"

# ============================================================
# Step 3: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting peer..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
