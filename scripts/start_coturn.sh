#!/bin/bash
# Start coturn with the project configuration.
# Usage: ./scripts/start_coturn.sh [--build]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
COTURN_DIR="$PROJECT_DIR/coturn"
CONF_FILE="$PROJECT_DIR/conf/coturn.conf"

set -e

if [ "$1" = "--build" ]; then
    echo "Building coturn..."
    cd "$COTURN_DIR"
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    echo "coturn built successfully."
fi

# Try to find the turnserver binary
TURNSERVER=""
if [ -x "$COTURN_DIR/build/bin/turnserver" ]; then
    TURNSERVER="$COTURN_DIR/build/bin/turnserver"
elif command -v turnserver &> /dev/null; then
    TURNSERVER="turnserver"
else
    echo "Error: turnserver not found. Run with --build first or install coturn."
    exit 1
fi

echo "Starting coturn with config: $CONF_FILE"
exec $TURNSERVER -c "$CONF_FILE" "$@"
