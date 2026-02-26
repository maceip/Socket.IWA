ReSpec
1
Direct Sockets API
Draft Community Group Report 24 February 2026

Latest published version:
none
Latest editor's draft:
WICG Direct Sockets specification
Editor:
Andrew Rayskiy (Google Inc.)
Former editor:
Eric Willigers (Google Inc.)
Feedback:
GitHub WICG/direct-sockets (pull requests, new issue, open issues)
Copyright © 2026 the Contributors to the Direct Sockets API Specification, published by the Web Platform Incubator Community Group under the W3C Community Contributor License Agreement (CLA). A human-readable summary is available.

Abstract
This specification defines an API that allows web applications to talk to servers and devices that have their own protocols incompatible with those available on the web.

Status of This Document
This specification was published by the Web Platform Incubator Community Group. It is not a W3C Standard nor is it on the W3C Standards Track. Please note that under the W3C Community Contributor License Agreement (CLA) there is a limited opt-out and other conditions apply. Learn more about W3C Community and Business Groups.

This is a work in progress. All contributions welcome.
GitHub Issues are preferred for discussion of this specification.

Table of Contents
Abstract
Status of This Document
1. TCPSocket interface
1.1 constructor() method
1.1.1 TCPSocketOptions dictionary
1.2 [[readable]] attribute (internal)
1.3 [[writable]] attribute (internal)
1.4 opened attribute
1.4.1 TCPSocketOpenInfo dictionary
1.5 closed attribute
1.6 close() method
2. UDPSocket interface
2.1 constructor() method
2.1.1 UDPSocketOptions dictionary
2.1.2 UDPMessage dictionary
2.2 [[readable]] attribute (internal)
2.3 [[writable]] attribute (internal)
2.4 opened attribute
2.4.1 UDPSocketOpenInfo dictionary
2.5 closed attribute
2.6 close() method
3. MulticastController interface
3.1 MulticastGroupOptions dictionary
3.2 MulticastMembership dictionary
3.3 joinGroup() method
3.4 leaveGroup() method
3.5 joinedGroups attribute
4. TCPServerSocket interface
4.1 constructor() method
4.1.1 TCPServerSocketOptions dictionary
4.2 [[readable]] attribute (internal)
4.3 opened attribute
4.3.1 TCPServerSocketOpenInfo dictionary
4.4 closed attribute
4.5 close() method
5. Integrations
5.1 Permissions Policy
5.2 Permissions Policy (Private Network Access)
5.3 Permissions Policy (Multicast)
6. Security and privacy considerations
7. IDL Index
8. Conformance
A. References
A.1 Normative references
1. TCPSocket interface
WebIDL
[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface TCPSocket {
  constructor(DOMString remoteAddress,
              unsigned short remotePort,
              optional TCPSocketOptions options = {});

  readonly attribute Promise<TCPSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};
Methods on this interface typically complete asynchronously, queuing work on the TCPSocket task source.

Instances of TCPSocket are created with the internal slots described in the following table:

Internal slot	Initial value	Description (non-normative)
[[readable]]	null	A ReadableStream that receives data from the socket
[[writable]]	null	A WritableStream that transmits data to the socket
[[openedPromise]]	new Promise	A Promise used to wait for the socket to be opened. Corresponds to the opened member.
[[closedPromise]]	new Promise	A Promise used to wait for the socket to close or error. Corresponds to the closed member.
1.1 constructor() method
Example 1
In order to communicate via TCP a socket connection must be requested first. The socket object constructor allows the site to specify the necessary parameters which control how data is transmitted and received.
const socket = new TCPSocket(/*remoteAddress=*/, /*remotePort=*/, /*options=*/);
The developer should wait for the opened promise to be resolved to gain access to readable and writable streams.
const { readable, writable } = await socket.opened;
Once opened has resolved, the readable and writable can be accessed to get the ReadableStream and WritableStream instances for receiving data from and sending data to the socket.
The constructor() steps are:
If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets", throw a "NotAllowedError" DOMException.
If options["keepAliveDelay"] is less than 1,000, throw a TypeError.
If options["sendBufferSize"] is equal to 0, throw a TypeError.
If options["receiveBufferSize"] is equal to 0, throw a TypeError.
Perform the following steps in parallel.
If remoteAddress resolves to an IP address belonging to the private network address space and this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets-private", queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NotAllowedError" DOMException.
Reject the [[closedPromise]] with a "NotAllowedError" DOMException.
Invoke the operating system to open a TCP socket using the given remoteAddress and remotePort and the connection parameters (or their defaults) specified in options.
If this fails for any other reason, queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NetworkError" DOMException.
Reject the [[closedPromise]] with a "NetworkError" DOMException.
On success, queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
Initialize [[readable]].
Initialize [[writable]].
Let openInfo be a new TCPSocketOpenInfo.
Set openInfo["readable"] to this.[[readable]].
Set openInfo["writable"] to this.[[writable]].
Populate the remaining fields of openInfo using the information provided by the operating system: openInfo["remoteAddress"], openInfo["remotePort"], openInfo["localAddress"] and openInfo["localPort"].
Resolve this.[[openedPromise]] with openInfo.
1.1.1 TCPSocketOptions dictionary
WebIDL
enum SocketDnsQueryType {
  "ipv4",
  "ipv6"
};

dictionary TCPSocketOptions {
  [EnforceRange] unsigned long sendBufferSize;
  [EnforceRange] unsigned long receiveBufferSize;

  boolean noDelay = false;
  [EnforceRange] unsigned long keepAliveDelay;

  SocketDnsQueryType dnsQueryType;
};
sendBufferSize member
The requested send buffer size, in bytes. If not specified, then platform-specific default value will be used.
receiveBufferSize member
The requested receive buffer size, in bytes. If not specified, then platform-specific default value will be used.
noDelay member
Enables the TCP_NODELAY option, disabling Nagle's algorithm.
Note
No-Delay is disabled by default.
keepAliveDelay member
If specified, enables TCP Keep-Alive by setting SO_KEEPALIVE option on the socket to true. The way the actual delay is set is platform-specific:
On Linux & ChromeOS keepAliveDelay is applied to TCP_KEEPIDLE and TCP_KEEPINTVL;
On MacOS keepAliveDelay affects TCP_KEEPALIVE;
On Windows keepAliveDelay is replicated to keepalivetime and keepaliveinterval of SIO_KEEPALIVE_VALS.
Note
Keep-Alive is disabled by default.
dnsQueryType member
Indicates whether IPv4 or IPv6 record should be returned during DNS lookup. If omitted, the OS will select the record type(s) to be queried automatically depending on IPv4/IPv6 settings and reachability.
1.2 [[readable]] attribute (internal)
Example 2
An application receiving data from a TCP socket will typically use the ReadableStream like this:
const { readable } = await socket.opened;

const reader = readable.getReader();

while (true) {
  const { value, done } = await reader.read();
  if (done) {
    // |reader| has been canceled.
    break;
  }
  // Do something with |value|...
}

reader.releaseLock();
The steps to initialize the TCPSocket readable stream are:
Let stream be a new ReadableStream.
Let pullAlgorithm be the following steps:
Let desiredSize be the desired size to fill up to the high water mark for this.[[readable]].
If this.[[readable]]'s current BYOB request view is non-null, then set desiredSize to this.[[readable]]'s current BYOB request view's byte length.
Run the following steps in parallel:
Invoke the operating system to read up to desiredSize bytes from the socket, placing the result in the byte sequence bytes.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
If the connection was closed gracefully, run the following steps:
Invoke close on this.[[readable]].
Invoke the steps to handle closing the TCPSocket readable stream.
Note
This is triggered by the peer sending a packet with the FIN flag set and is typically indicated by the operating system returning 0 bytes when asked for more data from the socket.
If no errors were encountered, then:
If this.[[readable]]'s current BYOB request view is non-null, then write bytes into this.[[readable]]'s current BYOB request view, and set view to this.[[readable]]'s current BYOB request view.
Otherwise, set view to the result of creating a Uint8Array from bytes in this's relevant Realm.
Enqueue view into this.[[readable]].
If a network or operating system error was encountered, invoke error on this.[[readable]] with a "NetworkError" DOMException and invoke the steps to handle closing the TCPSocket readable stream.
Return a promise resolved with undefined.
Let cancelAlgorithm be the following steps:
Invoke the steps to handle closing the TCPSocket readable stream.
Return a promise resolved with undefined.
Set up with byte reading support stream with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm, and highWaterMark set to an implementation-defined value.
Set this.[[readable]] to stream.
To handle closing the TCPSocket readable stream perform the following steps:
If this.[[writable]] is active, abort these steps.
Run the following steps in parallel.
Invoke the operating system to close the socket.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
If this.[[writable]] is errored, reject this.[[closedPromise]] with this.[[writable]].[[storedPromise]].
Otherwise, if this.[[readable]] is errored, reject this.[[closedPromise]] with this.[[readable]].[[storedPromise]].
Otherwise, resolve this.[[closedPromise]] with undefined.
1.3 [[writable]] attribute (internal)
Example 3
To write individual chunks of data to the socket a WritableStreamDefaultWriter can be created and released as necessary. This example uses a TextEncoder to encode a DOMString as the necessary Uint8Array for transmission.
const encoder = new TextEncoder();

const { writable } = await socket.opened;
const writer = writable.getWriter();

await writer.write(encoder.encode("PING"));

writer.releaseLock();
The write() method returns a Promise which resolves when data has been written. While having some data available in the transmit buffer is important to maintain good throughput awaiting this Promise before generating too many chunks of data is a good practice to avoid excessive buffering.
The steps to initialize the TCPSocket writable stream are:
Let stream be a new WritableStream.
Let signal be stream's signal.
Let writeAlgorithm be the following steps, given chunk:
Let promise be a new promise.
Assert: signal is not aborted.
If chunk cannot be converted to an IDL value of type BufferSource, reject promise with a TypeError and return promise. Otherwise, save the result of the conversion in source.
Get a copy of the buffer source source and save the result in bytes.
In parallel, run the following steps:
Invoke the operating system to write bytes to the socket.
Note
The operating system may return from this operation once bytes has been queued for transmission rather than after it has been transmitted.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
If the chunk was successfully written, resolve promise with undefined.
Note
[STREAMS] specifies that writeAlgorithm will only be invoked after the Promise returned by a previous invocation of this algorithm has resolved. For efficiency an implementation is allowed to resolve this Promise early in order to coalesce multiple chunks waiting in the WritableStream's internal queue into a single request to the operating system.
If a network or operating system error was encountered:
Reject promise with a "NetworkError" DOMException.
Invoke the steps to handle closing the TCPSocket writable stream.
If signal is aborted, reject promise with signal's abort reason.
Return promise.
Let abortAlgorithm be the following steps:
Let promise be a new promise.
Run the following steps in parallel:
Invoke the operating system to shutdown the socket for writing.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
Invoke the steps to handle closing the TCPSocket writable stream.
Resolve promise with undefined.
Return promise.
Let closeAlgorithm be the following steps:
Let promise be a new promise.
Run the following steps in parallel.
Invoke the operating system to shutdown the socket for writing.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
Invoke the steps to handle closing the TCPSocket writable stream.
If signal is aborted, reject promise with signal's abort reason.
Resolve promise with undefined.
Return promise.
Set up stream with writeAlgorithm set to writeAlgorithm, abortAlgorithm set to abortAlgorithm, closeAlgorithm set to closeAlgorithm, highWaterMark set to an implementation-defined value.
Add the following abort steps to signal:
Cause any invocation of the operating system to write to the socket to return as soon as possible no matter how much data has been written.
Set this.[[writable]] to stream.
To handle closing the TCPSocket writable stream perform the following steps:
If this.[[readable]] is active, abort these steps.
Run the following steps in parallel.
Invoke the operating system to close the socket.
Queue a global task on the relevant global object of this using the TCPSocket task source to run the following steps:
If this.[[readable]] is errored, reject this.[[closedPromise]] with this.[[readable]].[[storedPromise]].
Otherwise, if this.[[writable]] is errored, reject this.[[closedPromise]] with this.[[writable]].[[storedPromise]].
Otherwise, resolve this.[[closedPromise]] with undefined.
1.4 opened attribute
Example 4
The opened is an attribute containing all information about the socket. Once it is resolved, the developer can use the readable and writable to read from and write to the socket.
const { readable, writable } = await socket.opened;

const reader = readable.getReader();
// Read from the socket using |reader|...

const writer = writable.getWriter();
// Write to the socket using |writer|...
When called, returns the this.[[openedPromise]].
1.4.1 TCPSocketOpenInfo dictionary
WebIDL
dictionary TCPSocketOpenInfo {
  ReadableStream readable;
  WritableStream writable;

  DOMString remoteAddress;
  unsigned short remotePort;

  DOMString localAddress;
  unsigned short localPort;
};
readable member
The readable side of the socket. Set to [[readable]].
writable member
The writable side of the socket. Set to [[writable]].
remoteAddress member
Resolved remote IP address that the socket is connected to.
remotePort member
Remote port that the socket is connected to.
localAddress member
Local IP address that the socket is bound to.
localPort member
Local port that the socket is bound to.
1.5 closed attribute
Example 5
The closed is an attribute that keeps track of the socket state. It gets resolved if the socket is closed gracefully (i.e. by the user) or rejected in case of an error.
socket.closed.then(() => console.log("Closed"), () => console.log("Errored"));
Note that closed will be automatically rejected or resolved once both [[readable]] and [[writable]] reach a closed or errored state.
When called, returns the this.[[closedPromise]].
1.6 close() method
Example 6
When communication with the port is no longer required it can be closed and the associated resources released by the system.
Calling socket.close() implicitly invokes opened.readable.cancel() and opened.writable.abort() in order to clear any buffered data. If the application has called opened.readable.getReader() or opened.writable.getWriter() the stream is locked and the socket cannot be closed. This forces the developer to decide how to handle any read or write operations that are in progress. For example, to ensure that all buffered data has been transmitted before the socket is closed the application must await the Promise returned by writer.close().

const encoder = new TextEncoder();
const { writable } = await socket.opened;
const writer = writable.getWriter();

writer.write(encoder.encode("A long message that will take..."));
await writer.close();
await socket.close();
To discard any unsent data the application could instead call writer.abort(). If a loop is being used to read data from the socket, then it must be exited before calling socket.close().
const { readable } = await socket.opened;
const reader = readable.getReader();

async function readUntilClosed() {
  while (true) {
    const { value, done } = await reader.read();
    if (done) {
      // |reader| has been canceled.
      break;
    }
    // Do something with |value|...
  }

  await socket.close();
}

const read_complete = readUntilClosed();

// Sometime later...
reader.releaseLock();
await read_complete;
The close() method steps are:
If this.[[openedPromise]] is rejected or not yet resolved, reject with "InvalidStateError" DOMException.
If this.[[closedPromise]] is settled, return this.[[closedPromise]].
If this.[[readable]] or this.[[writable]] are locked, reject with "InvalidStateError" DOMException.
Let cancelPromise be the result of invoking cancel on this.[[readable]].
Set cancelPromise.[[PromiseIsHandled]] to true.
Let abortPromise be the result of invoking abort on this.[[writable]].
Set abortPromise.[[PromiseIsHandled]] to true.
Return this.[[closedPromise]].
2. UDPSocket interface
WebIDL
[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface UDPSocket {
  constructor(optional UDPSocketOptions options = {});

  readonly attribute Promise<UDPSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};
Methods on this interface typically complete asynchronously, queuing work on the UDPSocket task source.
Instances of UDPSocket are created with the internal slots described in the following table:

Internal slot	Initial value	Description (non-normative)
[[readable]]	null	A ReadableStream that receives data from the socket
[[writable]]	null	A WritableStream that transmits data to the socket
[[openedPromise]]	new Promise	A Promise used to wait for the socket to be opened. Corresponds to the opened member.
[[closedPromise]]	new Promise	A Promise used to wait for the socket to close or error. Corresponds to the closed member.
2.1 constructor() method
Example 7
In order to communicate via UDP a socket must be opened first. The socket object constructor allows the site to specify the necessary parameters which control how data is transmitted and received.
const socket = new UDPSocket(/*options=*/);
The developer should wait for the opened promise to be resolved to gain access to readable and writable streams.
const { readable, writable } = await socket.opened;
Once opened has resolved, the readable and writable can be accessed to get the ReadableStream and WritableStream instances for receiving data from and sending data to the socket.
UDPSocket can operate in either connected or bound mode which is decided based on the provided set of constructor options.
In connected mode, the UDP socket is associated with a specific destination IP address and port number. This means that any data sent using the socket is automatically sent to the specified destination without the need to specify the address and port every time. This mode is assumed when remoteAddress and remotePort are specified in UDPSocketOptions.
Note
This mode is useful for applications that require a constant communication channel between two endpoints, such as real-time streaming applications.
In bound mode, the UDP socket is bound to a specific local IP address and port number. This means that any data received on that IP address and port is delivered to the socket. Similarly, any data sent using the socket is sent from the bound IP address and port. This mode is assumed when localAddress is specified in UDPSocketOptions.
Note
This mode is useful for applications that need to listen for incoming data on a specific port or interface, such as server applications that receive incoming messages from multiple clients.
The constructor() steps are:
If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets", throw a "NotAllowedError" DOMException.
If any of options["multicastTimeToLive"], options["multicastLoopback"], or options["multicastAllowAddressSharing"] is specified, and this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets-multicast", throw a "NotAllowedError" DOMException.
If only one of options["remoteAddress"] and options["remotePort"] is specified, throw a TypeError.
Alternatively, if both options["remoteAddress"] and options["remotePort"] are specified, assume connected mode.
If options["localPort"] is equal to 0 or specified without options["localAddress"], throw a TypeError.
If options["localAddress"] is specified:
If connected mode was previously inferred, throw a TypeError.
If options["localAddress"] is not a valid IP address, throw a TypeError.
Assume bound mode.
If no mode has been inferred at this point, throw a TypeError.
If options["dnsQueryType"] is specified in bound mode, throw a TypeError.
If options["ipv6Only"] is specified:
If inferred mode is connected, throw a TypeError.
If options["localAddress"] is not equal to the IPv6 unspecified address (::), throw a TypeError.
If options["sendBufferSize"] is equal to 0, throw a TypeError.
If options["receiveBufferSize"] is equal to 0, throw a TypeError.
Perform the following steps in parallel.
If the inferred mode is connected and remoteAddress resolves to an IP address belonging to the private network address space and this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets-private", queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NotAllowedError" DOMException.
Reject the [[closedPromise]] with a "NotAllowedError" DOMException.
If the inferred mode is bound and either this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets-private" or the requested localPort is less than 1024, queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NotAllowedError" DOMException.
Reject the [[closedPromise]] with a "NotAllowedError" DOMException.
Note
While these conditions could technically be checked at construction time, they're deliberately addressed here to unify the behavior with connected mode.
Invoke the operating system to open a UDP socket using the inferred mode and the parameters (or their defaults) specified in options.
If this fails for any reason, queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NetworkError" DOMException.
Reject the [[closedPromise]] with a "NetworkError" DOMException.
On success, queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
Initialize [[readable]].
Initialize [[writable]].
Let openInfo be a new UDPSocketOpenInfo.
Set openInfo["readable"] to this.[[readable]].
Set openInfo["writable"] to this.[[writable]].
Populate the remaining fields of openInfo using the information provided by the operating system:
For bound mode, populate only openInfo["localAddress"] and openInfo["localPort"].
For connected mode, populate openInfo["localAddress"] and openInfo["localPort"] as well as openInfo["remoteAddress"] and openInfo["remotePort"].
Resolve this.[[openedPromise]] with openInfo.
2.1.1 UDPSocketOptions dictionary
WebIDL
dictionary UDPSocketOptions {
  DOMString remoteAddress;
  [EnforceRange] unsigned short remotePort;

  DOMString localAddress;
  [EnforceRange] unsigned short localPort;

  [EnforceRange] unsigned long sendBufferSize;
  [EnforceRange] unsigned long receiveBufferSize;

  SocketDnsQueryType dnsQueryType;
  boolean ipv6Only;

  [EnforceRange] octet multicastTimeToLive;
  boolean multicastLoopback;
  boolean multicastAllowAddressSharing;
};
remoteAddress member
The remote IP address to connect the socket to.
remotePort member
The remote port to connect the socket to.
localAddress member
The local IP address to bind the socket to.
localPort member
The local port to bind the socket to. Leave this field empty to let the OS pick one on its own.
sendBufferSize member
The requested send buffer size, in bytes. If not specified, then platform-specific default value will be used.
receiveBufferSize member
The requested receive buffer size, in bytes. If not specified, then platform-specific default value will be used.
dnsQueryType member
Indicates whether IPv4 or IPv6 record should be returned during DNS lookup. If omitted, the OS will select the record type(s) to be queried automatically depending on IPv4/IPv6 settings and reachability.
Note
This field can only be supplied in connected mode.
ipv6Only member
Enables or disables IPV6_V6ONLY to either restrict connections to IPv6 only or allow both IPv4/IPv6 connections.
Note
This field can only be supplied in bound mode when localAddress is equal to the IPv6 unspecified address (::).
Note
Leave this field empty to retain default platform-defined behavior (true on Windows and false on Posix).
multicastTimeToLive member
This option controls how far your multicast packets can travel across a network. Each time a packet passes through a router, its TTL value is decremented. If the TTL reaches zero, the packet is discarded. It corresponds to IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS in Unix. The default value is 1, which restricts packets to the local network segment.
Note
This option requires policy-controlled feature named "direct-sockets-multicast".
multicastLoopback member
Sets whether multicast packets sent from the host will be looped back to local listeners on the same host. It corresponds to IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP in Unix. The default is true.
Note
This option requires policy-controlled feature named "direct-sockets-multicast".
multicastAllowAddressSharing member
Whether multicast ipAddress:localPort tuple reuse is allowed. This is useful for allowing multiple applications to listen on the same multicast group address and port for multicast messages, such as in device discovery protocols. It corresponds to SO_REUSEADDR / SO_REUSEPORT in Unix. The default is false.
Note
This option requires policy-controlled feature named "direct-sockets-multicast".
2.1.2 UDPMessage dictionary
[[readable]] and [[writable]] streams operate on UDPMessage objects.
WebIDL
dictionary UDPMessage {
  BufferSource data;
  DOMString remoteAddress;
  unsigned short remotePort;
  SocketDnsQueryType dnsQueryType;
};
data member
The user message represented as BufferSource.
Note
For [[readable]] the underlying type is always Uint8Array.
remoteAddress member
The remote address where the message came from or where it should be send to.
Note
In connected mode this field is always unspecified; attempting to set it while writing will throw (see [[writable]]).
In bound mode this field represents the remote host that this packet came from for [[readable]] or instructs the socket about the destination host for [[writable]].
remotePort member
The remote port where the message came from or where it should be sent to.
Note
In connected mode this field is always unspecified; attempting to set it while writing will throw (see [[writable]]).
In bound mode this field represents the remote port that this packet came from for [[readable]] or instructs the socket about the destination port for [[writable]].
dnsQueryType member
Indicates whether IPv4 or IPv6 record should be returned during DNS lookup. If omitted, the OS will select the record type(s) to be queried automatically depending on IPv4/IPv6 settings and reachability.
Note
This field is always unset for UDPMessage instances received from [[readable]] stream regardless of socket's operating mode. For [[writable]] this can only be specified in bound mode; attempting to set the field in connected mode will throw.
2.2 [[readable]] attribute (internal)
Example 8
An application receiving data from a UDP socket will typically use the ReadableStream like this:
const { readable } = await socket.opened;

const reader = readable.getReader();

while (true) {
  const { value, done } = await reader.read();
  if (done) {
    // |reader| has been canceled.
    break;
  }
  // In {{UDPSocket/connected}} {{UDPSocket/mode}}:
  const { data } = value;
  // Do something with |data|...

  // In {{UDPSocket/bound}} {{UDPSocket/mode}}:
  const { data, remoteAddress, remotePort } = value;
  // Do something with |data|, |remoteAddress| and |remotePort|...
}

reader.releaseLock();
The steps to initialize the UDPSocket readable stream are:
Let stream be a new ReadableStream.
Let pullAlgorithm be the following steps:
Let desiredSize be the desired size to fill up to the high water mark for this.[[readable]].
Run the following steps in parallel:
Invoke the operating system to provide up to desiredSize UDP packets from the socket.
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps for each received packet:
If no errors were encountered, for each packet received run the following steps:
Let bytes be a byte sequence containing the packet payload.
Let buffer be a new ArrayBuffer created from bytes.
Let chunk be a new Uint8Array view over buffer, who's length is the length of bytes.
Let message be a new UDPMessage.
Set message["data"] to chunk.
If the socket is operating in bound mode:
Set message["remoteAddress"] to the source address of the packet.
Set message["remotePort"] to the source port of the packet.
Invoke enqueue on this.[[readable]] with message.
If a network or operating system error was encountered, invoke error on this.[[readable]] with a "NetworkError" DOMException, discard other packets and invoke the steps to handle closing the UDPSocket readable stream.
Return a promise resolved with undefined.
Let cancelAlgorithm be the following steps:
Invoke the steps to handle closing the UDPSocket readable stream.
Return a promise resolved with undefined.
Set up stream with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm, highWaterMark Set up stream with set to an implementation-defined value.
Set this.[[readable]] to stream.
To handle closing the UDPSocket readable stream perform the following steps:
If this.[[writable]] is active, abort these steps.
Run the following steps in parallel.
Invoke the operating system to close the socket.
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
If this.[[writable]] is errored, reject this.[[closedPromise]] with this.[[writable]].[[storedPromise]].
Otherwise, if this.[[readable]] is errored, reject this.[[closedPromise]] with this.[[readable]].[[storedPromise]].
Otherwise, resolve this.[[closedPromise]] with undefined.
2.3 [[writable]] attribute (internal)
Example 9
To write individual chunks of data to the socket a WritableStreamDefaultWriter can be created and released as necessary. This example uses a TextEncoder to encode a DOMString as the necessary Uint8Array for transmission.
const encoder = new TextEncoder();

const { writable } = await socket.opened;
const writer = writable.getWriter();

// In {{UDPSocket/connected}} {{UDPSocket/mode}}:
const message = {
  data: encoder.encode("PING")
};

// In {{UDPSocket/bound}} {{UDPSocket/mode}}:
const message = {
  data: encoder.encode("PING"),
  remoteAddress: ...,
  remotePort: ...
};

await writer.ready;
await writer.write(message);

writer.releaseLock();
The write() method returns a Promise which resolves when data has been written. While having some data available in the transmit buffer is important to maintain good throughput awaiting this Promise before generating too many chunks of data is a good practice to avoid excessive buffering.
The steps to initialize the UDPSocket writable stream are:
Let stream be a new WritableStream.
Let signal be stream's signal.
Let writeAlgorithm be the following steps, given chunk:
Let promise be a new promise.
Assert: signal is not aborted.
Let message be a new UDPMessage.
If chunk cannot be converted to an IDL value of type UDPMessage, reject promise with a TypeError and return promise. Otherwise, save the result of the conversion in message.
If either message["remoteAddress"], message["remotePort"] or message["dnsQueryType"] is specified in connected mode, reject promise with a TypeError and return promise.
If either message["remoteAddress"] or message["remotePort"] is not specified in bound mode, reject promise with a TypeError and return promise.
Get a copy of the buffer source message["data"] and save the result in bytes.
In parallel, run the following steps:
Invoke the operating system to send bytes to the socket.
In connected mode the data is routed to the address/port specified upon construction.
In bound mode the data is routed to message["remoteAddress"] and message["remotePort"]. message["dnsQueryType"] can be optionally supplied to control whether DNS resolution routine returns an IPv4 or an IPv6 record.
Note
The operating system may return from this operation once bytes has been queued for transmission rather than after it has been transmitted.
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
If the data was successfully written, resolve promise with undefined.
If a network or operating system error was encountered:
Reject promise with a "NetworkError" DOMException.
Invoke the steps to handle closing the UDPSocket writable stream.
If signal is aborted, reject promise with signal's abort reason.
Return promise.
Let abortAlgorithm be the following steps:
Invoke the steps to handle closing the UDPSocket writable stream.
Return a promise resolved with undefined.
Let closeAlgorithm be the following steps:
Invoke the steps to handle closing the UDPSocket writable stream.
Return a promise resolved with undefined.
Set up stream with writeAlgorithm set to writeAlgorithm, abortAlgorithm set to abortAlgorithm, closeAlgorithm set to closeAlgorithm, highWaterMark set to an implementation-defined value.
Add the following abort steps to signal:
Cause any invocation of the operating system to write to the socket to return as soon as possible no matter how much data has been written.
Set this.[[writable]] to stream.
To handle closing the UDPSocket writable stream perform the following steps:
If this.[[readable]] is active, abort these steps.
Run the following steps in parallel.
Invoke the operating system to close the socket.
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
If this.[[readable]] is errored, reject this.[[closedPromise]] with this.[[readable]].[[storedPromise]].
Otherwise, if this.[[writable]] is errored, reject this.[[closedPromise]] with this.[[writable]].[[storedPromise]].
Otherwise, resolve this.[[closedPromise]] with undefined.
2.4 opened attribute
Example 10
The opened is an attribute containing all information about the socket. Once it is resolved, the developer can use the readable and writable to read from and write to the socket.
const { readable, writable } = await socket.opened;

const reader = readable.getReader();
// Read from the socket using |reader|...

const writer = writable.getWriter();
// Write to the socket using |writer|...
When called, returns the this.[[openedPromise]].
2.4.1 UDPSocketOpenInfo dictionary
WebIDL
dictionary UDPSocketOpenInfo {
  ReadableStream readable;
  WritableStream writable;

  DOMString remoteAddress;
  unsigned short remotePort;

  DOMString localAddress;
  unsigned short localPort;

  MulticastController multicastController;
};
readable member
The readable side of the socket. Set to [[readable]].
writable member
The writable side of the socket. Set to [[writable]].
remoteAddress member
Resolved remote IP address that the socket is communicating with.
remotePort member
Remote port that the socket is communicating with.
localAddress member
Local IP address that the socket is bound to.
localPort member
Local port that the socket is bound to.
multicastController member
An object to control multicast group membership for receiving packets. This member is only present if the socket is in bound mode and the document has policy-controlled feature named "direct-sockets-multicast".
2.5 closed attribute
Example 11
The closed is an attribute that keeps track of the socket state. It gets resolved if the socket is closed gracefully (i.e. by the user) or rejected in case of an error.
socket.closed.then(() => console.log("Closed"), () => console.log("Errored"));
Note that closed will be automatically rejected or resolved once both [[readable]] and [[writable]] reach a closed or errored state.
When called, returns the this.[[closedPromise]].
2.6 close() method
Example 12
When communication with the port is no longer required it can be closed and the associated resources released by the system.
Calling socket.close() implicitly invokes opened.readable.cancel() and opened.writable.abort() in order to clear any buffered data. If the application has called opened.readable.getReader() or opened.writable.getWriter() the stream is locked and the socket cannot be closed. This forces the developer to decide how to handle any read or write operations that are in progress. For example, to ensure that all buffered data has been transmitted before the socket is closed the application must await the Promise returned by writer.close().

const encoder = new TextEncoder();
const { writable } = await socket.opened;
const writer = writable.getWriter();

const message = {
  data: encoder.encode("A long message that will take...")
};

writer.write(message);
await writer.close();
await socket.close();
To discard any unsent data the application could instead call writer.abort(). If a loop is being used to read data from the socket, then it must be exited before calling socket.close().
const { readable } = await socket.opened;
const reader = readable.getReader();

async function readUntilClosed() {
  while (true) {
    const { value, done } = await reader.read();
    if (done) {
      // |reader| has been canceled.
      break;
    }
    const { data } = value;
    // Do something with |data|...
  }

  await socket.close();
}

const readComplete = readUntilClosed();

// Sometime later...
reader.releaseLock();
await readComplete;
The close() method steps are:
If this.[[openedPromise]] is rejected or not yet resolved, reject with "InvalidStateError" DOMException.
If this.[[closedPromise]] is settled, return this.[[closedPromise]].
If this.[[readable]] or this.[[writable]] are locked, reject with "InvalidStateError" DOMException.
Let cancelPromise be the result of invoking cancel on this.[[readable]].
Set cancelPromise.[[PromiseIsHandled]] as handled.
Let abortPromise be the result of invoking abort on this.[[writable]].
Set abortPromise.[[PromiseIsHandled]] as handled.
Return this.[[closedPromise]].
3. MulticastController interface
The MulticastController interface provides methods to join and leave multicast groups and to inspect which groups have been joined. It supports both Any-Source Multicast (ASM) and Source-Specific Multicast (SSM). An instance of this interface is obtained from the multicastController member of the dictionary returned by the opened promise. This member is only present if the socket is in bound mode and and this's relevant global object's associated Document is allowed to use the policy-controlled feature named "direct-sockets-multicast".

WebIDL
dictionary MulticastGroupOptions {
  DOMString sourceAddress;
};

dictionary MulticastMembership {
  DOMString groupAddress;
  DOMString sourceAddress;
};

[
  Exposed=(Window, DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface MulticastController {
  Promise<undefined> joinGroup(DOMString groupAddress, optional MulticastGroupOptions options = {});
  Promise<undefined> leaveGroup(DOMString groupAddress, optional MulticastGroupOptions options = {});
  readonly attribute FrozenArray<(DOMString or MulticastMembership)> joinedGroups;
};
3.1 MulticastGroupOptions dictionary
The MulticastGroupOptions dictionary is used to specify options when joining or leaving a multicast group.

sourceAddress member
The optional IP address of the source for Source-Specific Multicast (SSM). When present, creates or references a source-specific membership that only receives multicast packets from this specific source address to the group address. When absent, creates or references an Any-Source Multicast (ASM) membership that receives packets from any source. The source address must be a unicast address and match the IP version of the group address.
3.2 MulticastMembership dictionary
The MulticastMembership dictionary represents a multicast group membership, which may be either Any-Source Multicast (ASM) or Source-Specific Multicast (SSM).

groupAddress member
The IP address of the multicast group. This must be a valid multicast address (IPv4: 224.0.0.0/4 or IPv6: ff00::/8).
sourceAddress member
The optional IP address of the source for Source-Specific Multicast (SSM). When present, the socket will only receive multicast packets from this specific source address to the group address. When absent, the membership is Any-Source Multicast (ASM) and will receive packets from any source. The source address must be a unicast address and match the IP version of the group address.
Methods on this interface typically complete asynchronously, queuing work on the UDPSocket task source.

Instances of MulticastController are created with the internal slots described in the following table:

Internal slot	Description (non-normative)
[[socket]]	The UDPSocket that this MulticastController manages multicast membership for.
[[joinedGroups]]	A set of MulticastMembership objects representing the multicast group memberships (both ASM and SSM) that have been successfully joined, ordered from the earliest joined membership to the latest.
3.3 joinGroup() method
Example 13
To receive multicast datagrams, a socket must join a multicast group. For Any-Source Multicast (ASM), only the group address is specified:
const socket = new UDPSocket({ localAddress: '0.0.0.0', localPort: 12345 });
const { multicastController } = await socket.opened;
// Join ASM group - receive from any source
await multicastController.joinGroup("239.0.0.1");
console.log('Successfully joined ASM group!');
For Source-Specific Multicast (SSM), the source address is specified in the options:
// Join SSM group - receive only from 192.0.2.100
await multicastController.joinGroup("232.1.1.1", { sourceAddress: "192.0.2.100" });
console.log('Successfully joined SSM group!');
The joinGroup(groupAddress, options) method joins the multicast group at the given groupAddress and the localPort it was bound to. If options.sourceAddress is provided, this creates a Source-Specific Multicast (SSM) membership that only receives packets from that specific source. If options.sourceAddress is omitted, this creates an Any-Source Multicast (ASM) membership that receives packets from any source.

The joinGroup(groupAddress, options) method steps are:
If groupAddress is not a valid IP address, throw a TypeError.
If groupAddress is not a valid multicast address (IPv4: 224.0.0.0/4 or IPv6: ff00::/8), throw a TypeError.
If options.sourceAddress is present:
If options.sourceAddress is not a valid IP address, throw a TypeError.
If options.sourceAddress is a multicast, loopback, or unspecified address, throw a TypeError.
If options.sourceAddress and groupAddress are not the same IP version (both IPv4 or both IPv6), throw a TypeError.
If the socket has already joined or in the process of joining a multicast group with the same groupAddress and options.sourceAddress combination, throw an "InvalidStateError" DOMException.
If the associated [[socket]] was closed or aborted, throw an "InvalidStateError" DOMException.
Let promise be a new promise.
Perform the following steps in parallel:
If options.sourceAddress is present, invoke the operating system to join the source-specific multicast group specified by groupAddress and options.sourceAddress (e.g., using IP_ADD_SOURCE_MEMBERSHIP or MCAST_JOIN_SOURCE_GROUP on POSIX systems).
Otherwise, invoke the operating system to join the any-source multicast group specified by groupAddress (e.g., using IP_ADD_MEMBERSHIP or IPV6_JOIN_GROUP on POSIX systems).
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
If the operating system returned a failure, reject promise with a "NetworkError" DOMException and abort these steps.
Let membership be a new MulticastMembership with groupAddress set to groupAddress and sourceAddress set to options.sourceAddress (or undefined if not provided).
Add membership to [[joinedGroups]].
Resolve promise with undefined.
Return promise.
3.4 leaveGroup() method
Example 14
An application can leave a multicast group to stop receiving datagrams. This is often done before closing the socket if the socket will be repurposed.
// Leave an ASM group
await multicastController.leaveGroup("239.0.0.1");
console.log('Successfully left ASM group!');

// Leave an SSM group - must match both group and source
await multicastController.leaveGroup("232.1.1.1", { sourceAddress: "192.0.2.100" });
console.log('Successfully left SSM group!');
The leaveGroup(groupAddress, options) method leaves a multicast group previously joined via joinGroup(groupAddress, options). The groupAddress and options.sourceAddress must match those used when joining the group.

Note
It is not necessary to leave joined multicast groups to clean up the resources. On the socket close, operating system will leave the groups automatically.
The leaveGroup(groupAddress, options) method steps are:
If groupAddress is not a valid IP address, throw a TypeError.
If options.sourceAddress is present and is not a valid IP address, throw a TypeError.
If the socket has never joined a multicast group with the specified groupAddress and options.sourceAddress combination, already left it, or is in the process of leaving it, throw an "InvalidStateError" DOMException.
If the associated [[socket]] was closed or aborted, throw an "InvalidStateError" DOMException.
Let promise be a new promise.
Perform the following steps in parallel:
If options.sourceAddress is present, invoke the operating system to leave the source-specific multicast group specified by groupAddress and options.sourceAddress (e.g., using IP_DROP_SOURCE_MEMBERSHIP or MCAST_LEAVE_SOURCE_GROUP on POSIX systems).
Otherwise, invoke the operating system to leave the any-source multicast group specified by groupAddress (e.g., using IP_DROP_MEMBERSHIP or IPV6_LEAVE_GROUP on POSIX systems).
Queue a global task on the relevant global object of this using the UDPSocket task source to run the following steps:
If the operating system returned a failure, reject promise with a "NetworkError" DOMException and abort these steps.
Remove the MulticastMembership with matching groupAddress and options.sourceAddress from [[joinedGroups]].
Resolve promise with undefined.
Return promise.
3.5 joinedGroups attribute
Example 15
The joinedGroups attribute provides a list of all multicast group memberships the socket is currently subscribed to. Any-Source Multicast (ASM) memberships are returned as strings for backward compatibility, while Source-Specific Multicast (SSM) memberships are returned as MulticastMembership objects.
await multicastController.joinGroup("239.0.0.1");
await multicastController.joinGroup("232.1.1.1", { sourceAddress: "192.0.2.100" });

for (const membership of multicastController.joinedGroups) {
  if (typeof membership === 'string') {
    // ASM membership (backward compatible)
    console.log(`ASM: Group ${membership}`);
  } else {
    // SSM membership
    console.log(`SSM: Group ${membership.groupAddress}, Source ${membership.sourceAddress}`);
  }
}
// Logs:
// "ASM: Group 239.0.0.1"
// "SSM: Group 232.1.1.1, Source 192.0.2.100"
The joinedGroups getter steps are:

Let result be a new list.
For each membership of this.[[joinedGroups]]:
If membership.sourceAddress is undefined:
Append membership.groupAddress to result.
Otherwise:
Let dict be a new MulticastMembership with:
groupAddress set to membership.groupAddress
sourceAddress set to membership.sourceAddress
Append dict to result.
Return result.
Note
Any-Source Multicast (ASM) memberships are represented as DOMString values (the group address) for backward compatibility, while Source-Specific Multicast (SSM) memberships are represented as MulticastMembership objects containing both group and source addresses.

4. TCPServerSocket interface
WebIDL
[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface TCPServerSocket {
  constructor(DOMString localAddress,
              optional TCPServerSocketOptions options = {});

  readonly attribute Promise<TCPServerSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};
Methods on this interface typically complete asynchronously, queuing work on the TCPServerSocket task source.

Instances of TCPServerSocket are created with the internal slots described in the following table:

Internal slot	Initial value	Description (non-normative)
[[readable]]	null	A ReadableStream used to accept incoming connections.
[[openedPromise]]	new Promise	A Promise used to wait for the socket to be opened. Corresponds to the opened member.
[[closedPromise]]	new Promise	A Promise used to wait for the socket to close or error. Corresponds to the closed member.
4.1 constructor() method
Example 16
In order to accept incoming TCP connections a listening socket must be requested first. The socket object constructor allows the site to specify necessary parameters which control what interfaces the socket is supposed to listen on and how many connections can be queued up. Once opened has resolved, readable can be accessed to get the ReadableStream instance to start accepting connections to the server.
The constructor() steps are:
If this's relevant global object's associated Document is not allowed to use the policy-controlled feature named "direct-sockets", throw a "NotAllowedError" DOMException.
If localAddress is not a valid IP address, throw a TypeError.
If options["localPort"] is equal to 0, throw a TypeError.
If options["backlog"] is equal to 0, throw a TypeError.
If options["ipv6Only"] is true but localAddress is not equal to the IPv6 unspecified address (::), throw a TypeError.
Perform the following steps in parallel.
If the requested localPort is less than 32678, queue a global task on the relevant global object of this using the TCPServerSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NotAllowedError" DOMException.
Reject the [[closedPromise]] with a "NotAllowedError" DOMException.
Note
While this condition could technically be checked at construction time, it's deliberately addressed here to allow for potentially loosening it with additional user-facing permissions.
Invoke the operating system to open a TCP server socket using the given localAddress and the connection parameters (or their defaults) specified in options.
If this fails for any reason, queue a global task on the relevant global object of this using the TCPServerSocket task source to run the following steps:
Reject the [[openedPromise]] with a "NetworkError" DOMException.
Reject the [[closedPromise]] with a "NetworkError" DOMException.
On success, queue a global task on the relevant global object of this using the TCPServerSocket task source to run the following steps:
Initialize [[readable]].
Let openInfo be a new TCPServerSocketOpenInfo.
Set openInfo["readable"] to this.[[readable]].
Populate the remaining fields of openInfo using the information provided by the operating system: openInfo["localAddress"] and openInfo["localPort"].
Resolve this.[[openedPromise]] with openInfo.
4.1.1 TCPServerSocketOptions dictionary
WebIDL
dictionary TCPServerSocketOptions {
  [EnforceRange] unsigned short localPort;
  [EnforceRange] unsigned long backlog;

  boolean ipv6Only;
};
localPort member
The port to open the socket on.
Note
Leave this field empty to let the OS pick one on its own.
backlog member
The size of the OS accept queue.
Note
Leave this field empty to let the OS pick a reasonable platform-specific default.
ipv6Only member
Enables or disables IPV6_V6ONLY to either restrict connections to IPv6 only or allow both IPv4/IPv6 connections.
Note
This field can only be supplied when localAddress is equal to the IPv6 unspecified address (::).
Note
Leave this field empty to retain default platform-defined behavior (true on Windows and false on Posix).
4.2 [[readable]] attribute (internal)
Example 17
An application listening for incoming connections on a TCPServerSocket will typically use the ReadableStream like this:
const { readable } = await socket.opened;
const reader = readable.getReader();

while (true) {
  const { value: acceptedSocket, done } = await reader.read();
  if (done) {
    // |reader| has been canceled.
    break;
  }
  // Do something with |acceptedSocket|...
}

reader.releaseLock();
The steps to initialize the TCPServerSocket readable stream are:
Let stream be a new ReadableStream.
Let pullAlgorithm be the following steps:
Let desiredSize be the desired size of this.[[readable]]'s internal queue.
Run the following steps in parallel:
Invoke the operating system to accept up to desiredSize incoming connections.
Queue a global task on the relevant global object of this using the TCPServerSocket task source to run the following steps:
If the connection was closed gracefully, run the following steps:
Invoke close on this.[[readable]].
Invoke the steps to handle closing the TCPServerSocket readable stream.
If no errors were encountered run the following steps:
Let tcpSocket be a new TCPSocket accepted by the operating system.
Invoke enqueue on this.[[readable]] with tcpSocket.
If a network or operating system error was encountered, invoke error on this.[[readable]] with a "NetworkError" DOMException and invoke the steps to handle closing the TCPServerSocket readable stream.
Return a promise resolved with undefined.
Let cancelAlgorithm be the following steps:
Invoke the steps to handle closing the TCPServerSocket readable stream.
Return a promise resolved with undefined.
Set up stream with pullAlgorithm set to pullAlgorithm, cancelAlgorithm set to cancelAlgorithm, highWaterMark set to an implementation-defined value.
Set this.[[readable]] to stream.
To handle closing the TCPServerSocket readable stream run the following steps in parallel:
Invoke the operating system to close the socket.
Queue a global task on the relevant global object of this using the TCPServerSocket task source to resolve this.[[closedPromise]] with undefined.
4.3 opened attribute
Example 18
opened is an attribute containing all information about the socket. Once it is resolved, the developer can use readable to accept new connections to the server.
const { readable } = await socket.opened;

const reader = readable.getReader();
// Accept new connection to the socket using |reader|...
When called, returns the this.[[openedPromise]].
4.3.1 TCPServerSocketOpenInfo dictionary
WebIDL
dictionary TCPServerSocketOpenInfo {
  ReadableStream readable;

  DOMString localAddress;
  unsigned short localPort;
};
readable member
The readable side of the socket. Set to [[readable]].
localAddress member
Local IP address that the socket is bound to.
localPort member
Local port that the socket is bound to.
4.4 closed attribute
Example 19
The closed is an attribute that keeps track of the socket state. It gets resolved if the socket is closed gracefully (i.e. by the user) or rejected in case of an error.
socket.closed.then(() => console.log("Closed"), () => console.log("Errored"));
Note that closed will be automatically resolved or rejected once [[readable]] reaches a closed or errored state respectively.
When called, returns the this.[[closedPromise]].
4.5 close() method
Example 20
When listening to new connections is no longer required, the socket can be closed and the associated resources released by the system.
Calling socket.close() implicitly invokes opened.readable.cancel(). If the application has previously called opened.readable.getReader(), the stream is locked and the socket cannot be closed. This forces the developer to decide how to handle any accept operations that are in progress.

The close() method steps are:
If this.[[openedPromise]] is rejected or not yet resolved, reject with "InvalidStateError" DOMException.
If this.[[closedPromise]] is settled, return this.[[closedPromise]].
If this.[[readable]] is locked, reject with "InvalidStateError" DOMException.
Let cancelPromise be the result of invoking cancel on this.[[readable]].
Set cancelPromise.[[PromiseIsHandled]] to true.
Return this.[[closedPromise]].
5. Integrations
5.1 Permissions Policy
This specification defines a feature that controls whether TCPSocket, UDPSocket and TCPServerSocket classes may be created.

The feature name for this feature is "direct-sockets"`.

The default allowlist for this feature is 'none'.

Note
A document’s permission policy determines whether a new TCPSocket(...), new UDPSocket(...) or new TCPServerSocket(...) call rejects with a "NotAllowedError" DOMException.
5.2 Permissions Policy (Private Network Access)
This specification defines a feature that controls whether TCPSocket and UDPSocket classes might connect to addresses belonging to the private network address space.

The feature name for this feature is "direct-sockets-private"`.

The default allowlist for this feature is 'none'.

Note
A document’s permission policy determines whether a TCPSocket [[openedPromise]] or UDPSocket [[openedPromise]] promise rejects with a "NotAllowedError" DOMException.
For TCPSocket, the [[openedPromise]] will be rejected if the resolved address belongs to the private network address space.
For UDPSocket in connected mode, the [[openedPromise]] will be rejected if the resolved address belongs to the private network address space.
For UDPSocket in bound mode, the [[openedPromise]] will be rejected unconditionally.
5.3 Permissions Policy (Multicast)
This specification defines a feature that controls whether an application can receive multicast packets using UDPSocket and MulticastController.

The feature name for this feature is "direct-sockets-multicast"`.

The default allowlist for this feature is 'none'.

Note
A document’s permission policy determines whether multicast-specific members of UDPSocketOptions can be used and whether the MulticastController interface is available.
If an application without the permission attempts to construct a UDPSocket with the multicastTimeToLive, multicastLoopback, or multicastAllowAddressSharing members set, the constructor will throw a "NotAllowedError" DOMException.
If an application without the permission successfully opens a UDPSocket in bound mode, the returned UDPSocketOpenInfo dictionary will not contain the multicastController member.
6. Security and privacy considerations
This section is non-normative.

See Explainer.
7. IDL Index
WebIDL
[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface TCPSocket {
  constructor(DOMString remoteAddress,
              unsigned short remotePort,
              optional TCPSocketOptions options = {});

  readonly attribute Promise<TCPSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};

enum SocketDnsQueryType {
  "ipv4",
  "ipv6"
};

dictionary TCPSocketOptions {
  [EnforceRange] unsigned long sendBufferSize;
  [EnforceRange] unsigned long receiveBufferSize;

  boolean noDelay = false;
  [EnforceRange] unsigned long keepAliveDelay;

  SocketDnsQueryType dnsQueryType;
};

dictionary TCPSocketOpenInfo {
  ReadableStream readable;
  WritableStream writable;

  DOMString remoteAddress;
  unsigned short remotePort;

  DOMString localAddress;
  unsigned short localPort;
};

[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface UDPSocket {
  constructor(optional UDPSocketOptions options = {});

  readonly attribute Promise<UDPSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};

dictionary UDPSocketOptions {
  DOMString remoteAddress;
  [EnforceRange] unsigned short remotePort;

  DOMString localAddress;
  [EnforceRange] unsigned short localPort;

  [EnforceRange] unsigned long sendBufferSize;
  [EnforceRange] unsigned long receiveBufferSize;

  SocketDnsQueryType dnsQueryType;
  boolean ipv6Only;

  [EnforceRange] octet multicastTimeToLive;
  boolean multicastLoopback;
  boolean multicastAllowAddressSharing;
};

dictionary UDPMessage {
  BufferSource data;
  DOMString remoteAddress;
  unsigned short remotePort;
  SocketDnsQueryType dnsQueryType;
};

dictionary UDPSocketOpenInfo {
  ReadableStream readable;
  WritableStream writable;

  DOMString remoteAddress;
  unsigned short remotePort;

  DOMString localAddress;
  unsigned short localPort;

  MulticastController multicastController;
};

dictionary MulticastGroupOptions {
  DOMString sourceAddress;
};

dictionary MulticastMembership {
  DOMString groupAddress;
  DOMString sourceAddress;
};

[
  Exposed=(Window, DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface MulticastController {
  Promise<undefined> joinGroup(DOMString groupAddress, optional MulticastGroupOptions options = {});
  Promise<undefined> leaveGroup(DOMString groupAddress, optional MulticastGroupOptions options = {});
  readonly attribute FrozenArray<(DOMString or MulticastMembership)> joinedGroups;
};

[
  Exposed=(Window,DedicatedWorker),
  SecureContext,
  IsolatedContext
]
interface TCPServerSocket {
  constructor(DOMString localAddress,
              optional TCPServerSocketOptions options = {});

  readonly attribute Promise<TCPServerSocketOpenInfo> opened;
  readonly attribute Promise<undefined> closed;

  Promise<undefined> close();
};

dictionary TCPServerSocketOptions {
  [EnforceRange] unsigned short localPort;
  [EnforceRange] unsigned long backlog;

  boolean ipv6Only;
};

dictionary TCPServerSocketOpenInfo {
  ReadableStream readable;

  DOMString localAddress;
  unsigned short localPort;
};
8. Conformance
As well as sections marked as non-normative, all authoring guidelines, diagrams, examples, and notes in this specification are non-normative. Everything else in this specification is normative.

A. References
A.1 Normative references
[dom]
DOM Standard. Anne van Kesteren. WHATWG. Living Standard.
[html]
HTML Standard. Anne van Kesteren; Domenic Denicola; Dominic Farolino; Ian Hickson; Philip Jägenstedt; Simon Pieters. WHATWG. Living Standard.
[infra]
Infra Standard. Anne van Kesteren; Domenic Denicola. WHATWG. Living Standard.
[isolated-contexts]
Isolated Contexts. W3C. Draft Community Group Report.
[permissions-policy]
Permissions Policy. Ian Clelland. W3C. 6 October 2025. W3C Working Draft.
[STREAMS]
Streams Standard. Adam Rice; Domenic Denicola; Mattias Buelens; 吉野剛史 (Takeshi Yoshino). WHATWG. Living Standard.
[WEBIDL]
Web IDL Standard. Edgar Chen; Timothy Gu. WHATWG. Living Standard.
↑
