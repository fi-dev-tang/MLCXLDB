#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Stress test with TUNED CMS parameters for 10 GiB working set.
#
# Key changes from stress_test_high_concurrency_ycsb_bc.sh:
#   1. Record CMS: 4×64K → 8×2M  (noise floor 0.25 << threshold 4)
#   2. trigger_visit_histogram_update_size: 1M → 5M (scale with working set)
#   3. max_sampled_page_ids: 4096 → 16384 (better sampling coverage)
#   4. max_per_page_visits: 5000 → 15000 (scale with trigger window)
#   5. max_global_requests_window: 1M → 5M (match trigger window)
#
# Usage:
#   ./stress_test_cms_tuned_ycsb_bc.sh
#   WORKING_SET_GIB=20.0 ./stress_test_cms_tuned_ycsb_bc.sh
#   PERF_RECORD=1 ./stress_test_cms_tuned_ycsb_bc.sh
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/frontend}"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/optimization_step5/stress_cms_tuned}"
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
WORKER_THREADS_LIST=(${WORKER_THREADS_LIST:-64 128})
PP_THREADS="${PP_THREADS:-4}"
CXL_PP_THREADS="${CXL_PP_THREADS:-4}"
TWO_LEVEL_ADMISSION_THREADS="${TWO_LEVEL_ADMISSION_THREADS:-2}"
FORWARD_EPOCH_THREAD="${FORWARD_EPOCH_THREAD:-1}"
SIEVE_EVICTION_THREAD="${SIEVE_EVICTION_THREAD:-2}"
RECORD_CACHE_PROMOTE_THREAD="${RECORD_CACHE_PROMOTE_THREAD:-2}"

# -------- Paths --------
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison}"
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.2}"

# -------- Perf integration (optional) --------
PERF_RECORD="${PERF_RECORD:-0}"
PERF_DURATION="${PERF_DURATION:-20}"

# ============================================================================
# CMS and Admission Tuning — computed from WORKING_SET_GIB
# ============================================================================
#
# Formula (see cms_parameter_sensitivity.md):
#   PAGE_SIZE = 16384
#   N_pages = WORKING_SET_GIB * 1024^3 / PAGE_SIZE
#   N_records = N_pages * records_per_page (≈56)
#
#   Record CMS cols = max(2M, N_records / 8)   (target noise < 1.0 per epoch)
#   Record CMS rows = 8                        (min-of-8 further suppresses noise)
#   trigger_window  = 1M * (N_pages / 131072)  (constant visits/page density)
#   max_sampled_page_ids = min(N_pages/4, 16384)
#   max_per_page_visits = trigger_window * avg_top_page_visits_fraction
# ============================================================================

