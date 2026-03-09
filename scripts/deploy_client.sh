#!/usr/bin/env bash
#
# deploy_client.sh -- 一键运行 p2p_client (发布端)
#
# 连接远程 K8s 信令/TURN 服务器，采集本地摄像头+麦克风，通过 P2P 发送音视频。
# 如果不指定 --room，p2p_client 自动生成 6 位随机 room_id 并打印在日志中。
# 对端 p2p_peer 输入同一 room_id 即可加入。
#
# 用法:
#   ./scripts/deploy_client.sh [选项]
#
# 选项:
#   --server IP         远程服务器 IP               (默认: 106.54.30.119)
#   --sig-port PORT     信令端口                     (默认: 30443)
#   --stun-port PORT    STUN/TURN 端口              (默认: 3478)
#   --room ROOM         房间 ID (不填则自动生成)
#   --peer-id ID        发布端 ID                    (默认: pub1)
#   --video-dev DEV     视频设备                     (默认: 自动检测)
#   --audio-dev DEV     音频设备                     (默认: default)
#   --admin-secret S    JWT 管理密钥
#   --no-video          仅音频模式 (不采集/编码视频)
#   --no-audio          仅视频模式 (不采集/编码音频)
#   --rebuild           强制重新编译
#   --duration SEC      运行时长 (0 = 无限)          (默认: 0)
#   -h, --help          显示帮助
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# ---- 默认配置 ----
SERVER_IP="106.54.30.119"
SIGNALING_PORT="30443"
STUN_PORT="3478"
ROOM_ID=""
PEER_ID="pub1"
ADMIN_SECRET="eLTGSBSmlCZqar7lwkf4GFje"
AUDIO_DEV="default"
VIDEO_DEV=""
FORCE_BUILD=false
DURATION=0
NO_VIDEO=false
NO_AUDIO=false

# ---- 路径 ----
CLIENT_BIN="$BUILD_DIR/src/p2p/p2p_client"
LIB_DIR="$BUILD_DIR/src/p2p"
CERT_DIR="$BUILD_DIR/certs"
LOG_DIR="$BUILD_DIR/logs"

# ---- 工具函数 ----
GREEN='\033[1;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[client]${NC} $*"; }
warn() { echo -e "${YELLOW}[client]${NC} $*"; }
die()  { echo -e "${RED}[client]${NC} $*"; exit 1; }

detect_video() {
    for vdev in /dev/video*; do
        [[ -c "$vdev" ]] && v4l2-ctl -d "$vdev" --info 2>/dev/null | grep -q "uvcvideo" && { echo "$vdev"; return; }
    done
    echo "/dev/video0"
}

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)       SERVER_IP="$2"; shift 2 ;;
        --sig-port)     SIGNALING_PORT="$2"; shift 2 ;;
        --stun-port)    STUN_PORT="$2"; shift 2 ;;
        --room)         ROOM_ID="$2"; shift 2 ;;
        --peer-id)      PEER_ID="$2"; shift 2 ;;
        --video-dev)    VIDEO_DEV="$2"; shift 2 ;;
        --audio-dev)    AUDIO_DEV="$2"; shift 2 ;;
        --admin-secret) ADMIN_SECRET="$2"; shift 2 ;;
        --no-video)     NO_VIDEO=true; shift ;;
        --no-audio)     NO_AUDIO=true; shift ;;
        --rebuild)      FORCE_BUILD=true; shift ;;
        --duration)     DURATION="$2"; shift 2 ;;
        -h|--help)      head -28 "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *)              die "未知选项: $1" ;;
    esac
done

SIGNALING="${SERVER_IP}:${SIGNALING_PORT}"
STUN="${SERVER_IP}:${STUN_PORT}"
[[ -z "$VIDEO_DEV" ]] && VIDEO_DEV=$(detect_video)

# ============================================================
# 1. 编译 (仅在二进制不存在或 --rebuild 时)
# ============================================================
if $FORCE_BUILD || [[ ! -x "$CLIENT_BIN" ]]; then
    log "编译项目..."
    "$SCRIPT_DIR/build.sh"
