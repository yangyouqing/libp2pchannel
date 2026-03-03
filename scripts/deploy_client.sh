#!/usr/bin/env bash
#
# deploy_client.sh -- Build and deploy the P2P AV Publisher (p2p_client)
#                     Supports Linux and Windows (via Git Bash / MSYS2)
#
# Usage:
#   ./scripts/deploy_client.sh [OPTIONS]
#
# Options:
#   --install-deps     Install system build dependencies via apt (Linux only)
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
#   VIDEO_DEV        Video device                     (Linux: /dev/video0, Windows: Integrated Camera)
#   AUDIO_DEV        Audio device                     (Linux: default, Windows: Microphone)
#   TURN_HOST        TURN server host                 (default: 127.0.0.1)
#   TURN_PORT        TURN server port                 (default: 3478)
#   TURN_SECRET      TURN shared secret               (default: p2p-turn-secret)
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
    DEFAULT_VIDEO_DEV="Integrated Camera"
    DEFAULT_AUDIO_DEV="Microphone"
else
    EXE_EXT=""
    DEFAULT_VIDEO_DEV="/dev/video0"
    DEFAULT_AUDIO_DEV="default"
fi

DEPLOY_DIR="${DEPLOY_DIR:-$PROJECT_DIR/deploy/client}"
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
PEER_ID="${PEER_ID:-pub1}"
VIDEO_DEV="${VIDEO_DEV:-$DEFAULT_VIDEO_DEV}"
AUDIO_DEV="${AUDIO_DEV:-$DEFAULT_AUDIO_DEV}"
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
            echo "Detected platform: $P2P_OS"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

log() { echo -e "\033[1;32m[deploy_client]\033[0m $*"; }

log "Platform: $P2P_OS"

