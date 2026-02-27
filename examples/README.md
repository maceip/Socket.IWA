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

Shows an HTTP/3 Extended CONNECT + WebTransport setup that can be consumed by Chrome using:

```js
const wt = new WebTransport('https://127.0.0.1:4433/wt');
await wt.ready;
```

## 3) Multicast media on LAN

Path: `examples/multicast-media-lan/`

Shows sender/receiver sockets using IPv4 multicast for local media fanout.
