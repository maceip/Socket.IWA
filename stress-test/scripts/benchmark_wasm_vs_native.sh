#!/bin/bash
# benchmark_wasm_vs_native.sh — Compare WASM vs Native QUIC echo server performance.
#
# Measures:
#   1. UDP packet handling throughput (packets/sec the server can ingest)
#   2. Server CPU usage during traffic
#   3. Memory usage
#
# Usage:
#   bash benchmark_wasm_vs_native.sh
#
# Output: results/benchmark_<timestamp>/

set -euo pipefail

SRCDIR="$(cd "$(dirname "$0")/../.." && pwd)"
NATIVE_BIN="$SRCDIR/stress-test/native-baseline/build/quic_echo_server_native"
FLOOD_SCRIPT="$SRCDIR/stress-test/scripts/quic_flood.py"
RESULTS_BASE="$SRCDIR/stress-test/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_DIR="$RESULTS_BASE/benchmark_${TIMESTAMP}"
HOST="127.0.0.1"
PORT=4433

mkdir -p "$RESULTS_DIR"

echo "╔══════════════════════════════════════════════════╗"
echo "║   QUIC Echo Server Benchmark: WASM vs Native    ║"
echo "╚══════════════════════════════════════════════════╝"
echo ""
echo "Results: $RESULTS_DIR"
echo ""

# ── Helper: collect CPU/memory of a PID every 200ms ──
collect_metrics() {
    local pid="$1"
    local outfile="$2"
    echo "ts_ms,cpu_pct,rss_kb,vsz_kb" > "$outfile"
    while kill -0 "$pid" 2>/dev/null; do
        local ts=$(date +%s%3N)
        # ps gives cumulative CPU %, RSS (resident), VSZ (virtual)
        local stats=$(ps -p "$pid" -o %cpu=,rss=,vsz= 2>/dev/null || echo "0 0 0")
        local cpu=$(echo "$stats" | awk '{print $1}')
        local rss=$(echo "$stats" | awk '{print $2}')
        local vsz=$(echo "$stats" | awk '{print $3}')
        echo "$ts,$cpu,$rss,$vsz" >> "$outfile"
        sleep 0.2
    done
}

# ── Helper: wait for server to be ready ──
wait_for_server() {
    local max_wait=10
    for i in $(seq 1 $max_wait); do
        if ss -uln | grep -q ":${PORT} " 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done
    echo "WARNING: Server may not be listening on port $PORT"
    return 1
}

# ── Helper: run a single flood test ──
run_flood_test() {
    local label="$1"
    local mode="$2"
    local extra_args="$3"
    local server_pid="$4"

    echo "  [$label] Running flood: mode=$mode $extra_args"

    # Start metrics collection
    collect_metrics "$server_pid" "$RESULTS_DIR/${label}_metrics.csv" &
    local metrics_pid=$!

    # Run the flood
    python3 "$FLOOD_SCRIPT" \
        --host "$HOST" --port "$PORT" \
        --mode "$mode" $extra_args \
        --output "$RESULTS_DIR/${label}_flood.json" \
        2>&1 | sed 's/^/    /'

    # Stop metrics
    sleep 0.5
    kill "$metrics_pid" 2>/dev/null || true
    wait "$metrics_pid" 2>/dev/null || true
}

# ── Helper: summarize metrics CSV ──
summarize_metrics() {
    local csvfile="$1"
    python3 - "$csvfile" <<'PYEOF'
import sys, csv
f = sys.argv[1]
cpus, rss_vals = [], []
with open(f) as fh:
    reader = csv.DictReader(fh)
    for row in reader:
        try:
            cpus.append(float(row['cpu_pct']))
            rss_vals.append(int(row['rss_kb']))
        except (ValueError, KeyError):
            pass
if cpus:
    print(f"    CPU:  avg={sum(cpus)/len(cpus):.1f}%  max={max(cpus):.1f}%")
if rss_vals:
    print(f"    RSS:  avg={sum(rss_vals)//len(rss_vals):,}KB  max={max(rss_vals):,}KB")
PYEOF
}

