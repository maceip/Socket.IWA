#!/bin/bash
# run_stress_suite.sh — Orchestrate a full stress test suite against the QUIC echo server.
#
# Runs multiple test scenarios with varying:
#   - Concurrent connections
#   - Payload sizes
#   - Packet loss rates
#   - Duration
#
# Prerequisites:
#   - ngtcp2 client (native) built with wolfSSL (see native-baseline/build_native.sh)
#   - h2load with HTTP/3 support (optional, for HTTP/3 benchmarking)
#   - sudo access for tc/netem
#   - QUIC echo server already running (WASM or native, depending on target)
#
# Usage:
#   ./run_stress_suite.sh [--target wasm|native] [--host 127.0.0.1] [--port 4433]

set -euo pipefail

# ── Configuration ──

TARGET="${TARGET:-wasm}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-4433}"
RESULTS_DIR="${RESULTS_DIR:-$(dirname "$0")/../results/$(date +%Y%m%d_%H%M%S)_${TARGET}}"
CERT_HASH="${CERT_HASH:-}"  # base64 cert hash for the server's self-signed cert
NGTCP2_CLIENT="${NGTCP2_CLIENT:-/usr/local/bin/ngtcp2_client}"
H2LOAD="${H2LOAD:-h2load}"
NETEM_SCRIPT="$(dirname "$0")/netem_loss.sh"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --target) TARGET="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --cert-hash) CERT_HASH="$2"; shift 2 ;;
        --results) RESULTS_DIR="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$RESULTS_DIR"

echo "============================================"
echo " QUIC Stress Test Suite"
echo " Target:  $TARGET"
echo " Server:  $HOST:$PORT"
echo " Results: $RESULTS_DIR"
echo "============================================"
echo ""

# ── Helper functions ──

timestamp() {
    date +%s%N
}

log() {
    echo "[$(date +%H:%M:%S)] $*"
}

# Collect system metrics during a test run
start_metrics() {
    local label="$1"
    local metrics_file="$RESULTS_DIR/${label}_metrics.csv"

    echo "timestamp_ms,cpu_user,cpu_sys,cpu_idle,mem_used_kb,mem_free_kb" > "$metrics_file"

    # Background metrics collector: sample every 100ms
    (
        while true; do
            local ts=$(date +%s%3N)
            # CPU from /proc/stat (instantaneous delta would need two samples)
            local cpu_line=$(head -1 /proc/stat)
            local user=$(echo "$cpu_line" | awk '{print $2}')
            local sys=$(echo "$cpu_line" | awk '{print $4}')
            local idle=$(echo "$cpu_line" | awk '{print $5}')
            local mem_used=$(grep MemTotal /proc/meminfo | awk '{print $2}')
            local mem_free=$(grep MemAvailable /proc/meminfo | awk '{print $2}')
            echo "$ts,$user,$sys,$idle,$mem_used,$mem_free" >> "$metrics_file"
            sleep 0.1
        done
    ) &
    echo $!
}

