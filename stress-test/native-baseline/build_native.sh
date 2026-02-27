#!/bin/bash
# Build the QUIC echo server as a native Linux binary for baseline comparison.
# Uses the same wolfSSL + nghttp3 + ngtcp2 source trees as the Emscripten build.
set -e

SRCDIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILDDIR="$SRCDIR/stress-test/native-baseline/build"
DEPS="$BUILDDIR/deps"

mkdir -p "$BUILDDIR" "$DEPS"

echo "=== Building wolfSSL (native) ==="
mkdir -p "$BUILDDIR/wolfssl"
cd "$BUILDDIR/wolfssl"
cmake "$SRCDIR/wolfssl" \
    -DWOLFSSL_QUIC=yes \
    -DWOLFSSL_SESSION_TICKET=yes \
    -DWOLFSSL_CERTGEN=yes \
    -DWOLFSSL_KEYGEN=yes \
    -DWOLFSSL_ASM=yes \
    -DWOLFSSL_CRYPT_TESTS=no \
    -DWOLFSSL_EXAMPLES=no \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS" \
    -DCMAKE_C_FLAGS="-DWOLFSSL_EARLY_DATA" \
    2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -1
make install 2>&1 | tail -1
echo "wolfSSL installed"

echo "=== Building nghttp3 (native) ==="
mkdir -p "$BUILDDIR/nghttp3"
cd "$BUILDDIR/nghttp3"
cmake "$SRCDIR/nghttp3" \
    -DENABLE_LIB_ONLY=ON \
    -DENABLE_STATIC_LIB=ON \
    -DENABLE_SHARED_LIB=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS" \
    2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -1
make install 2>&1 | tail -1
echo "nghttp3 installed"

echo "=== Building ngtcp2 (native, with examples) ==="
mkdir -p "$BUILDDIR/ngtcp2"
cd "$BUILDDIR/ngtcp2"
cmake "$SRCDIR/ngtcp2" \
    -DENABLE_WOLFSSL=ON \
    -DENABLE_OPENSSL=OFF \
    -DENABLE_GNUTLS=OFF \
    -DENABLE_PICOTLS=OFF \
    -DENABLE_LIB_ONLY=OFF \
    -DENABLE_STATIC_LIB=ON \
    -DENABLE_SHARED_LIB=OFF \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$DEPS" \
    -DCMAKE_PREFIX_PATH="$DEPS" \
    -DCMAKE_FIND_ROOT_PATH="$DEPS" \
    2>&1 | tail -3
make -j$(nproc) 2>&1 | tail -1
make install 2>&1 | tail -1
echo "ngtcp2 installed"

echo "=== Generating cert ==="
bash "$SRCDIR/gen_cert.sh" "$SRCDIR"

echo "=== Compiling quic_echo_server (native) ==="
cc -O2 -o "$BUILDDIR/quic_echo_server_native" "$SRCDIR/quic_echo_server.c" \
    -I"$DEPS/include" \
    -L"$DEPS/lib" \
    -lngtcp2 -lngtcp2_crypto_wolfssl -lnghttp3 -lwolfssl \
    -DNGTCP2_STATICLIB -DNGHTTP3_STATICLIB -DWOLFSSL_EARLY_DATA \
    -lpthread -lm 2>&1

echo "=== Compiling test_session_ticket (native) ==="
cc -O2 -o "$BUILDDIR/test_session_ticket" "$SRCDIR/test_session_ticket.c" \
    -I"$DEPS/include" \
    -L"$DEPS/lib" \
    -lngtcp2 -lngtcp2_crypto_wolfssl -lnghttp3 -lwolfssl \
    -DNGTCP2_STATICLIB -DNGHTTP3_STATICLIB -DWOLFSSL_EARLY_DATA \
    -lpthread -lm 2>&1

echo ""
echo "=== Native build complete ==="
ls -la "$BUILDDIR/quic_echo_server_native" "$BUILDDIR/test_session_ticket"
echo "Run: $BUILDDIR/quic_echo_server_native"
echo "Test: $BUILDDIR/test_session_ticket"
