#!/bin/bash
set -e

EMDIR=/emsdk/upstream/emscripten
INSTALL_PREFIX=/build/deps

# ============================================================
# Phase 1: Patch Emscripten for Direct Sockets (same as before)
# ============================================================

# 1. Add DIRECT_SOCKETS setting to settings.js
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

# 3. Patch fd_close in libwasi.js
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

# 5. Register libdirectsockets.js in modules.mjs
if ! grep -q 'libdirectsockets' "$EMDIR/src/modules.mjs"; then
python3 -c "
path = '$EMDIR/src/modules.mjs'
with open(path) as f:
    content = f.read()

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

# 6. Patch syscall stubs to remove setsockopt C stub
if grep -q 'weak int __syscall_setsockopt' "$EMDIR/system/lib/libc/emscripten_syscall_stubs.c" 2>/dev/null; then
python3 -c "
path = '$EMDIR/system/lib/libc/emscripten_syscall_stubs.c'
with open(path) as f:
    content = f.read()

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

# 7. Clear emscripten cache
rm -rf "$EMDIR/cache"
echo "Cleared cache"

# ============================================================
# Phase 2: Build wolfSSL
# ============================================================
echo ""
echo "=== Building wolfSSL ==="
echo ""

mkdir -p /build/wolfssl
cd /build/wolfssl
emcmake cmake /src/wolfssl \
  -DWOLFSSL_QUIC=yes \
  -DWOLFSSL_SESSION_TICKET=yes \
  -DWOLFSSL_CERTGEN=yes \
  -DWOLFSSL_KEYGEN=yes \
  -DWOLFSSL_ASM=no \
  -DWOLFSSL_CRYPT_TESTS=no \
  -DWOLFSSL_EXAMPLES=no \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  -DCMAKE_C_FLAGS="-pthread -DWOLFSSL_EARLY_DATA" \
  2>&1
emmake make -j$(nproc) 2>&1
emmake make install 2>&1
echo "wolfSSL built and installed"

# ============================================================
# Phase 3: Build nghttp3
# ============================================================
echo ""
echo "=== Building nghttp3 ==="
echo ""

# Patch nghttp3 to add SETTINGS_WT_MAX_SESSIONS for WebTransport
if ! grep -q 'WT_MAX_SESSIONS' /src/nghttp3/lib/nghttp3_stream.c; then
python3 -c "
path = '/src/nghttp3/lib/nghttp3_stream.c'
with open(path) as f:
    content = f.read()

old = '''  if (local_settings->enable_connect_protocol) {
    ents[fr.niv] = (nghttp3_settings_entry){
      .id = NGHTTP3_SETTINGS_ID_ENABLE_CONNECT_PROTOCOL,
      .value = 1,
    };

    ++fr.niv;
  }'''

new = old + '''

  /* WebTransport: SETTINGS_WT_MAX_SESSIONS (draft-ietf-webtrans-http3) */
  if (local_settings->enable_connect_protocol && local_settings->h3_datagram) {
    ents[fr.niv] = (nghttp3_settings_entry){
      .id = 0x14e9cd29,  /* SETTINGS_WT_MAX_SESSIONS */
      .value = 100,
    };

    ++fr.niv;
  }'''

content = content.replace(old, new)
with open(path, 'w') as f:
    f.write(content)
print('Patched nghttp3_stream.c with SETTINGS_WT_MAX_SESSIONS')
"
fi

mkdir -p /build/nghttp3
cd /build/nghttp3
emcmake cmake /src/nghttp3 \
  -DENABLE_LIB_ONLY=ON \
  -DENABLE_STATIC_LIB=ON \
  -DENABLE_SHARED_LIB=OFF \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  -DCMAKE_C_FLAGS="-pthread" \
  2>&1
emmake make -j$(nproc) 2>&1
emmake make install 2>&1
echo "nghttp3 built and installed"

# ============================================================
# Phase 4: Build ngtcp2
# ============================================================
echo ""
echo "=== Building ngtcp2 ==="
echo ""

mkdir -p /build/ngtcp2
cd /build/ngtcp2
emcmake cmake /src/ngtcp2 \
  -DENABLE_WOLFSSL=ON \
  -DENABLE_OPENSSL=OFF \
  -DENABLE_GNUTLS=OFF \
  -DENABLE_PICOTLS=OFF \
  -DENABLE_LIB_ONLY=ON \
  -DENABLE_STATIC_LIB=ON \
  -DENABLE_SHARED_LIB=OFF \
  -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  -DCMAKE_PREFIX_PATH="$INSTALL_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$INSTALL_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
  -DCMAKE_C_FLAGS="-pthread" \
  2>&1
emmake make -j$(nproc) 2>&1
emmake make install 2>&1
echo "ngtcp2 built and installed"

# ============================================================
# Phase 5: Compile test program
# ============================================================
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
  -I"$INSTALL_PREFIX/include" \
  -L"$INSTALL_PREFIX/lib" \
  2>&1

echo ""
echo "=== Compiling test_quic_link.cpp ==="
echo ""

em++ /src/test_quic_link.cpp -o /build/test_quic.js \
  -sDIRECT_SOCKETS \
  -sJSPI \
  -sPROXY_TO_PTHREAD \
  -pthread \
  -sEXIT_RUNTIME=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -I"$INSTALL_PREFIX/include" \
  -L"$INSTALL_PREFIX/lib" \
  -lngtcp2 \
  -lngtcp2_crypto_wolfssl \
  -lnghttp3 \
  -lwolfssl \
  -DNGTCP2_STATICLIB \
  -DNGHTTP3_STATICLIB \
  2>&1

echo ""
echo "=== Generating TLS certificate ==="
echo ""

bash /src/gen_cert.sh /src
cat /src/cert_data.h | head -5

echo ""
echo "=== Compiling quic_echo_server.c ==="
echo ""

emcc /src/quic_echo_server.c -o /build/quic_echo_server.js \
  -sDIRECT_SOCKETS \
  -sJSPI \
  -sPROXY_TO_PTHREAD \
  -pthread \
  -sEXIT_RUNTIME=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sSTACK_SIZE=1048576 \
  -sINITIAL_MEMORY=33554432 \
  -sDEFAULT_PTHREAD_STACK_SIZE=1048576 \
  -I"$INSTALL_PREFIX/include" \
  -L"$INSTALL_PREFIX/lib" \
  -lngtcp2 \
  -lngtcp2_crypto_wolfssl \
  -lnghttp3 \
  -lwolfssl \
  -DNGTCP2_STATICLIB \
  -DNGHTTP3_STATICLIB \
  2>&1

echo ""
echo "=== Build complete ==="
ls -la /build/test.* /build/test_quic.* /build/quic_echo_server.* 2>/dev/null
echo ""
echo "=== Installed libraries ==="
ls -la "$INSTALL_PREFIX/lib/"*.a 2>/dev/null || echo "No .a files"