stop_metrics() {
    local pid="$1"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

# Run a single ngtcp2 echo test and record results
run_echo_test() {
    local label="$1"
    local payload_size="$2"
    local num_requests="$3"
    local concurrent="$4"
    local duration_sec="${5:-0}"

    log "Running: $label (payload=${payload_size}B, reqs=${num_requests}, conc=${concurrent})"

    local output_file="$RESULTS_DIR/${label}_output.txt"
    local timing_file="$RESULTS_DIR/${label}_timing.csv"

    echo "request_id,start_ns,end_ns,latency_us,bytes_sent,bytes_received,status" > "$timing_file"

    # Generate payload
    local payload_file=$(mktemp)
    dd if=/dev/urandom bs="$payload_size" count=1 of="$payload_file" 2>/dev/null

    # Start metrics collection
    local metrics_pid
    metrics_pid=$(start_metrics "$label")

    local test_start=$(timestamp)

    # Use ngtcp2 example client if available, otherwise use a custom loop
    # The ngtcp2 examples/client can do echo with the "echo" ALPN
    if command -v "$NGTCP2_CLIENT" &>/dev/null; then
        # ngtcp2 client supports --data flag for sending data on a stream
        for i in $(seq 1 "$num_requests"); do
            local req_start=$(timestamp)
            "$NGTCP2_CLIENT" \
                --exit-on-first-stream-close \
                --no-quic-dump \
                --no-http-dump \
                --data "$payload_file" \
                "$HOST" "$PORT" 2>"$output_file.${i}" || true
            local req_end=$(timestamp)
            local latency_us=$(( (req_end - req_start) / 1000 ))
            local bytes_recv=$(wc -c < "$output_file.${i}" 2>/dev/null || echo 0)
            echo "$i,$req_start,$req_end,$latency_us,$payload_size,$bytes_recv,ok" >> "$timing_file"
        done &

        # If concurrent > 1, launch more in parallel
        local bg_pids=()
        for c in $(seq 2 "$concurrent"); do
            (
                for i in $(seq 1 "$num_requests"); do
                    "$NGTCP2_CLIENT" \
                        --exit-on-first-stream-close \
                        --no-quic-dump \
                        --no-http-dump \
                        --data "$payload_file" \
                        "$HOST" "$PORT" 2>/dev/null || true
                done
            ) &
            bg_pids+=($!)
        done

        # Wait for all
        wait "${bg_pids[@]}" 2>/dev/null || true
    else
        log "WARNING: ngtcp2 client not found at $NGTCP2_CLIENT"
        log "Using UDP raw echo test instead..."

        # Fallback: raw UDP ping (not QUIC, just measures UDP round-trip)
        for i in $(seq 1 "$num_requests"); do
            local req_start=$(timestamp)
            echo "PING$i" | nc -u -w1 "$HOST" "$PORT" > /dev/null 2>&1 || true
            local req_end=$(timestamp)
            local latency_us=$(( (req_end - req_start) / 1000 ))
            echo "$i,$req_start,$req_end,$latency_us,${#i},0,raw_udp" >> "$timing_file"
        done
    fi

    local test_end=$(timestamp)
    local total_ms=$(( (test_end - test_start) / 1000000 ))

    stop_metrics "$metrics_pid"
    rm -f "$payload_file"

    log "  Completed in ${total_ms}ms"

    # Generate summary
    python3 - "$timing_file" "$label" "$RESULTS_DIR" <<'PYEOF'
import sys, csv, statistics

timing_file, label, results_dir = sys.argv[1], sys.argv[2], sys.argv[3]

latencies = []
with open(timing_file) as f:
    reader = csv.DictReader(f)
    for row in reader:
        latencies.append(int(row['latency_us']))

if not latencies:
    print(f"  No data for {label}")
    sys.exit(0)

latencies.sort()
n = len(latencies)
summary = {
    'label': label,
    'count': n,
    'min_us': min(latencies),
    'max_us': max(latencies),
    'mean_us': int(statistics.mean(latencies)),
    'median_us': int(statistics.median(latencies)),
    'p50_us': latencies[int(n * 0.50)],
    'p95_us': latencies[int(n * 0.95)] if n >= 20 else latencies[-1],
    'p99_us': latencies[int(n * 0.99)] if n >= 100 else latencies[-1],
    'stddev_us': int(statistics.stdev(latencies)) if n > 1 else 0,
}

summary_file = f"{results_dir}/{label}_summary.json"
import json
with open(summary_file, 'w') as f:
    json.dump(summary, f, indent=2)

print(f"  Requests: {summary['count']}")
print(f"  Latency:  p50={summary['p50_us']}us  p95={summary['p95_us']}us  p99={summary['p99_us']}us")
print(f"  Range:    {summary['min_us']}us - {summary['max_us']}us  (stddev={summary['stddev_us']}us)")
PYEOF
}

# ── h2load HTTP/3 benchmark (if available) ──

run_h2load_test() {
    local label="$1"
    local num_requests="$2"
    local concurrent="$3"
    local duration_sec="${4:-0}"

    if ! command -v "$H2LOAD" &>/dev/null; then
        log "h2load not found, skipping HTTP/3 benchmark"
        return 0
    fi

    log "Running h2load: $label (reqs=$num_requests, conc=$concurrent)"

    local output_file="$RESULTS_DIR/${label}_h2load.txt"
    local metrics_pid
    metrics_pid=$(start_metrics "${label}_h2load")

    local h2load_args=(
        --npn-list h3
        --requests "$num_requests"
        --clients "$concurrent"
        --threads "$(( concurrent < $(nproc) ? concurrent : $(nproc) ))"
    )

    if [ "$duration_sec" -gt 0 ]; then
        h2load_args+=(--duration "$duration_sec")
    fi

    # h2load with QUIC needs the URI scheme to be https
    "$H2LOAD" "${h2load_args[@]}" "https://${HOST}:${PORT}/" 2>&1 | tee "$output_file"

    stop_metrics "$metrics_pid"
    log "  h2load results saved to $output_file"
}

# ── Test Scenarios ──

# Scenario 1: Baseline (no impairment)
run_scenario_baseline() {
    log ""
    log "========== SCENARIO: Baseline (no packet loss) =========="
    sudo "$NETEM_SCRIPT" stop 2>/dev/null || true

    run_echo_test "baseline_small_1conn"   64    100 1
    run_echo_test "baseline_medium_1conn"  1024  100 1
    run_echo_test "baseline_large_1conn"   8192  50  1
    run_echo_test "baseline_small_4conn"   64    100 4
    run_echo_test "baseline_medium_4conn"  1024  100 4
}

# Scenario 2: Mild loss (1%)
run_scenario_mild_loss() {
    log ""
    log "========== SCENARIO: Mild loss (1%) =========="
    sudo "$NETEM_SCRIPT" start 1

    run_echo_test "loss1pct_small_1conn"   64    100 1
    run_echo_test "loss1pct_medium_1conn"  1024  100 1
    run_echo_test "loss1pct_large_1conn"   8192  50  1

    sudo "$NETEM_SCRIPT" stop
}

# Scenario 3: Moderate loss (5%)
run_scenario_moderate_loss() {
    log ""
    log "========== SCENARIO: Moderate loss (5%) =========="
    sudo "$NETEM_SCRIPT" start 5

    run_echo_test "loss5pct_small_1conn"   64    100 1
    run_echo_test "loss5pct_medium_1conn"  1024  100 1
    run_echo_test "loss5pct_large_1conn"   8192  50  1

    sudo "$NETEM_SCRIPT" stop
}

# Scenario 4: Heavy loss (10%)
run_scenario_heavy_loss() {
    log ""
    log "========== SCENARIO: Heavy loss (10%) =========="
    sudo "$NETEM_SCRIPT" start 10

    run_echo_test "loss10pct_small_1conn"  64    100 1
    run_echo_test "loss10pct_medium_1conn" 1024  100 1

    sudo "$NETEM_SCRIPT" stop
}

# Scenario 5: Extreme loss (25%) — QUIC should still survive
run_scenario_extreme_loss() {
    log ""
    log "========== SCENARIO: Extreme loss (25%) =========="
    sudo "$NETEM_SCRIPT" start 25

    run_echo_test "loss25pct_small_1conn"  64    50  1
    run_echo_test "loss25pct_medium_1conn" 1024  50  1

    sudo "$NETEM_SCRIPT" stop
}

# Scenario 6: Realistic WAN (20ms delay + 5ms jitter + 2% loss)
run_scenario_wan() {
    log ""
    log "========== SCENARIO: Simulated WAN (20ms, 5ms jitter, 2% loss) =========="
    sudo "$NETEM_SCRIPT" start 2 20 5 25

    run_echo_test "wan_small_1conn"   64    100 1
    run_echo_test "wan_medium_1conn"  1024  100 1
    run_echo_test "wan_large_1conn"   8192  50  1
    run_echo_test "wan_medium_4conn"  1024  100 4

    sudo "$NETEM_SCRIPT" stop
}

# Scenario 7: Burst traffic (rapid-fire small packets)
run_scenario_burst() {
    log ""
    log "========== SCENARIO: Burst traffic (1000 small packets) =========="
    sudo "$NETEM_SCRIPT" stop 2>/dev/null || true

    run_echo_test "burst_tiny_1conn"  16   1000 1
    run_echo_test "burst_tiny_8conn"  16   500  8
}

# ── Run all scenarios ──

run_scenario_baseline
run_scenario_mild_loss
run_scenario_moderate_loss
run_scenario_heavy_loss
run_scenario_extreme_loss
run_scenario_wan
run_scenario_burst

# ── Ensure netem is cleaned up ──
sudo "$NETEM_SCRIPT" stop 2>/dev/null || true

# ── Generate final report ──
log ""
log "========== Generating Report =========="

python3 - "$RESULTS_DIR" <<'PYEOF'
import os, json, glob

results_dir = os.sys.argv[1]
summaries = []

for f in sorted(glob.glob(os.path.join(results_dir, '*_summary.json'))):
    with open(f) as fh:
        summaries.append(json.load(fh))

if not summaries:
    print("No summary files found.")
    os.sys.exit(0)

# Print table
print(f"\n{'Label':<35} {'Count':>6} {'p50(us)':>10} {'p95(us)':>10} {'p99(us)':>10} {'stddev':>10}")
print("-" * 90)
for s in summaries:
    print(f"{s['label']:<35} {s['count']:>6} {s['p50_us']:>10} {s['p95_us']:>10} {s['p99_us']:>10} {s['stddev_us']:>10}")

# Write combined JSON
combined_file = os.path.join(results_dir, 'combined_results.json')
with open(combined_file, 'w') as f:
    json.dump(summaries, f, indent=2)

print(f"\nCombined results: {combined_file}")
PYEOF

log ""
log "All results saved to: $RESULTS_DIR"
log "Done."
