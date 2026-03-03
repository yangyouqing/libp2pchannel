#!/usr/bin/env bash
#
# deploy_peer.sh -- Build and deploy the P2P AV Subscriber (p2p_peer)
#                   Supports Linux and Windows (via Git Bash / MSYS2)
#
# Usage:
#   ./scripts/deploy_peer.sh [OPTIONS]
#
# Options:
#   --install-deps     Install system build dependencies via apt (Linux only)
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

# ---- Platform detection ----
detect_platform() {
    case "$(uname -s)" in
        Linux*)         P2P_OS=linux ;;
        MINGW*|MSYS*)   P2P_OS=windows ;;
        CYGWIN*)        P2P_OS=windows ;;
        Darwin*)        P2P_OS=macos ;;
        *)              P2P_OS=linux ;;
    esac
}
detect_platform

if [[ "$P2P_OS" == "windows" ]]; then
    EXE_EXT=".exe"
else
    EXE_EXT=""
fi

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/peer}"
INSTALL_DEPS=false
START_SERVICES=false
STOP_SERVICES=false
SKIP_BUILD=false

SIGNALING_ADDR="${SIGNALING_ADDR:-127.0.0.1:8443}"
JWT_SECRET="${JWT_SECRET:-p2p-jwt-secret}"
ADMIN_SECRET="${ADMIN_SECRET:-p2p-admin-secret}"
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
            echo "Detected platform: $P2P_OS"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;36m[deploy_peer]\033[0m $*"; }

log "Platform: $P2P_OS"

# ============================================================
# Stop running services
# ============================================================
stop_services() {
    log "Stopping running services..."
    if [[ "$P2P_OS" == "windows" ]]; then
        taskkill //F //IM "p2p_peer.exe" 2>/dev/null && log "  p2p_peer stopped" || true
    else
        local pids
        pids=$(pgrep -f "$DEPLOY_DIR/bin/p2p_peer" 2>/dev/null || true)
        [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  p2p_peer stopped" || true
    fi
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
PEER_BIN="$BUILD_DIR/p2p/p2p_peer${EXE_EXT}"
if [[ ! -f "$PEER_BIN" ]]; then
    log "ERROR: $PEER_BIN not found. Run without --skip-build first."
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

cp -f "$PEER_BIN" "$DEPLOY_DIR/bin/"

cat > "$DEPLOY_DIR/conf/peer.env" <<EOF
# P2P Peer (Subscriber) Configuration
# Platform: $P2P_OS
SIGNALING_ADDR=${SIGNALING_ADDR}
STUN_SERVER=${STUN_SERVER}
ROOM_ID=${ROOM_ID}
PEER_ID=${PEER_ID}
JWT_SECRET=${JWT_SECRET}
ADMIN_SECRET=${ADMIN_SECRET}
EOF

# ---- Generate bash launcher ----
cat > "$DEPLOY_DIR/start.sh" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/conf/peer.env"
LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR"

# Detect executable extension
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE_EXT=".exe" ;;
    *)                     EXE_EXT="" ;;
esac

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

# Request JWT token from signaling server
SIG_HOST="${SIGNALING_ADDR%:*}"
SIG_PORT="${SIGNALING_ADDR##*:}"
[[ "$SIG_HOST" == "0.0.0.0" || "$SIG_HOST" == "" ]] && SIG_HOST="127.0.0.1"
TOKEN_URL="https://${SIG_HOST}:${SIG_PORT}/v1/token?peer_id=${PEER_ID}"
echo "[launcher] Requesting JWT token from $TOKEN_URL ..."
SUB_TOKEN=$(curl -sk -H "Authorization: Bearer $ADMIN_SECRET" "$TOKEN_URL" | \
    sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
TOKEN_ARG=""
if [[ -n "$SUB_TOKEN" ]]; then
    TOKEN_ARG="--token $SUB_TOKEN"
else
    echo "[launcher] WARNING: Could not obtain JWT token, proceeding without auth"
fi

echo "[launcher] Starting p2p_peer (subscriber)..."
echo "[launcher]   Signaling: $SIGNALING_ADDR  Room: $ROOM_ID  STUN: $STUN_HOST:$STUN_PORT"
echo "[launcher]   Log: $LOG_DIR/p2p_peer.log"

exec "$SCRIPT_DIR/bin/p2p_peer${EXE_EXT}" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --stun "$STUN_HOST:$STUN_PORT" \
    $TOKEN_ARG 2>&1 | tee "$LOG_DIR/p2p_peer.log"
LAUNCHER
chmod +x "$DEPLOY_DIR/start.sh"

# ---- Generate Windows batch launcher ----
if [[ "$P2P_OS" == "windows" ]]; then
    cat > "$DEPLOY_DIR/start.bat" <<WINLAUNCHER
@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

:: Load config
for /f "usebackq tokens=1* delims==" %%a in ("%SCRIPT_DIR%\\conf\\peer.env") do (
    set "line=%%a"
    if not "!line:~0,1!"=="#" if not "%%a"=="" set "%%a=%%b"
)

if not exist "%SCRIPT_DIR%\\logs" mkdir "%SCRIPT_DIR%\\logs"

for /f "tokens=1 delims=:" %%h in ("%STUN_SERVER%") do set "STUN_HOST=%%h"
for /f "tokens=2 delims=:" %%p in ("%STUN_SERVER%") do set "STUN_PORT=%%p"
if "%STUN_PORT%"=="" set "STUN_PORT=3478"

echo [launcher] Starting p2p_peer (subscriber)...
echo [launcher]   Signaling: %SIGNALING_ADDR%  Room: %ROOM_ID%  STUN: %STUN_HOST%:%STUN_PORT%

"%SCRIPT_DIR%\\bin\\p2p_peer.exe" ^
    --signaling "%SIGNALING_ADDR%" ^
    --room "%ROOM_ID%" --peer-id "%PEER_ID%" ^
    --stun "%STUN_HOST%:%STUN_PORT%"

echo [launcher] Peer stopped.
WINLAUNCHER
    log "Windows batch launcher generated: start.bat"
fi

# ---- Generate stop scripts ----
cat > "$DEPLOY_DIR/stop.sh" <<'STOPPER'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        taskkill //F //IM "p2p_peer.exe" 2>/dev/null || true
        ;;
    *)
        pkill -f "$SCRIPT_DIR/bin/p2p_peer" 2>/dev/null || true
        ;;
esac
sleep 1
echo "[stop] Done."
STOPPER
chmod +x "$DEPLOY_DIR/stop.sh"

if [[ "$P2P_OS" == "windows" ]]; then
    cat > "$DEPLOY_DIR/stop.bat" <<'WINSTOPPER'
@echo off
echo [stop] Stopping p2p_peer...
taskkill /F /IM p2p_peer.exe 2>nul
timeout /t 1 /nobreak >nul
echo [stop] Done.
WINSTOPPER
fi

log "Deployment ready ($P2P_OS): $DEPLOY_DIR/"
log "  bin/p2p_peer${EXE_EXT}    Subscriber binary"
log "  conf/peer.env   Configuration"
log "  logs/           Runtime logs"
if [[ "$P2P_OS" == "windows" ]]; then
    log "  start.bat       Start subscriber (Windows native)"
    log "  stop.bat        Stop subscriber (Windows native)"
fi
log "  start.sh        Start subscriber (bash)"
log "  stop.sh         Stop subscriber (bash)"

# ============================================================
# Step 4: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting peer..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
