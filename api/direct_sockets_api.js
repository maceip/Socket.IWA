/**
 * Socket.IWA user-facing API.
 *
 * Design goals:
 * - WebSocket-like basics: send(), close(), readyState, binaryType, open/message/error/close events.
 * - Socket.IO-style events: emit(), on(), off(), once(), emitWithAck().
 * - dgram-style multicast ergonomics on UDP sockets.
 * - Direct Sockets support: UDP, TCP, TCP server, multicast, permissions checks.
 */

const CONNECTING = 0;
const OPEN = 1;
const CLOSING = 2;
const CLOSED = 3;

const ENVELOPE_TYPE_EVENT = 'event';
const ENVELOPE_TYPE_ACK_REQUEST = 'ack_request';
const ENVELOPE_TYPE_ACK_RESPONSE = 'ack_response';

function ensureSupported(name, value) {
  if (!value) throw new Error(`${name} is not available in this runtime`);
}

function getPolicy() {
  if (typeof document === 'undefined') return null;
  return document.permissionsPolicy || document.featurePolicy || null;
}

function isFeatureAllowed(featureName) {
  const policy = getPolicy();
  if (!policy || typeof policy.allowsFeature !== 'function') return true;
  return policy.allowsFeature(featureName);
}

function maybeParseEnvelope(value) {
  if (typeof value !== 'string') return null;
  try {
    const parsed = JSON.parse(value);
    if (parsed && parsed.__socketiwa === 1) return parsed;
  } catch (_) {
    return null;
  }
  return null;
}

function defaultEncode(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (typeof value === 'string') return new TextEncoder().encode(value);
  return new TextEncoder().encode(JSON.stringify(value));
}

function defaultDecode(value, binaryType) {
  if (value == null) return value;
  if (typeof value === 'string') return value;

  const bytes = value instanceof Uint8Array ? value : new Uint8Array(value);

  if (binaryType === 'blob' && typeof Blob !== 'undefined') {
    return new Blob([bytes]);
  }
  if (binaryType === 'arraybuffer') {
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  }
  return bytes;
}

function isLifecycleEvent(name) {
  return (
    name === 'open' ||
    name === 'connect' ||
    name === 'message' ||
    name === 'error' ||
    name === 'close' ||
    name === 'disconnect' ||
    name === 'connection'
  );
}

function isLikelyIpAddress(value) {
  return typeof value === 'string' && (value.includes('.') || value.includes(':'));
}

function parseUdpSendArgs(data, args) {
  // dgram-like signatures supported:
  // send(data)
  // send(data, port, address)
  // send(data, offset, length, port, address)
  let payload = data;

  if (args.length === 0) return { payload, target: undefined };

  if (args.length >= 2 && Number.isInteger(args[0]) && Number.isInteger(args[1])) {
    const [offset, length, port, address] = args;
    const encoded = defaultEncode(data);
    payload = encoded.subarray(offset, offset + length);
    return {
      payload,
      target: Number.isInteger(port) && typeof address === 'string' ? { remotePort: port, remoteAddress: address } : undefined,
    };
  }

  const [port, address] = args;
  if (Number.isInteger(port) && typeof address === 'string') {
    return { payload, target: { remotePort: port, remoteAddress: address } };
  }

  return { payload, target: undefined };
}

class Emitter {
  constructor() {
    this.listeners = new Map();
  }

  on(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set());
    this.listeners.get(type).add(listener);
    return this;
  }

  off(type, listener) {
    const set = this.listeners.get(type);
    if (!set) return this;
    set.delete(listener);
    if (set.size === 0) this.listeners.delete(type);
    return this;
  }

  once(type, listener) {
    const wrapped = (...args) => {
      this.off(type, wrapped);
      listener(...args);
    };
    return this.on(type, wrapped);
  }

  emit(type, ...args) {
    const set = this.listeners.get(type);
    if (!set) return false;
    [...set].forEach((fn) => fn(...args));
    return true;
  }
}

export class DirectSocketConnection extends Emitter {
  constructor({ kind, socket, opened, mode = 'stream', options = {} }) {
    super();
    this.kind = kind;
    this.socket = socket;
    this.opened = opened;
    this.mode = mode;
    this.readyState = OPEN;
    this.binaryType = 'arraybuffer';
    this._ackSeq = 1;
    this._pendingAcks = new Map();
    this._reader = opened.readable.getReader();
    this._writer = opened.writable.getWriter();
    this._multicastOptions = {
      ttl: options.multicastTimeToLive,
      loopback: options.multicastLoopback,
      allowAddressSharing: options.multicastAllowAddressSharing,
    };

    if (mode === 'datagram') {
      this.multicast = createMulticastFacade(this, opened);
    }

    queueMicrotask(() => {
      super.emit('open');
      super.emit('connect');
    });

    this.socket.closed
      .then(() => this._finalizeClose())
      .catch((err) => {
        super.emit('error', err);
        this._finalizeClose();
      });

    this._pumpReadLoop();
  }

