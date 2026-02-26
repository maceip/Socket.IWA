Full POSIX Sockets over WebSocket Proxy Server
Emscripten provides a native POSIX Sockets proxy server program, located in directory tools/websocket_to_posix_proxy/, that allows full POSIX Sockets API access from a web browser. This support works by proxying all POSIX Sockets API calls from the browser to the Emscripten POSIX Sockets proxy server (via transparent use of the WebSockets API), and the proxy server then performs the native TCP/UDP calls on behalf of the page. This allows a web browser page to run full TCP & UDP connections, act as a server to accept incoming connections, and perform host name lookups and reverse lookups. Because all API calls are individually proxied, this support can be slow. This support is mostly useful for developing testing infrastructure and debugging.

The following POSIX sockets functions are proxied in this manner:
socket(), socketpair(), shutdown(), bind(), connect(), listen(), accept(), getsockname(), getpeername(), send(), recv(), sendto(), recvfrom(), sendmsg(), recvmsg(), getsockopt(), setsockopt(), getaddrinfo(), getnameinfo().

The following POSIX sockets functions are currently not proxied (and will not work):
poll(), close() (use shutdown() instead), select()

To use POSIX sockets proxying, link the application with flags -lwebsocket.js -sPROXY_POSIX_SOCKETS -pthread -sPROXY_TO_PTHREAD. That is, POSIX sockets proxying builds on top of the Emscripten WebSockets library, and requires multithreading and proxying the application main() to a pthread.

For an example of how the POSIX Sockets proxy server works in an Emscripten client program, see the file test/websocket/tcp_echo_client.c.