# ══════════════════════════════════════════════════════
# PHASE 1: NATIVE SERVER BENCHMARK
# ══════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  PHASE 1: Native Server"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ ! -f "$NATIVE_BIN" ]; then
    echo "ERROR: Native binary not found at $NATIVE_BIN"
    echo "Run: bash stress-test/native-baseline/build_native.sh"
    exit 1
fi

# Start native server
echo "  Starting native server..."
"$NATIVE_BIN" &
NATIVE_PID=$!
sleep 1
wait_for_server || true
echo "  Server PID: $NATIVE_PID"

# Test 1: Burst — max speed, 50k packets
run_flood_test "native_burst_50k" "burst" "--packets 50000 --packet-type initial" "$NATIVE_PID"
summarize_metrics "$RESULTS_DIR/native_burst_50k_metrics.csv"

# Test 2: Constant rate — 10k pps for 10s
run_flood_test "native_const_10kpps" "constant" "--rate 10000 --duration 10 --packet-type initial" "$NATIVE_PID"
summarize_metrics "$RESULTS_DIR/native_const_10kpps_metrics.csv"

# Test 3: Ramp — 0 to 50k pps over 15s
run_flood_test "native_ramp_50k" "ramp" "--max-rate 50000 --duration 15 --packet-type initial" "$NATIVE_PID"
summarize_metrics "$RESULTS_DIR/native_ramp_50k_metrics.csv"

# Test 4: Chaos — random mix for 10s
run_flood_test "native_chaos_10s" "chaos" "--duration 10" "$NATIVE_PID"
summarize_metrics "$RESULTS_DIR/native_chaos_10s_metrics.csv"

# Test 5: Garbage — non-QUIC traffic, 30k packets
run_flood_test "native_garbage_30k" "burst" "--packets 30000 --packet-type garbage" "$NATIVE_PID"
summarize_metrics "$RESULTS_DIR/native_garbage_30k_metrics.csv"

# Collect final memory snapshot
echo ""
echo "  Native final state:"
ps -p "$NATIVE_PID" -o pid,pcpu,pmem,rss,vsz,etime 2>/dev/null | head -5 || true

# Stop native server
kill "$NATIVE_PID" 2>/dev/null || true
wait "$NATIVE_PID" 2>/dev/null || true
echo "  Native server stopped."
echo ""

# ══════════════════════════════════════════════════════
# PHASE 2: WASM SERVER BENCHMARK (via Node.js/Chrome)
# ══════════════════════════════════════════════════════

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  PHASE 2: WASM Server (Chrome IWA)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

# The WASM server runs inside Chrome as an IWA.
# We need to launch Chrome with the IWA, click start, then run flood tests.
# We'll use puppeteer for this.

SWBN_PATH="$SRCDIR/iwa-sink/iwa-sink/dist/iwa-sink.swbn"
CHROME_BIN="/home/devuser/.cache/puppeteer/chrome/linux-145.0.7632.77/chrome-linux64/chrome"
WEB_BUNDLE_ID="va5nmqd44umdnobnnp7xpxdbhjn6dlsrdgirrnsm6envbjndx2waaaic"
IWA_ORIGIN="isolated-app://${WEB_BUNDLE_ID}"

if [ ! -f "$SWBN_PATH" ]; then
    echo "ERROR: .swbn not found at $SWBN_PATH"
    echo "Build with: cd iwa-sink/iwa-sink && npm run build:swbn"
    exit 1
fi

if [ ! -f "$CHROME_BIN" ]; then
    echo "ERROR: Chrome not found at $CHROME_BIN"
    exit 1
fi

# Create a Node.js script to launch Chrome, start server, wait for signal, then exit
WASM_LAUNCHER=$(mktemp --suffix=.mjs)
cat > "$WASM_LAUNCHER" <<'LAUNCHER_EOF'
import puppeteer from 'puppeteer';
import fs from 'fs';

