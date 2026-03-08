#!/usr/bin/env bash
#
# deploy_to_server.sh -- One-click deployment of P2P AV services to a remote k3s server.
#
# Usage:
#   ./deploy/k8s/deploy_to_server.sh --server root@43.156.123.38 --public-ip 43.156.123.38
#
# Options:
#   --server   SSH destination (user@host)
#   --public-ip  Public IP for TURN external-ip and TLS SAN
#   --skip-build  Skip Docker image build (use cached images)
#   --skip-setup  Skip remote server setup (k3s already installed)
#   --help        Show this help
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
K8S_DIR="$SCRIPT_DIR"
TMP_DIR="/tmp/p2p-deploy-$$"

SERVER=""
PUBLIC_IP=""
SKIP_BUILD=false
SKIP_SETUP=false

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[deploy]${NC} $1"; }
pass()  { echo -e "${GREEN}[  OK  ]${NC} $1"; }
warn()  { echo -e "${YELLOW}[ WARN ]${NC} $1"; }
fail()  { echo -e "${RED}[ FAIL ]${NC} $1"; }

usage() {
    echo "Usage: $0 --server USER@HOST --public-ip IP [--skip-build] [--skip-setup]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)     SERVER="$2"; shift 2 ;;
        --public-ip)  PUBLIC_IP="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=true; shift ;;
        --skip-setup) SKIP_SETUP=true; shift ;;
        --help|-h)    usage ;;
        *)            echo "Unknown option: $1"; usage ;;
    esac
done

if [[ -z "$SERVER" || -z "$PUBLIC_IP" ]]; then
    fail "Both --server and --public-ip are required"
    usage
fi

SSH_HOST="${SERVER##*@}"
SSH_USER="${SERVER%%@*}"
[[ "$SSH_USER" == "$SSH_HOST" ]] && SSH_USER="root"

echo ""
echo "============================================"
echo "  P2P AV K8s Deployment"
echo "  Server:    $SERVER"
echo "  Public IP: $PUBLIC_IP"
echo "============================================"
echo ""

