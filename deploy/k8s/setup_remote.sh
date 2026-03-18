#!/usr/bin/env bash
#
# setup_remote.sh -- Prepare a remote Ubuntu server for k3s + P2P AV deployment.
#
# Run on the remote server via:
#   ssh root@SERVER 'bash -s' < deploy/k8s/setup_remote.sh
#
# What it does:
#   1. Install k3s (single-node, no Traefik/ServiceLB)
#   2. Configure container registry mirrors for Docker Hub
#   3. Open required firewall ports (ufw)
#   4. Verify k3s is running
#
set -euo pipefail

info()  { echo -e "\033[0;36m[setup]\033[0m $1"; }
pass()  { echo -e "\033[0;32m[  OK ]\033[0m $1"; }
fail()  { echo -e "\033[0;31m[FAIL]\033[0m $1"; }


# ── 1. System prerequisites ──────────────────────────────────────────
info "Installing prerequisites..."
apt-get update -qq
apt-get install -y -qq curl openssl > /dev/null
pass "Prerequisites installed"

# ── 2. Install k3s ───────────────────────────────────────────────────
if command -v k3s &>/dev/null && systemctl is-active --quiet k3s; then
    pass "k3s already installed and running ($(k3s --version | head -1))"
else
    info "Installing k3s..."
    curl -sfL https://get.k3s.io | sh -s - \
        --write-kubeconfig-mode 644 \
        --disable traefik \
        --disable servicelb
    pass "k3s installed"
fi

# Wait for node to be ready
info "Waiting for k3s node to be ready..."
export KUBECONFIG=/etc/rancher/k3s/k3s.yaml
for i in $(seq 1 60); do
    if kubectl get nodes 2>/dev/null | grep -q " Ready"; then
        break
    fi
    sleep 2
done

if kubectl get nodes | grep -q " Ready"; then
    pass "k3s node ready"
else
    fail "k3s node not ready after 120s"
    exit 1
fi

# ── 3. Configure registry mirrors ────────────────────────────────────
REGISTRIES_FILE="/etc/rancher/k3s/registries.yaml"
if [ ! -f "$REGISTRIES_FILE" ]; then
    info "Configuring Docker Hub registry mirrors..."
    mkdir -p /etc/rancher/k3s
    cat > "$REGISTRIES_FILE" << 'MIRRORS'
mirrors:
  docker.io:
    endpoint:
      - "https://docker.m.daocloud.io"
      - "https://registry.cn-hangzhou.aliyuncs.com"
MIRRORS
    systemctl restart k3s
    sleep 10
    pass "Registry mirrors configured"
else
    pass "Registry mirrors already configured"
fi

# ── 4. Firewall (ufw) ────────────────────────────────────────────────
info "Configuring firewall..."
if command -v ufw &>/dev/null; then
    ufw --force enable > /dev/null 2>&1 || true
    ufw allow 22/tcp        comment "SSH"                > /dev/null 2>&1
    ufw allow 6443/tcp      comment "K8s API"            > /dev/null 2>&1
    ufw allow 30443/tcp     comment "Signaling HTTPS"    > /dev/null 2>&1
    ufw allow 3478/udp      comment "TURN/STUN UDP"      > /dev/null 2>&1
    ufw allow 3478/tcp      comment "TURN/STUN TCP"      > /dev/null 2>&1
    ufw allow 5349/tcp      comment "TURN TLS"           > /dev/null 2>&1
    ufw allow 49152:50200/udp comment "TURN relay range" > /dev/null 2>&1
    pass "ufw rules applied"
else
    info "ufw not found -- skipping (ensure cloud security group allows the ports)"
fi

# ── 5. Verify ─────────────────────────────────────────────────────────
echo ""
info "=== k3s status ==="
kubectl get nodes -o wide
echo ""
kubectl get pods -A
echo ""
pass "Server setup complete!"
echo ""
echo "IMPORTANT: Open these ports in your cloud provider's Security Group:"
echo "  30443/TCP   - Signaling server (HTTPS)"
echo "  3478/UDP    - TURN/STUN"
echo "  3478/TCP    - TURN/STUN"
echo "  5349/TCP    - TURN TLS"
echo "  49152-50200/UDP - TURN relay range"
echo ""
