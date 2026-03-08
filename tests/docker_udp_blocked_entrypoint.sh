#!/usr/bin/env bash
#
# docker_udp_blocked_entrypoint.sh -- Entrypoint for the UDP-blocked Docker test.
#
# Runs publisher and subscriber as SEPARATE processes to avoid same-process
# ICE-TCP timing issues.
#
# Environment variables (set by the host run script):
#   SIGNALING_URL   - signaling server host:port  (required)
#   STUN_SERVER     - STUN/TURN host:port          (required)
#   ADMIN_SECRET    - admin secret for JWT tokens   (required)
#   DURATION        - test duration in seconds      (default: 600)
#   ENABLE_TCP      - "1" to enable ICE-TCP         (default: 1)
#   SSL_CERT        - path to TLS certificate       (default: /opt/p2pav/certs/server.crt)
#   SSL_KEY         - path to TLS private key       (default: /opt/p2pav/certs/server.key)
#

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${CYAN}[DOCKER]${NC} $1"; }
pass()  { echo -e "${GREEN}[  OK  ]${NC} $1"; }
fail()  { echo -e "${RED}[ FAIL ]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ WARN ]${NC} $1"; }

SIGNALING_URL="${SIGNALING_URL:?SIGNALING_URL is required}"
STUN_SERVER="${STUN_SERVER:?STUN_SERVER is required}"
ADMIN_SECRET="${ADMIN_SECRET:?ADMIN_SECRET is required}"
DURATION="${DURATION:-600}"
ENABLE_TCP="${ENABLE_TCP:-1}"
SSL_CERT="${SSL_CERT:-/opt/p2pav/certs/server.crt}"
SSL_KEY="${SSL_KEY:-/opt/p2pav/certs/server.key}"

TEST_BIN="/opt/p2pav/bin/test_p2pav_udp_blocked"
RESULT_FILE="/tmp/sub_result.txt"
ROOM="udp-blocked-$(date +%s)"

echo ""
echo "================================================"
echo "  P2P AV UDP-Blocked Test (Docker)"
echo "================================================"
info "Signaling:  $SIGNALING_URL"
info "STUN/TURN:  $STUN_SERVER"
info "Duration:   ${DURATION}s"
info "ICE-TCP:    $([ "$ENABLE_TCP" = "1" ] && echo ENABLED || echo disabled)"
info "Room:       $ROOM"
echo ""

# ---- Step 1: Verify test binary ----
if [[ ! -x "$TEST_BIN" ]]; then
    fail "Test binary not found: $TEST_BIN"
    exit 1
fi
pass "Test binary found"

# ---- Step 2: Block all UDP via iptables ----
info "Blocking ALL UDP traffic via iptables..."

iptables -A OUTPUT -p udp --dport 53 -j ACCEPT 2>/dev/null || warn "iptables OUTPUT DNS rule failed"
iptables -A OUTPUT -p udp -j DROP    2>/dev/null || warn "iptables OUTPUT DROP rule failed"
iptables -A INPUT  -p udp -j DROP    2>/dev/null || warn "iptables INPUT DROP rule failed"

pass "UDP blocked (DNS port 53 preserved)"

info "Verifying UDP is blocked..."
if timeout 3 bash -c "echo test | nc -u -w1 8.8.8.8 53" 2>/dev/null; then
    warn "UDP may not be fully blocked (nc test succeeded)"
else
    pass "UDP block verified"
fi

# ---- Step 3: Check signaling server health ----
info "Checking signaling server health..."
HEALTH=$(curl -sk --connect-timeout 10 "https://${SIGNALING_URL}/health" 2>/dev/null || echo "{}")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "Signaling server healthy"
else
    fail "Signaling server unreachable at ${SIGNALING_URL}: $HEALTH"
    exit 1
fi

# ---- Step 4: Obtain JWT tokens ----
info "Requesting JWT tokens..."

