#!/usr/bin/env node
/**
 * chrome_profiler.mjs — Capture Chrome trace + V8 CPU profile of WASM QUIC server
 * Run while traffic is being generated against the server.
 *
 * Usage: node chrome_profiler.mjs [--duration 30] [--url <url>]
 */
import puppeteer from 'puppeteer';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const resultsDir = path.join(__dirname, '..', 'results');
fs.mkdirSync(resultsDir, { recursive: true });

const duration = parseInt(process.env.PROFILE_DURATION || '30');
const swbnPath = path.resolve(__dirname, '../../iwa-sink/iwa-sink/dist/iwa-sink.swbn');
const webBundleId = 'va5nmqd44umdnobnnp7xpxdbhjn6dlsrdgirrnsm6envbjndx2waaaic';
const iwaOrigin = `isolated-app://${webBundleId}`;

async function main() {
  const ts = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);

  console.log(`Profiling for ${duration}s...`);

  const browser = await puppeteer.launch({
    headless: 'shell',
    executablePath: '/home/devuser/.cache/puppeteer/chrome/linux-145.0.7632.77/chrome-linux64/chrome',
    args: [
      '--no-sandbox',
      '--disable-setuid-sandbox',
      '--disable-gpu',
      '--enable-features=IsolatedWebApps,IsolatedWebAppDevMode',
      '--install-isolated-web-app-from-file=' + swbnPath,
      '--enable-precise-memory-info',
      '--disable-background-timer-throttling',
      '--disable-renderer-backgrounding',
      '--js-flags=--experimental-wasm-stack-switching',
    ],
    userDataDir: '/tmp/iwa-profile-' + Date.now(),
  });

  await new Promise(r => setTimeout(r, 3000));

  const page = await browser.newPage();
  const client = await page.createCDPSession();

  // Navigate to IWA
  await page.goto(iwaOrigin + '/quic.html', {
    waitUntil: 'networkidle2',
    timeout: 15000,
  }).catch(() => {});
  await new Promise(r => setTimeout(r, 2000));

  console.log('Page loaded:', page.url());

  // Start Chrome trace
  const tracePath = path.join(resultsDir, `trace_${ts}.json`);
  await page.tracing.start({
    path: tracePath,
    categories: [
      'v8', 'v8.wasm', 'v8.wasm.detailed', 'v8.wasm.turbofan',
      'devtools.timeline', 'disabled-by-default-v8.cpu_profiler',
      'blink.user_timing', 'loading',
    ],
  });

  // Start V8 CPU profiler via CDP
  await client.send('Profiler.enable');
  await client.send('Profiler.setSamplingInterval', { interval: 100 }); // 100us
  await client.send('Profiler.start');

  // Collect memory snapshots
  const memSnapshots = [];
  const memInterval = setInterval(async () => {
    try {
      const metrics = await page.metrics();
      memSnapshots.push({
        ts: Date.now(),
        jsHeapUsed: metrics.JSHeapUsedSize,
        jsHeapTotal: metrics.JSHeapTotalSize,
      });
    } catch {}
  }, 1000);

  // Click start server button
  await page.click('#btn-start').catch(() => {});
  console.log('Server started. Profiling...');

  // Wait for duration
  await new Promise(r => setTimeout(r, duration * 1000));

  // Stop profiling
  clearInterval(memInterval);

  const { profile } = await client.send('Profiler.stop');
  const cpuProfilePath = path.join(resultsDir, `cpuprofile_${ts}.cpuprofile`);
  fs.writeFileSync(cpuProfilePath, JSON.stringify(profile));
  console.log('CPU profile:', cpuProfilePath);

  await page.tracing.stop();
  console.log('Chrome trace:', tracePath);

  const memPath = path.join(resultsDir, `memory_${ts}.json`);
  fs.writeFileSync(memPath, JSON.stringify(memSnapshots, null, 2));
  console.log('Memory snapshots:', memPath, `(${memSnapshots.length} samples)`);

  // Screenshot
  await page.screenshot({
    path: path.join(resultsDir, `screenshot_${ts}.png`),
    fullPage: true,
  });

  await browser.close();
  console.log('Done.');
}

main().catch(err => {
  console.error('Fatal:', err);
  process.exit(1);
});
