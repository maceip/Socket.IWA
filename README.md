# stare-socket

Real POSIX sockets from WebAssembly — replacing Emscripten's WebSocket proxy with Chrome's Direct Sockets API.

## What this is

A proof-of-concept that compiles a full QUIC server stack to WebAssembly and runs it in the browser with real UDP networking. No WebSocket tunneling, no server-side proxies — actual `bind()` / `recvfrom()` / `sendto()` over the network.

### QUIC Server Stack

| Layer | Library | Version |
|-------|---------|---------|
| **Transport** | ngtcp2 | v1.20.90 |
| **TLS 1.3** | wolfSSL | latest |
| **HTTP/3** | nghttp3 | v1.15.90 |
| **Compiler** | Emscripten | 5.0.1 |
| **Async bridge** | JSPI | (not Asyncify) |

### Protocol Support

- Raw QUIC echo (ALPN `echo`)
- HTTP/3 with Extended CONNECT
- WebTransport (DATAGRAM frames + bidi/uni streams)
- WebSocket over HTTP/3 (RFC 9220)

## Performance

**WASM achieves 90.4% of native Linux throughput** on UDP packet handling.

| Test | Native | WASM | Ratio |
|------|--------|------|-------|
| Burst 50k pkts | 85,669 pps | 62,170 pps | 72.6% |
| Constant 10k pps | 5,956 pps | 5,783 pps | 97.1% |
| Ramp 0→50k pps | 8,139 pps | 7,741 pps | 95.1% |
| Chaos (random mix) | 1,014 pps | 1,037 pps | 102.3% |

At realistic traffic rates, WASM is indistinguishable from native.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Chrome Isolated Web App                    │
│  ┌───────────────────────────────────────┐  │
│  │  quic_echo_server.wasm               │  │
│  │  (ngtcp2 + wolfSSL + nghttp3)        │  │
│  │         ↕ JSPI async bridge          │  │
│  │  libdirectsockets.js                 │  │
│  │  (Emscripten syscall override)       │  │
│  └───────────┬───────────────────────────┘  │
│              ↕ Direct Sockets API           │
└──────────────┬──────────────────────────────┘
               ↕ UDP kernel sockets
           [ Network ]
```

## Build

### Prerequisites

- Docker (for Emscripten cross-compilation)
- The dependency repos are cloned automatically by the build scripts

### QUIC echo server (WASM)

```bash
bash docker_build_quic.sh
```

Produces `quic_echo_server.js` + `quic_echo_server.wasm`.

### Native baseline (for benchmarking)

```bash
bash stress-test/native-baseline/build_native.sh
```

### IWA web app

The Chrome Isolated Web App is in a separate repo: maceip/socket-iwa

## Emscripten Patches

The key innovation is `emscripten/src/lib/libdirectsockets.js` — a JS library that intercepts Emscripten's socket syscalls (`__syscall_socket`, `__syscall_bind`, `__syscall_recvfrom`, etc.) and routes them through Chrome's Direct Sockets API instead of the default WebSocket proxy.

Uses JSPI (JavaScript Promise Integration) to bridge async Direct Sockets promises to synchronous POSIX socket calls, without Asyncify overhead.

## Stress Testing

```bash
# UDP flood test
python3 stress-test/scripts/quic_flood.py --mode burst --packets 50000

# Full benchmark suite (WASM vs native)
bash stress-test/scripts/benchmark_wasm_vs_native.sh
```

## Files

| File | Description |
|------|-------------|
| `quic_echo_server.c` | QUIC server with HTTP/3 + WebTransport + RFC 9220 |
| `docker_build_quic.sh` | Cross-compile wolfSSL → nghttp3 → ngtcp2 → WASM |
| `docker_build.sh` | Emscripten patches + basic socket test build |
| `gen_cert.sh` | Generate ECDSA P-256 self-signed cert |
| `emscripten/src/lib/libdirectsockets.js` | Direct Sockets syscall implementation |
| `stress-test/` | Benchmark scripts, flood tools, native baseline |

## License

Apache-2.0
