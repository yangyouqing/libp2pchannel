#!/usr/bin/env bash
#
# deploy_peer.sh -- 一键运行 p2p_peer (接收端)
#
# 连接远程 K8s 信令/TURN 服务器，接收并播放 P2P 音视频。
# 启动后会显示 SDL 窗口：
#   - 如果不指定 --room，窗口中提示输入 room_id，回车加入
#   - 如果指定 --room，直接跳过输入界面加入房间
# 视频界面包含 OSD 叠加 (时间/延迟/RTT)、暂停/静音按钮、状态栏。
#
# 用法:
#   ./scripts/deploy_peer.sh [选项]
#
# 选项:
#   --server IP         远程服务器 IP               (默认: 106.54.30.119)
#   --sig-port PORT     信令端口                     (默认: 30443)
#   --stun-port PORT    STUN/TURN 端口              (默认: 3478)
#   --room ROOM         房间 ID (不填则在 UI 中输入)
#   --peer-id ID        接收端 ID                    (默认: sub1)
#   --admin-secret S    JWT 管理密钥
#   --no-video          仅音频模式 (不解码/显示视频)
#   --no-audio          仅视频模式 (不解码/播放音频)
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
PEER_ID="sub1"
ADMIN_SECRET="eLTGSBSmlCZqar7lwkf4GFje"
FORCE_BUILD=false
DURATION=0
NO_VIDEO=false
NO_AUDIO=false

# ---- 路径 ----
PEER_BIN="$BUILD_DIR/src/p2p/p2p_peer"
LIB_DIR="$BUILD_DIR/src/p2p"
LOG_DIR="$BUILD_DIR/logs"

# ---- 工具函数 ----
CYAN='\033[1;36m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; NC='\033[0m'
log()  { echo -e "${CYAN}[peer]${NC} $*"; }
warn() { echo -e "${YELLOW}[peer]${NC} $*"; }
die()  { echo -e "${RED}[peer]${NC} $*"; exit 1; }

# ---- 参数解析 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)       SERVER_IP="$2"; shift 2 ;;
        --sig-port)     SIGNALING_PORT="$2"; shift 2 ;;
        --stun-port)    STUN_PORT="$2"; shift 2 ;;
        --room)         ROOM_ID="$2"; shift 2 ;;
        --peer-id)      PEER_ID="$2"; shift 2 ;;
        --admin-secret) ADMIN_SECRET="$2"; shift 2 ;;
        --no-video)     NO_VIDEO=true; shift ;;
        --no-audio)     NO_AUDIO=true; shift ;;
        --rebuild)      FORCE_BUILD=true; shift ;;
        --duration)     DURATION="$2"; shift 2 ;;
        -h|--help)      head -26 "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
        *)              die "未知选项: $1" ;;
    esac
done

SIGNALING="${SERVER_IP}:${SIGNALING_PORT}"
STUN="${SERVER_IP}:${STUN_PORT}"

# ============================================================
# 1. 编译 (仅在二进制不存在或 --rebuild 时)
# ============================================================
if $FORCE_BUILD || [[ ! -x "$PEER_BIN" ]]; then
    log "编译项目..."
    "$SCRIPT_DIR/build.sh"
fi
[[ -x "$PEER_BIN" ]] || die "二进制不存在: $PEER_BIN (请先执行 scripts/build.sh)"

# ============================================================
# 2. 检查 DISPLAY (SDL 需要)
# ============================================================
if [[ -z "${DISPLAY:-}" ]]; then
    die "DISPLAY 未设置，无法启动 SDL 窗口。请在图形桌面环境下运行。"
fi

# ============================================================
# 3. 清理残留进程
# ============================================================
OLD_PIDS=$(pgrep -f "$PEER_BIN" 2>/dev/null || true)
if [[ -n "$OLD_PIDS" ]]; then
    warn "终止残留 p2p_peer 进程: $OLD_PIDS"
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
# 6. 启动 p2p_peer
# ============================================================
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/p2p_peer.log"

ROOM_ARG=""
[[ -n "$ROOM_ID" ]] && ROOM_ARG="--room $ROOM_ID"
AV_ARGS=""
$NO_VIDEO && AV_ARGS="$AV_ARGS --no-video"
$NO_AUDIO && AV_ARGS="$AV_ARGS --no-audio"

echo ""
echo "============================================"
echo "  P2P Peer (接收端) -> $SERVER_IP"
echo "============================================"
log "信令:     $SIGNALING"
log "STUN/TURN: $STUN"
[[ -n "$ROOM_ID" ]] && log "房间:     $ROOM_ID" || log "房间:     (启动后在窗口中输入)"
log "日志:     $LOG_FILE"
echo ""

export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cleanup() {
    echo ""
    log "正在停止 p2p_peer ..."
    kill $PEER_PID 2>/dev/null || true
    wait $PEER_PID 2>/dev/null || true
    echo ""
    log "最后 10 行日志:"
    tail -10 "$LOG_FILE" 2>/dev/null | sed 's/^/  /'
}

"$PEER_BIN" \
    --signaling "$SIGNALING" \
    $ROOM_ARG \
    --peer-id "$PEER_ID" \
    --stun "$STUN" \
    $TOKEN_ARG $AV_ARGS 2>&1 | tee "$LOG_FILE" &
PEER_PID=$!
trap cleanup EXIT INT TERM

if [[ "$DURATION" -gt 0 ]]; then
    log "运行 ${DURATION} 秒..."
    sleep "$DURATION"
else
    log "运行中，按 Ctrl+C 停止"
    wait $PEER_PID
fi
