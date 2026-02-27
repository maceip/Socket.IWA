# Socket.IWA

**Simple to use, blazing fast.**

Socket.IWA brings low-level networking to JavaScript in Chrome Isolated Web Apps (IWA): UDP, TCP, server sockets, multicast, and QUIC-based application protocols.

If you already use `socket.io`, `ws`, or raw `WebSocket`, think of this as the same event-first ergonomics on top of real sockets.

## Supported protocols

Socket.IWA supports these transport and app-level protocols today:

- **UDP datagrams** (unicast + multicast)
- **TCP streams** (client and server)
- **QUIC**
- **HTTP/3** (including Extended CONNECT)
- **WebTransport** (datagrams + uni/bidi streams)
- **WebSocket over HTTP/3** (RFC 9220)
- **TLS 1.3 session tickets** for 0-RTT resume

## JavaScript API (user-facing)

The user-facing API is exported from `api/direct_sockets_api.js`.

### Core primitives

- `SocketIWA.createUDP(options)` → UDP-style connection object
- `SocketIWA.createTCP(host, port, options)` → TCP-style connection object
- `SocketIWA.createTCPServer(host, options)` → server object with `connection` events
- `SocketIWA.createMulticastSocket(options)` → multicast-friendly UDP socket
- `SocketIWA.permissions.*` and `SocketIWA.assertPermissions()` → policy checks

### Familiar event model

Connection and server objects support:

- lifecycle events: `open`, `message`, `error`, `close`, `connect`, `disconnect`
- event-bus methods: `on`, `off`, `once`, `emit`
- request/response style events: `emitWithAck(event, payload, { timeoutMs })`

### UDP + multicast ergonomics

UDP connections support dgram-like and multicast helpers:

- `send(data)`
- `send(data, port, address)`
- `send(data, offset, length, port, address)`
- `addMembership`, `dropMembership`
- `addSourceSpecificMembership`, `dropSourceSpecificMembership`
- `setMulticastTTL`, `setMulticastLoopback`

## Quick example

```js
import SocketIWA from './api/direct_sockets_api.js';

const udp = await SocketIWA.createMulticastSocket({
  localAddress: '0.0.0.0',
  localPort: 5000,
  multicastTimeToLive: 8,
});

udp.on('message', (data, rinfo) => {
  console.log('datagram', rinfo.address, rinfo.port, data);
});

await udp.addMembership('239.10.10.10');
await udp.send('hello lan', 5000, '239.10.10.10');
```

## Examples

See `examples/` for end-to-end demos:

- `examples/iwa-session-ticket/` — isolated web app server + client using session tickets
- `examples/webtransport-iwa/` — IWA WebTransport server and browser client via `new WebTransport()`
- `examples/multicast-media-lan/` — LAN multicast media sender/receiver

## Build and test

```bash
bash docker_build_quic.sh
bash stress-test/native-baseline/build_native.sh
```

CI runs both WASM and native builds, then executes the native session ticket test.

## Session Tickets

**TL;DR** — Without session tickets, a returning client repeats the full 2-RTT handshake. With session tickets, a returning client can send app data in 0-RTT.

```text
Connection 1 — full handshake                        2 RTT ≈ 100ms @ 50ms ping

    Client                                     Server
      |                                          |
      |--- Initial [CRYPTO ClientHello] -------> |
      |                                          |  RTT 1
      |<-- Initial [CRYPTO ServerHello] ---------|
      |<-- Handshake [CRYPTO Cert+Fin] ----------|
      |<-- 1-RTT [NEW_CONNECTION_ID] ------------|
      |                                          |
      |--- Handshake [CRYPTO Finished] --------> |
      |--- 1-RTT [STREAM "hello"] -------------> |  RTT 2
      |                                          |
      |<-- 1-RTT [HANDSHAKE_DONE] ---------------|
      |<-- 1-RTT [CRYPTO NewSessionTicket] ------|  <- ticket saved
      |<-- 1-RTT [STREAM "hello"] --------------|  <- echo received
      |                                          |

Connection 2 — 0-RTT resumption                    0 RTT ≈ 0ms (data in flight)

    Client                                     Server
      |                                          |
      |--- Initial [CRYPTO ClientHello] -------> |  <- includes session ticket
      |--- 0-RTT [STREAM "hello"] ------------> |  <- data sent immediately
      |                                          |  RTT 1
      |<-- Initial [CRYPTO ServerHello] ---------|
      |<-- Handshake [CRYPTO Fin] ---------------|
      |<-- 1-RTT [STREAM "hello"] --------------|  <- echo received
      |                                          |
```

Local validation:

```bash
bash stress-test/native-baseline/build_native.sh
./stress-test/native-baseline/build/quic_echo_server_native &
./stress-test/native-baseline/build/test_session_ticket
```

## Architecture and internals

For wolfSSL, Emscripten, syscall bridging, and QUIC stack internals, see:

- `docs/ARCHITECTURE.md`

## License

Apache-2.0
