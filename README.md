# Socket.IWA

Real TCP/UDP networking for WebAssembly and JavaScript in Chrome [Isolated Web Apps](https://chromestatus.com/feature/5146307550248960).

Socket.IWA replaces Emscripten's default socket layer with the [Direct Sockets API](https://wicg.github.io/direct-sockets/), giving C/C++ programs compiled to WASM real POSIX socket semantics -- no proxy server, no WebSocket tunneling.

**Tor bootstraps 100% in ~15 seconds running as unmodified upstream C, compiled to WASM, in a browser.**

## How it works

```
C/C++ code (Tor, OpenSSL, curl, etc.)
  -> libc socket syscalls (write, read, send, recv, connect, bind, ...)
  -> Emscripten FS layer (fd_write / fd_read)
  -> libdirectsockets.js stream_ops (SOCKFS pattern)
  -> Direct Sockets API (TCPSocket / UDPSocket)
  -> real TCP/UDP on the network
```

The key insight: socket file descriptors are registered in Emscripten's virtual filesystem using `FS.createNode()` + `FS.createStream()` with custom `stream_ops`. This is the same pattern Emscripten's own SOCKFS uses for WebSocket-backed sockets. It means `write(fd)` and `read(fd)` work on socket fds -- critical for OpenSSL and other libraries that use `write()`/`read()` instead of `send()`/`recv()`.

### Why stream_ops must be synchronous

Emscripten's FS layer calls `stream_ops.write()` and `stream_ops.read()` from JavaScript-to-JavaScript (not WASM-to-JS), so JSPI cannot suspend execution. The solution:

- **write**: fire-and-forget via `writer.write()`, return byte count immediately
- **read**: consume from `recvQueue` (filled by a background reader), throw `EAGAIN` if empty
- **poll**: check `recvQueue.length` for readability

OpenSSL handles `EAGAIN` naturally -- `SSL_connect()` returns `SSL_ERROR_WANT_READ`, Tor's event loop waits for readability, retries when data arrives.

### SharedArrayBuffer

With `-sPROXY_TO_PTHREAD`, Emscripten uses `SharedArrayBuffer` for the WASM heap. Direct Sockets streams reject `SharedArrayBuffer` views, so `stream_ops.write` copies data to a regular `ArrayBuffer` before writing.

## Emscripten integration

### Build flags

```
emcc ... -sDIRECT_SOCKETS -sJSPI -sPROXY_TO_PTHREAD -sFORCE_FILESYSTEM=1
```

### What gets patched

The `docker_build.sh` script patches Emscripten to add the `-sDIRECT_SOCKETS` setting. A PR to upstream this is in progress. The patches:

| File | Change |
|------|--------|
| `src/settings.js` | Add `DIRECT_SOCKETS` setting |
| `src/modules.mjs` | Conditionally load `libdirectsockets.js` |
| `src/lib/libsyscall.js` | Guard default socket syscalls when `DIRECT_SOCKETS` is set |
| `src/lib/libwasi.js` | `fd_close` for pipe fds, `fd_read`/`fd_write` for pipe fds |
| `src/lib/libdirectsockets.js` | **The implementation** |
| `emscripten_syscall_stubs.c` | Remove stubs that conflict with JS implementations |

### Syscalls implemented

| Syscall | Notes |
|---------|-------|
| `socket` | AF_INET/AF_INET6, SOCK_STREAM/SOCK_DGRAM |
| `connect` | TCP via `new TCPSocket()`, UDP via `new UDPSocket()` |
| `bind` | TCP server via `new TCPServerSocket()`, UDP via `new UDPSocket()` |
| `listen` / `accept4` | TCP server accept loop |
| `sendto` / `recvfrom` | Async via JSPI |
| `sendmsg` / `recvmsg` | With sockaddr support |
| `getsockname` / `getpeername` | Local/remote address |
| `setsockopt` / `getsockopt` | TCP_NODELAY, SO_KEEPALIVE, SO_RCVBUF, SO_SNDBUF, multicast |
| `shutdown` | Half-close support |
| `poll` / `pselect6` | Via `recvQueue` length checks |
| `pipe2` / `socketpair` | In-memory pipe buffers |
| `fcntl64` | O_NONBLOCK, F_GETFL/F_SETFL |
| `ioctl` | FIONBIO (non-blocking mode) |
| `write` / `read` | Via FS `stream_ops` (the SOCKFS pattern) |

### DNS

DNS resolution uses DNS-over-HTTPS (DoH) via `fetch()` to Cloudflare's resolver, with an in-memory cache. This works because IWAs can `fetch()` external HTTPS endpoints.

## JavaScript API

Socket.IWA also provides a high-level JavaScript API for IWA apps that don't need WASM:

```js
import SocketIWA from './api/direct_sockets_api.js';

// TCP client
const tcp = await SocketIWA.createTCP('example.com', 80);
tcp.on('message', (data) => console.log('received', data));
tcp.send('GET / HTTP/1.0\r\n\r\n');

// UDP multicast
const udp = await SocketIWA.createMulticastSocket({
  localAddress: '0.0.0.0',
  localPort: 5000,
});
await udp.addMembership('239.10.10.10');
udp.on('message', (data, rinfo) => console.log(rinfo.address, data));
```

## Example: Tor in a browser

See [maceip/tor.iwa](https://github.com/maceip/tor.iwa) for the full build pipeline that compiles upstream Tor + OpenSSL + libevent to WASM and runs it in an IWA. Zero modifications to Tor source code.

```bash
# Build
cd tor-iwa && bash build.sh

# Bundle into signed .swbn
node bundle.mjs

# Test with Puppeteer
node test-iwa.mjs --timeout 90
# -> Bootstrapped 100% (done): Done
```

## Build

```bash
# Build the QUIC stack (ngtcp2 + nghttp3 + wolfSSL) for WASM
bash docker_build_quic.sh

# Build native baseline for comparison
bash stress-test/native-baseline/build_native.sh
```

## Project structure

```
emscripten/src/lib/
  libdirectsockets.js    # Direct Sockets -> POSIX syscall bridge

docker_build.sh          # Emscripten patch pipeline for -sDIRECT_SOCKETS

api/
  direct_sockets_api.js  # High-level JS API

examples/
  iwa-session-ticket/    # QUIC server + client with session tickets
  webtransport-iwa/      # WebTransport server and browser client
  multicast-media-lan/   # LAN multicast media sender/receiver
```

## License

Apache-2.0
