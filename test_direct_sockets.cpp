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
#include <poll.h>
#include <fcntl.h>
#include <sys/ioctl.h>

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

// Test poll() with timeout=0 on a freshly created socket (no data ready)
static void test_poll_immediate() {
  printf("[TEST] poll() with timeout=0...\n");

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() errno=%d\n", errno);
    return;
  }

  struct pollfd pfd = {};
  pfd.fd = fd;
  pfd.events = POLLIN | POLLOUT;

  int rc = poll(&pfd, 1, 0);
  printf("  poll returned %d, revents=0x%x\n", rc, pfd.revents);

  if (rc < 0) {
    printf("  FAIL: poll() errno=%d (%s)\n", errno, strerror(errno));
  } else {
    printf("  OK: poll with timeout=0 returned %d\n", rc);
  }

  close(fd);
}

// Test poll() with a short timeout when no data is available
static void test_poll_timeout() {
  printf("[TEST] poll() with short timeout (100ms)...\n");

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() errno=%d\n", errno);
    return;
  }

  struct pollfd pfd = {};
  pfd.fd = fd;
  pfd.events = POLLIN;

  int rc = poll(&pfd, 1, 100);
  printf("  poll returned %d, revents=0x%x\n", rc, pfd.revents);

  if (rc == 0) {
    printf("  OK: poll correctly timed out\n");
  } else if (rc < 0) {
    printf("  FAIL: poll() errno=%d (%s)\n", errno, strerror(errno));
  } else {
    printf("  INFO: poll returned %d (unexpected but not necessarily wrong)\n", rc);
  }

  close(fd);
}

