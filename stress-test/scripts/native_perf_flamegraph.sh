#!/bin/bash
# native_perf_flamegraph.sh — Generate a Linux perf flame graph for the native QUIC server.
#
# Usage:
#   sudo ./native_perf_flamegraph.sh <server_pid> [duration_sec]
#
# Prerequisites:
#   - linux-tools (perf) installed
#   - FlameGraph tools (Brendan Gregg's FlameGraph repo, clone to /opt/FlameGraph)
#   - Server running with debug symbols (build with -g -O2)
#
# Output:
#   results/native_flamegraph_<timestamp>.svg

set -euo pipefail

PID="${1:?Usage: $0 <server_pid> [duration_sec]}"
DURATION="${2:-30}"
RESULTS_DIR="$(dirname "$0")/../results"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-/opt/FlameGraph}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

echo "=== Native perf Flame Graph ==="
echo "  PID:      $PID"
echo "  Duration: ${DURATION}s"
echo ""

# Verify FlameGraph tools exist
if [ ! -f "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]; then
    echo "FlameGraph tools not found at $FLAMEGRAPH_DIR"
    echo "Install with: git clone brendangregg/FlameGraph repo to $FLAMEGRAPH_DIR"
    exit 1
fi

# Record
echo "Recording perf data for ${DURATION}s..."
perf record -F 997 -g -p "$PID" -o "$RESULTS_DIR/perf_${TIMESTAMP}.data" -- sleep "$DURATION"

# Generate flame graph
echo "Generating flame graph..."
perf script -i "$RESULTS_DIR/perf_${TIMESTAMP}.data" > "$RESULTS_DIR/perf_${TIMESTAMP}.stacks"

"$FLAMEGRAPH_DIR/stackcollapse-perf.pl" "$RESULTS_DIR/perf_${TIMESTAMP}.stacks" \
    > "$RESULTS_DIR/perf_${TIMESTAMP}.collapsed"

"$FLAMEGRAPH_DIR/flamegraph.pl" \
    --title "Native QUIC Server (ngtcp2 + wolfSSL)" \
    --subtitle "pid=$PID, ${DURATION}s sample" \
    --colors hot \
    --width 1200 \
    "$RESULTS_DIR/perf_${TIMESTAMP}.collapsed" \
    > "$RESULTS_DIR/native_flamegraph_${TIMESTAMP}.svg"

echo ""
echo "Flame graph: $RESULTS_DIR/native_flamegraph_${TIMESTAMP}.svg"
echo "Raw data:    $RESULTS_DIR/perf_${TIMESTAMP}.data"
echo "Collapsed:   $RESULTS_DIR/perf_${TIMESTAMP}.collapsed"

# Also generate a differential-ready collapsed file
echo ""
echo "For differential flame graph (WASM vs Native), use:"
echo "  $FLAMEGRAPH_DIR/difffolded.pl native.collapsed wasm.collapsed | $FLAMEGRAPH_DIR/flamegraph.pl > diff.svg"