  async _pumpReadLoop() {
    try {
      while (this.readyState === OPEN) {
        const { value, done } = await this._reader.read();
        if (done) break;

        const message = this._normalizeIncoming(value);
        if (this.mode === 'datagram') {
          const rinfo = {
            address: message.remoteAddress,
            port: message.remotePort,
            size: message.byteLength,
          };
          super.emit('message', message.data, rinfo);
        } else {
          super.emit('message', message.data);
        }

        this._handleStructuredMessage(message.data);
      }
    } catch (err) {
      super.emit('error', err);
    } finally {
      this._finalizeClose();
    }
  }

  _normalizeIncoming(value) {
    if (this.mode === 'datagram') {
      const data = value?.data ?? value;
      const decoded = defaultDecode(data, this.binaryType);
      const byteLength = data?.byteLength ?? data?.length ?? 0;
      return {
        data: decoded,
        byteLength,
        remoteAddress: value?.remoteAddress,
        remotePort: value?.remotePort,
      };
    }

    return { data: defaultDecode(value, this.binaryType) };
  }

  _handleStructuredMessage(payload) {
    let asString = null;

    if (typeof payload === 'string') {
      asString = payload;
    } else if (payload instanceof ArrayBuffer) {
      asString = new TextDecoder().decode(new Uint8Array(payload));
    } else if (payload instanceof Uint8Array) {
      asString = new TextDecoder().decode(payload);
    }

    const envelope = maybeParseEnvelope(asString);
    if (!envelope) return;

    if (envelope.t === ENVELOPE_TYPE_EVENT) {
      super.emit(envelope.e, ...(envelope.args || []));
      return;
    }

    if (envelope.t === ENVELOPE_TYPE_ACK_REQUEST) {
      const respond = async (...args) => {
        await this._sendEnvelope({
          __socketiwa: 1,
          t: ENVELOPE_TYPE_ACK_RESPONSE,
          id: envelope.id,
          args,
        });
      };
      super.emit(envelope.e, ...(envelope.args || []), respond);
      return;
    }

    if (envelope.t === ENVELOPE_TYPE_ACK_RESPONSE && this._pendingAcks.has(envelope.id)) {
      const pending = this._pendingAcks.get(envelope.id);
      clearTimeout(pending.timeout);
      this._pendingAcks.delete(envelope.id);
      pending.resolve((envelope.args || [])[0]);
    }
  }

  async _sendEnvelope(envelope, target) {
    return this.send(JSON.stringify(envelope), target);
  }

  async send(data, ...args) {
    if (this.readyState !== OPEN) throw new Error('Socket is not open');

    if (this.mode === 'datagram') {
      const parsed = parseUdpSendArgs(data, args);
      const payload = defaultEncode(parsed.payload);

      if (parsed.target?.remoteAddress && parsed.target?.remotePort) {
        await this._writer.write({
          data: payload,
          remoteAddress: parsed.target.remoteAddress,
          remotePort: parsed.target.remotePort,
        });
        return;
      }

      await this._writer.write({ data: payload });
      return;
    }

    await this._writer.write(defaultEncode(data));
  }

  async emitEvent(eventName, ...args) {
    await this._sendEnvelope({ __socketiwa: 1, t: ENVELOPE_TYPE_EVENT, e: eventName, args });
  }

  async emit(eventName, ...args) {
    if (isLifecycleEvent(eventName)) {
      return super.emit(eventName, ...args);
    }
    await this.emitEvent(eventName, ...args);
    return true;
  }

  async emitWithAck(eventName, payload, { timeoutMs = 10000 } = {}) {
    if (this.readyState !== OPEN) throw new Error('Socket is not open');

    const id = this._ackSeq++;
    return new Promise(async (resolve, reject) => {
      const timeout = setTimeout(() => {
        this._pendingAcks.delete(id);
        reject(new Error(`Ack timeout for event '${eventName}'`));
      }, timeoutMs);

      this._pendingAcks.set(id, { resolve, reject, timeout });
      try {
        await this._sendEnvelope({
          __socketiwa: 1,
          t: ENVELOPE_TYPE_ACK_REQUEST,
          e: eventName,
          args: [payload],
          id,
        });
      } catch (err) {
        clearTimeout(timeout);
        this._pendingAcks.delete(id);
        reject(err);
      }
    });
  }

  async close(code = 1000, reason = 'normal closure') {
    if (this.readyState >= CLOSING) return;
    this.readyState = CLOSING;
    await this.socket.close?.(code, reason);
    this._finalizeClose(code, reason);
  }

