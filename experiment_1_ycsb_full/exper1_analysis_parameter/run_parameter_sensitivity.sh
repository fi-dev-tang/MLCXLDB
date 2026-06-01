#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# Experiment 1: CMS Parameter Sensitivity Analysis
#
# Tests the impact of Record CMS dimensions and trigger_window on
# RecordCache fill rate, hit rate, and throughput.
#
# Constraint: /dev/dax0.2 only supports one writer → ALL runs are SERIAL.
#
# Experiment groups:
#   A: Record CMS width sweep (cols: 64K, 256K, 1M, 2M)
#   B: trigger_window sweep (500K, 1M, 5M, 10M)
#   C: Default vs recommended joint config
#
# Each group runs YCSB-C then YCSB-B.
# Total: 20 runs × ~1.5 min = ~30 min
#
# Usage:
#   ./run_parameter_sensitivity.sh
#   EXPERIMENT_GROUPS="A" ./run_parameter_sensitivity.sh          # Only run group A
#   EXPERIMENT_GROUPS="A B" ./run_parameter_sensitivity.sh        # Only A and B
#   DRY_RUN=1 ./run_parameter_sensitivity.sh           # Print commands only
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build/frontend}"
RESULT_DIR="$SCRIPT_DIR/results"

# -------- Fixed parameters --------
WORKING_SET_GIB="10.0"
PAYLOAD_SIZE_BYTES="100"
WARMUP_LOOKUPS="50000000"
MEASURE_LOOKUPS="200000000"
WARMUP_PROGRESS_INTERVAL="500000"
PROGRESS_INTERVAL="1000000"

# -------- Memory config --------
DRAM_BP="2.0"
DRAM_RC="4.0"
CXL_GIB="3.0"

# -------- Threading --------
WORKER_THREADS="64"
PP_THREADS="4"
CXL_PP_THREADS="4"
TWO_LEVEL_ADMISSION_THREADS="2"
FORWARD_EPOCH_THREAD="1"
SIEVE_EVICTION_THREAD="2"
RECORD_CACHE_PROMOTE_THREAD="2"

# -------- Paths --------
SSD_PATH="${SSD_PATH:-/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison}"
CXL_DAX_DEVICE="${CXL_DAX_DEVICE:-/dev/dax0.2}"

# -------- Control --------
EXPERIMENT_GROUPS="${EXPERIMENT_GROUPS:-A B C}"
DRY_RUN="${DRY_RUN:-0}"
COOLDOWN="${COOLDOWN:-15}"

# -------- Derived --------
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"

mkdir -p "$RESULT_DIR"

log() {
  echo "[$(date '+%F %T')] $*"
}

get_binary() {
  local wl="$1"
  echo "$BUILD_DIR/experiment_1_ycsb_${wl}"
}

# Preflight check
for wl in b c; do
  b="$(get_binary "$wl")"
  if [[ ! -x "$b" ]]; then
    echo "[ERROR] Missing binary: $b"
    echo "[HINT] cmake --build build -- experiment_1_ycsb_b experiment_1_ycsb_c"
    exit 1
  fi
done

# ============================================================================
# Core runner: executes one test configuration
# ============================================================================
run_one() {
  local group_name="$1"
  local wl="$2"              # b or c
  local record_cms_rows="$3"
  local record_cms_cols="$4"
  local trigger_window="$5"
  local extra_label="$6"     # for result file naming

  local bin result_file
  bin="$(get_binary "$wl")"
  result_file="$RESULT_DIR/result_${group_name}_ycsb${wl}_${extra_label}_${TIMESTAMP}.csv"

  # Compute associated parameters based on trigger_window
  local max_per_page_visits max_global_requests_window max_sampled_page_ids
  max_per_page_visits=$(python3 -c "print(max(3, int(5000 * ${trigger_window} / 1000000)))")
  max_global_requests_window="$trigger_window"
  max_sampled_page_ids="16384"

  log "RUN group=${group_name} wl=YCSB-${wl^^} cms=${record_cms_rows}x${record_cms_cols} window=${trigger_window} -> $(basename "$result_file")"

  if [[ "$DRY_RUN" == "1" ]]; then
    log "  [DRY_RUN] Would execute: $bin with cms=${record_cms_rows}x${record_cms_cols} window=${trigger_window}"
    return 0
  fi

  local exit_code=0
  {
    "$bin" \
      --test_admission_mode=two_level \
      --test_zipf_theta=0.90 \
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
      --record_cms_row_num="$record_cms_rows" \
      --record_cms_col_num="$record_cms_cols" \
      --page_cms_row_num=12 \
      --page_cms_col_num=1048576 \
      --trigger_visit_histogram_update_size="$trigger_window" \
      --max_sampled_page_ids="$max_sampled_page_ids" \
      --max_per_page_visits="$max_per_page_visits" \
      --max_global_requests_window="$max_global_requests_window" \
      --skew_threshold_ratio=0.10 \
      --uniform_threshold_ratio=0.50 \
      2>&1
  } > "$result_file" || exit_code=$?

  if [[ "$exit_code" -ne 0 ]]; then
    log "  WARN: exit_code=$exit_code (teardown crash is OK if data was captured)"
  fi

  # Print key result
  local final_line
  final_line=$(grep "mode=two_level" "$result_file" | tail -1)
  local rc_state
  rc_state=$(grep "RC State.*Final" "$result_file" | tail -1)
  if [[ -n "$final_line" ]]; then
    log "  $(echo "$final_line" | sed 's/\x1b\[[0-9;]*m//g' | grep -oP 'Mqps=[0-9.]+|RC_HR=[0-9.]+%'| tr '\n' ' ')"
  fi
  if [[ -n "$rc_state" ]]; then
    log "  $(echo "$rc_state" | sed 's/\x1b\[[0-9;]*m//g')"
  fi
}