compute_tuning_params() {
  local ws_gib="$1"
  local theta="$2"

  python3 -c "
import math

ws_gib = ${ws_gib}
theta = ${theta}
PAGE_SIZE = 16384
RECORDS_PER_PAGE = 56
REFERENCE_PAGES = 131072  # 2 GiB baseline

n_pages = int(ws_gib * 1024**3 / PAGE_SIZE)
n_records = n_pages * RECORDS_PER_PAGE

# Trigger window: scale linearly with working set to maintain constant visits/page density
trigger_window = max(1000000, int(1000000 * n_pages / REFERENCE_PAGES))

# Record CMS sizing: target steady-state noise < 2.0 (safe margin below threshold ~4)
# noise = 2 * trigger_window / cols < 2.0  →  cols > trigger_window
# Use 4 rows (practical memory) with cols = 2 * trigger_window (noise = 1.0)
# Memory: 4 * cols * 8B = 32 * trigger_window bytes
record_cms_cols = max(2 * 1024 * 1024, trigger_window * 2)
record_cms_rows = 4

# Page CMS: keep at 12×1M (already fine for all practical working sets)
page_cms_rows = 12
page_cms_cols = 1024 * 1024

# Sampling: scale with working set, cap at 16384
max_sampled = min(16384, max(4096, n_pages // 40))

# max_per_page_visits: scale with trigger_window
# In one window, top page gets W/H(N,θ) visits.
# Condition 1 fires when per_page_visits >= this value without skew.
max_per_page_visits = int(5000 * trigger_window / 1000000)

# max_global_requests_window: match trigger_window
max_global_requests_window = trigger_window

memory_mb = record_cms_rows * record_cms_cols * 8 / 1024 / 1024

print(f'RECORD_CMS_ROWS={record_cms_rows}')
print(f'RECORD_CMS_COLS={record_cms_cols}')
print(f'PAGE_CMS_ROWS={page_cms_rows}')
print(f'PAGE_CMS_COLS={page_cms_cols}')
print(f'TRIGGER_WINDOW={trigger_window}')
print(f'MAX_SAMPLED_PAGES={max_sampled}')
print(f'MAX_PER_PAGE_VISITS={max_per_page_visits}')
print(f'MAX_GLOBAL_REQUESTS_WINDOW={max_global_requests_window}')
print(f'N_PAGES={n_pages}')
print(f'N_RECORDS={n_records}')
print(f'EXPECTED_NOISE={2.0 * trigger_window / record_cms_cols:.3f}')
print(f'RECORD_CMS_MEMORY_MB={memory_mb:.0f}')
"
}

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
  # Base skew/uniform ratios (theta-dependent, independent of scale)
  case "$theta" in
    0.80) echo "--skew_threshold_ratio=0.08 --uniform_threshold_ratio=0.35" ;;
    0.85) echo "--skew_threshold_ratio=0.09 --uniform_threshold_ratio=0.40" ;;
    0.90) echo "--skew_threshold_ratio=0.10 --uniform_threshold_ratio=0.50" ;;
    0.95) echo "--skew_threshold_ratio=0.12 --uniform_threshold_ratio=0.60" ;;
    0.99) echo "--skew_threshold_ratio=0.15 --uniform_threshold_ratio=0.70" ;;
    *)    echo "--skew_threshold_ratio=0.10 --uniform_threshold_ratio=0.50" ;;
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

# Compute tuning parameters
log "Computing CMS tuning parameters for working_set=${WORKING_SET_GIB} GiB, theta=${THETAS[0]}..."
eval "$(compute_tuning_params "$WORKING_SET_GIB" "${THETAS[0]}")"

log "========================================================================"
log "CMS-Tuned Stress Test"
log "========================================================================"
log "  workloads             = ${WORKLOADS[*]}"
log "  thetas                = ${THETAS[*]}"
log "  worker_threads        = ${WORKER_THREADS_LIST[*]}"
log "  working_set           = ${WORKING_SET_GIB} GiB (${N_PAGES} pages, ${N_RECORDS} records)"
log "  warmup_ops            = ${WARMUP_LOOKUPS}"
log "  measure_ops           = ${MEASURE_LOOKUPS}"
log "  dram_bp               = ${DRAM_BP_TWO_LEVEL} GiB"
log "  dram_rc               = ${DRAM_RC_TWO_LEVEL} GiB"
log "  cxl                   = ${CXL_GIB} GiB"
log "  --- CMS Tuning ---"
log "  record_cms            = ${RECORD_CMS_ROWS} rows x ${RECORD_CMS_COLS} cols"
log "  page_cms              = ${PAGE_CMS_ROWS} rows x ${PAGE_CMS_COLS} cols"
log "  trigger_window        = ${TRIGGER_WINDOW}"
log "  max_sampled_pages     = ${MAX_SAMPLED_PAGES}"
log "  max_per_page_visits   = ${MAX_PER_PAGE_VISITS}"
log "  max_global_req_window = ${MAX_GLOBAL_REQUESTS_WINDOW}"
log "  expected CMS noise    = ${EXPECTED_NOISE} (target < 2.0)"
log "  record CMS memory     = ${RECORD_CMS_MEMORY_MB} MB"
log "  perf_record           = ${PERF_RECORD}"
log "========================================================================"