TOKEN_PUB=$(curl -sk --connect-timeout 10 \
    "https://${SIGNALING_URL}/v1/token?peer_id=pub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

TOKEN_SUB=$(curl -sk --connect-timeout 10 \
    "https://${SIGNALING_URL}/v1/token?peer_id=sub1" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" | \
    python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null) || true

if [[ -z "${TOKEN_PUB:-}" || -z "${TOKEN_SUB:-}" ]]; then
    fail "Failed to obtain JWT tokens"
    exit 1
fi
pass "Tokens obtained for pub1 and sub1"

# ---- Step 5: Build common args ----
TCP_FLAG=""
if [[ "$ENABLE_TCP" == "1" ]]; then
    TCP_FLAG="--enable-tcp"
fi

COMMON_ARGS=(
    --signaling "$SIGNALING_URL"
    --room "$ROOM"
    --duration "$DURATION"
)
# Skip --stun in TCP-only mode: STUN/TURN over UDP will just timeout
if [[ "$ENABLE_TCP" != "1" ]]; then
    COMMON_ARGS+=(--stun "$STUN_SERVER")
fi

# ---- Step 6: Start publisher in background ----
info "Starting publisher process..."
"$TEST_BIN" --role publisher \
    "${COMMON_ARGS[@]}" \
    --cert "$SSL_CERT" --key "$SSL_KEY" \
    --token "$TOKEN_PUB" \
    $TCP_FLAG >/tmp/pub.log 2>&1 &
PUB_PID=$!
info "Publisher PID: $PUB_PID"

sleep 3

# ---- Step 7: Start subscriber in foreground ----
info "Starting subscriber process..."
"$TEST_BIN" --role subscriber \
    "${COMMON_ARGS[@]}" \
    --token "$TOKEN_SUB" \
    --result-file "$RESULT_FILE" \
    $TCP_FLAG >/tmp/sub.log 2>&1 &
SUB_PID=$!
info "Subscriber PID: $SUB_PID"

# ---- Step 8: Wait for both processes ----
SUB_EXIT=0
PUB_EXIT=0

wait $SUB_PID || SUB_EXIT=$?
info "Subscriber exited with code: $SUB_EXIT"

if kill -0 $PUB_PID 2>/dev/null; then
    kill $PUB_PID 2>/dev/null || true
    wait $PUB_PID 2>/dev/null || PUB_EXIT=$?
fi
info "Publisher exited with code: $PUB_EXIT"

echo ""
echo "======== PUBLISHER LOG ========"
cat /tmp/pub.log
echo ""
echo "======== SUBSCRIBER LOG ========"
cat /tmp/sub.log

# ---- Step 9: Report results ----
echo ""
echo "================================================"
echo "  Test Results"
echo "================================================"

if [[ -f "$RESULT_FILE" ]]; then
    echo "Subscriber results:"
    cat "$RESULT_FILE"
    echo ""

    VIDEO_RECV=$(grep -oP 'video_recv=\K\d+' "$RESULT_FILE" 2>/dev/null || echo "0")
    AUDIO_RECV=$(grep -oP 'audio_recv=\K\d+' "$RESULT_FILE" 2>/dev/null || echo "0")
    ELAPSED=$(grep -oP 'elapsed=\K\d+' "$RESULT_FILE" 2>/dev/null || echo "0")

    REQUIRED_DURATION=$((DURATION - 5))

    echo "  Video received: $VIDEO_RECV"
    echo "  Audio received: $AUDIO_RECV"
    echo "  Elapsed:        ${ELAPSED}s (required: >= ${REQUIRED_DURATION}s)"
    echo ""

    if [[ "$VIDEO_RECV" -gt 0 && "$AUDIO_RECV" -gt 0 && "$ELAPSED" -ge "$REQUIRED_DURATION" ]]; then
        pass "ALL CHECKS PASSED"
        exit 0
    else
        fail "TEST FAILED"
        exit 1
    fi
else
    fail "No result file found - subscriber may have crashed"
    exit 1
fi
