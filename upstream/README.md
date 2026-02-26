# Emscripten Direct Sockets Backend

**Linker flag: `-sDIRECT_SOCKETS`**

This patch adds a new Emscripten backend that replaces the WebSocket-to-POSIX-socket
proxy with the Direct Sockets API, enabling
real TCP/UDP networking from WebAssembly in Isolated Web Apps without a proxy server.

## Usage

```bash
emcc myapp.c -o myapp.js -sDIRECT_SOCKETS -sJSPI -sPROXY_TO_PTHREAD -pthread
```

## What it provides

Standard POSIX socket syscalls routed through Direct Sockets:

| Syscall | Direct Sockets API |
|---|---|
| `socket()` | Creates socket state (deferred construction) |
| `connect()` | `new TCPSocket()` / `new UDPSocket()` |
| `bind()` | `new UDPSocket({localAddress, localPort})` (UDP) or deferred (TCP) |
| `listen()` | `new TCPServerSocket()` |
| `accept()` | `await tcpServer.readable.read()` |
| `send()/sendto()` | `writer.write()` |
| `recv()/recvfrom()` | `reader.read()` |
| `sendmsg()/recvmsg()` | Scatter-gather over Direct Sockets streams |
| `setsockopt()` | Deferred options applied at construction time |
| `getsockopt()` | SO_ERROR, SO_TYPE |
| `getsockname()/getpeername()` | Address info from opened socket |
| `shutdown()/close()` | Stream cancel/close + socket close |

## Requirements

- **JSPI** (`-sJSPI`): Direct Sockets is promise-based; JSPI suspends the Wasm
  stack during async calls without blocking the browser.
- **PROXY_TO_PTHREAD** (`-sPROXY_TO_PTHREAD`): Runs `main()` on a worker thread
  so blocking socket calls don't freeze the UI.
- **Isolated Web App context**: Direct Sockets requires an IWA with
  `"direct-sockets": ["self"]` in the manifest.

## Files changed

| File | Change |
|---|---|
| `src/settings.js` | Add `DIRECT_SOCKETS = false` setting |
| `src/lib/libdirectsockets.js` | New JS library (all socket syscalls) |
| `src/lib/libsyscall.js` | Guard default impls with `&& DIRECT_SOCKETS == 0` |
| `src/lib/libwasi.js` | Handle `fd_close` for Direct Sockets fds |
| `src/modules.mjs` | Register `libdirectsockets.js` in `calculateLibraries()` |
| `system/lib/libc/emscripten_syscall_stubs.c` | Remove `__syscall_setsockopt` C stub |

## Design decisions

- **Deferred socket options**: Direct Sockets requires options at construction
  time, not after. `setsockopt()` stores options, applied at `connect()`/`bind()`/`listen()`.
- **FD namespace**: Starts at 100 to avoid conflicts with stdio/FS fds.
- **Read buffering**: Direct Sockets gives variable-length chunks, C `recv()` wants
  exact sizes. The library buffers excess data from oversized reads.
- **Fake DNS preserved**: Uses Emscripten's existing `$DNS` for hostname↔fake-IP mapping.
- **Numeric option constants**: Socket option values (SO_REUSEADDR, etc.) are not
  in Emscripten's `struct_info` — musl numeric literals are used directly.
