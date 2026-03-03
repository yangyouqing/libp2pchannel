# libp2pchannel

QUIC over P2P, embedded media transport solution.

## KEYWORDS

：QUIC、P2P、NAT穿透、文件传输、XQUIC、libjuice、C/C++、跨平台、目标直连率>95%, MPQUIC, 连接迁移, 首帧秒加载, 先通后优

## Overview

A peer-to-peer audio/video transmission system that combines ICE-based NAT traversal with QUIC transport for low-latency, reliable media streaming.

**Architecture:**

```
Publisher (p2p_client)                    Subscriber (p2p_peer)
┌─────────────────────┐                  ┌─────────────────────┐
│ V4L2 Video Capture  │                  │ SDL2 Video Display   │
│ ALSA Audio Capture  │                  │ SDL2 Audio Playback  │
│ FFmpeg H.264/Opus   │                  │ FFmpeg H.264/Opus    │
│   Encoder           │                  │   Decoder            │
└────────┬────────────┘                  └────────┬────────────┘
         │                                        │
    ┌────▼────────────────────────────────────────▼────┐
    │              xquic (QUIC transport)              │
    ├─────────────────────────────────────────────────-─┤
    │         libjuice (ICE / STUN / TURN)             │
    └────────┬────────────────────────────────────┬────┘
             │          Signaling Server          │
             └──────────► (Go, TCP) ◄─────────────┘
```

## Project Structure

```
libp2pchannel/
├── CMakeLists.txt              # Root build configuration
├── README.md
├── LICENSE
├── cmake/                      # CMake toolchains & cross-compilation support
│   ├── mingw-w64-x86_64.cmake  #   MinGW-w64 toolchain file
│   └── mingw-compat/           #   POSIX→Winsock header wrappers
├── p2p/                        # Core P2P library (C11)
│   ├── include/                #   Public headers
│   └── src/                    #   p2p_client.c, p2p_peer.c, adapters, codecs
├── signaling-server/           # Go signaling server (TCP JSON)
├── scripts/                    # Build & deployment scripts
│   ├── build.sh                #   Native build (Linux / MSYS2)
│   ├── build_win.sh            #   Cross-compile to Windows (MinGW-w64)
│   ├── deploy_client.sh        #   Deploy publisher
│   └── deploy_peer.sh          #   Deploy subscriber
├── tests/                      # Integration tests
├── third_party/                # External dependencies
│   ├── libjuice/               #   ICE / STUN / TURN agent
│   └── xquic/                  #   QUIC protocol (+ BoringSSL)
├── conf/                       # Configuration templates
├── docs/                       # Design documents
├── build/                      # Native build output (gitignored)
└── build_win/                  # Windows cross-build output (gitignored)
```

## Components

| Component | Description |
|-----------|-------------|
| **p2p/** | Core P2P adapter library — bridges libjuice ICE and xquic QUIC |
| **third_party/libjuice/** | ICE agent for STUN/TURN NAT traversal |
| **third_party/xquic/** | QUIC protocol implementation (requires BoringSSL) |
| **signaling-server/** | Go TCP server for SDP/candidate exchange and TURN credential distribution |
| **scripts/** | Build, deployment, and test scripts |

## Quick Start

### Prerequisites

```bash
# Install system dependencies
sudo apt-get install -y \
    build-essential cmake pkg-config git golang \
    libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libsdl2-dev libasound2-dev v4l-utils
```

### Build (Linux)

All build artifacts go into the `build/` directory:

```bash
# Full build (BoringSSL → xquic → libjuice + p2p + tests + signaling server)
./scripts/build.sh

# With system dependency installation
./scripts/build.sh --install-deps

# Clean rebuild
./scripts/build.sh --clean
```

### Cross-compile for Windows

Cross-compile from Linux to Windows x86_64 using MinGW-w64:

```bash
# First time (installs mingw-w64, downloads FFmpeg/SDL2 dev libs)
./scripts/build_win.sh --install-deps --clean

# Subsequent builds
./scripts/build_win.sh
```

Output in `build_win/bin/` — copy to Windows machine:

```
build_win/bin/
  p2p_client.exe           # Publisher
  p2p_peer.exe             # Subscriber
  signaling-server.exe     # Signaling server
  *.dll                    # Runtime DLLs (FFmpeg, SDL2, MinGW runtime)
```

### Build output (native)

```
build/
  boringssl/libssl.a, libcrypto.a
  xquic/libxquic-static.a
  third_party/libjuice/libjuice.a
  p2p/p2p_client
  p2p/p2p_peer
  signaling-server
  tests/test_signaling
  tests/test_ice_connectivity
```

### Run Local Test

```bash
./scripts/run_av_test.sh
```

### Deploy

**Publisher (p2p_client + signaling server):**

```bash
# Build and deploy
./scripts/deploy_client.sh

# Build, deploy, and start
./scripts/deploy_client.sh --start

# Custom signaling address
SIGNALING_ADDR=192.168.1.100:8080 ./scripts/deploy_client.sh --start
```

**Subscriber (p2p_peer):**

```bash
# Connect to a running publisher
SIGNALING_ADDR=192.168.1.100:8080 ./scripts/deploy_peer.sh --start
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `SIGNALING_ADDR` | `127.0.0.1:8080` | Signaling server address |
| `STUN_SERVER` | `127.0.0.1:3478` | STUN/TURN server address |
| `ROOM_ID` | `test-room` | Room identifier |
| `PEER_ID` | `pub1` / `sub1` | Peer identifier |
| `VIDEO_DEV` | `/dev/video0` | V4L2 video device |
| `AUDIO_DEV` | `default` | ALSA audio device |
| `TURN_HOST` | `127.0.0.1` | TURN server host |
| `TURN_PORT` | `3478` | TURN server port |
| `TURN_SECRET` | `p2p-turn-secret` | TURN shared secret |

## Performance Targets

- First frame load: < 1 second
- End-to-end latency: < 200ms
- Video codec: H.264 (zerolatency tuning)
- Audio codec: Opus
- p2p directly connect rate: > 95%

## License

[MIT](LICENSE)
