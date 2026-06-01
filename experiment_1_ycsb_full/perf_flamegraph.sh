#!/usr/bin/env bash
set -euo pipefail

# Profile YCSB-B or YCSB-C during the measurement phase and generate a flame graph.
# Usage: ./perf_flamegraph.sh <b|c> [perf_duration_seconds]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/frontend}"
FLAMEGRAPH_DIR="/home/zhizhi.tyf/FlameGraph"
RESULT_DIR="$SCRIPT_DIR/optimization_step5/perf"

WORKLOAD="${1:-b}"
PERF_DURATION="${2:-8}"

mkdir -p "$RESULT_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

BIN="$BUILD_DIR/experiment_1_ycsb_${WORKLOAD}"
if [[ ! -x "$BIN" ]]; then
  echo "[ERROR] Missing binary: $BIN"
  exit 1
fi

OUTFILE="$RESULT_DIR/run_ycsb${WORKLOAD}_${TIMESTAMP}.log"
PERF_DATA="$RESULT_DIR/perf_ycsb${WORKLOAD}_${TIMESTAMP}.data"
SVG_FILE="$RESULT_DIR/flamegraph_ycsb${WORKLOAD}_${TIMESTAMP}.svg"

echo "[perf] Starting YCSB-${WORKLOAD^^} binary..."

SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison}"
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.2}"

# Launch binary in background
"$BIN" \
  --test_admission_mode=two_level \
  --test_zipf_theta=0.90 \
  --cxl_tiering_enabled=true \
  --cxl_gib=3.0 \
  --cxl_dax_device_path="$CXL_DAX_DEVICE" \
  --pp_threads=1 \
  --cxl_pp_threads=1 \
  --two_level_admission_threads=1 \
  --delay_admission_recordcache_threads_start=true \
  --dram_buffer_pool_gib=0.25 \
  --dram_recordcache_gib=0.5 \
  --forward_epoch_thread=1 \
  --sieve_eviction_thread=1 \
  --record_cache_promote_thread=1 \
  --test_working_set_gib=2.0 \
  --test_payload_size_bytes=100 \
  --worker_threads=4 \
  --vi=true \
  --test_warmup_lookups=2000000 \
  --test_measure_lookups=50000000 \
  --test_warmup_progress_interval=50000 \
  --test_progress_interval=100000 \
  --ssd_path="$SSD_PATH" \
  --trunc=true \
  --wal=true \
  --skew_threshold_ratio=0.10 \
  --uniform_threshold_ratio=0.50 \
  --max_per_page_visits=5000 \
  --max_global_requests_window=1000000 \
  --trigger_visit_histogram_update_size=1000000 \
  > "$OUTFILE" 2>&1 &

PID=$!
echo "[perf] Binary PID=$PID, waiting for measurement phase..."

# Wait for measurement phase (look for "Phase 3:" or "Warmup done")
MAX_WAIT=120
WAITED=0
while ! grep -q "Phase 3:" "$OUTFILE" 2>/dev/null; do
  sleep 1
  WAITED=$((WAITED + 1))
  if [[ $WAITED -ge $MAX_WAIT ]]; then
    echo "[ERROR] Timeout waiting for measurement phase"
    kill $PID 2>/dev/null || true
    cat "$OUTFILE"
    exit 1
  fi
  # Check if process died
  if ! kill -0 $PID 2>/dev/null; then
    echo "[ERROR] Binary exited before measurement phase"
    cat "$OUTFILE"
    exit 1
  fi
done

echo "[perf] Measurement phase started. Sleeping 3s for warmup settle..."
sleep 3

echo "[perf] Recording for ${PERF_DURATION}s (PID=$PID)..."
perf record -g -F 997 -p "$PID" -o "$PERF_DATA" -- sleep "$PERF_DURATION" || true

echo "[perf] Waiting for binary to finish..."
wait $PID 2>/dev/null || true

echo "[perf] Generating flame graph..."
perf script -i "$PERF_DATA" | \
  "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" | \
  "$FLAMEGRAPH_DIR/flamegraph.pl" --title "YCSB-${WORKLOAD^^} (theta=0.90, two_level)" > "$SVG_FILE"

echo "[perf] Text report (top 30):"
perf report -i "$PERF_DATA" --stdio --no-children -n 2>/dev/null | head -80

echo ""
echo "[DONE] Flame graph: $SVG_FILE"
echo "[DONE] Perf data:   $PERF_DATA"
echo "[DONE] Run log:     $OUTFILE"
