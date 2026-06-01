#!/usr/bin/env bash
set -euo pipefail

# High-concurrency stress test for YCSB-B/C to expose frontend lookup/update bottlenecks.
# Iterates over multiple worker thread counts (default: 64, 128).
# Usage:
#   ./stress_test_high_concurrency_ycsb_bc.sh                       # 64+128 threads
#   WORKER_THREADS_LIST="64" ./stress_test_high_concurrency_ycsb_bc.sh  # 64 only
#   PERF_RECORD=1 ./stress_test_high_concurrency_ycsb_bc.sh         # with flame graphs

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/frontend}"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/optimization_step6/stress_high_concurrency}"
FLAMEGRAPH_DIR="/home/zhizhi.tyf/FlameGraph"

# -------- Workload scope --------
WORKLOADS=(c b)
THETAS=(${THETAS:-0.90})

# -------- Scale knobs --------
WORKING_SET_GIB="${WORKING_SET_GIB:-10.0}"
PAYLOAD_SIZE_BYTES="${PAYLOAD_SIZE_BYTES:-100}"
WARMUP_LOOKUPS="${WARMUP_LOOKUPS:-100000000}"
MEASURE_LOOKUPS="${MEASURE_LOOKUPS:-1000000000}"
WARMUP_PROGRESS_INTERVAL="${WARMUP_PROGRESS_INTERVAL:-500000}"
PROGRESS_INTERVAL="${PROGRESS_INTERVAL:-1000000}"

# -------- Memory config --------
DRAM_BP_TWO_LEVEL="${DRAM_BP_TWO_LEVEL:-2.0}"
DRAM_RC_TWO_LEVEL="${DRAM_RC_TWO_LEVEL:-4.0}"
CXL_GIB="${CXL_GIB:-3.0}"

# -------- Threading --------
WORKER_THREADS_LIST=(${WORKER_THREADS_LIST:-64})
PP_THREADS="${PP_THREADS:-4}"
CXL_PP_THREADS="${CXL_PP_THREADS:-4}"
TWO_LEVEL_ADMISSION_THREADS="${TWO_LEVEL_ADMISSION_THREADS:-2}"
FORWARD_EPOCH_THREAD="${FORWARD_EPOCH_THREAD:-1}"
SIEVE_EVICTION_THREAD="${SIEVE_EVICTION_THREAD:-2}"
RECORD_CACHE_PROMOTE_THREAD="${RECORD_CACHE_PROMOTE_THREAD:-2}"

# -------- Paths --------
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_ssd}"
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.2}"

# -------- Perf integration (optional) --------
PERF_RECORD="${PERF_RECORD:-0}"
PERF_DURATION="${PERF_DURATION:-20}"

mkdir -p "$RESULT_DIR"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

log() {
  echo "[$(date '+%F %T')] $*"
}

get_binary() {
  local wl="$1"
  echo "$BUILD_DIR/experiment_1_ycsb_${wl}"
}

get_theta_admission_tuning_flags() {
  local theta="$1"
  case "$theta" in
    0.80)
      cat <<'EOF'
--skew_threshold_ratio=0.08
--uniform_threshold_ratio=0.35
--max_per_page_visits=7000
--max_global_requests_window=2000000
--trigger_visit_histogram_update_size=1200000
EOF
      ;;
    0.85)
      cat <<'EOF'
--skew_threshold_ratio=0.09
--uniform_threshold_ratio=0.40
--max_per_page_visits=6000
--max_global_requests_window=1500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.90)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=5000
--max_global_requests_window=1000000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.95)
      cat <<'EOF'
--skew_threshold_ratio=0.12
--uniform_threshold_ratio=0.60
--max_per_page_visits=4000
--max_global_requests_window=700000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    0.99)
      cat <<'EOF'
--skew_threshold_ratio=0.15
--uniform_threshold_ratio=0.70
--max_per_page_visits=3000
--max_global_requests_window=500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
    *)
      cat <<'EOF'
--skew_threshold_ratio=0.10
--uniform_threshold_ratio=0.50
--max_per_page_visits=3000
--max_global_requests_window=500000
--trigger_visit_histogram_update_size=1000000
EOF
      ;;
  esac
}

# Preflight
for wl in "${WORKLOADS[@]}"; do
  b="$(get_binary "$wl")"
  if [[ ! -x "$b" ]]; then
    echo "[ERROR] Missing binary: $b"
    echo "[HINT] cmake --build build -- experiment_1_ycsb_b experiment_1_ycsb_c"
    exit 1
  fi
done

log "========================================================================"
log "High-concurrency stress test"
log "  workloads        = ${WORKLOADS[*]}"
log "  thetas           = ${THETAS[*]}"
log "  worker_threads   = ${WORKER_THREADS_LIST[*]}"
log "  working_set      = ${WORKING_SET_GIB} GiB"
log "  warmup_ops       = ${WARMUP_LOOKUPS}"
log "  measure_ops      = ${MEASURE_LOOKUPS}"
log "  dram_bp          = ${DRAM_BP_TWO_LEVEL} GiB"
log "  dram_rc          = ${DRAM_RC_TWO_LEVEL} GiB"
log "  cxl              = ${CXL_GIB} GiB"
log "  pp/cxl_pp        = ${PP_THREADS}/${CXL_PP_THREADS}"
log "  perf_record      = ${PERF_RECORD} (duration=${PERF_DURATION}s)"
log "========================================================================"

