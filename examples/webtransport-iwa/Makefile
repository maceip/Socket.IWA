# WebTransport Echo Server — build targets
#
# Prerequisites (native):
#   ngtcp2, nghttp3, wolfSSL installed (with QUIC + TLS 1.3 support)
#
# Prerequisites (WASM):
#   Emscripten SDK + the full build pipeline from ../../docker_build_quic.sh

INSTALL_PREFIX ?= /build/deps

# ── Native build (for local testing outside of IWA) ──────────────

native: cert_data.h
	gcc -O2 -o wt_echo_server server.c \
		-lngtcp2 -lngtcp2_crypto_wolfssl -lnghttp3 -lwolfssl -lm \
		-DNGTCP2_STATICLIB -DNGHTTP3_STATICLIB
	@echo "Built: ./wt_echo_server (native)"
	@echo "Run:   ./wt_echo_server"
	@echo "Then open client.html in Chrome"

# ── WASM build (for IWA deployment via Emscripten) ───────────────

wasm: cert_data.h
	emcc -O2 server.c -o wt_echo_server.js \
		-sDIRECT_SOCKETS \
		-sJSPI \
		-sPROXY_TO_PTHREAD \
		-pthread \
		-sEXIT_RUNTIME=1 \
		-sALLOW_MEMORY_GROWTH=1 \
		-sSTACK_SIZE=1048576 \
		-sINITIAL_MEMORY=33554432 \
		-sDEFAULT_PTHREAD_STACK_SIZE=1048576 \
		-I"$(INSTALL_PREFIX)/include" \
		-L"$(INSTALL_PREFIX)/lib" \
		-lngtcp2 \
		-lngtcp2_crypto_wolfssl \
		-lnghttp3 \
		-lwolfssl \
		-DNGTCP2_STATICLIB \
		-DNGHTTP3_STATICLIB
	@echo "Built: wt_echo_server.js + wt_echo_server.wasm"

# ── Generate self-signed cert (14-day validity, ECDSA P-256) ─────

cert_data.h:
	bash ../../gen_cert.sh .
	@echo ""
	@echo "Copy the base64 cert hash above into client.html"

# ── Clean ─────────────────────────────────────────────────────────

clean:
	rm -f wt_echo_server wt_echo_server.js wt_echo_server.wasm \
	      wt_echo_server.worker.js cert_data.h \
	      server.key server.crt server.crt.der server.key.der

.PHONY: native wasm clean