  _finalizeClose(code = 1000, reason = 'closed') {
    if (this.readyState === CLOSED) return;
    this.readyState = CLOSED;
    super.emit('close', { code, reason });
    super.emit('disconnect', { code, reason });
  }

  // dgram-style multicast convenience methods.
  async addMembership(groupAddress, ifaceOrSource) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');

    // If second argument looks like an IP, treat as SSM source address.
    const options = isLikelyIpAddress(ifaceOrSource)
      ? { sourceAddress: ifaceOrSource }
      : ifaceOrSource
      ? { localAddress: ifaceOrSource }
      : {};

    return this.multicast.join(groupAddress, options);
  }

  async dropMembership(groupAddress, ifaceOrSource) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');

    const options = isLikelyIpAddress(ifaceOrSource)
      ? { sourceAddress: ifaceOrSource }
      : ifaceOrSource
      ? { localAddress: ifaceOrSource }
      : {};

    return this.multicast.leave(groupAddress, options);
  }

  async addSourceSpecificMembership(sourceAddress, groupAddress) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');
    return this.multicast.joinSourceSpecific(sourceAddress, groupAddress);
  }

  async dropSourceSpecificMembership(sourceAddress, groupAddress) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');
    return this.multicast.leaveSourceSpecific(sourceAddress, groupAddress);
  }

  setMulticastTTL(ttl) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');
    this._multicastOptions.ttl = ttl;
    this.multicast.setTTL(ttl);
    return this;
  }

  setMulticastLoopback(loopback) {
    if (!this.multicast) throw new Error('Multicast is only available on UDP sockets');
    this._multicastOptions.loopback = !!loopback;
    this.multicast.setLoopback(!!loopback);
    return this;
  }
}

function createMulticastFacade(connection, udpOpenInfo) {
  const controller = udpOpenInfo?.multicast || udpOpenInfo?.multicastController;

  const facade = {
    available: !!controller,
    joinedGroups: () => (controller?.joinedGroups || []).slice(),
    join: async (groupAddress, options = {}) => {
      if (!controller) throw new Error('Multicast is unavailable for this socket');
      return controller.joinGroup(groupAddress, options);
    },
    leave: async (groupAddress, options = {}) => {
      if (!controller) throw new Error('Multicast is unavailable for this socket');
      return controller.leaveGroup(groupAddress, options);
    },
    joinSourceSpecific: async (sourceAddress, groupAddress, options = {}) => {
      if (!controller) throw new Error('Multicast is unavailable for this socket');
      return controller.joinGroup(groupAddress, { ...options, sourceAddress });
    },
    leaveSourceSpecific: async (sourceAddress, groupAddress, options = {}) => {
      if (!controller) throw new Error('Multicast is unavailable for this socket');
      return controller.leaveGroup(groupAddress, { ...options, sourceAddress });
    },
    setTTL: (ttl) => {
      connection._multicastOptions.ttl = ttl;
      return ttl;
    },
    setLoopback: (loopback) => {
      connection._multicastOptions.loopback = !!loopback;
      return !!loopback;
    },
  };

  return facade;
}

export class DirectSocketServer extends Emitter {
  constructor({ socket, opened }) {
    super();
    this.socket = socket;
    this.opened = opened;
    this.readyState = OPEN;
    this.binaryType = 'arraybuffer';
    this._rooms = new Map();
    this._reader = opened.readable.getReader();

    queueMicrotask(() => {
      super.emit('open');
      super.emit('connect');
    });

    this._acceptLoop();
    this.socket.closed
      .then(() => this._finalizeClose())
      .catch((err) => {
        super.emit('error', err);
        this._finalizeClose();
      });
  }

  async _acceptLoop() {
    try {
      while (this.readyState === OPEN) {
        const { value, done } = await this._reader.read();
        if (done || !value) break;

        const openInfo = await value.opened;
        const conn = new DirectSocketConnection({ kind: 'tcp', socket: value, opened: openInfo, mode: 'stream' });
        conn.binaryType = this.binaryType;

        conn.join = (roomName) => this._joinRoom(roomName, conn);
        conn.leave = (roomName) => this._leaveRoom(roomName, conn);

        super.emit('connection', conn);
      }
    } catch (err) {
      super.emit('error', err);
    } finally {
      this._finalizeClose();
    }
  }

  _joinRoom(roomName, conn) {
    if (!this._rooms.has(roomName)) this._rooms.set(roomName, new Set());
    this._rooms.get(roomName).add(conn);
    return conn;
  }

  _leaveRoom(roomName, conn) {
    const room = this._rooms.get(roomName);
    if (!room) return conn;
    room.delete(conn);
    if (room.size === 0) this._rooms.delete(roomName);
    return conn;
  }

