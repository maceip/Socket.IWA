import SocketIWA from '../../api/direct_sockets_api.js';

const GROUP = '239.10.10.10';
const PORT = 5000;

const socket = await SocketIWA.createMulticastSocket({
  localAddress: '0.0.0.0',
  localPort: PORT,
  multicastAllowAddressSharing: true,
  multicastLoopback: true,
});

await socket.addMembership(GROUP);

socket.on('message', (data, rinfo) => {
  const text = typeof data === 'string' ? data : new TextDecoder().decode(data);
  console.log(`[${new Date().toISOString()}] ${rinfo.address}:${rinfo.port} ${text}`);
});
