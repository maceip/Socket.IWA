<img align="left" height="128" alt="socket iwa" src="https://github.com/user-attachments/assets/9a9f761a-91cc-4e2b-ab67-ef73dfb18e3c" />

<div style="overflow: hidden;">

# 𝙎𝙤𝙘𝙠𝙚𝙩.𝙄𝙒𝘼

Socket.IWA is a full QUIC server stack that runs in the browser with real UDP networking. No WebSocket tunneling, no server-side proxies — actual `bind()` / `recvfrom()` / `sendto()` over the network. It turns your browser into a high performance, secure server that can listen on _most_ ports, serve websockets; webtransport; http3; *anything* at nearly line speed. + full certificate lifecycle implementation for webtransport:// included 
</div>

> [!IMPORTANT]
> As of Feb 2026, [browser support is evovling. ](https://caniuse.com/?search=direct+sockets) chrome support requires sockets be wrapped in an [isolated web app](https://developer.chrome.com/docs/iwa/introduction)


<br clear="left" />



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
- TLS 1.3 session tickets for 0-RTT resumption

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
    │  │  quic_echo_server.wasm                │  │
    │  │           ⁿᵍᵗᶜᵖ² ⁺ ʷᵒˡᶠˢˢᴸ ⁺ ⁿᵍʰᵗᵗᵖ³  │  │
    │  │           ↕ JSPI async bridge         │  │
    │  │  libdirectsockets.js                  │  │
    │  │           ᴱᵐˢᶜʳᶦᵖᵗᵉⁿ ˢʸˢᶜᵃˡˡ ᵒᵛᵉʳʳᶦᵈᵉ │  │
    │  └───────────┬───────────────────────────┘  │
    │              ↕ Direct Sockets API           │
    └──────────────┬──────────────────────────────┘
                   ↕ UDP kernel sockets
                   │
Incoming ───▶ [ Network ] ───▶ Outgoing
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

### WebTransfer Isolated Web App Server

The Chrome Isolated Web App is in a separate repo: [maceip/stare-socket](https://github.com/maceip/stare-socket)

## Emscripten Patches

The workhorse is `emscripten/src/lib/libdirectsockets.js` — a JS library that intercepts Emscripten's socket syscalls (`__syscall_socket`, `__syscall_bind`, `__syscall_recvfrom`, etc.) and routes them through Chrome's Direct Sockets API instead of the default WebSocket proxy.

Uses JSPI (JavaScript Promise Integration) to bridge async Direct Sockets promises to synchronous POSIX socket calls, without Asyncify overhead.

## Stress Testing

```bash
# UDP flood test
python3 stress-test/scripts/quic_flood.py --mode burst --packets 50000

# Full benchmark suite (WASM vs native)
bash stress-test/scripts/benchmark_wasm_vs_native.sh
```

## <img  height="120" alt="ddd-removebg-preview" src="https://github.com/user-attachments/assets/d7d78010-a19f-469c-a9da-da1e521a761b"  align="middle"/> Session Tickets 

**TL;DR** — Without session tickets a returning client repeats the full 2-RTT handshake every time (~100 ms at a typical 50 ms one-way datacenter latency). With session tickets the second connection sends data immediately in 0-RTT — zero round trips, zero wait.

The server issues TLS 1.3 session tickets after the first handshake. A returning client can skip the full handshake and send 0-RTT early data on reconnect — cutting one round trip from connection setup.
<br clear="middle"/>

<pre style="font-family: 'SFMono-Regular', Consolas, 'Liberation Mono', Menlo, monospace; line-height: 1.2; font-size: 13px;">
Connection 1 — full handshake                    2 RTT ≈ 100ms @ 50ms ping

    Client                                  Server
      │                                       │
      ├─── Initial [CRYPTO ClientHello] ───▶  │ ╮
      │                                       │ │ RTT 1
      │◀── Initial [CRYPTO ServerHello] ──────┤ │
      │◀── Handshake [CRYPTO Cert+Fin] ───────┤ ╯
      │◀── 1-RTT [NEW_CONNECTION_ID] ─────────┤
      │                                       │
      ├─── Handshake [CRYPTO Finished] ────▶  │ ╮
      ├─── 1-RTT [STREAM "hello"] ─────────▶  │ │ RTT 2
      │                                       │ │
      │◀── 1-RTT [HANDSHAKE_DONE] ────────────┤ ╯
      │◀── 1-RTT [CRYPTO NewSessionTicket] ───┤  ← ticket saved
      │◀── 1-RTT [STREAM "hello"] ────────────┤  ← echo received
      │                                       │

Connection 2 — 0-RTT resumption                  0 RTT ≈ 0ms (data in flight)

    Client                                  Server
      │                                       │
      ├─── Initial [CRYPTO ClientHello] ───▶  │  ← includes session ticket
      ├─── 0-RTT [STREAM "hello"] ─────────▶  │  ← data sent immediately
      │                                       │ ╮
      │◀── Initial [CRYPTO ServerHello] ──────┤ │ RTT 1
      │◀── Handshake [CRYPTO Fin] ────────────┤ ╯
      │◀── 1-RTT [STREAM "hello"] ────────────┤  ← echo received
      │                                       │
</pre>

To test locally against the native build:

```bash
# build everything (wolfSSL + nghttp3 + ngtcp2 + server + test client)
bash stress-test/native-baseline/build_native.sh

# start the echo server
./stress-test/native-baseline/build/quic_echo_server_native &

# run the session ticket test
./stress-test/native-baseline/build/test_session_ticket
```
<br clear="left"/>
The test makes two connections to `127.0.0.1:4433`:

1. **Connection 1** — full TLS 1.3 handshake, sends "hello from 0-RTT", receives echo, saves the session ticket and transport params
2. **Connection 2** — resumes with the saved ticket, sends early data in the 0-RTT packet (before the handshake completes), receives echo

Expected output:

```
=== Connection 1 (full handshake) ===
[QUIC] handshake completed
[QUIC] sent 'hello from 0-RTT' (16 bytes)
[TICKET] saved session (301 bytes)
[ECHO] received: 'hello from 0-RTT' (16 bytes)

=== Connection 2 (0-RTT resumption) ===
[TICKET] restored session, 0-RTT enabled
[0-RTT] restored transport params (40 bytes)
[QUIC] sent 'hello from 0-RTT' (16 bytes) [0-RTT]
[ECHO] received: 'hello from 0-RTT' (16 bytes)

=== Summary ===
connection 1 (full handshake): PASS
connection 2 (0-RTT resume):   PASS
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
