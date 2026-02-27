import { spawn } from 'node:child_process';

// Wrapper around the native reference server built by:
// bash stress-test/native-baseline/build_native.sh
const proc = spawn('./stress-test/native-baseline/build/quic_echo_server_native', {
  stdio: 'inherit',
});

process.on('SIGINT', () => proc.kill('SIGINT'));
process.on('SIGTERM', () => proc.kill('SIGTERM'));