const swbnPath = process.argv[2];
const chromeBin = process.argv[3];
const iwaOrigin = process.argv[4];
const signalFile = process.argv[5];

async function main() {
  const userDataDir = '/tmp/iwa-bench-' + Date.now();

  const browser = await puppeteer.launch({
    headless: 'shell',
    executablePath: chromeBin,
    args: [
      '--no-sandbox',
      '--disable-setuid-sandbox',
      '--disable-gpu',
      '--enable-features=IsolatedWebApps,IsolatedWebAppDevMode',
      '--install-isolated-web-app-from-file=' + swbnPath,
      '--disable-background-timer-throttling',
      '--disable-renderer-backgrounding',
      '--js-flags=--experimental-wasm-stack-switching',
    ],
    userDataDir,
  });

  await new Promise(r => setTimeout(r, 3000));

  const page = await browser.newPage();
  await page.goto(iwaOrigin + '/quic.html', {
    waitUntil: 'networkidle2',
    timeout: 15000,
  }).catch(() => {});

  await new Promise(r => setTimeout(r, 2000));

  // Click start button
  await page.click('#btn-start').catch(() => {});
  console.log('WASM server starting...');

  // Wait for server to bind
  await new Promise(r => setTimeout(r, 3000));

  // Signal that we're ready
  fs.writeFileSync(signalFile, 'ready');
  console.log('WASM server ready, waiting for benchmark to finish...');

  // Wait for stop signal
  while (true) {
    await new Promise(r => setTimeout(r, 1000));
    try {
      const content = fs.readFileSync(signalFile, 'utf8');
      if (content.trim() === 'stop') break;
    } catch {}
  }

  // Grab console output
  const logs = await page.evaluate(() => {
    const el = document.querySelector('#log');
    return el ? el.textContent : '';
  });
  console.log('Server logs (last 500 chars):', logs.slice(-500));

  await browser.close();
  fs.rmSync(userDataDir, { recursive: true, force: true });
}

main().catch(err => {
  console.error('Chrome launcher error:', err.message);
  process.exit(1);
});
LAUNCHER_EOF

SIGNAL_FILE=$(mktemp)
echo "starting" > "$SIGNAL_FILE"

echo "  Launching Chrome with IWA..."
node "$WASM_LAUNCHER" "$SWBN_PATH" "$CHROME_BIN" "$IWA_ORIGIN" "$SIGNAL_FILE" &
CHROME_PID=$!

# Wait for ready signal
echo "  Waiting for WASM server to start..."
MAX_WAIT=30
for i in $(seq 1 $MAX_WAIT); do
    if [ -f "$SIGNAL_FILE" ] && grep -q "ready" "$SIGNAL_FILE" 2>/dev/null; then
        break
    fi
    sleep 1
done

if ! grep -q "ready" "$SIGNAL_FILE" 2>/dev/null; then
    echo "WARNING: WASM server may not have started (timed out after ${MAX_WAIT}s)"
fi

# Give it an extra moment
sleep 2
echo "  WASM server ready. Chrome PID: $CHROME_PID"

# Run the same tests against WASM
run_flood_test "wasm_burst_50k" "burst" "--packets 50000 --packet-type initial" "$CHROME_PID"
summarize_metrics "$RESULTS_DIR/wasm_burst_50k_metrics.csv" 2>/dev/null || true

run_flood_test "wasm_const_10kpps" "constant" "--rate 10000 --duration 10 --packet-type initial" "$CHROME_PID"
summarize_metrics "$RESULTS_DIR/wasm_const_10kpps_metrics.csv" 2>/dev/null || true

run_flood_test "wasm_ramp_50k" "ramp" "--max-rate 50000 --duration 15 --packet-type initial" "$CHROME_PID"
summarize_metrics "$RESULTS_DIR/wasm_ramp_50k_metrics.csv" 2>/dev/null || true

