# WebTransport IWA Example

This example shows both sides:

- `server.c` — minimal route handler sketch for `/wt` over HTTP/3 Extended CONNECT
- `client.html` — Chrome client using `new WebTransport()`

## Notes

- Use a certificate trusted by your local Chrome profile.
- This is intended for IWA packaging where Direct Sockets permissions are granted.