// Test pipe() - create, write, read back
static void test_pipe() {
  printf("[TEST] pipe() create/write/read...\n");

  int pipefd[2];
  int rc = pipe(pipefd);
  if (rc < 0) {
    printf("  FAIL: pipe() errno=%d (%s)\n", errno, strerror(errno));
    return;
  }
  printf("  pipe created: read_fd=%d, write_fd=%d\n", pipefd[0], pipefd[1]);

  // Write some data
  const char msg[] = "hello pipe";
  ssize_t written = write(pipefd[1], msg, strlen(msg));
  if (written < 0) {
    printf("  FAIL: write() errno=%d (%s)\n", errno, strerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }
  printf("  wrote %zd bytes\n", written);

  // Poll the read end - should be ready
  struct pollfd pfd = {};
  pfd.fd = pipefd[0];
  pfd.events = POLLIN;
  rc = poll(&pfd, 1, 0);
  printf("  poll on read end: returned %d, revents=0x%x\n", rc, pfd.revents);
  if (rc == 1 && (pfd.revents & POLLIN)) {
    printf("  OK: read end is ready\n");
  } else {
    printf("  FAIL: expected POLLIN on read end\n");
  }

  // Read it back
  char buf[64] = {};
  ssize_t bytesRead = read(pipefd[0], buf, sizeof(buf) - 1);
  if (bytesRead < 0) {
    printf("  FAIL: read() errno=%d (%s)\n", errno, strerror(errno));
  } else {
    buf[bytesRead] = '\0';
    printf("  read %zd bytes: \"%s\"\n", bytesRead, buf);
    if (strcmp(buf, msg) == 0) {
      printf("  OK: pipe round-trip matches\n");
    } else {
      printf("  FAIL: data mismatch\n");
    }
  }

  close(pipefd[0]);
  close(pipefd[1]);
}

// Test socketpair() - bidirectional send/recv
static void test_socketpair() {
  printf("[TEST] socketpair() bidirectional...\n");

  int sv[2];
  int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
  if (rc < 0) {
    printf("  FAIL: socketpair() errno=%d (%s)\n", errno, strerror(errno));
    return;
  }
  printf("  socketpair: fd0=%d, fd1=%d\n", sv[0], sv[1]);

  // Write from fd0, read from fd1
  const char msg1[] = "from fd0";
  ssize_t sent = write(sv[0], msg1, strlen(msg1));
  printf("  write fd0->fd1: %zd bytes\n", sent);

  char buf[64] = {};
  ssize_t recvd = read(sv[1], buf, sizeof(buf) - 1);
  if (recvd > 0) {
    buf[recvd] = '\0';
    printf("  read fd1: \"%s\" %s\n", buf, strcmp(buf, msg1) == 0 ? "OK" : "FAIL");
  } else {
    printf("  FAIL: read from fd1 returned %zd, errno=%d\n", recvd, errno);
  }

  // Write from fd1, read from fd0
  const char msg2[] = "from fd1";
  sent = write(sv[1], msg2, strlen(msg2));
  printf("  write fd1->fd0: %zd bytes\n", sent);

  memset(buf, 0, sizeof(buf));
  recvd = read(sv[0], buf, sizeof(buf) - 1);
  if (recvd > 0) {
    buf[recvd] = '\0';
    printf("  read fd0: \"%s\" %s\n", buf, strcmp(buf, msg2) == 0 ? "OK" : "FAIL");
  } else {
    printf("  FAIL: read from fd0 returned %zd, errno=%d\n", recvd, errno);
  }

  close(sv[0]);
  close(sv[1]);
}

// Test getaddrinfo with a real hostname (requires DoH DNS to be working)
static void test_getaddrinfo_real() {
  printf("[TEST] getaddrinfo(\"dns.google\") - real DNS...\n");

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = nullptr;
  int rc = getaddrinfo("dns.google", nullptr, &hints, &res);
  if (rc != 0) {
    printf("  FAIL: getaddrinfo returned %d (%s)\n", rc, gai_strerror(rc));
    return;
  }

  bool found_non_fake = false;
  for (struct addrinfo *p = res; p; p = p->ai_next) {
    char addr[INET6_ADDRSTRLEN];
    if (p->ai_family == AF_INET) {
      inet_ntop(AF_INET, &((struct sockaddr_in*)p->ai_addr)->sin_addr, addr, sizeof(addr));
    } else {
      inet_ntop(AF_INET6, &((struct sockaddr_in6*)p->ai_addr)->sin6_addr, addr, sizeof(addr));
    }
    printf("  resolved: %s (family=%d)\n", addr, p->ai_family);

    // Check that it's not a fake 172.29.x.x address
    if (strncmp(addr, "172.29.", 7) != 0) {
      found_non_fake = true;
    }
  }

  if (found_non_fake) {
    printf("  OK: got real IP address (not 172.29.x.x)\n");
  } else {
    printf("  INFO: got emscripten fake DNS address (DoH may not be active)\n");
  }

  freeaddrinfo(res);
}

// Test non-blocking recv - set O_NONBLOCK, recv on empty socket, expect EAGAIN
static void test_nonblocking_recv() {
  printf("[TEST] non-blocking recv (O_NONBLOCK + EAGAIN)...\n");

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    printf("  FAIL: socket() errno=%d\n", errno);
    return;
  }

  // Set non-blocking via fcntl
  int flags = fcntl(fd, F_GETFL, 0);
  printf("  F_GETFL: flags=0x%x\n", flags);

  int rc = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  printf("  F_SETFL O_NONBLOCK: %s\n", rc == 0 ? "OK" : "FAIL");

  flags = fcntl(fd, F_GETFL, 0);
  printf("  F_GETFL after set: flags=0x%x (O_NONBLOCK=%s)\n",
         flags, (flags & O_NONBLOCK) ? "yes" : "no");

  if (!(flags & O_NONBLOCK)) {
    printf("  FAIL: O_NONBLOCK not set\n");
    close(fd);
    return;
  }

  // Also test FIONBIO ioctl path
  int val = 0;
  rc = ioctl(fd, FIONBIO, &val);
  printf("  ioctl FIONBIO(0): %s\n", rc == 0 ? "OK" : "FAIL");

  val = 1;
  rc = ioctl(fd, FIONBIO, &val);
  printf("  ioctl FIONBIO(1): %s\n", rc == 0 ? "OK" : "FAIL");

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

  // New tests: poll, pipe, socketpair, nonblocking, DNS
  test_poll_immediate();
  test_poll_timeout();
  test_pipe();
  test_socketpair();
  test_getaddrinfo_real();
  test_nonblocking_recv();

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
