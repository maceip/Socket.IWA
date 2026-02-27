# IWA Session Ticket Example

## Files

- `server.js` — starts a QUIC/WebTransport-capable endpoint (native binary wrapper for local testing)
- `client.js` — performs two connections and verifies ticket-based 0-RTT resume

## Run

```bash
node server.js
node client.js
```

Expected behavior:

1. First connect does full handshake.
2. Ticket is saved.
3. Second connect sends early data in 0-RTT.