cleanup() {
    rm -rf "$TMP_DIR" 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$TMP_DIR"

# ── Step 0: SSH connectivity check ────────────────────────────────────
info "Checking SSH connectivity..."
if ! ssh -o ConnectTimeout=10 -o BatchMode=yes "$SERVER" "echo ok" >/dev/null 2>&1; then
    fail "Cannot SSH to $SERVER. Check your SSH key and connectivity."
    exit 1
fi
pass "SSH connection OK"

# ── Step 1: Remote server setup ───────────────────────────────────────
if [[ "$SKIP_SETUP" == "false" ]]; then
    info "Setting up remote server (k3s + firewall)..."
    ssh "$SERVER" 'bash -s' < "$K8S_DIR/setup_remote.sh"
    pass "Remote setup complete"
else
    info "Skipping remote setup (--skip-setup)"
fi

# ── Step 2: Build Docker images ──────────────────────────────────────
if [[ "$SKIP_BUILD" == "false" ]]; then
    info "Building Docker images..."
    cd "$PROJECT_DIR"

    info "  Building p2p-signaling..."
    docker build -t p2p-signaling:latest -f deploy/k8s/signaling/Dockerfile . > /dev/null 2>&1
    pass "  p2p-signaling:latest built"

    info "  Building p2p-coturn..."
    docker build -t p2p-coturn:latest -f deploy/k8s/coturn/Dockerfile . > /dev/null 2>&1
    pass "  p2p-coturn:latest built"
else
    info "Skipping image build (--skip-build)"
fi

# ── Step 3: Export images ─────────────────────────────────────────────
info "Exporting Docker images..."
docker save p2p-signaling:latest | gzip > "$TMP_DIR/p2p-signaling.tar.gz"
pass "  p2p-signaling.tar.gz ($(du -h "$TMP_DIR/p2p-signaling.tar.gz" | cut -f1))"

docker save p2p-coturn:latest | gzip > "$TMP_DIR/p2p-coturn.tar.gz"
pass "  p2p-coturn.tar.gz ($(du -h "$TMP_DIR/p2p-coturn.tar.gz" | cut -f1))"

# Also export redis:7-alpine and alpine:3.19 for the server
info "Exporting base images..."
docker pull redis:7-alpine > /dev/null 2>&1 || true
docker pull alpine:3.19 > /dev/null 2>&1 || true
docker save redis:7-alpine | gzip > "$TMP_DIR/redis.tar.gz"
docker save alpine:3.19 | gzip > "$TMP_DIR/alpine.tar.gz"
pass "  Base images exported"

# ── Step 4: Generate secrets ──────────────────────────────────────────
info "Generating deployment secrets..."
JWT_SECRET=$(openssl rand -base64 32 | tr -d '=/+' | head -c 40)
ADMIN_SECRET=$(openssl rand -base64 24 | tr -d '=/+' | head -c 24)
TURN_SECRET=$(openssl rand -base64 32 | tr -d '=/+' | head -c 40)

cat > "$TMP_DIR/secrets.yaml" << EOF
apiVersion: v1
kind: Secret
metadata:
  name: p2p-secrets
  namespace: p2p-av
  labels:
    app.kubernetes.io/part-of: p2p-av
type: Opaque
stringData:
  jwt-secret: "${JWT_SECRET}"
  admin-secret: "${ADMIN_SECRET}"
  turn-secret: "${TURN_SECRET}"
EOF
pass "Secrets generated (admin-secret: ${ADMIN_SECRET})"

# ── Step 5: Generate TLS certificate ─────────────────────────────────
info "Generating TLS certificate for ${PUBLIC_IP}..."
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -subj "/CN=signaling/O=p2p-av" \
    -addext "subjectAltName=DNS:signaling,DNS:signaling.p2p-av.svc,DNS:localhost,IP:127.0.0.1,IP:${PUBLIC_IP}" \
    -keyout "$TMP_DIR/tls.key" -out "$TMP_DIR/tls.crt" 2>/dev/null
pass "TLS certificate generated"

# ── Step 6: Prepare adapted manifests ────────────────────────────────
info "Preparing K8s manifests for public IP ${PUBLIC_IP}..."

cp "$K8S_DIR/namespace.yaml" "$TMP_DIR/"
cp "$K8S_DIR/redis/service.yaml" "$TMP_DIR/redis-service.yaml"
cp "$K8S_DIR/redis/statefulset.yaml" "$TMP_DIR/redis-statefulset.yaml"
cp "$K8S_DIR/coturn/configmap.yaml" "$TMP_DIR/coturn-configmap.yaml"
cp "$K8S_DIR/coturn/deployment.yaml" "$TMP_DIR/coturn-deployment.yaml"
cp "$K8S_DIR/signaling/service.yaml" "$TMP_DIR/signaling-service.yaml"

# Patch signaling deployment: replace TURN_SERVERS host with public IP
sed "s|\"host\":\"[0-9.]*\"|\"host\":\"${PUBLIC_IP}\"|g" \
    "$K8S_DIR/signaling/deployment.yaml" > "$TMP_DIR/signaling-deployment.yaml"

# Patch coturn deployment: set EXTERNAL_IP to the public IP
sed "s|value: \"\"|value: \"${PUBLIC_IP}\"|" \
    "$K8S_DIR/coturn/deployment.yaml" > "$TMP_DIR/coturn-deployment.yaml"

pass "Manifests adapted for ${PUBLIC_IP}"

# ── Step 7: Transfer files to server ─────────────────────────────────
info "Transferring files to server..."
REMOTE_DIR="/tmp/p2p-deploy"
ssh "$SERVER" "rm -rf $REMOTE_DIR && mkdir -p $REMOTE_DIR"
scp -q "$TMP_DIR"/*.tar.gz "$SERVER:$REMOTE_DIR/"
scp -q "$TMP_DIR"/*.yaml "$TMP_DIR"/tls.key "$TMP_DIR"/tls.crt "$SERVER:$REMOTE_DIR/"
pass "Files transferred"

# ── Step 8: Import images on server ──────────────────────────────────
info "Importing Docker images into k3s..."
ssh "$SERVER" << 'REMOTE_IMPORT'
set -e
cd /tmp/p2p-deploy
for img in redis.tar.gz alpine.tar.gz p2p-signaling.tar.gz p2p-coturn.tar.gz; do
    echo "  importing $img..."
    gunzip -c "$img" | k3s ctr images import -
done
echo "Images imported:"
k3s ctr images list | grep -E "redis|alpine|p2p-" | awk '{print "  "$1}'
REMOTE_IMPORT
pass "Images imported"

# ── Step 9: Apply K8s manifests ──────────────────────────────────────
info "Applying K8s manifests..."
ssh "$SERVER" << REMOTE_APPLY
set -e
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
cd /tmp/p2p-deploy

echo "  Creating namespace..."
kubectl apply -f namespace.yaml

echo "  Creating secrets..."
kubectl apply -f secrets.yaml

echo "  Creating TLS secret..."
kubectl -n p2p-av create secret tls signaling-tls \
    --cert=tls.crt --key=tls.key \
    --dry-run=client -o yaml | kubectl apply -f -

echo "  Deploying Redis..."
kubectl apply -f redis-service.yaml
kubectl apply -f redis-statefulset.yaml

echo "  Deploying Coturn..."
kubectl apply -f coturn-configmap.yaml
kubectl apply -f coturn-deployment.yaml

echo "  Deploying Signaling..."
kubectl apply -f signaling-deployment.yaml
kubectl apply -f signaling-service.yaml

echo "  Waiting for pods..."
kubectl -n p2p-av wait --for=condition=Ready pod -l app.kubernetes.io/name=redis --timeout=120s || true
kubectl -n p2p-av wait --for=condition=Ready pod -l app.kubernetes.io/name=coturn --timeout=60s || true
kubectl -n p2p-av wait --for=condition=Ready pod -l app.kubernetes.io/name=signaling --timeout=60s || true
REMOTE_APPLY
pass "K8s manifests applied"

# ── Step 10: Verify deployment ────────────────────────────────────────
echo ""
info "=== Deployment Verification ==="
echo ""

ssh "$SERVER" << REMOTE_VERIFY
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
echo "--- Pods ---"
kubectl -n p2p-av get pods -o wide
echo ""
echo "--- Services ---"
kubectl -n p2p-av get svc
echo ""
echo "--- Health Check ---"
curl -sk "https://127.0.0.1:30443/health" || echo "(health check pending)"
echo ""
REMOTE_VERIFY

echo ""
info "Testing external access..."
HEALTH=$(curl -sk --connect-timeout 10 "https://${PUBLIC_IP}:30443/health" 2>/dev/null || echo "")
if echo "$HEALTH" | grep -q '"status":"ok"'; then
    pass "External health check: $HEALTH"
else
    warn "External health check failed (ensure port 30443/TCP is open in cloud security group)"
    warn "Response: $HEALTH"
fi

# ── Summary ───────────────────────────────────────────────────────────
echo ""
echo "============================================"
echo "  Deployment Complete!"
echo "============================================"
echo ""
echo "  Server:       $SERVER"
echo "  Public IP:    $PUBLIC_IP"
echo "  Signaling:    https://${PUBLIC_IP}:30443"
echo "  TURN/STUN:    ${PUBLIC_IP}:3478"
echo ""
echo "  Admin Secret: $ADMIN_SECRET"
echo "  (Save this -- needed for JWT token generation)"
echo ""
echo "  Get a JWT token:"
echo "    curl -sk 'https://${PUBLIC_IP}:30443/v1/token?peer_id=myid' \\"
echo "      -H 'Authorization: Bearer ${ADMIN_SECRET}'"
echo ""
echo "  Connect p2p_client:"
echo "    p2p_client --signaling ${PUBLIC_IP}:30443 --stun ${PUBLIC_IP}:3478 \\"
echo "      --token <JWT_TOKEN> --room myroom --peer-id pub1 ..."
echo ""
echo "  IMPORTANT: Ensure these ports are open in Tencent Cloud Security Group:"
echo "    30443/TCP, 3478/UDP, 3478/TCP, 5349/TCP, 49152-50200/UDP"
echo ""
