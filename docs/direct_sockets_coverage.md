# Direct Sockets coverage and library investigation

## 1) Current C-library stack sufficiency for expanded Direct Sockets scope

### Current stack
- Transport / QUIC: `ngtcp2`
- HTTP/3: `nghttp3`
- TLS: `wolfSSL`
- Syscall bridge: `emscripten/src/lib/libdirectsockets.js`

### Sufficiency assessment
- **UDP / TCP / TCPServerSocket**: Supported by the bridge; maps POSIX socket syscalls to `UDPSocket`, `TCPSocket`, and `TCPServerSocket`.
- **MulticastController surface**: The bridge now includes multicast membership control via `setsockopt` mappings to `joinGroup()` / `leaveGroup()` with state tracking of joined groups.
- **Permissions-policy integrations**: Primarily browser-enforced. The bridge converts common permission failures (`NotAllowedError`) into POSIX-like errno values.

Conclusion: for Direct Sockets browser plumbing, **the existing C libraries are sufficient**. The key gap is JS bridge behavior, not QUIC/TLS C dependencies.

## 2) GitHub ecosystem scan (well-supported alternatives)

The following repos were checked via the GitHub API for recent activity and maintenance signals:

| Library | Repo | Stars | Updated (UTC) | Notes |
|---|---|---:|---|---|
| ngtcp2 | ngtcp2/ngtcp2 | 1424 | 2026-02-27 | Current choice, active |
| nghttp3 | ngtcp2/nghttp3 | 1058 | 2026-02-22 | Current choice, active |
| wolfSSL | wolfSSL/wolfssl | 2740 | 2026-02-27 | Current choice, active |
| MsQuic | microsoft/msquic | 4636 | 2026-02-27 | High-performance QUIC in C; stronger ecosystem, but larger integration shift |
| quiche | cloudflare/quiche | 11277 | 2026-02-27 | Very mature QUIC stack; Rust-centric build complexity |
| picoquic | private-octopus/picoquic | 703 | 2026-02-27 | Lightweight research-friendly option |
| lsquic | litespeedtech/lsquic | 1792 | 2026-02-27 | Mature C implementation |

Recommendation: keep `ngtcp2 + nghttp3 + wolfSSL` for now; evaluate MsQuic and lsquic only if you need materially different perf/operational behavior.

## 3) Spec coverage map for requested areas

### MulticastController
- `MulticastController interface`: supported through `UDPSocket` open info attachment and socket state controller caching.
- `MulticastGroupOptions`: parsed from IP-level membership structs.
- `MulticastMembership`: mapped via IPv4/IPv6 membership parsers.
- `joinGroup()`: wired to `setsockopt(IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP)`.
- `leaveGroup()`: wired to `setsockopt(IP_DROP_MEMBERSHIP / IPV6_LEAVE_GROUP)`.
- `joinedGroups`: tracked from browser controller when present.

### TCPServerSocket
- `constructor()`: already used in `listen` path.
- `TCPServerSocketOptions`: local port and backlog supported.
- `[[readable]]`: consumed through accept reader.
- `opened`: used to retrieve streams and local bind details.
- `closed` and `close()`: mapped in shutdown/close helper.

### Integrations
- `Permissions Policy`: browser-enforced; wrapped into errno conversions.
- `Permissions Policy (Private Network Access)`: browser-enforced on open/connect.
- `Permissions Policy (Multicast)`: multicast controller availability and option use depend on browser policy.

## 4) JavaScript-facing API surface for users building on Socket.IWA

A new module `api/direct_sockets_api.js` provides a focused public interface:
- `SocketIWA.createUDP(...)`
- `SocketIWA.createTCP(...)`
- `SocketIWA.createTCPServer(...)`
- `SocketIWA.permissions.*` for policy checks
- `SocketIWA.createMulticastController(...)` for join/leave/list joined groups

The intent is to expose just the useful primitives for dynamic runtime management while preserving underlying Direct Sockets behavior.


## 5) User-facing API redesign notes

The public wrapper now follows a thin WebSocket + Socket.IO hybrid:
- WebSocket-style primitives: `send(data)`, `close(code, reason)`, `readyState`, `binaryType`, and lifecycle events (`open`, `message`, `error`, `close`).
- Socket.IO-style named events: `on(event, fn)`, `off(event, fn)`, `once(event, fn)`, `emit(event, ...args)`.
- dgram-style multicast helpers: `addMembership(group, ifaceOrSource)`, `addSourceSpecificMembership(source, group)`, `setMulticastTTL(ttl)`, `setMulticastLoopback(bool)`, and `send(data, port, address)`.
- Direct-Sockets style multicast sub-controller: `socket.multicast.join(...)`, `socket.multicast.leave(...)`, `socket.multicast.joinSourceSpecific(...)`.
- Modern request/response over sockets: `emitWithAck(event, payload, { timeoutMs })`.
- Server-side room helper model: `socket.join(room)`, `socket.leave(room)`, and `server.to(room).emit(...)`.

This keeps the low-level transport readable for WebSocket users while adding structured event ergonomics expected by modern realtime developers.

- Factory helper for multicast-first UDP sockets: `SocketIWA.createMulticastSocket({ allowAddressSharing, reuseAddr, type, ... })`.
