#!/usr/bin/env bash

set -euo pipefail

# =============================================================================
# Baseline comparison experiment: two_level vs bf-tree vs 3T vs hybridtier
#
# Covers YCSB A/B/C/D/E/F + TPC-C across 4 admission modes:
#   1) two_level              (our system, MLCXLDB)
#   2) bf-tree                (Bf-Tree [Hao & Chandramouli, PVLDB '24])
#   3) tiered-indexing-zxj    (3T [Zhou+Hao+Yu+Stonebraker, VLDB-J '25])
#   4) hybried-tier-asplos2025 (HybridTier [Song+Yang+Wang+Zhao+Liu+Pekhimenko, ASPLOS '25])
#
# Default configuration:
#   working_set = 4 GiB, CXL = 2.5 GiB, DRAM BP = 0.125 GiB, RC = 0.500 GiB
#   theta = 0.99 (zipfian skew), worker_threads = 8
#   TPC-C: warehouse_count = 20, warmup = 30s, measure = 60s
#
# SSD:  /data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison
# CXL:  /dev/dax0.1
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/results_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
WORKING_SET_GIB=4.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.5
ZIPF_THETA=0.99

# Memory tier config (total DRAM = BP + RC = 0.625 GiB)
DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.500
# For baselines without record cache: all DRAM goes to BP
DRAM_BP_BASELINE=0.625

# Threading
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

# Workload parameters
WARMUP_LOOKUPS=100000000
MEASURE_LOOKUPS=100000000
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=2000000

# TPC-C parameters
TPCC_WAREHOUSE_COUNT=20
TPCC_WARMUP_SECONDS=30
TPCC_MEASURE_SECONDS=60

COOLDOWN_SECONDS=30

# Admission modes to compare
MODES=(two_level bf-tree tiered-indexing-zxj hybried-tier-asplos2025)

# YCSB workloads
YCSB_WORKLOADS=(a b c d e f)

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "============================================================"
}

log_info() {
   echo "[INFO] $1"
}

# For D/E workloads: reduce warmup and give more to BP (scan-heavy, RC unfriendly)
is_scan_heavy() {
   case "$1" in
      d|e) return 0 ;;
      *)   return 1 ;;
   esac
}

# Total experiment count
TOTAL_EXPERIMENTS=$(( ${#YCSB_WORKLOADS[@]} * ${#MODES[@]} + ${#MODES[@]} ))
CURRENT_EXPERIMENT=0

# -----------------------------------------------------------------------------
# run_ycsb: run a single YCSB experiment
# -----------------------------------------------------------------------------
run_ycsb() {
   local wl="$1"
   local mode="$2"

   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/result_ycsb_${wl}_${mode}_theta${ZIPF_THETA}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      return 1
   fi

   local warmup="$WARMUP_LOOKUPS"
   if is_scan_heavy "$wl"; then
      warmup=30000000
   fi

   # Build flags based on mode
   local mode_flags=()

   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
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
         )
         ;;
      bf-tree)
         mode_flags=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

   echo ""
   echo "[RUN][$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=${mode} theta=${ZIPF_THETA}"
   echo "[RUN] result -> $result_file"

   local start_ts end_ts elapsed_sec elapsed_min
   start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$warmup" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true \
      --wal=true \
      --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${mode_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   end_ts=$(date +%s)
   elapsed_sec=$((end_ts - start_ts))
   elapsed_min=$(awk "BEGIN {printf \"%.2f\", $elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] ycsb_${wl} mode=${mode} exit code $exit_code (continuing)"
   fi
   echo "[TIME] ycsb_${wl} mode=${mode} elapsed: ${elapsed_sec}s (${elapsed_min} min)" \
      | tee -a "$result_file"

   log_info "cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# run_tpcc: run a single TPC-C experiment
# -----------------------------------------------------------------------------
run_tpcc() {
   local mode="$1"

   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))
   local binary="$BUILD_DIR/tpcc_compare_test"
   local result_file="$RESULT_DIR/result_tpcc_${mode}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      return 1
   fi

   local mode_flags=()

   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
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
         )
         ;;
      bf-tree)
         mode_flags=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

   echo ""
   echo "[RUN][$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc mode=${mode}"
   echo "[RUN] result -> $result_file"

   local start_ts end_ts elapsed_sec elapsed_min
   start_ts=$(date +%s)
   local exit_code=0

   "$binary" \
      --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT" \
      --test_warmup_seconds="$TPCC_WARMUP_SECONDS" \
      --test_measure_seconds="$TPCC_MEASURE_SECONDS" \
      --test_load_data=true \
      --worker_threads="$WORKER_THREADS" \
      --vi=true \
      --wal=true \
      --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${mode_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   end_ts=$(date +%s)
   elapsed_sec=$((end_ts - start_ts))
   elapsed_min=$(awk "BEGIN {printf \"%.2f\", $elapsed_sec / 60.0}")

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[WARN] tpcc mode=${mode} exit code $exit_code (continuing)"
   fi
   echo "[TIME] tpcc mode=${mode} elapsed: ${elapsed_sec}s (${elapsed_min} min)" \
      | tee -a "$result_file"

   log_info "cooldown ${COOLDOWN_SECONDS}s"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
log_phase "Baseline Comparison: two_level vs bf-tree vs 3T vs hybridtier"
log_info "timestamp              = $TIMESTAMP"
log_info "result_dir             = $RESULT_DIR"
log_info "working_set            = ${WORKING_SET_GIB} GiB"
log_info "cxl_gib                = ${CXL_GIB} GiB"
log_info "zipf_theta             = ${ZIPF_THETA}"
log_info "worker_threads         = ${WORKER_THREADS}"
log_info "modes                  = ${MODES[*]}"
log_info "ycsb_workloads         = ${YCSB_WORKLOADS[*]}"
log_info "tpcc_warehouse_count   = ${TPCC_WAREHOUSE_COUNT}"
log_info "ssd_path               = ${SSD_PATH}"
log_info "cxl_dax_device         = ${CXL_DAX_DEVICE}"
log_info "total_experiments      = ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)

# --- Phase 1: YCSB workloads ---
log_phase "Phase 1: YCSB Workloads (${#YCSB_WORKLOADS[@]} workloads x ${#MODES[@]} modes)"

for wl in "${YCSB_WORKLOADS[@]}"; do
   log_phase "YCSB-${wl^^} (all modes)"
   for mode in "${MODES[@]}"; do
      run_ycsb "$wl" "$mode"
   done
done

# --- Phase 2: TPC-C ---
log_phase "Phase 2: TPC-C (${#MODES[@]} modes)"

for mode in "${MODES[@]}"; do
   run_tpcc "$mode"
done

# --- Summary ---
OVERALL_END=$(date +%s)
OVERALL_SEC=$((OVERALL_END - OVERALL_START))
OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

log_phase "ALL EXPERIMENTS COMPLETE"
log_info "result_dir    = $RESULT_DIR"
log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
echo ""
echo "Result files:"
ls -1 "$RESULT_DIR"