run_one() {
  local wl="$1" worker_threads="$2" theta="$3"
  local bin result_file

  bin="$(get_binary "$wl")"
  result_file="$RESULT_DIR/result_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.csv"

  local theta_tuning_flags=()
  while IFS= read -r line; do
    [[ -n "$line" ]] && theta_tuning_flags+=("$line")
  done < <(get_theta_admission_tuning_flags "$theta")

  log "RUN wl=YCSB-${wl^^} workers=${worker_threads} theta=${theta} -> $result_file"

  if [[ "$PERF_RECORD" == "1" ]]; then
    local PERF_DATA="$RESULT_DIR/perf_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.data"
    local SVG_FILE="$RESULT_DIR/flamegraph_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.svg"

    "$bin" \
      --test_admission_mode=two_level \
      --test_zipf_theta="$theta" \
      --cxl_tiering_enabled=true \
      --cxl_gib="$CXL_GIB" \
      --cxl_dax_device_path="$CXL_DAX_DEVICE" \
      --pp_threads="$PP_THREADS" \
      --cxl_pp_threads="$CXL_PP_THREADS" \
      --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
      --delay_admission_recordcache_threads_start=true \
      --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
      --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
      --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
      --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
      --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --worker_threads="$worker_threads" \
      --vi=true \
      --test_warmup_lookups="$WARMUP_LOOKUPS" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --ssd_path="$SSD_PATH" \
      --trunc=true \
      --wal=true \
      "${theta_tuning_flags[@]}" \
      > "$result_file" 2>&1 &

    local BIN_PID=$!
    log "  Binary PID=$BIN_PID, waiting for measurement phase..."

    local MAX_WAIT=600 WAITED=0
    while ! grep -q "Phase 3:" "$result_file" 2>/dev/null; do
      sleep 2
      WAITED=$((WAITED + 2))
      if [[ $WAITED -ge $MAX_WAIT ]]; then
        log "  [ERROR] Timeout (${MAX_WAIT}s) waiting for measurement phase"
        kill $BIN_PID 2>/dev/null || true
        tail -30 "$result_file"
        return 1
      fi
      if ! kill -0 $BIN_PID 2>/dev/null; then
        log "  [ERROR] Binary exited before measurement phase"
        tail -30 "$result_file"
        return 1
      fi
    done

    log "  Measurement phase started. Settling 5s before perf..."
    sleep 5

    log "  perf record -g -F 997 for ${PERF_DURATION}s (PID=$BIN_PID)..."
    perf record -g -F 997 -p "$BIN_PID" -o "$PERF_DATA" -- sleep "$PERF_DURATION" 2>/dev/null || true

    log "  Waiting for binary to finish..."
    wait $BIN_PID 2>/dev/null || true

    log "  === Final summary ==="
    grep -E "Final Summary|mode=two_level|Measured Phase Summary|throughput=|Mqps" "$result_file" | tail -10

    if [[ -f "$PERF_DATA" ]] && command -v perf &>/dev/null; then
      log "  Generating flame graph..."
      perf script -i "$PERF_DATA" 2>/dev/null | \
        "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null | \
        "$FLAMEGRAPH_DIR/flamegraph.pl" --title "YCSB-${wl^^} w=${worker_threads} theta=${theta}" \
        > "$SVG_FILE" 2>/dev/null || true

      log "  Perf top-30:"
      perf report -i "$PERF_DATA" --stdio --no-children -n 2>/dev/null | head -60 || true

      log "  Flame graph: $SVG_FILE"
      log "  Perf data:   $PERF_DATA"
    fi
  else
    local exit_code=0
    {
      "$bin" \
        --test_admission_mode=two_level \
        --test_zipf_theta="$theta" \
        --cxl_tiering_enabled=true \
        --cxl_gib="$CXL_GIB" \
        --cxl_dax_device_path="$CXL_DAX_DEVICE" \
        --pp_threads="$PP_THREADS" \
        --cxl_pp_threads="$CXL_PP_THREADS" \
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
        --delay_admission_recordcache_threads_start=true \
        --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
        --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
        --test_working_set_gib="$WORKING_SET_GIB" \
        --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
        --worker_threads="$worker_threads" \
        --vi=true \
        --test_warmup_lookups="$WARMUP_LOOKUPS" \
        --test_measure_lookups="$MEASURE_LOOKUPS" \
        --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
        --test_progress_interval="$PROGRESS_INTERVAL" \
        --ssd_path="$SSD_PATH" \
        --trunc=true \
        --wal=true \
        "${theta_tuning_flags[@]}" \
        2>&1
    } | tee "$result_file" || exit_code=$?

    if [[ "$exit_code" -ne 0 ]]; then
      log "WARN: wl=$wl workers=$worker_threads theta=$theta exit_code=$exit_code"
    fi
  fi
}

for theta in "${THETAS[@]}"; do
  for wt in "${WORKER_THREADS_LIST[@]}"; do
    for wl in "${WORKLOADS[@]}"; do
      run_one "$wl" "$wt" "$theta" || log "SKIP: wl=$wl workers=$wt theta=$theta failed, continuing..."
      log "Cooldown 15s..."
      sleep 15
    done
  done
done

log "========================================================================"
log "Done. Results in: $RESULT_DIR"
log "========================================================================"
