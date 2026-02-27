import { spawn } from 'node:child_process';

const proc = spawn('./stress-test/native-baseline/build/test_session_ticket', {
  stdio: 'inherit',
});

proc.on('exit', (code) => {
  process.exit(code ?? 1);
});
