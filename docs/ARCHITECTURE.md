# Socket.IWA Architecture

This document captures the internal architecture details that are intentionally kept out of the top-level README.

## High-level runtime model

Socket.IWA runs a QUIC stack compiled to WebAssembly inside a Chrome Isolated Web App (IWA), and replaces Emscripten's default socket path with Direct Sockets-backed syscall implementations.

## QUIC/TLS/HTTP stack

Current stack:

- **QUIC transport:** `ngtcp2`
- **HTTP/3:** `nghttp3`
- **TLS 1.3:** `wolfSSL`
- **WASM toolchain:** Emscripten 5.0.1

## Direct Sockets bridge in Emscripten

The key integration point is:

- `emscripten/src/lib/libdirectsockets.js`

This file overrides socket-related libc syscalls and routes them to browser Direct Sockets APIs, including syscall paths like:

- `__syscall_socket`
- `__syscall_bind`
- `__syscall_recvfrom`
- `__syscall_sendto`

## Async model (JSPI)

Socket operations are asynchronous in browser APIs, while the C-side networking stack expects blocking semantics.

Socket.IWA uses **JavaScript Promise Integration (JSPI)** to bridge promise-based browser operations into the Emscripten runtime without Asyncify.

## Data path overview

```text
C code (ngtcp2/nghttp3/wolfSSL app)
  -> libc socket syscalls
  -> Emscripten JS library hooks (libdirectsockets.js)
  -> Direct Sockets API (IWA)
  -> host UDP/TCP networking
```

## Build orchestration

- `docker_build_quic.sh` builds the QUIC server for WASM in an Emscripten container.
- `stress-test/native-baseline/build_native.sh` builds native counterparts for perf and correctness comparison.

## Session ticket behavior

The native and WASM servers support TLS 1.3 session tickets:

1. Initial connection completes full handshake.
2. Server issues NewSessionTicket.
3. Returning client resumes and can send 0-RTT early data.

Reference test binary:

- `stress-test/native-baseline/build/test_session_ticket`

## CI pipeline notes

GitHub Actions workflow (`.github/workflows/build.yml`) runs:

- WASM build job
- Native build job
- Session ticket integration test
- JavaScript syntax check for user-facing API