run_flood_test "wasm_chaos_10s" "chaos" "--duration 10" "$CHROME_PID"
summarize_metrics "$RESULTS_DIR/wasm_chaos_10s_metrics.csv" 2>/dev/null || true

run_flood_test "wasm_garbage_30k" "burst" "--packets 30000 --packet-type garbage" "$CHROME_PID"
summarize_metrics "$RESULTS_DIR/wasm_garbage_30k_metrics.csv" 2>/dev/null || true

echo ""
echo "  WASM final state:"
ps -p "$CHROME_PID" -o pid,pcpu,pmem,rss,vsz,etime 2>/dev/null | head -5 || true

# Stop Chrome
echo "stop" > "$SIGNAL_FILE"
sleep 3
kill "$CHROME_PID" 2>/dev/null || true
wait "$CHROME_PID" 2>/dev/null || true
echo "  WASM server stopped."
rm -f "$WASM_LAUNCHER" "$SIGNAL_FILE"

# ══════════════════════════════════════════════════════
# PHASE 3: COMPARISON REPORT
# ══════════════════════════════════════════════════════

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  COMPARISON REPORT"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

python3 - "$RESULTS_DIR" <<'PYEOF'
import os, json, glob

results_dir = os.sys.argv[1]

# Load all flood results
tests = {}
for f in sorted(glob.glob(os.path.join(results_dir, '*_flood.json'))):
    name = os.path.basename(f).replace('_flood.json', '')
    with open(f) as fh:
        tests[name] = json.load(fh)

# Load all metrics
metrics = {}
for f in sorted(glob.glob(os.path.join(results_dir, '*_metrics.csv'))):
    name = os.path.basename(f).replace('_metrics.csv', '')
    import csv
    cpus, rss_vals = [], []
    with open(f) as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                cpus.append(float(row['cpu_pct']))
                rss_vals.append(int(row['rss_kb']))
            except (ValueError, KeyError):
                pass
    metrics[name] = {
        'avg_cpu': sum(cpus)/len(cpus) if cpus else 0,
        'max_cpu': max(cpus) if cpus else 0,
        'avg_rss_kb': sum(rss_vals)//len(rss_vals) if rss_vals else 0,
        'max_rss_kb': max(rss_vals) if rss_vals else 0,
    }

# Print comparison table
print()
print(f"{'Test':<30} {'Packets':>10} {'PPS':>12} {'Mbps':>10} {'Time(s)':>8} {'AvgCPU':>8} {'MaxRSS':>10}")
print("=" * 98)

# Group by test type
test_types = ['burst_50k', 'const_10kpps', 'ramp_50k', 'chaos_10s', 'garbage_30k']
for tt in test_types:
    native_key = f'native_{tt}'
    wasm_key = f'wasm_{tt}'

    for key, label in [(native_key, 'Native'), (wasm_key, 'WASM  ')]:
        if key in tests:
            t = tests[key]
            m = metrics.get(key, {})
            print(f"  {label} {tt:<22} {t['sent']:>10,} {t['pps']:>12,.0f} {t['mbps']:>10.2f} {t['elapsed']:>8.2f} {m.get('avg_cpu',0):>7.1f}% {m.get('max_rss_kb',0):>8,}KB")

    # Print ratio if both exist
    if native_key in tests and wasm_key in tests:
        n_pps = tests[native_key]['pps']
        w_pps = tests[wasm_key]['pps']
        ratio = w_pps / n_pps * 100 if n_pps > 0 else 0
        print(f"  {'':30} WASM/Native ratio: {ratio:.1f}%")
    print()

# Save combined report
report = {
    'tests': tests,
    'metrics': metrics,
    'timestamp': os.path.basename(results_dir),
}
report_file = os.path.join(results_dir, 'benchmark_report.json')
with open(report_file, 'w') as f:
    json.dump(report, f, indent=2)
print(f"Full report: {report_file}")
PYEOF

echo ""
echo "All results saved to: $RESULTS_DIR"
echo "Done."
