#!/usr/bin/env bash
set -euo pipefail

WIN_HOST="yqyang@192.168.31.36"
DURATION_MIN=120
CHECK_INTERVAL_MIN=5
LOG_FILE="/home/yq/p2pchannel_server/libp2pchannel/logs/win_endurance_$(date +%s).log"
mkdir -p "$(dirname "$LOG_FILE")"

PUB_TERM_FILE="${1:-}"
SUB_TERM_FILE="${2:-}"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg" | tee -a "$LOG_FILE"
}

get_rx_stats() {
    if [ -n "$SUB_TERM_FILE" ] && [ -f "$SUB_TERM_FILE" ]; then
        local last_stat
        last_stat=$(grep '\[RX-STAT\]' "$SUB_TERM_FILE" 2>/dev/null | tail -1 || echo "")
        if [ -n "$last_stat" ]; then
            echo "$last_stat"
        else
            echo "(no RX-STAT yet)"
        fi
    else
        echo "(no subscriber terminal file)"
    fi
}

get_tx_stats() {
    if [ -n "$PUB_TERM_FILE" ] && [ -f "$PUB_TERM_FILE" ]; then
        local last_stat
        last_stat=$(grep '\[TX-STAT\]' "$PUB_TERM_FILE" 2>/dev/null | tail -1 || echo "")
        if [ -n "$last_stat" ]; then
            echo "$last_stat"
        else
            echo "(no TX-STAT yet)"
        fi
    else
        echo "(no publisher terminal file)"
    fi
}

check_errors() {
    local errors=""
    if [ -n "$SUB_TERM_FILE" ] && [ -f "$SUB_TERM_FILE" ]; then
        local sub_errors
        sub_errors=$(grep -iE 'FATAL|ERROR|failed|disconnected|timeout' "$SUB_TERM_FILE" 2>/dev/null | grep -v 'exit_code' | tail -5 || true)
        if [ -n "$sub_errors" ]; then
            errors="SUB_ERRORS: $sub_errors"
        fi
    fi
    if [ -n "$PUB_TERM_FILE" ] && [ -f "$PUB_TERM_FILE" ]; then
        local pub_errors
        pub_errors=$(grep -iE 'FATAL|ERROR|failed|disconnected|timeout' "$PUB_TERM_FILE" 2>/dev/null | grep -v 'exit_code' | tail -5 || true)
        if [ -n "$pub_errors" ]; then
            errors="${errors:+$errors | }PUB_ERRORS: $pub_errors"
        fi
    fi
    echo "$errors"
}

check_processes() {
    ssh -o ConnectTimeout=5 "$WIN_HOST" "tasklist | findstr p2p" 2>/dev/null || echo "NO_P2P_PROCESSES"
}

log "=== Windows Endurance Test Monitor ==="
log "Duration: ${DURATION_MIN} minutes, Check interval: ${CHECK_INTERVAL_MIN} minutes"
log "Publisher terminal: ${PUB_TERM_FILE:-none}"
log "Subscriber terminal: ${SUB_TERM_FILE:-none}"
log "Monitor log: $LOG_FILE"

TOTAL_CHECKS=$((DURATION_MIN / CHECK_INTERVAL_MIN))
FAILURES=0

for ((i = 1; i <= TOTAL_CHECKS; i++)); do
    ELAPSED=$((i * CHECK_INTERVAL_MIN))
    sleep $((CHECK_INTERVAL_MIN * 60))

    log "--- Checkpoint $i/$TOTAL_CHECKS (T+${ELAPSED}min) ---"

    PROCS=$(check_processes)
    log "Processes: $PROCS"

    if echo "$PROCS" | grep -q "NO_P2P_PROCESSES"; then
        log "CRITICAL: No p2p processes running!"
        FAILURES=$((FAILURES + 1))
    fi

    RX=$(get_rx_stats)
    TX=$(get_tx_stats)
    log "RX: $RX"
    log "TX: $TX"

    ERRS=$(check_errors)
    if [ -n "$ERRS" ]; then
        log "WARNINGS: $ERRS"
    else
        log "No errors detected"
    fi

    if [ -n "$SUB_TERM_FILE" ] && [ -f "$SUB_TERM_FILE" ]; then
        if grep -q 'exit_code:' "$SUB_TERM_FILE" 2>/dev/null; then
            EXIT_CODE=$(grep 'exit_code:' "$SUB_TERM_FILE" | tail -1)
            log "CRITICAL: Subscriber process exited! $EXIT_CODE"
            FAILURES=$((FAILURES + 1))
        fi
    fi
    if [ -n "$PUB_TERM_FILE" ] && [ -f "$PUB_TERM_FILE" ]; then
        if grep -q 'exit_code:' "$PUB_TERM_FILE" 2>/dev/null; then
            EXIT_CODE=$(grep 'exit_code:' "$PUB_TERM_FILE" | tail -1)
            log "CRITICAL: Publisher process exited! $EXIT_CODE"
            FAILURES=$((FAILURES + 1))
        fi
    fi

    log ""
done

log "=== ENDURANCE TEST COMPLETE ==="
log "Total checkpoints: $TOTAL_CHECKS"
log "Failures: $FAILURES"

FINAL_RX=$(get_rx_stats)
log "Final RX stats: $FINAL_RX"

if echo "$FINAL_RX" | grep -qP 'lost=0'; then
    log "RESULT: PASS (0 frames lost)"
elif [ "$FAILURES" -eq 0 ]; then
    log "RESULT: PASS (all checkpoints healthy)"
else
    log "RESULT: FAIL ($FAILURES failures detected)"
fi

log "Full log at: $LOG_FILE"