run_one() {
  local wl="$1" worker_threads="$2" theta="$3"
  local bin result_file

  bin="$(get_binary "$wl")"
  result_file="$RESULT_DIR/result_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.csv"

  local theta_ratio_flags
  theta_ratio_flags="$(get_theta_admission_tuning_flags "$theta")"

  log "RUN wl=YCSB-${wl^^} workers=${worker_threads} theta=${theta} -> $result_file"

  local CMD=(
    "$bin"
    --test_admission_mode=two_level
    --test_zipf_theta="$theta"
    --cxl_tiering_enabled=true
    --cxl_gib="$CXL_GIB"
    --cxl_dax_device_path="$CXL_DAX_DEVICE"
    --pp_threads="$PP_THREADS"
    --cxl_pp_threads="$CXL_PP_THREADS"
    --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
    --delay_admission_recordcache_threads_start=true
    --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL"
    --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL"
    --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
    --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
    --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
    --test_working_set_gib="$WORKING_SET_GIB"
    --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES"
    --worker_threads="$worker_threads"
    --vi=true
    --test_warmup_lookups="$WARMUP_LOOKUPS"
    --test_measure_lookups="$MEASURE_LOOKUPS"
    --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL"
    --test_progress_interval="$PROGRESS_INTERVAL"
    --ssd_path="$SSD_PATH"
    --trunc=true
    --wal=true
    # --- CMS tuning flags ---
    --record_cms_row_num="$RECORD_CMS_ROWS"
    --record_cms_col_num="$RECORD_CMS_COLS"
    --page_cms_row_num="$PAGE_CMS_ROWS"
    --page_cms_col_num="$PAGE_CMS_COLS"
    --trigger_visit_histogram_update_size="$TRIGGER_WINDOW"
    --max_sampled_page_ids="$MAX_SAMPLED_PAGES"
    --max_per_page_visits="$MAX_PER_PAGE_VISITS"
    --max_global_requests_window="$MAX_GLOBAL_REQUESTS_WINDOW"
    # --- Theta-dependent ratio flags ---
    $theta_ratio_flags
  )

  if [[ "$PERF_RECORD" == "1" ]]; then
    local PERF_DATA="$RESULT_DIR/perf_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.data"
    local SVG_FILE="$RESULT_DIR/flamegraph_ycsb${wl}_w${worker_threads}_theta${theta}_${TIMESTAMP}.svg"

    "${CMD[@]}" > "$result_file" 2>&1 &
    local BIN_PID=$!
    log "  Binary PID=$BIN_PID, waiting for measurement phase..."

    local MAX_WAIT=900 WAITED=0
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
    grep -E "Final Summary|mode=two_level|RC State" "$result_file" | tail -5

    if [[ -f "$PERF_DATA" ]] && command -v perf &>/dev/null; then
      log "  Generating flame graph..."
      perf script -i "$PERF_DATA" 2>/dev/null | \
        "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" 2>/dev/null | \
        "$FLAMEGRAPH_DIR/flamegraph.pl" --title "YCSB-${wl^^} w=${worker_threads} theta=${theta} (CMS-tuned)" \
        > "$SVG_FILE" 2>/dev/null || true
      log "  Flame graph: $SVG_FILE"
    fi
  else
    local exit_code=0
    { "${CMD[@]}" 2>&1; } | tee "$result_file" || exit_code=$?

    if [[ "$exit_code" -ne 0 ]]; then
      log "WARN: wl=$wl workers=$worker_threads theta=$theta exit_code=$exit_code"
    fi
  fi

  # Print key results
  log "  === Key metrics ==="
  grep -E "RC State|Final Summary|mode=two_level" "$result_file" | tail -5
  echo ""
}

for theta in "${THETAS[@]}"; do
  # Recompute tuning for this theta (in case THETAS has multiple values)
  eval "$(compute_tuning_params "$WORKING_SET_GIB" "$theta")"

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
