# P2P AV K8s Deployment

Kubernetes manifests for deploying the P2P audio/video signaling infrastructure with high availability.

## Components

| Component | Replicas | Exposure | Purpose |
|-----------|----------|----------|---------|
| signaling-server | 2 | NodePort 30443 (HTTPS) | WebRTC signaling, SSE events, TURN credential distribution |
| coturn | 1 | hostNetwork UDP 3478 | TURN/STUN relay server |
| redis | 1 | ClusterIP 6379 | Shared state + cross-instance event pub/sub |

## Prerequisites

- Kubernetes cluster (k3s, kubeadm, etc.)
- `kubectl` configured to access the cluster
- `docker` for building images

## Quick Start

All commands are run from the `libp2pchannel/` directory.

### 1. Build Docker Images

```bash
docker build -t p2p-signaling:latest -f deploy/k8s/signaling/Dockerfile .
docker build -t p2p-coturn:latest -f deploy/k8s/coturn/Dockerfile .
```

Import images into **k3s** containerd:

```bash
docker save p2p-signaling:latest | sudo k3s ctr images import -
docker save p2p-coturn:latest | sudo k3s ctr images import -
```

### 2. Create Namespace and Secrets

```bash
kubectl apply -f deploy/k8s/namespace.yaml
```

Generate a self-signed TLS certificate:

```bash
openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout /tmp/tls.key -out /tmp/tls.crt -subj "/CN=signaling"

kubectl -n p2p-av create secret tls signaling-tls \
  --cert=/tmp/tls.crt --key=/tmp/tls.key
```

Edit `deploy/k8s/secrets.yaml` to set real secrets, then apply:

```bash
kubectl apply -f deploy/k8s/secrets.yaml
```

### 3. Update TURN Server Address

Edit `deploy/k8s/signaling/deployment.yaml` and replace `NODE_IP_PLACEHOLDER` in the `TURN_SERVERS` ConfigMap value with your node's IP address:

```bash
NODE_IP=$(kubectl get nodes -o jsonpath='{.items[0].status.addresses[?(@.type=="InternalIP")].address}')
echo "Node IP: $NODE_IP"
```

### 4. Deploy

```bash
kubectl apply -f deploy/k8s/redis/
kubectl apply -f deploy/k8s/coturn/
kubectl apply -f deploy/k8s/signaling/
```

### 5. Verify

```bash
kubectl -n p2p-av get pods -w
kubectl -n p2p-av logs -l app.kubernetes.io/name=signaling --tail=20
curl -k https://localhost:30443/health
```

Expected health output:

```json
{"mode":"redis","node_id":"signaling-xxxxx","redis":"ok","status":"ok"}
```

## Architecture

```
Client --HTTPS:30443--> [K8s Service] --> signaling-1 --\
                                      --> signaling-2 --|-- Redis (shared state + pub/sub)
Client --UDP:3478-----> coturn (hostNetwork)
```

- Signaling instances share room/peer state via Redis
- SSE events are routed cross-instance via Redis Pub/Sub
- Each signaling pod uses its pod name as NODE_ID
- Coturn uses hostNetwork to avoid UDP NAT hairpinning issues

## Configuration

### Secrets (secrets.yaml)

| Key | Description |
|-----|-------------|
| jwt-secret | JWT signing key for peer authentication |
| admin-secret | Admin API bearer token |
| turn-secret | TURN credential shared secret (must match coturn) |

### Signaling ConfigMap (in deployment.yaml)

| Key | Default | Description |
|-----|---------|-------------|
| LISTEN_ADDR | :8443 | Server listen address |
| REDIS_URL | redis://redis.p2p-av.svc:6379/0 | Redis connection |
| MAX_SUBSCRIBERS | 10 | Max subscribers per room |
| TURN_SERVERS | JSON array | TURN server list |

## Scaling

### Signaling

```bash
kubectl -n p2p-av scale deployment signaling --replicas=3
```

### Coturn

For multi-node clusters, convert to DaemonSet to run one coturn per node.

### Redis

For production, replace the single-instance Redis with Redis Sentinel or a managed Redis service.

## Cleanup

```bash
kubectl delete namespace p2p-av
```