  to(roomName) {
    const room = this._rooms.get(roomName) || new Set();
    return {
      emit: async (eventName, ...args) => {
        await Promise.all([...room].map((conn) => conn.emitEvent(eventName, ...args)));
      },
      send: async (data) => {
        await Promise.all([...room].map((conn) => conn.send(data)));
      },
    };
  }

  async close(code = 1000, reason = 'server closed') {
    if (this.readyState >= CLOSING) return;
    this.readyState = CLOSING;
    await this.socket.close?.(code, reason);
    this._finalizeClose(code, reason);
  }

  _finalizeClose(code = 1000, reason = 'closed') {
    if (this.readyState === CLOSED) return;
    this.readyState = CLOSED;
    super.emit('close', { code, reason });
    super.emit('disconnect', { code, reason });
  }
}

export class SocketIWA {
  static CONNECTING = CONNECTING;
  static OPEN = OPEN;
  static CLOSING = CLOSING;
  static CLOSED = CLOSED;

  static permissions = {
    directSockets: () => isFeatureAllowed('direct-sockets'),
    directSocketsPrivate: () => isFeatureAllowed('direct-sockets-private'),
    directSocketsMulticast: () => isFeatureAllowed('direct-sockets-multicast'),
  };

  static assertPermissions({ privateNetwork = false, multicast = false } = {}) {
    if (!SocketIWA.permissions.directSockets()) throw new Error('Permission denied by policy: direct-sockets');
    if (privateNetwork && !SocketIWA.permissions.directSocketsPrivate()) {
      throw new Error('Permission denied by policy: direct-sockets-private');
    }
    if (multicast && !SocketIWA.permissions.directSocketsMulticast()) {
      throw new Error('Permission denied by policy: direct-sockets-multicast');
    }
  }

  static async createTCP(remoteAddress, remotePort, options = {}) {
    SocketIWA.assertPermissions({ privateNetwork: !!options.requiresPrivateNetwork });
    ensureSupported('TCPSocket', globalThis.TCPSocket);

    const socket = new TCPSocket(remoteAddress, remotePort, options);
    const opened = await socket.opened;
    return new DirectSocketConnection({ kind: 'tcp', socket, opened, mode: 'stream' });
  }

  static async createUDP(options = {}) {
    const multicastRequested =
      typeof options.multicastTimeToLive === 'number' ||
      typeof options.multicastLoopback === 'boolean' ||
      typeof options.multicastAllowAddressSharing === 'boolean';

    SocketIWA.assertPermissions({
      privateNetwork: !!(options.remoteAddress && options.requiresPrivateNetwork),
      multicast: multicastRequested,
    });

    ensureSupported('UDPSocket', globalThis.UDPSocket);
    const socket = new UDPSocket(options);
    const opened = await socket.opened;

    return new DirectSocketConnection({ kind: 'udp', socket, opened, mode: 'datagram', options });
  }

  static async createMulticastSocket({
    allowAddressSharing = true,
    reuseAddr,
    type = 'udp4',
    localAddress,
    localPort,
    multicastTimeToLive,
    multicastLoopback,
  } = {}) {
    const multicastAllowAddressSharing = reuseAddr ?? allowAddressSharing;

    const udpOptions = {
      localAddress,
      localPort,
      multicastAllowAddressSharing,
      multicastTimeToLive,
      multicastLoopback,
    };

    if (type === 'udp6') {
      udpOptions.dnsQueryType = 'ipv6';
    }

    return SocketIWA.createUDP(udpOptions);
  }

  static async createTCPServer(localAddress, { localPort, backlog } = {}) {
    SocketIWA.assertPermissions();
    ensureSupported('TCPServerSocket', globalThis.TCPServerSocket);

    const socket = new TCPServerSocket(localAddress, { localPort, backlog });
    const opened = await socket.opened;
    return new DirectSocketServer({ socket, opened });
  }

  static createMulticastController(udpOpenInfo) {
    const controller = udpOpenInfo?.multicast || udpOpenInfo?.multicastController;
    if (!controller) {
      return {
        available: false,
        joinedGroups: () => [],
        joinGroup: async () => {
          throw new Error('Multicast is unavailable for this socket');
        },
        leaveGroup: async () => {
          throw new Error('Multicast is unavailable for this socket');
        },
      };
    }

    return {
      available: true,
      joinedGroups: () => controller.joinedGroups || [],
      joinGroup: (groupAddress, options = {}) => controller.joinGroup(groupAddress, options),
      leaveGroup: (groupAddress, options = {}) => controller.leaveGroup(groupAddress, options),
    };
  }
}

export default SocketIWA;