# ============================================================
# Stop running services
# ============================================================
stop_services() {
    log "Stopping running services..."
    if [[ "$P2P_OS" == "windows" ]]; then
        taskkill //F //IM "p2p_client.exe" 2>/dev/null && log "  p2p_client stopped" || true
        taskkill //F //IM "signaling-server.exe" 2>/dev/null && log "  signaling-server stopped" || true
    else
        local pids
        pids=$(pgrep -f "$DEPLOY_DIR/bin/p2p_client" 2>/dev/null || true)
        [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  p2p_client stopped" || true

        pids=$(pgrep -f "$DEPLOY_DIR/bin/signaling-server" 2>/dev/null || true)
        [[ -n "$pids" ]] && kill $pids 2>/dev/null && log "  signaling-server stopped" || true
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

# Verify binaries exist
CLIENT_BIN="$BUILD_DIR/p2p/p2p_client${EXE_EXT}"
SIGNALING_BIN="$BUILD_DIR/signaling-server${EXE_EXT}"

for bin in "$CLIENT_BIN" "$SIGNALING_BIN"; do
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

cp -f "$CLIENT_BIN"      "$DEPLOY_DIR/bin/"
cp -f "$SIGNALING_BIN"   "$DEPLOY_DIR/bin/"
cp -f "$CERT_DIR/server.crt" "$DEPLOY_DIR/certs/"
cp -f "$CERT_DIR/server.key" "$DEPLOY_DIR/certs/"

# Generate JWT token for the publisher
log "Generating JWT token for publisher..."
PUB_TOKEN=$("$DEPLOY_DIR/bin/signaling-server${EXE_EXT}" 2>/dev/null & SIG_PID=$!; sleep 0; kill $SIG_PID 2>/dev/null || true; echo "")
# Use a simple token generation via the server's /v1/token endpoint, or embed in env
# For simplicity, we'll store the JWT secret and let the launcher generate tokens

# Generate signaling server TLS certificate
SIG_CERT_DIR="$DEPLOY_DIR/certs"
mkdir -p "$SIG_CERT_DIR"
if [[ ! -f "$SIG_CERT_DIR/sig_server.crt" ]]; then
    log "Generating signaling server TLS certificate..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$SIG_CERT_DIR/sig_server.key" -out "$SIG_CERT_DIR/sig_server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-signaling" 2>/dev/null
fi

cat > "$DEPLOY_DIR/conf/client.env" <<EOF
# P2P Client (Publisher) Configuration
# Platform: $P2P_OS
SIGNALING_ADDR=${SIGNALING_ADDR}
STUN_SERVER=${STUN_SERVER}
ROOM_ID=${ROOM_ID}
PEER_ID=${PEER_ID}
VIDEO_DEV=${VIDEO_DEV}
AUDIO_DEV=${AUDIO_DEV}

# Signaling server settings (HTTPS)
LISTEN_ADDR=:${SIGNALING_ADDR##*:}
TLS_CERT_FILE=\${SCRIPT_DIR}/certs/sig_server.crt
TLS_KEY_FILE=\${SCRIPT_DIR}/certs/sig_server.key
JWT_SECRET=${JWT_SECRET}
ADMIN_SECRET=${ADMIN_SECRET}
TURN_HOST=${TURN_HOST}
TURN_PORT=${TURN_PORT}
TURN_SECRET=${TURN_SECRET}
MAX_SUBSCRIBERS=10
EOF

# ---- Generate bash launcher (works on both platforms via Git Bash) ----
cat > "$DEPLOY_DIR/start.sh" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/conf/client.env"
LOG_DIR="$SCRIPT_DIR/logs"
mkdir -p "$LOG_DIR"

# Detect executable extension
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE_EXT=".exe" ;;
    *)                     EXE_EXT="" ;;
esac

PIDS=()
cleanup() {
    echo "[launcher] Stopping services..."
    for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null
    echo "[launcher] All services stopped."
}
trap cleanup EXIT INT TERM

echo "[launcher] Starting signaling server (HTTPS) on $LISTEN_ADDR ..."
LISTEN_ADDR="$LISTEN_ADDR" \
TLS_CERT_FILE="$TLS_CERT_FILE" TLS_KEY_FILE="$TLS_KEY_FILE" \
JWT_SECRET="$JWT_SECRET" ADMIN_SECRET="$ADMIN_SECRET" \
TURN_HOST="$TURN_HOST" TURN_PORT="$TURN_PORT" \
TURN_SECRET="$TURN_SECRET" MAX_SUBSCRIBERS="${MAX_SUBSCRIBERS:-10}" \
"$SCRIPT_DIR/bin/signaling-server${EXE_EXT}" 2>&1 | tee "$LOG_DIR/signaling.log" &
PIDS+=($!)
sleep 2

# Generate JWT token for publisher via signaling server's /v1/token endpoint
SIG_HOST="${SIGNALING_ADDR%:*}"
SIG_PORT="${SIGNALING_ADDR##*:}"
[[ "$SIG_HOST" == "0.0.0.0" || "$SIG_HOST" == "" ]] && SIG_HOST="127.0.0.1"
TOKEN_URL="https://${SIG_HOST}:${SIG_PORT}/v1/token?peer_id=${PEER_ID}"
echo "[launcher] Requesting JWT token from $TOKEN_URL ..."
PUB_TOKEN=$(curl -sk -H "Authorization: Bearer $ADMIN_SECRET" "$TOKEN_URL" | \
    sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
if [[ -z "$PUB_TOKEN" ]]; then
    echo "[launcher] WARNING: Could not obtain JWT token, proceeding without auth"
fi

STUN_HOST="${STUN_SERVER%:*}"
STUN_PORT="${STUN_SERVER##*:}"
[[ "$STUN_PORT" == "$STUN_HOST" ]] && STUN_PORT=3478

echo "[launcher] Starting p2p_client (publisher)..."
echo "[launcher]   Signaling: $SIGNALING_ADDR  Room: $ROOM_ID  STUN: $STUN_HOST:$STUN_PORT"
echo "[launcher]   Video: $VIDEO_DEV  Audio: $AUDIO_DEV"

TOKEN_ARG=""
[[ -n "$PUB_TOKEN" ]] && TOKEN_ARG="--token $PUB_TOKEN"

"$SCRIPT_DIR/bin/p2p_client${EXE_EXT}" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" --peer-id "$PEER_ID" \
    --video-dev "$VIDEO_DEV" --audio-dev "$AUDIO_DEV" \
    --stun "$STUN_HOST:$STUN_PORT" \
    --ssl-cert "$SCRIPT_DIR/certs/server.crt" \
    --ssl-key "$SCRIPT_DIR/certs/server.key" \
    $TOKEN_ARG 2>&1 | tee "$LOG_DIR/p2p_client.log" &
PIDS+=($!)

echo "[launcher] Publisher running. Press Ctrl+C to stop."
echo "[launcher] Logs: $LOG_DIR/"
wait
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
for /f "usebackq tokens=1* delims==" %%a in ("%SCRIPT_DIR%\\conf\\client.env") do (
    set "line=%%a"
    if not "!line:~0,1!"=="#" if not "%%a"=="" set "%%a=%%b"
)

if not exist "%SCRIPT_DIR%\\logs" mkdir "%SCRIPT_DIR%\\logs"

echo [launcher] Starting signaling server on %LISTEN_ADDR% ...
start "signaling-server" /B cmd /c "%SCRIPT_DIR%\\bin\\signaling-server.exe 2>&1 | tee %SCRIPT_DIR%\\logs\\signaling.log"
timeout /t 1 /nobreak >nul

for /f "tokens=1 delims=:" %%h in ("%STUN_SERVER%") do set "STUN_HOST=%%h"
for /f "tokens=2 delims=:" %%p in ("%STUN_SERVER%") do set "STUN_PORT=%%p"
if "%STUN_PORT%"=="" set "STUN_PORT=3478"

echo [launcher] Starting p2p_client (publisher)...
echo [launcher]   Signaling: %SIGNALING_ADDR%  Room: %ROOM_ID%  STUN: %STUN_HOST%:%STUN_PORT%
echo [launcher]   Video: %VIDEO_DEV%  Audio: %AUDIO_DEV%

"%SCRIPT_DIR%\\bin\\p2p_client.exe" ^
    --signaling "%SIGNALING_ADDR%" ^
    --room "%ROOM_ID%" --peer-id "%PEER_ID%" ^
    --video-dev "%VIDEO_DEV%" --audio-dev "%AUDIO_DEV%" ^
    --stun "%STUN_HOST%:%STUN_PORT%" ^
    --ssl-cert "%SCRIPT_DIR%\\certs\\server.crt" ^
    --ssl-key "%SCRIPT_DIR%\\certs\\server.key"

echo [launcher] Publisher stopped.
taskkill /F /IM signaling-server.exe 2>nul
WINLAUNCHER
    log "Windows batch launcher generated: start.bat"
fi

# ---- Generate stop scripts ----
cat > "$DEPLOY_DIR/stop.sh" <<'STOPPER'
#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        taskkill //F //IM "p2p_client.exe" 2>/dev/null || true
        taskkill //F //IM "signaling-server.exe" 2>/dev/null || true
        ;;
    *)
        pkill -f "$SCRIPT_DIR/bin/p2p_client" 2>/dev/null || true
        pkill -f "$SCRIPT_DIR/bin/signaling-server" 2>/dev/null || true
        ;;
esac
sleep 1
echo "[stop] Done."
STOPPER
chmod +x "$DEPLOY_DIR/stop.sh"

if [[ "$P2P_OS" == "windows" ]]; then
    cat > "$DEPLOY_DIR/stop.bat" <<'WINSTOPPER'
@echo off
echo [stop] Stopping p2p_client...
taskkill /F /IM p2p_client.exe 2>nul
echo [stop] Stopping signaling-server...
taskkill /F /IM signaling-server.exe 2>nul
timeout /t 1 /nobreak >nul
echo [stop] Done.
WINSTOPPER
fi

log "Deployment ready ($P2P_OS): $DEPLOY_DIR/"
log "  bin/p2p_client${EXE_EXT}        Publisher binary"
log "  bin/signaling-server${EXE_EXT}  Signaling server binary"
log "  certs/                TLS certificates"
log "  conf/client.env       Configuration"
log "  logs/                 Runtime logs"
if [[ "$P2P_OS" == "windows" ]]; then
    log "  start.bat             Start all services (Windows native)"
    log "  stop.bat              Stop all services (Windows native)"
fi
log "  start.sh              Start all services (bash)"
log "  stop.sh               Stop all services (bash)"

# ============================================================
# Step 5: Optionally start
# ============================================================
if $START_SERVICES; then
    log "Starting services..."
    exec "$DEPLOY_DIR/start.sh"
fi

log "Done. Run: $DEPLOY_DIR/start.sh"
