# Examples

These examples are intentionally isolated and minimal so you can lift them into your own IWA project.

## 1) IWA session ticket flow

Path: `examples/iwa-session-ticket/`

Shows a local IWA-friendly server/client flow where:

- connection 1 performs full handshake
- session ticket is stored
- connection 2 resumes with 0-RTT data

## 2) WebTransport over IWA

Path: `examples/webtransport-iwa/`

A complete WebTransport echo server and client using the ngtcp2 + nghttp3 + wolfSSL stack:

- **`server.c`** — Full QUIC/HTTP/3/WebTransport server that accepts Extended CONNECT sessions, echoes bidirectional stream data and datagrams back to the client
- **`client.html`** — Browser client using `new WebTransport()` with support for `serverCertificateHashes`, datagram send/receive, and bidirectional streams
- **`Makefile`** — Native and Emscripten/WASM build targets

```bash
cd examples/webtransport-iwa
make cert_data.h   # generate self-signed cert
make native        # build server
./wt_echo_server   # start on UDP :4433
# open client.html in Chrome, paste the cert hash, click Connect
```

## 3) Multicast media on LAN

Path: `examples/multicast-media-lan/`

Shows sender/receiver sockets using IPv4 multicast for local media fanout.
