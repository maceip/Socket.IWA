// Pseudocode-style integration point for quic_echo_server.c
// Demonstrates the shape of a WebTransport endpoint.

#include <stdio.h>

int handle_webtransport_session(const char *path) {
  if (!path) return -1;
  if (path[0] == '/' && path[1] == 'w' && path[2] == 't') {
    // Accept Extended CONNECT and start WebTransport session.
    // In full integration this hooks into nghttp3 callbacks.
    printf("accepted webtransport session at %s\n", path);
    return 0;
  }
  return -1;
}
