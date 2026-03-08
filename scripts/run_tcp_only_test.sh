#!/usr/bin/env bash
#
# run_tcp_only_test.sh -- Block all local UDP, then run p2p_client + p2p_peer
#                         with --enable-tcp so ICE naturally falls back to TCP.
#
# Requires: sudo (for iptables manipulation)
#
# Usage:
#   sudo ./scripts/run_tcp_only_test.sh [--duration SEC]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

DURATION="${1:-30}"
if [[ "${1:-}" == "--duration" ]]; then DURATION="${2:-30}"; fi

SIGNALING_PORT=8443
SIGNALING_ADDR="127.0.0.1:${SIGNALING_PORT}"
ROOM_ID="tcp-test-room"
ADMIN_SECRET="p2p-admin-secret"
JWT_SECRET="p2p-jwt-secret"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[TCP-TEST]${NC} $1"; }
pass()  { echo -e "${GREEN}[  PASS  ]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ WARN  ]${NC} $1"; }
fail()  { echo -e "${RED}[  FAIL  ]${NC} $1"; }

if [[ $EUID -ne 0 ]]; then
    echo "This script needs root for iptables. Re-running with sudo..."
    exec sudo -E "$0" "$@"
fi

# ---- Verify binaries ----
CLIENT_BIN="$BUILD_DIR/src/p2p/p2p_client"
PEER_BIN="$BUILD_DIR/src/p2p/p2p_peer"
SIG_BIN="$BUILD_DIR/signaling-server"

for bin in "$CLIENT_BIN" "$PEER_BIN" "$SIG_BIN"; do
    if [[ ! -f "$bin" ]]; then
        fail "Binary not found: $bin (run build.sh first)"
        exit 1
    fi
done

# ---- TLS certs ----
CERT_DIR="$BUILD_DIR/certs"
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating TLS certificates..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-tcp-test" 2>/dev/null
fi

