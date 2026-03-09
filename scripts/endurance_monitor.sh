#!/bin/bash
set -euo pipefail

ANDROID_DEVICE="192.168.31.73:5555"
PI_HOST="yq@192.168.31.198"
LOG_FILE="/tmp/endurance_test_$(date +%Y%m%d_%H%M%S).log"
INTERVAL=300  # 5 minutes
TOTAL_CHECKS=24  # 24 x 5min = 2 hours
START_TIME=$(date +%s)

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg" | tee -a "$LOG_FILE"
}

get_android_rx_count() {
    adb -s "$ANDROID_DEVICE" logcat -d -s "p2p_peer:*" 2>/dev/null | \
        grep -oP '\[RX\] seq=\K[0-9]+' | tail -1
}

get_android_rx_stat() {
    adb -s "$ANDROID_DEVICE" logcat -d -s "p2p_peer:*" 2>/dev/null | \
        grep '\[RX-STAT\]' | tail -1
}

get_android_errors() {
    adb -s "$ANDROID_DEVICE" logcat -d -s "p2p_peer:*" 2>/dev/null | \
        grep -iE 'error|fatal|crash|disconnect|timeout' | tail -5
}

get_pi_tx_count() {
    ssh "$PI_HOST" "grep -oP '\\[TX\\] seq=\\K[0-9]+' /tmp/p2p_client_endurance.log | tail -1" 2>/dev/null
}

get_pi_tx_stat() {
    ssh "$PI_HOST" "grep '\\[TX-STAT\\]' /tmp/p2p_client_endurance.log | tail -1" 2>/dev/null
}

check_pi_alive() {
    ssh "$PI_HOST" "ps aux | grep p2p_client | grep -v grep | wc -l" 2>/dev/null
}

check_android_alive() {
    adb -s "$ANDROID_DEVICE" shell pidof org.p2p.peer 2>/dev/null
}

log "=========================================="
log "ENDURANCE TEST STARTED"
log "Log file: $LOG_FILE"
log "Interval: ${INTERVAL}s, Total checks: $TOTAL_CHECKS"
log "=========================================="

PREV_RX=0
PREV_TX=0

for i in $(seq 1 $TOTAL_CHECKS); do
    ELAPSED=$(( ($(date +%s) - START_TIME) / 60 ))
    log ""
    log "--- Checkpoint $i/$TOTAL_CHECKS (T+${ELAPSED}min) ---"

    # Check Pi publisher
    PI_ALIVE=$(check_pi_alive || echo "0")
    PI_TX=$(get_pi_tx_count || echo "?")
    PI_STAT=$(get_pi_tx_stat || echo "N/A")

    if [ "$PI_ALIVE" = "0" ]; then
        log "FAIL: Pi publisher NOT RUNNING!"
    else
        log "Pi: alive, TX seq=$PI_TX"
        log "Pi: $PI_STAT"
        if [ "$PI_TX" != "?" ] && [ "$PREV_TX" != "0" ]; then
            TX_DELTA=$((PI_TX - PREV_TX))
            log "Pi: TX delta since last check: $TX_DELTA frames"
            if [ "$TX_DELTA" -lt 100 ]; then
                log "WARN: TX delta very low ($TX_DELTA < 100)"
            fi
        fi
        PREV_TX=${PI_TX:-0}
    fi

    # Check Android subscriber
    ANDROID_PID=$(check_android_alive || echo "")
    ANDROID_RX=$(get_android_rx_count || echo "?")
    ANDROID_STAT=$(get_android_rx_stat || echo "N/A")
    ANDROID_ERRORS=$(get_android_errors || echo "")

    if [ -z "$ANDROID_PID" ]; then
        log "FAIL: Android subscriber NOT RUNNING!"
    else
        log "Android: alive (PID $ANDROID_PID), RX seq=$ANDROID_RX"
        log "Android: $ANDROID_STAT"
        if [ "$ANDROID_RX" != "?" ] && [ "$PREV_RX" != "0" ]; then
            RX_DELTA=$((ANDROID_RX - PREV_RX))
            log "Android: RX delta since last check: $RX_DELTA frames"
            if [ "$RX_DELTA" -lt 3000 ]; then
                log "WARN: RX delta low ($RX_DELTA < 3000, expected ~6000-9000)"
            fi
        fi
        PREV_RX=${ANDROID_RX:-0}
    fi

    if [ -n "$ANDROID_ERRORS" ]; then
        log "Android errors:"
        echo "$ANDROID_ERRORS" | while read -r line; do log "  $line"; done
    fi

    # Summary for this checkpoint
    if [ "$PI_ALIVE" != "0" ] && [ -n "$ANDROID_PID" ]; then
        log "STATUS: OK"
    else
        log "STATUS: PROBLEM DETECTED"
    fi

    if [ "$i" -lt "$TOTAL_CHECKS" ]; then
        log "Sleeping ${INTERVAL}s until next checkpoint..."
        sleep "$INTERVAL"
    fi
done

# Final report
TOTAL_ELAPSED=$(( ($(date +%s) - START_TIME) / 60 ))
FINAL_TX=$(get_pi_tx_count || echo "?")
FINAL_RX=$(get_android_rx_count || echo "?")
FINAL_TX_STAT=$(get_pi_tx_stat || echo "N/A")
FINAL_RX_STAT=$(get_android_rx_stat || echo "N/A")

log ""
log "=========================================="
log "ENDURANCE TEST COMPLETED"
log "Total duration: ${TOTAL_ELAPSED} minutes"
log "Final Pi TX seq: $FINAL_TX"
log "Final Pi stat: $FINAL_TX_STAT"
log "Final Android RX seq: $FINAL_RX"
log "Final Android stat: $FINAL_RX_STAT"
if [ "$FINAL_TX" != "?" ] && [ "$FINAL_RX" != "?" ]; then
    LOSS=$((FINAL_TX - FINAL_RX))
    if [ "$FINAL_TX" -gt 0 ]; then
        LOSS_PCT=$(echo "scale=2; $LOSS * 100 / $FINAL_TX" | bc)
        log "Frame loss: $LOSS / $FINAL_TX = ${LOSS_PCT}%"
        if [ "$(echo "$LOSS_PCT < 5" | bc)" = "1" ]; then
            log "RESULT: PASS (loss < 5%)"
        else
            log "RESULT: FAIL (loss >= 5%)"
        fi
    fi
fi
log "=========================================="