run_group_workloads() {
  local group_name="$1"
  local record_cms_rows="$2"
  local record_cms_cols="$3"
  local trigger_window="$4"
  local extra_label="$5"

  # Run YCSB-C first (pure read)
  run_one "$group_name" "c" "$record_cms_rows" "$record_cms_cols" "$trigger_window" "$extra_label"
  log "Cooldown ${COOLDOWN}s..."
  [[ "$DRY_RUN" == "1" ]] || sleep "$COOLDOWN"

  # Run YCSB-B (95% read, 5% write)
  run_one "$group_name" "b" "$record_cms_rows" "$record_cms_cols" "$trigger_window" "$extra_label"
  log "Cooldown ${COOLDOWN}s..."
  [[ "$DRY_RUN" == "1" ]] || sleep "$COOLDOWN"
}

# ============================================================================
# Experiment Groups
# ============================================================================

log "========================================================================"
log "CMS Parameter Sensitivity Experiment"
log "  groups           = ${EXPERIMENT_GROUPS}"
log "  working_set      = ${WORKING_SET_GIB} GiB"
log "  worker_threads   = ${WORKER_THREADS}"
log "  warmup_ops       = ${WARMUP_LOOKUPS}"
log "  measure_ops      = ${MEASURE_LOOKUPS}"
log "  dram_bp/rc/cxl   = ${DRAM_BP}/${DRAM_RC}/${CXL_GIB} GiB"
log "  results_dir      = ${RESULT_DIR}"
log "========================================================================"

# --- Group A: Record CMS Width Sweep ---
if [[ "$EXPERIMENT_GROUPS" == *"A"* ]]; then
  log ""
  log "=== Group A: Record CMS Width Sweep (rows=4, trigger_window=1M) ==="
  run_group_workloads "A1" 4 65536    1000000 "cols64K"
  run_group_workloads "A2" 4 262144   1000000 "cols256K"
  run_group_workloads "A3" 4 1048576  1000000 "cols1M"
  run_group_workloads "A4" 4 2097152  1000000 "cols2M"
fi

# --- Group B: trigger_window Sweep ---
if [[ "$EXPERIMENT_GROUPS" == *"B"* ]]; then
  log ""
  log "=== Group B: Trigger Window Sweep (record_cms=4x1M) ==="
  run_group_workloads "B1" 4 1048576  500000    "win500K"
  run_group_workloads "B2" 4 1048576  1000000   "win1M"
  run_group_workloads "B3" 4 1048576  5000000   "win5M"
  run_group_workloads "B4" 4 1048576  10000000  "win10M"
fi

# --- Group C: Default vs Recommended Joint Config ---
if [[ "$EXPERIMENT_GROUPS" == *"C"* ]]; then
  log ""
  log "=== Group C: Default vs Recommended Joint Configuration ==="
  run_group_workloads "C1" 4 65536    1000000   "default"
  run_group_workloads "C2" 4 1048576  5000000   "recommended"
fi

log ""
log "========================================================================"
log "All experiments complete. Results in: $RESULT_DIR"
log "========================================================================"
log "Run ./extract_metrics.sh to generate summary table."
