#!/usr/bin/env bash
set -euo pipefail

# Perf profile YCSB-E during measure phase
SCRIPT_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/experiment_1_ycsb_full"
RESULT_DIR="$SCRIPT_DIR/optimization_step6/perf_ycsbe_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULT_DIR"

WORKING_SET_GIB=1.0
CXL_GIB=0.8
PAYLOAD_SIZE_BYTES=100
THETA=0.90

WORKER_THREADS=4
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=1
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=2

WARMUP_LOOKUPS=5000000
MEASURE_LOOKUPS=10000000
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=1000000

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd"
CXL_DAX_DEVICE="/dev/dax0.2"
BUILD_DIR="/home/zhizhi.tyf/cxl-recordcache-dev/build/frontend"

SKEW_THRESHOLD_RATIO=0.08
UNIFORM_THRESHOLD_RATIO=0.45
MAX_PER_PAGE_VISITS=8000
MAX_GLOBAL_REQUESTS_WINDOW=2000000
TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE=160000

DRAM_BP=0.25
DRAM_RC=0.1

LOG="$RESULT_DIR/ycsbe_two_level_theta${THETA}.log"
PERF_DATA="$RESULT_DIR/perf.data"
FLAMEGRAPH_SVG="$RESULT_DIR/flamegraph.svg"

echo "[INFO] Starting YCSB-E, output -> $LOG"
echo "[INFO] Result dir: $RESULT_DIR"

# Run in background, tee to log
"$BUILD_DIR/experiment_1_ycsb_e" \
    --test_admission_mode=two_level \
    --test_zipf_theta="$THETA" \
    --cxl_tiering_enabled=true \
    --cxl_gib="$CXL_GIB" \
    --cxl_dax_device_path="$CXL_DAX_DEVICE" \
    --pp_threads="$PP_THREADS" \
    --cxl_pp_threads="$CXL_PP_THREADS" \
    --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
    --delay_admission_recordcache_threads_start=true \
    --dram_buffer_pool_gib="$DRAM_BP" \
    --dram_recordcache_gib="$DRAM_RC" \
    --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
    --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
    --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
    --skew_threshold_ratio="$SKEW_THRESHOLD_RATIO" \
    --uniform_threshold_ratio="$UNIFORM_THRESHOLD_RATIO" \
    --max_per_page_visits="$MAX_PER_PAGE_VISITS" \
    --max_global_requests_window="$MAX_GLOBAL_REQUESTS_WINDOW" \
    --trigger_visit_histogram_update_size="$TRIGGER_VISIT_HISTOGRAM_UPDATE_SIZE" \
    --test_working_set_gib="$WORKING_SET_GIB" \
    --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
    --worker_threads="$WORKER_THREADS" \
    --vi=true \
    --test_warmup_lookups="$WARMUP_LOOKUPS" \
    --test_measure_lookups="$MEASURE_LOOKUPS" \
    --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
    --test_progress_interval="$PROGRESS_INTERVAL" \
    --ssd_path="$SSD_PATH" \
    --trunc=true \
    --wal=true \
    2>&1 | tee "$LOG" &

PID=$!
echo "[INFO] YCSB-E PID=$PID, waiting for measure phase..."

# Wait for Phase 3 to appear
while ! grep -q "Phase 3" "$LOG" 2>/dev/null; do
    sleep 2
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "[ERROR] YCSB-E exited before measure phase"
        exit 1
    fi
done

echo "[INFO] Measure phase detected. Sleeping 15s to let it stabilize..."
sleep 15

# Find the actual experiment process (child of tee pipe)
YCSB_PID=$(pgrep -f "experiment_1_ycsb_e.*two_level" | head -1)
if [ -z "$YCSB_PID" ]; then
    echo "[ERROR] Cannot find YCSB-E process for perf"
    exit 1
fi
echo "[INFO] Profiling PID=$YCSB_PID for 60s..."

perf record -F 99 -p "$YCSB_PID" -g -o "$PERF_DATA" -- sleep 60

echo "[INFO] Perf recording done. Waiting for YCSB-E to finish..."
wait "$PID" || true

echo ""
echo "[INFO] Generating flamegraph..."
perf script -i "$PERF_DATA" | \
    /home/zhizhi.tyf/FlameGraph/stackcollapse-perf.pl | \
    /home/zhizhi.tyf/FlameGraph/flamegraph.pl > "$FLAMEGRAPH_SVG"

echo ""
echo "=========================================="
echo "  Results"
echo "=========================================="
echo "  Log:        $LOG"
echo "  Perf data:  $PERF_DATA"
echo "  Flamegraph: $FLAMEGRAPH_SVG"
echo ""

echo "--- Top functions (perf report --stdio) ---"
perf report -i "$PERF_DATA" --stdio --no-children -n --percent-limit 1.5 2>/dev/null | head -60