# ---- Log / PID tracking ----
LOG_DIR="$BUILD_DIR/tcp_test_logs"
mkdir -p "$LOG_DIR"
rm -f "$LOG_DIR"/*.log

PIDS=()
UDP_BLOCKED=false

cleanup() {
    echo ""
    info "Cleaning up..."

    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true

    if $UDP_BLOCKED; then
        info "Restoring iptables (removing UDP block)..."
        iptables -D OUTPUT -p udp -j DROP 2>/dev/null || true
        iptables -D INPUT  -p udp -j DROP 2>/dev/null || true
        iptables -D OUTPUT -p udp --dport 53 -j ACCEPT 2>/dev/null || true
        info "iptables restored."
    fi

    info "Logs in: $LOG_DIR/"
}
trap cleanup EXIT INT TERM

echo ""
echo "================================================"
echo "  P2P AV over ICE-TCP Test (UDP blocked)"
echo "  Duration: ${DURATION}s"
echo "================================================"
echo ""

# ---- Step 1: Start signaling server ----
info "Starting signaling server on :${SIGNALING_PORT}..."
LISTEN_ADDR=":${SIGNALING_PORT}" \
TLS_CERT_FILE="$CERT_DIR/server.crt" \
TLS_KEY_FILE="$CERT_DIR/server.key" \
JWT_SECRET="$JWT_SECRET" \
ADMIN_SECRET="$ADMIN_SECRET" \
    "$SIG_BIN" > "$LOG_DIR/signaling.log" 2>&1 &
PIDS+=($!)
sleep 2

HEALTH=$(curl -sk "https://${SIGNALING_ADDR}/health" 2>/dev/null || echo "{}")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling server healthy"
else
    fail "Signaling server failed to start: $HEALTH"
    exit 1
fi

# ---- Step 2: Get JWT tokens ----
info "Generating JWT tokens..."
TOKEN_PUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=pub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_SUB=$(curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=sub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

if [[ -z "${TOKEN_PUB:-}" || -z "${TOKEN_SUB:-}" ]]; then
    fail "Failed to obtain JWT tokens"
    exit 1
fi
pass "Tokens obtained"

# ---- Step 3: Block all UDP ----
info "Blocking ALL UDP traffic via iptables..."
iptables -A OUTPUT -p udp --dport 53 -j ACCEPT
iptables -A OUTPUT -p udp -j DROP
iptables -A INPUT  -p udp -j DROP
UDP_BLOCKED=true
pass "UDP blocked (DNS port 53 preserved)"

# ---- Step 4: Start p2p_client with --enable-tcp ----
export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

info "Starting p2p_client (publisher) with --enable-tcp..."
"$CLIENT_BIN" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" \
    --peer-id pub1 \
    --token "$TOKEN_PUB" \
    --enable-tcp \
    --ssl-cert "$CERT_DIR/server.crt" \
    --ssl-key "$CERT_DIR/server.key" \
    > "$LOG_DIR/p2p_client.log" 2>&1 &
CLIENT_PID=$!
PIDS+=($CLIENT_PID)
info "  PID=$CLIENT_PID"

sleep 3

# ---- Step 5: Start p2p_peer with --enable-tcp ----
info "Starting p2p_peer (subscriber) with --enable-tcp..."
"$PEER_BIN" \
    --signaling "$SIGNALING_ADDR" \
    --room "$ROOM_ID" \
    --peer-id sub1 \
    --token "$TOKEN_SUB" \
    --enable-tcp \
    > "$LOG_DIR/p2p_peer.log" 2>&1 &
PEER_PID=$!
PIDS+=($PEER_PID)
info "  PID=$PEER_PID"

echo ""
info "Both processes started. Monitoring for ${DURATION}s..."
echo ""

# ---- Step 6: Monitor ----
ELAPSED=0
CHECK_INTERVAL=5
CLIENT_SIG_OK=false
PEER_SIG_OK=false
ICE_CONNECTED=false
ICE_TCP_USED=false
TX_SENDING=false
RX_RECEIVING=false

while [[ $ELAPSED -lt $DURATION ]]; do
    sleep $CHECK_INTERVAL
    ELAPSED=$((ELAPSED + CHECK_INTERVAL))

    if ! kill -0 $CLIENT_PID 2>/dev/null; then
        fail "[${ELAPSED}s] p2p_client exited prematurely"
        tail -10 "$LOG_DIR/p2p_client.log" 2>/dev/null | sed 's/^/    /'
        exit 1
    fi

    if ! kill -0 $PEER_PID 2>/dev/null; then
        fail "[${ELAPSED}s] p2p_peer exited prematurely"
        tail -10 "$LOG_DIR/p2p_peer.log" 2>/dev/null | sed 's/^/    /'
        exit 1
    fi

    if ! $CLIENT_SIG_OK && grep -q "signaling connected\|SSE connected\|room created" "$LOG_DIR/p2p_client.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Publisher connected to signaling"
        CLIENT_SIG_OK=true
    fi

    if ! $PEER_SIG_OK && grep -q "signaling connected\|SSE connected\|joined room" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Subscriber connected to signaling"
        PEER_SIG_OK=true
    fi

    if ! $ICE_CONNECTED && grep -q "ICE connected\|ICE_CONNECTED\|state=connected" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] ICE connection established"
        ICE_CONNECTED=true
    fi

    if ! $ICE_TCP_USED && grep -q "ICE-TCP enabled\|tcptype\|TCP" "$LOG_DIR/p2p_client.log" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] ICE-TCP candidates detected"
        ICE_TCP_USED=true
    fi

    if ! $TX_SENDING && grep -q "\[TX\]" "$LOG_DIR/p2p_client.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Publisher sending data"
        TX_SENDING=true
    fi

    if ! $RX_RECEIVING && grep -q "\[RX\]" "$LOG_DIR/p2p_peer.log" 2>/dev/null; then
        pass "[${ELAPSED}s] Subscriber receiving data (over TCP!)"
        RX_RECEIVING=true
    fi

    if $RX_RECEIVING; then
        STAT_LINE=$(grep "\[RX-STAT\]" "$LOG_DIR/p2p_peer.log" 2>/dev/null | tail -1 || true)
        if [[ -n "$STAT_LINE" ]]; then
            info "[${ELAPSED}s] $STAT_LINE"
        fi
    fi
done

# ---- Step 7: Results ----
echo ""
echo "================================================"
echo "  Test Results (${DURATION}s, UDP blocked)"
echo "================================================"

PASS_COUNT=0
FAIL_COUNT=0

check_result() {
    local desc="$1" val="$2"
    if $val; then
        pass "$desc"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        fail "$desc"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

check_result "Publisher signaling connected"    $CLIENT_SIG_OK
check_result "Subscriber signaling connected"   $PEER_SIG_OK
check_result "ICE connection established"       $ICE_CONNECTED
check_result "ICE-TCP candidates used"          $ICE_TCP_USED
check_result "Publisher sending [TX] data"      $TX_SENDING
check_result "Subscriber receiving [RX] data"   $RX_RECEIVING

CLIENT_ALIVE=true; kill -0 $CLIENT_PID 2>/dev/null || CLIENT_ALIVE=false
PEER_ALIVE=true;   kill -0 $PEER_PID 2>/dev/null  || PEER_ALIVE=false
check_result "Publisher still running"          $CLIENT_ALIVE
check_result "Subscriber still running"         $PEER_ALIVE

ZERO_LOSS=false
if $RX_RECEIVING; then
    LAST_STAT=$(grep "\[RX-STAT\]" "$LOG_DIR/p2p_peer.log" 2>/dev/null | tail -1 || true)
    if echo "$LAST_STAT" | grep -q "lost=0"; then
        ZERO_LOSS=true
    fi
fi
check_result "Zero packet loss"                 $ZERO_LOSS

echo ""
if [[ $FAIL_COUNT -eq 0 ]]; then
    echo -e "${GREEN}All $PASS_COUNT checks passed! XQUIC over ICE-TCP works.${NC}"
else
    echo -e "${RED}$FAIL_COUNT/$((PASS_COUNT + FAIL_COUNT)) checks failed.${NC}"
    echo ""
    echo "=== p2p_client log (last 30 lines) ==="
    tail -30 "$LOG_DIR/p2p_client.log" 2>/dev/null | sed 's/^/  /'
    echo ""
    echo "=== p2p_peer log (last 30 lines) ==="
    tail -30 "$LOG_DIR/p2p_peer.log" 2>/dev/null | sed 's/^/  /'
fi
echo ""