fi
[[ -x "$CLIENT_BIN" ]] || die "二进制不存在: $CLIENT_BIN (请先执行 scripts/build.sh)"

# ============================================================
# 2. QUIC TLS 证书 (首次自动生成)
# ============================================================
mkdir -p "$CERT_DIR"
if [[ ! -f "$CERT_DIR/server.crt" ]]; then
    log "生成 QUIC TLS 证书..."
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -batch -subj "/CN=p2p-quic" 2>/dev/null
fi

# ============================================================
# 3. 清理残留进程 (避免摄像头占用)
# ============================================================
OLD_PIDS=$(pgrep -f "$CLIENT_BIN" 2>/dev/null || true)
if [[ -n "$OLD_PIDS" ]]; then
    warn "终止残留 p2p_client 进程: $OLD_PIDS"
    kill $OLD_PIDS 2>/dev/null || true
    sleep 1
fi

# ============================================================
# 4. 检查信令服务器
# ============================================================
log "连接信令服务器 $SIGNALING ..."
HEALTH=$(curl -sk --connect-timeout 5 "https://${SIGNALING}/health" 2>/dev/null || echo "{}")
echo "$HEALTH" | grep -q '"status":"ok"' || die "信令服务器不可达: $HEALTH"
log "信令正常"

# ============================================================
# 5. 获取 JWT Token
# ============================================================
log "获取 JWT Token (peer=$PEER_ID) ..."
TOKEN_JSON=$(curl -sk --connect-timeout 10 \
    "https://${SIGNALING}/v1/token?peer_id=${PEER_ID}" \
    -H "Authorization: Bearer ${ADMIN_SECRET}" 2>/dev/null)
TOKEN=$(echo "$TOKEN_JSON" | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])" 2>/dev/null || true)
TOKEN_ARG=""
if [[ -n "$TOKEN" ]]; then
    TOKEN_ARG="--token $TOKEN"
    log "Token 获取成功"
else
    warn "未能获取 Token，继续运行（无认证）"
fi

# ============================================================
# 6. 启动 p2p_client
# ============================================================
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/p2p_client.log"

ROOM_ARG=""
[[ -n "$ROOM_ID" ]] && ROOM_ARG="--room $ROOM_ID"
AV_ARGS=""
$NO_VIDEO && AV_ARGS="$AV_ARGS --no-video"
$NO_AUDIO && AV_ARGS="$AV_ARGS --no-audio"

echo ""
echo "============================================"
echo "  P2P Client (发布端) -> $SERVER_IP"
echo "============================================"
log "信令:     $SIGNALING"
log "STUN/TURN: $STUN"
log "视频设备:  $VIDEO_DEV"
log "音频设备:  $AUDIO_DEV"
[[ -n "$ROOM_ID" ]] && log "房间:     $ROOM_ID" || log "房间:     (自动生成, 见下方日志)"
log "日志:     $LOG_FILE"
echo ""

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cleanup() {
    echo ""
    log "正在停止 p2p_client ..."
    kill $CLIENT_PID 2>/dev/null || true
    wait $CLIENT_PID 2>/dev/null || true
    echo ""
    log "最后 10 行日志:"
    tail -10 "$LOG_FILE" 2>/dev/null | sed 's/^/  /'
}

"$CLIENT_BIN" \
    --signaling "$SIGNALING" \
    $ROOM_ARG \
    --peer-id "$PEER_ID" \
    --video-dev "$VIDEO_DEV" \
    --audio-dev "$AUDIO_DEV" \
    --stun "$STUN" \
    --ssl-cert "$CERT_DIR/server.crt" \
    --ssl-key "$CERT_DIR/server.key" \
    $TOKEN_ARG $AV_ARGS 2>&1 | tee "$LOG_FILE" &
CLIENT_PID=$!
trap cleanup EXIT INT TERM

if [[ "$DURATION" -gt 0 ]]; then
    log "运行 ${DURATION} 秒..."
    sleep "$DURATION"
else
    log "运行中，按 Ctrl+C 停止"
    wait $CLIENT_PID
fi
