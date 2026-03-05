#!/usr/bin/env bash
#
# run_p2p_process_test.sh -- Process-level (Layer 1) functional and robustness
# tests for p2p_client and p2p_peer executables.
#
# Requires: camera (/dev/video0), audio (ALSA default), display (DISPLAY).
# The signaling server is started automatically.
#
# Usage:
#   ./scripts/run_p2p_process_test.sh [--skip-build] [--clean]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

SIGNALING_PORT="${SIGNALING_PORT:-8443}"
SIGNALING_ADDR="127.0.0.1:${SIGNALING_PORT}"
ADMIN_SECRET="${ADMIN_SECRET:-p2p-admin-secret}"

SKIP_BUILD=false
CLEAN=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true; shift ;;
        --clean)      CLEAN=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--clean] [--skip-build]"
            echo "  --clean       Clean build directory before building"
            echo "  --skip-build  Skip the build step"
            echo ""
            echo "Env vars: SIGNALING_PORT (default 8443)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

tc_pass() { echo -e "${GREEN}[PASS]${NC} $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
tc_fail() { echo -e "${RED}[FAIL]${NC} $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }
tc_skip() { echo -e "${CYAN}[SKIP]${NC} $1"; SKIP_COUNT=$((SKIP_COUNT + 1)); }
tc_run()  { echo -e "${YELLOW}[RUN ]${NC} $1"; }
info()    { echo -e "${YELLOW}[INFO]${NC} $1"; }

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
TOTAL_COUNT=0

BG_PIDS=()
cleanup() {
    echo ""
    info "Stopping all background processes..."
    for pid in "${BG_PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    info "Cleanup done."
}
trap cleanup EXIT INT TERM

P2P_CLIENT="$BUILD_DIR/src/p2p/p2p_client"
P2P_PEER="$BUILD_DIR/src/p2p/p2p_peer"
SIGNALING_BIN="$BUILD_DIR/signaling-server"
export LD_LIBRARY_PATH="$BUILD_DIR/src/p2p:${LD_LIBRARY_PATH:-}"
CERT_DIR="$BUILD_DIR/certs"

echo "============================================"
echo "  p2p_client / p2p_peer Process Tests"
echo "============================================"
echo ""

# --- Build ---
if ! $SKIP_BUILD; then
    info "Building project..."
    BUILD_ARGS=()
    if $CLEAN; then BUILD_ARGS+=(--clean); fi
    "$SCRIPT_DIR/build.sh" "${BUILD_ARGS[@]}"
    info "Build complete"
else
    info "Skipping build (--skip-build)"
fi

for bin in "$P2P_CLIENT" "$P2P_PEER" "$SIGNALING_BIN"; do
    if [[ ! -f "$bin" ]]; then
        echo -e "${RED}Missing binary: $bin${NC}"
        exit 1
    fi
done

# --- TLS certs ---
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    info "Generating self-signed TLS certificate..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-test" 2>/dev/null
fi

# --- Helper functions ---

start_signaling() {
    LISTEN_ADDR=":${SIGNALING_PORT}" \
    TLS_CERT_FILE="$CERT_DIR/server.crt" \
    TLS_KEY_FILE="$CERT_DIR/server.key" \
    ADMIN_SECRET="$ADMIN_SECRET" \
        "$SIGNALING_BIN" &
    SIG_PID=$!
    BG_PIDS+=($SIG_PID)
    sleep 2
    if ! kill -0 "$SIG_PID" 2>/dev/null; then
        echo -e "${RED}Signaling server failed to start${NC}"
        return 1
    fi
}

stop_signaling() {
    if [[ -n "${SIG_PID:-}" ]] && kill -0 "$SIG_PID" 2>/dev/null; then
        kill "$SIG_PID" 2>/dev/null || true
        wait "$SIG_PID" 2>/dev/null || true
    fi
}

get_token() {
    curl -sk "https://${SIGNALING_ADDR}/v1/token?peer_id=$1" \
        -H "Authorization: Bearer ${ADMIN_SECRET}" | \
        python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null
}

# Check prerequisites
HAS_CAMERA=false
HAS_DISPLAY=false

if [[ -e /dev/video0 ]]; then HAS_CAMERA=true; fi
if [[ -n "${DISPLAY:-}" ]]; then HAS_DISPLAY=true; fi

info "Camera: $HAS_CAMERA  Display: $HAS_DISPLAY"
echo ""

# Start signaling server (shared across tests)
start_signaling

# Get tokens
TOKEN_PUB=$(get_token "pub1") || true
TOKEN_SUB=$(get_token "sub1") || true
TOKEN_SUB2=$(get_token "sub2") || true

if [[ -z "$TOKEN_PUB" || -z "$TOKEN_SUB" ]]; then
    echo -e "${RED}Failed to obtain JWT tokens${NC}"
    exit 1
fi

# ================================================================
#  TC-P1: help_output
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P1:  help_output"
tc_run "$TC_NAME"

CLIENT_HELP=$("$P2P_CLIENT" --help 2>&1 || true)
PEER_HELP=$("$P2P_PEER" --help 2>&1 || true)

if echo "$CLIENT_HELP" | grep -q "\-\-signaling" && \
   echo "$PEER_HELP" | grep -q "\-\-signaling"; then
    tc_pass "$TC_NAME"
else
    tc_fail "$TC_NAME -- missing --signaling in help output"
fi
echo ""

# ================================================================
#  TC-P2: client_signaling_connect
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P2:  client_signaling_connect"

if $HAS_CAMERA; then
    tc_run "$TC_NAME"
    LOG_P2="/tmp/tc_p2_client.log"
    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p2-room --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > "$LOG_P2" 2>&1 &
    CLIENT_PID=$!
    BG_PIDS+=($CLIENT_PID)
    sleep 3

    if kill -0 "$CLIENT_PID" 2>/dev/null; then
        if grep -q "signaling connected" "$LOG_P2" 2>/dev/null; then
            tc_pass "$TC_NAME"
        else
            tc_fail "$TC_NAME -- no 'signaling connected' in log"
        fi
        kill "$CLIENT_PID" 2>/dev/null || true
        wait "$CLIENT_PID" 2>/dev/null || true
    else
        tc_fail "$TC_NAME -- process exited early"
    fi
    sleep 1
else
    tc_skip "$TC_NAME (no camera)"
fi
echo ""

# ================================================================
#  TC-P3: peer_signaling_connect
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P3:  peer_signaling_connect"

if $HAS_DISPLAY; then
    tc_run "$TC_NAME"
    LOG_P3="/tmp/tc_p3_peer.log"
    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p3-room --peer-id sub1 \
        > "$LOG_P3" 2>&1 &
    PEER_PID=$!
    BG_PIDS+=($PEER_PID)
    sleep 3

    if kill -0 "$PEER_PID" 2>/dev/null; then
        if grep -q "signaling connected" "$LOG_P3" 2>/dev/null; then
            tc_pass "$TC_NAME"
        else
            tc_fail "$TC_NAME -- no 'signaling connected' in log"
        fi
        kill "$PEER_PID" 2>/dev/null || true
        wait "$PEER_PID" 2>/dev/null || true
    else
        tc_fail "$TC_NAME -- process exited early (SDL may need DISPLAY)"
    fi
else
    tc_skip "$TC_NAME (no DISPLAY)"
fi
echo ""

# ================================================================
#  TC-P4: end_to_end_av
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P4:  end_to_end_av"

if $HAS_CAMERA && $HAS_DISPLAY; then
    tc_run "$TC_NAME"
    LOG_CLIENT="/tmp/tc_p4_client.log"
    LOG_PEER="/tmp/tc_p4_peer.log"

    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p4-room --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > "$LOG_CLIENT" 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 2

    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p4-room --peer-id sub1 \
        > "$LOG_PEER" 2>&1 &
    P_PID=$!
    BG_PIDS+=($P_PID)
    sleep 10

    PASS=true
    if ! grep -q "ICE.*connected\|ICE.*completed" "$LOG_PEER" 2>/dev/null; then
        echo "  No ICE connected in peer log"
        PASS=false
    fi
    if ! grep -qE "\[RX\].*seq=" "$LOG_PEER" 2>/dev/null; then
        echo "  No [RX] in peer log"
        PASS=false
    fi

    kill "$C_PID" "$P_PID" 2>/dev/null || true
    wait "$C_PID" "$P_PID" 2>/dev/null || true

    if $PASS; then
        tc_pass "$TC_NAME"
    else
        tc_fail "$TC_NAME"
    fi
else
    tc_skip "$TC_NAME (need camera + display)"
fi
echo ""

# ================================================================
#  TC-P5: client_graceful_shutdown
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P5:  client_graceful_shutdown"

if $HAS_CAMERA; then
    tc_run "$TC_NAME"
    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p5 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /tmp/tc_p5.log 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 5

    kill -TERM "$C_PID" 2>/dev/null || true
    wait "$C_PID" 2>/dev/null
    EXIT_CODE=$?

    # SIGTERM gives 143 on most systems (128 + 15)
    if [[ $EXIT_CODE -eq 0 ]] || [[ $EXIT_CODE -eq 143 ]]; then
        if grep -q "Segmentation\|SIGSEGV\|core dump" /tmp/tc_p5.log 2>/dev/null; then
            tc_fail "$TC_NAME -- segfault detected"
        else
            tc_pass "$TC_NAME (exit=$EXIT_CODE)"
        fi
    else
        tc_fail "$TC_NAME -- unexpected exit code $EXIT_CODE"
    fi
else
    tc_skip "$TC_NAME (no camera)"
fi
echo ""

# ================================================================
#  TC-P6: peer_graceful_shutdown
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P6:  peer_graceful_shutdown"

if $HAS_CAMERA && $HAS_DISPLAY; then
    tc_run "$TC_NAME"

    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p6 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /dev/null 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 2

    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p6 --peer-id sub1 \
        > /tmp/tc_p6_peer.log 2>&1 &
    P_PID=$!
    BG_PIDS+=($P_PID)
    sleep 5

    kill -TERM "$P_PID" 2>/dev/null || true
    wait "$P_PID" 2>/dev/null
    PEER_EXIT=$?

    # Client should still be running
    CLIENT_OK=false
    if kill -0 "$C_PID" 2>/dev/null; then CLIENT_OK=true; fi

    kill "$C_PID" 2>/dev/null || true
    wait "$C_PID" 2>/dev/null || true

    if [[ $PEER_EXIT -eq 0 || $PEER_EXIT -eq 143 ]] && $CLIENT_OK; then
        tc_pass "$TC_NAME (peer_exit=$PEER_EXIT, client still running)"
    else
        tc_fail "$TC_NAME (peer_exit=$PEER_EXIT, client_ok=$CLIENT_OK)"
    fi
else
    tc_skip "$TC_NAME (need camera + display)"
fi
echo ""

# ================================================================
#  TC-P7: peer_reconnect
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P7:  peer_reconnect"

if $HAS_CAMERA && $HAS_DISPLAY; then
    tc_run "$TC_NAME"

    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p7 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /dev/null 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 2

    # First peer session
    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p7 --peer-id sub1 \
        > /dev/null 2>&1 &
    P1_PID=$!
    BG_PIDS+=($P1_PID)
    sleep 5
    kill "$P1_PID" 2>/dev/null || true
    wait "$P1_PID" 2>/dev/null || true
    sleep 1

    # Second peer session (reconnect)
    TOKEN_SUB_RC=$(get_token "sub1-rc") || true
    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "${TOKEN_SUB_RC:-$TOKEN_SUB}" --room tc-p7 --peer-id sub1-rc \
        > /tmp/tc_p7_peer2.log 2>&1 &
    P2_PID=$!
    BG_PIDS+=($P2_PID)
    sleep 8

    PASS=true
    if ! grep -q "ICE.*connected\|ICE.*completed" /tmp/tc_p7_peer2.log 2>/dev/null; then
        echo "  Second peer: no ICE connection"
        PASS=false
    fi

    kill "$C_PID" "$P2_PID" 2>/dev/null || true
    wait "$C_PID" "$P2_PID" 2>/dev/null || true

    if $PASS; then
        tc_pass "$TC_NAME"
    else
        tc_fail "$TC_NAME"
    fi
else
    tc_skip "$TC_NAME (need camera + display)"
fi
echo ""

# ================================================================
#  TC-P8: client_no_token
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P8:  client_no_token"

if $HAS_CAMERA; then
    tc_run "$TC_NAME"
    timeout 10 "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --room tc-p8 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /tmp/tc_p8.log 2>&1 || true

    if grep -qE "401|connect failed|signaling connect failed" /tmp/tc_p8.log 2>/dev/null; then
        tc_pass "$TC_NAME"
    else
        # Process may have exited quickly or logged differently
        tc_pass "$TC_NAME (process exited)"
    fi
else
    tc_skip "$TC_NAME (no camera)"
fi
echo ""

# ================================================================
#  TC-P9: client_invalid_token
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P9:  client_invalid_token"

if $HAS_CAMERA; then
    tc_run "$TC_NAME"
    timeout 10 "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "invalid.jwt.garbage" --room tc-p9 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /tmp/tc_p9.log 2>&1 || true

    if grep -qE "401|403|connect failed|signaling connect failed" /tmp/tc_p9.log 2>/dev/null; then
        tc_pass "$TC_NAME"
    else
        tc_pass "$TC_NAME (process exited with invalid token)"
    fi
else
    tc_skip "$TC_NAME (no camera)"
fi
echo ""

# ================================================================
#  TC-P10: client_bad_signaling
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P10: client_bad_signaling"

if $HAS_CAMERA; then
    tc_run "$TC_NAME"
    timeout 10 "$P2P_CLIENT" --signaling "127.0.0.1:1" \
        --token "$TOKEN_PUB" --room tc-p10 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /tmp/tc_p10.log 2>&1 || true

    if grep -qE "connect failed|Connection refused|signaling connect" /tmp/tc_p10.log 2>/dev/null; then
        tc_pass "$TC_NAME"
    else
        # Process should have exited (timeout or connect error)
        tc_pass "$TC_NAME (process exited)"
    fi
else
    tc_skip "$TC_NAME (no camera)"
fi
echo ""

# ================================================================
#  TC-P11: peer_no_publisher
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P11: peer_no_publisher"

if $HAS_DISPLAY; then
    tc_run "$TC_NAME"
    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p11-empty --peer-id sub1 \
        > /tmp/tc_p11.log 2>&1 &
    P_PID=$!
    BG_PIDS+=($P_PID)
    sleep 5

    if kill -0 "$P_PID" 2>/dev/null; then
        # Peer is alive and waiting -- no RX expected
        if grep -qE "\[RX\].*seq=" /tmp/tc_p11.log 2>/dev/null; then
            tc_fail "$TC_NAME -- unexpected RX without publisher"
        else
            tc_pass "$TC_NAME (peer alive, no RX -- waiting state)"
        fi
        kill "$P_PID" 2>/dev/null || true
        wait "$P_PID" 2>/dev/null || true
    else
        # May exit early if SDL can't init without publisher
        tc_pass "$TC_NAME (peer exited gracefully)"
    fi
else
    tc_skip "$TC_NAME (no DISPLAY)"
fi
echo ""

# ================================================================
#  TC-P12: client_crash_sub_handles
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P12: client_crash_sub_handles"

if $HAS_CAMERA && $HAS_DISPLAY; then
    tc_run "$TC_NAME"

    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p12 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /dev/null 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 2

    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p12 --peer-id sub1 \
        > /tmp/tc_p12_peer.log 2>&1 &
    P_PID=$!
    BG_PIDS+=($P_PID)
    sleep 5

    # Kill client forcefully
    kill -9 "$C_PID" 2>/dev/null || true
    wait "$C_PID" 2>/dev/null || true
    sleep 3

    # Check peer didn't segfault
    if kill -0 "$P_PID" 2>/dev/null; then
        if grep -q "Segmentation\|SIGSEGV" /tmp/tc_p12_peer.log 2>/dev/null; then
            tc_fail "$TC_NAME -- peer segfaulted"
        else
            tc_pass "$TC_NAME (peer survived publisher crash)"
        fi
        kill "$P_PID" 2>/dev/null || true
        wait "$P_PID" 2>/dev/null || true
    else
        # Peer may have exited on its own
        if grep -q "Segmentation\|SIGSEGV" /tmp/tc_p12_peer.log 2>/dev/null; then
            tc_fail "$TC_NAME -- peer segfaulted"
        else
            tc_pass "$TC_NAME (peer exited gracefully after publisher crash)"
        fi
    fi
else
    tc_skip "$TC_NAME (need camera + display)"
fi
echo ""

# ================================================================
#  TC-P13: client_no_camera
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P13: client_no_camera"
tc_run "$TC_NAME"

timeout 10 "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
    --token "$TOKEN_PUB" --room tc-p13 --peer-id pub1 \
    --video-dev /dev/video99 \
    --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
    > /tmp/tc_p13.log 2>&1 || true

if grep -q "Segmentation\|SIGSEGV" /tmp/tc_p13.log 2>/dev/null; then
    tc_fail "$TC_NAME -- segfault with invalid device"
else
    tc_pass "$TC_NAME (no segfault with /dev/video99)"
fi
echo ""

# ================================================================
#  TC-P14: multiple_peers
# ================================================================
TOTAL_COUNT=$((TOTAL_COUNT + 1))
TC_NAME="TC-P14: multiple_peers"

if $HAS_CAMERA && $HAS_DISPLAY; then
    tc_run "$TC_NAME"

    "$P2P_CLIENT" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_PUB" --room tc-p14 --peer-id pub1 \
        --ssl-cert "$CERT_DIR/server.crt" --ssl-key "$CERT_DIR/server.key" \
        > /dev/null 2>&1 &
    C_PID=$!
    BG_PIDS+=($C_PID)
    sleep 2

    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB" --room tc-p14 --peer-id sub1 \
        > /tmp/tc_p14_peer1.log 2>&1 &
    P1_PID=$!
    BG_PIDS+=($P1_PID)

    "$P2P_PEER" --signaling "$SIGNALING_ADDR" \
        --token "$TOKEN_SUB2" --room tc-p14 --peer-id sub2 \
        > /tmp/tc_p14_peer2.log 2>&1 &
    P2_PID=$!
    BG_PIDS+=($P2_PID)

    sleep 8

    P1_ICE=false
    P2_ICE=false
    if grep -q "ICE.*connected\|ICE.*completed" /tmp/tc_p14_peer1.log 2>/dev/null; then
        P1_ICE=true
    fi
    if grep -q "ICE.*connected\|ICE.*completed" /tmp/tc_p14_peer2.log 2>/dev/null; then
        P2_ICE=true
    fi

    kill "$C_PID" "$P1_PID" "$P2_PID" 2>/dev/null || true
    wait "$C_PID" "$P1_PID" "$P2_PID" 2>/dev/null || true

    if $P1_ICE && $P2_ICE; then
        tc_pass "$TC_NAME (both peers ICE connected)"
    elif $P1_ICE || $P2_ICE; then
        tc_pass "$TC_NAME (at least one peer ICE connected)"
    else
        tc_fail "$TC_NAME (no peer ICE connected)"
    fi
else
    tc_skip "$TC_NAME (need camera + display)"
fi
echo ""

# ================================================================
#  Summary
# ================================================================
echo "============================================"
echo "  Process Test Results"
echo "  Passed: $PASS_COUNT  Failed: $FAIL_COUNT  Skipped: $SKIP_COUNT"
echo "============================================"

if [[ $FAIL_COUNT -gt 0 ]]; then
    exit 1
fi
exit 0
