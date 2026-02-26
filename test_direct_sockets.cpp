// Test program for the Direct Sockets Emscripten backend.
// Build with: emcc test_direct_sockets.cpp -o test.html -sDIRECT_SOCKETS -sJSPI -sPROXY_TO_PTHREAD -pthread
//
// This exercises the core POSIX socket API surface through the Direct Sockets layer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>

static void test_socket_create() {
  printf("[TEST] socket(AF_INET, SOCK_STREAM, 0)...\n");
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() returned %d, errno=%d (%s)\n", fd, errno, strerror(errno));
    return;
  }
  printf("  OK: fd=%d\n", fd);

  // Set some socket options before connect (tests deferred option storage)
  int flag = 1;
  int rc = setsockopt(fd, IPPROTO_TCP, 1 /*TCP_NODELAY*/, &flag, sizeof(flag));
  printf("  setsockopt TCP_NODELAY: %s\n", rc == 0 ? "OK" : "FAIL");

  int sndbuf = 65536;
  rc = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  printf("  setsockopt SO_SNDBUF: %s\n", rc == 0 ? "OK" : "FAIL");

  // SO_REUSEADDR should be silently accepted
  rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  printf("  setsockopt SO_REUSEADDR: %s\n", rc == 0 ? "OK" : "FAIL");

  // getsockopt SO_ERROR
  int err = -1;
  socklen_t errlen = sizeof(err);
  rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
  printf("  getsockopt SO_ERROR: %s (err=%d)\n", rc == 0 ? "OK" : "FAIL", err);

  close(fd);
  printf("  close: OK\n");
}

static void test_socket_udp_create() {
  printf("[TEST] socket(AF_INET, SOCK_DGRAM, 0)...\n");
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() returned %d, errno=%d\n", fd, errno);
    return;
  }
  printf("  OK: fd=%d\n", fd);
  close(fd);
}

static void test_bad_socket() {
  printf("[TEST] socket() with bad params...\n");

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);  // AF_UNIX not supported
  printf("  AF_UNIX: %s (fd=%d, errno=%d)\n",
         fd < 0 ? "correctly rejected" : "UNEXPECTED SUCCESS", fd, errno);

  fd = socket(AF_INET, SOCK_RAW, 0);  // SOCK_RAW not supported
  printf("  SOCK_RAW: %s (fd=%d, errno=%d)\n",
         fd < 0 ? "correctly rejected" : "UNEXPECTED SUCCESS", fd, errno);
}

static void test_getaddrinfo() {
  printf("[TEST] getaddrinfo(\"localhost\")...\n");
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  int rc = getaddrinfo("localhost", nullptr, &hints, &res);
  if (rc != 0) {
    printf("  FAIL: getaddrinfo returned %d\n", rc);
    return;
  }

  for (struct addrinfo *p = res; p; p = p->ai_next) {
    char addr[INET6_ADDRSTRLEN];
    if (p->ai_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, addr, sizeof(addr));
    } else {
      inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, addr, sizeof(addr));
    }
    printf("  resolved: %s (family=%d, socktype=%d)\n", addr, p->ai_family, p->ai_socktype);
  }
  freeaddrinfo(res);
  printf("  OK\n");
}

// This test will actually attempt a TCP connection.
// It requires a TCP echo server running at the specified address.
// Pass address:port as argv[1], e.g. "127.0.0.1:7777"
static void test_tcp_echo(const char* host, int port) {
  printf("[TEST] TCP echo to %s:%d...\n", host, port);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() errno=%d\n", errno);
    return;
  }

  struct sockaddr_in server = {};
  server.sin_family = AF_INET;
  server.sin_port = htons(port);
  inet_pton(AF_INET, host, &server.sin_addr);

  printf("  connecting...\n");
  if (connect(fd, (struct sockaddr*)&server, sizeof(server)) < 0) {
    printf("  FAIL: connect() errno=%d (%s)\n", errno, strerror(errno));
    close(fd);
    return;
  }
  printf("  connected!\n");

  // Check getsockname / getpeername
  struct sockaddr_in local = {};
  socklen_t len = sizeof(local);
  if (getsockname(fd, (struct sockaddr*)&local, &len) == 0) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf));
    printf("  local: %s:%d\n", buf, ntohs(local.sin_port));
  }

  struct sockaddr_in peer = {};
  len = sizeof(peer);
  if (getpeername(fd, (struct sockaddr*)&peer, &len) == 0) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
    printf("  peer: %s:%d\n", buf, ntohs(peer.sin_port));
  }

  // Send/recv loop
  const char msg[] = "hello direct sockets";
  for (int i = 0; i < 3; i++) {
    ssize_t sent = send(fd, msg, strlen(msg), 0);
    if (sent < 0) {
      printf("  FAIL: send() errno=%d\n", errno);
      break;
    }
    printf("  sent %zd bytes\n", sent);

    char reply[256] = {};
    ssize_t recvd = recv(fd, reply, sizeof(reply) - 1, 0);
    if (recvd < 0) {
      printf("  FAIL: recv() errno=%d\n", errno);
      break;
    }
    if (recvd == 0) {
      printf("  connection closed by peer\n");
      break;
    }
    reply[recvd] = '\0';
    printf("  recv'd %zd bytes: \"%s\"\n", recvd, reply);

    if (strcmp(reply, msg) != 0) {
      printf("  FAIL: echo mismatch!\n");
    }
  }

  shutdown(fd, SHUT_RDWR);
  close(fd);
  printf("  OK\n");
}

int main(int argc, char* argv[]) {
  printf("=== Direct Sockets Test Suite ===\n\n");

  // Non-network tests (always run)
  test_socket_create();
  test_socket_udp_create();
  test_bad_socket();
  test_getaddrinfo();

  // Network test (only if address provided)
  if (argc >= 3) {
    test_tcp_echo(argv[1], atoi(argv[2]));
  } else {
    printf("\n[SKIP] TCP echo test - pass <host> <port> to run\n");
    printf("  e.g.: test 127.0.0.1 7777\n");
  }

  printf("\n=== Done ===\n");
  return 0;
}
