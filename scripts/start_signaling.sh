#!/bin/bash
# Start the Go signaling server.
# Environment variables:
#   LISTEN_ADDR   (default: :8080)
#   TURN_HOST     (default: 127.0.0.1)
#   TURN_PORT     (default: 3478)
#   TURN_SECRET   (default: p2p-turn-secret)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SIG_DIR="$PROJECT_DIR/signaling-server"

set -e

cd "$SIG_DIR"

echo "Building signaling server..."
go build -o signaling-server .

echo "Starting signaling server..."
exec ./signaling-server
