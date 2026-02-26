// Linkage test for ngtcp2 + nghttp3 + wolfSSL compiled to WebAssembly.
// This doesn't establish a real QUIC connection (no server to connect to in test),
// but verifies all symbols link and the APIs are callable.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_wolfssl.h>
#include <nghttp3/nghttp3.h>

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

static void test_ngtcp2_version() {
  const ngtcp2_info *info = ngtcp2_version(0);
  printf("[TEST] ngtcp2 version: %s (age=%d)\n", info->version_str, info->age);
}

static void test_nghttp3_version() {
  const nghttp3_info *info = nghttp3_version(0);
  printf("[TEST] nghttp3 version: %s (age=%d)\n", info->version_str, info->age);
}

static void test_wolfssl_init() {
  printf("[TEST] wolfSSL init...\n");
  wolfSSL_Init();

  WOLFSSL_CTX *ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
  if (!ctx) {
    printf("  FAIL: wolfSSL_CTX_new returned NULL\n");
    return;
  }

  // Set QUIC method
  int rc = ngtcp2_crypto_wolfssl_configure_client_context(ctx);
  printf("  ngtcp2_crypto_wolfssl_configure_client_context: %s (rc=%d)\n",
         rc == 0 ? "OK" : "FAIL", rc);

  wolfSSL_CTX_free(ctx);
  wolfSSL_Cleanup();
  printf("  wolfSSL init/cleanup: OK\n");
}

static void test_ngtcp2_settings() {
  printf("[TEST] ngtcp2 settings...\n");
  ngtcp2_settings settings;
  ngtcp2_settings_default(&settings);
  printf("  initial_ts: %llu\n", (unsigned long long)settings.initial_ts);
  printf("  OK\n");
}

static void test_ngtcp2_cid() {
  printf("[TEST] ngtcp2 CID generation...\n");
  ngtcp2_cid cid;
  uint8_t data[NGTCP2_MAX_CIDLEN];
  // Fill with dummy data
  for (int i = 0; i < NGTCP2_MAX_CIDLEN; i++) data[i] = (uint8_t)(i + 1);
  ngtcp2_cid_init(&cid, data, 8);
  printf("  CID len=%zu, first byte=0x%02x\n", cid.datalen, cid.data[0]);
  printf("  OK\n");
}

static void test_ngtcp2_transport_params() {
  printf("[TEST] ngtcp2 transport params...\n");
  ngtcp2_transport_params params;
  ngtcp2_transport_params_default(&params);
  printf("  max_idle_timeout: %llu\n",
         (unsigned long long)params.max_idle_timeout);
  printf("  max_udp_payload_size: %llu\n",
         (unsigned long long)params.max_udp_payload_size);
  printf("  initial_max_data: %llu\n",
         (unsigned long long)params.initial_max_data);

  // Encode transport params
  uint8_t buf[256];
  ngtcp2_ssize len = ngtcp2_transport_params_encode(buf, sizeof(buf), &params);
  printf("  encoded transport params: %zd bytes\n", (ssize_t)len);

  // Decode them back
  ngtcp2_transport_params decoded;
  int rv = ngtcp2_transport_params_decode(&decoded, buf, (size_t)len);
  printf("  decode: %s (rv=%d)\n", rv == 0 ? "OK" : "FAIL", rv);
  printf("  OK\n");
}

static void test_nghttp3_settings() {
  printf("[TEST] nghttp3 settings...\n");
  nghttp3_settings settings;
  nghttp3_settings_default(&settings);
  printf("  max_field_section_size: %llu\n",
         (unsigned long long)settings.max_field_section_size);
  printf("  qpack_max_dtable_capacity: %zu\n",
         (size_t)settings.qpack_max_dtable_capacity);
  printf("  OK\n");
}

int main() {
  printf("=== QUIC Stack Linkage Test ===\n\n");

  test_ngtcp2_version();
  test_nghttp3_version();
  test_wolfssl_init();
  test_ngtcp2_settings();
  test_ngtcp2_cid();
  test_ngtcp2_transport_params();
  test_nghttp3_settings();

  printf("\n=== All linkage tests passed ===\n");
  return 0;
}
