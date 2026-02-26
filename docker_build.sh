#!/bin/bash
set -e

EMDIR=/emsdk/upstream/emscripten

# 1. Add DIRECT_SOCKETS setting to settings.js (only if not already present)
if ! grep -q 'var DIRECT_SOCKETS' "$EMDIR/src/settings.js"; then
  sed -i '/^var PROXY_POSIX_SOCKETS = false;/a\
var DIRECT_SOCKETS = false;' "$EMDIR/src/settings.js"
  echo "Patched settings.js"
fi

# 2. Guard default socket syscalls in libsyscall.js
if ! grep -q 'DIRECT_SOCKETS' "$EMDIR/src/lib/libsyscall.js"; then
  sed -i 's/^#if PROXY_POSIX_SOCKETS == 0 && WASMFS == 0$/#if PROXY_POSIX_SOCKETS == 0 \&\& WASMFS == 0 \&\& DIRECT_SOCKETS == 0/' \
    "$EMDIR/src/lib/libsyscall.js"
  echo "Patched libsyscall.js guard"
fi

# 3. Patch fd_close in libwasi.js - add DIRECT_SOCKETS handler after PROXY_POSIX_SOCKETS
if ! grep -q 'DIRECT_SOCKETS' "$EMDIR/src/lib/libwasi.js"; then
python3 -c "
path = '$EMDIR/src/lib/libwasi.js'
with open(path) as f:
    content = f.read()

old_block = '''#elif PROXY_POSIX_SOCKETS
    // close() is a tricky function because it can be used to close both regular file descriptors
    // and POSIX network socket handles, hence an implementation would need to track for each
    // file descriptor which kind of item it is. To simplify, when using PROXY_POSIX_SOCKETS
    // option, use shutdown() to close a socket, and this function should behave like a no-op.
    warnOnce('To close sockets with PROXY_POSIX_SOCKETS bridge, prefer to use the function shutdown() that is proxied, instead of close()')
    return 0;'''

new_block = old_block + '''
#elif DIRECT_SOCKETS
    var sock = DIRECT_SOCKETS.getSocket(fd);
    if (sock) {
      DIRECT_SOCKETS._closeSocket(sock);
      delete DIRECT_SOCKETS.sockets[fd];
      return 0;
    }
    return 0;'''

content = content.replace(old_block, new_block)
with open(path, 'w') as f:
    f.write(content)
print('Patched libwasi.js')
"
fi

# 4. Copy our new library file
cp /src/emscripten/src/lib/libdirectsockets.js "$EMDIR/src/lib/libdirectsockets.js"
echo "Copied libdirectsockets.js"

# 5. Register libdirectsockets.js in modules.mjs so the JS compiler loads it
if ! grep -q 'libdirectsockets' "$EMDIR/src/modules.mjs"; then
python3 -c "
path = '$EMDIR/src/modules.mjs'
with open(path) as f:
    content = f.read()

# Add after the libsyscall.js conditional block
old = '''  if (!WASMFS) {
    libraries.push('libsyscall.js');
  }'''

new = '''  if (!WASMFS) {
    libraries.push('libsyscall.js');
  }

  if (DIRECT_SOCKETS) {
    libraries.push('libdirectsockets.js');
  }'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Patched modules.mjs')
"
fi

# 6. Patch syscall stubs to remove setsockopt C stub (conflicts with our JS impl)
if grep -q '__syscall_setsockopt' "$EMDIR/system/lib/libc/emscripten_syscall_stubs.c" 2>/dev/null; then
python3 -c "
path = '/emsdk/upstream/emscripten/system/lib/libc/emscripten_syscall_stubs.c'
with open(path) as f:
    content = f.read()

# Remove the weak C stub for __syscall_setsockopt so our JS version wins
old = '''weak int __syscall_setsockopt(int sockfd, int level, int optname, intptr_t optval, size_t optlen, int dummy) {
  REPORT(setsockopt);
  return -ENOPROTOOPT; // The option is unknown at the level indicated.
}'''

new = '''// Removed: __syscall_setsockopt - provided by libdirectsockets.js when DIRECT_SOCKETS is enabled'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Patched emscripten_syscall_stubs.c')
"
fi

# 7. Clear emscripten cache to pick up JS changes
rm -rf "$EMDIR/cache"
echo "Cleared cache"

echo ""
echo "=== Compiling test_direct_sockets.cpp ==="
echo ""

em++ /src/test_direct_sockets.cpp -o /build/test.js \
  -sDIRECT_SOCKETS \
  -sJSPI \
  -sPROXY_TO_PTHREAD \
  -pthread \
  -sEXIT_RUNTIME=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  2>&1

echo ""
echo "=== Build complete ==="
ls -la /build/test.*
