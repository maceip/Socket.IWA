import SocketIWA from '../../api/direct_sockets_api.js';

const GROUP = '239.10.10.10';
const PORT = 5000;

const socket = await SocketIWA.createMulticastSocket({
  localAddress: '0.0.0.0',
  localPort: 0,
  multicastTimeToLive: 8,
  multicastLoopback: true,
});

setInterval(async () => {
  const payload = new TextEncoder().encode(`frame:${Date.now()}`);
  await socket.send(payload, PORT, GROUP);
}, 1000 / 10);
