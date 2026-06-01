#!/usr/bin/env bash

set -euo pipefail
trap '' HUP

# =============================================================================
# Exp7 Group 1: Workloads B, C, D + TPC-C
#
# DAX = dax0.6,  SSD = exp7_comparison_ssd
#
# YCSB:  B/C/D × 5 modes × 3 thetas = 45
# TPC-C: 5 modes                      =  5
# Total:                                 50
# =============================================================================

# -----------------------------------------------------------------------------
# Repo paths
# -----------------------------------------------------------------------------
BUILD_DIR_WT="/home/zhizhi.tyf/cxl-WT-comparison/build/frontend"
BUILD_DIR_RO="/home/zhizhi.tyf/cxl-recordcache-comparison/build/frontend"

SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/exp7_comparison_ssd"
CXL_DAX_DEVICE="/dev/dax0.6"

# -----------------------------------------------------------------------------
# Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/run_${TIMESTAMP}_exp7_group1_BCD"
mkdir -p "$RESULT_DIR"
MASTER_LOG="$RESULT_DIR/master.log"

# -----------------------------------------------------------------------------
# Matrix — only B, C, D + TPC-C
# -----------------------------------------------------------------------------
MODES=(two_level_readonly two_level_wt bf-tree tiered-indexing-zxj hybried-tier-asplos2025)
YCSB_WORKLOADS=(b c d)
THETAS=(0.90 0.95 0.99)

# -----------------------------------------------------------------------------
# Memory config
# -----------------------------------------------------------------------------
WS_GIB=23.0
CXL_GIB=18.0
DRAM_TOTAL=3.0

TWO_LEVEL_BP_ABCF=0.43
TWO_LEVEL_RC_ABCF=2.57
TWO_LEVEL_BP_DE=2.57
TWO_LEVEL_RC_DE=0.43
BASELINE_BP=3.0

# -----------------------------------------------------------------------------
# Lookup config
# -----------------------------------------------------------------------------
WARMUP_LOOKUPS_TIER1=800000000
WARMUP_LOOKUPS_TIER2=50000000
MEASURE_LOOKUPS=200000000

TPCC_WAREHOUSE_COUNT=100
TPCC_WARMUP_SECONDS=120
TPCC_MEASURE_SECONDS=180

# -----------------------------------------------------------------------------
# Threading & misc
# -----------------------------------------------------------------------------
WORKER_THREADS=20
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

PAYLOAD_SIZE_BYTES=100
PROGRESS_INTERVAL=1000000
WARMUP_PROGRESS_INTERVAL=2000000
COOLDOWN_SECONDS=30

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "  $(date '+%Y-%m-%d %H:%M:%S')"
   echo "============================================================"
}

log_info() { echo "[INFO] $(date '+%H:%M:%S') $1"; }

is_tier2() {
   case "$1" in
      d|e) return 0 ;;
      *)   return 1 ;;
   esac
}

get_build_dir() {
   case "$1" in
      two_level_readonly) echo "$BUILD_DIR_RO" ;;
      *)                  echo "$BUILD_DIR_WT" ;;
   esac
}

get_admission_mode() {
   case "$1" in
      two_level_readonly|two_level_wt) echo "two_level" ;;
      *) echo "$1" ;;
   esac
}

TOTAL_EXPERIMENTS=$(( ${#YCSB_WORKLOADS[@]} * ${#MODES[@]} * ${#THETAS[@]} + ${#MODES[@]} ))
CURRENT_EXPERIMENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

# -----------------------------------------------------------------------------
# Build mode-specific flags
# -----------------------------------------------------------------------------
build_mode_flags() {
   local mode="$1" wl="$2"
   local admission_mode
   admission_mode="$(get_admission_mode "$mode")"

   local bp_gib rc_gib

   case "$mode" in
      two_level_readonly|two_level_wt)
         if is_tier2 "$wl"; then
            bp_gib="$TWO_LEVEL_BP_DE"
            rc_gib="$TWO_LEVEL_RC_DE"
         else
            bp_gib="$TWO_LEVEL_BP_ABCF"
            rc_gib="$TWO_LEVEL_RC_ABCF"
         fi
         MODE_FLAGS=(
            --test_admission_mode="$admission_mode"
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$bp_gib"
            --dram_recordcache_gib="$rc_gib"
            --forward_epoch_thread="$FORWARD_EPOCH_THREAD"
            --sieve_eviction_thread="$SIEVE_EVICTION_THREAD"
            --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
         )
         ;;
      bf-tree)
         MODE_FLAGS=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         MODE_FLAGS=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
            --vi_fremove=true
         )
         ;;
      hybried-tier-asplos2025)
         MODE_FLAGS=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true
            --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS"
            --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$BASELINE_BP"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac
}

# -----------------------------------------------------------------------------
# run_ycsb
# -----------------------------------------------------------------------------
run_ycsb() {
   local wl="$1" mode="$2" theta="$3"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local build_dir
   build_dir="$(get_build_dir "$mode")"
   local binary="$build_dir/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_${mode}_theta${theta}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}_${mode}_theta${theta}:MISSING_BINARY")
      return 0
   fi

   local warmup="$WARMUP_LOOKUPS_TIER1"
   if is_tier2 "$wl"; then
      warmup="$WARMUP_LOOKUPS_TIER2"
   fi

   build_mode_flags "$mode" "$wl"

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=${mode} theta=${theta}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   "$binary" \
      --test_zipf_theta="$theta" \
      --test_working_set_gib="$WS_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$warmup" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --test_warmup_progress_interval="$WARMUP_PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${MODE_FLAGS[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl} mode=${mode} theta=${theta} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}_${mode}_theta${theta}:exit${exit_code}")
   else
      echo "[PASS] ycsb_${wl} mode=${mode} theta=${theta} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] ycsb_${wl} ${mode} theta=${theta} elapsed=${elapsed}s" | tee -a "$result_file"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# run_tpcc
# -----------------------------------------------------------------------------
run_tpcc() {
   local mode="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local build_dir
   build_dir="$(get_build_dir "$mode")"
   local binary="$build_dir/tpcc_compare_test"
   local result_file="$RESULT_DIR/tpcc_${mode}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc_${mode}:MISSING_BINARY")
      return 0
   fi

   build_mode_flags "$mode" "a"

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc mode=${mode}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   "$binary" \
      --tpcc_warehouse_count="$TPCC_WAREHOUSE_COUNT" \
      --test_warmup_seconds="$TPCC_WARMUP_SECONDS" \
      --test_measure_seconds="$TPCC_MEASURE_SECONDS" \
      --test_load_data=true \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${MODE_FLAGS[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] tpcc mode=${mode} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc_${mode}:exit${exit_code}")
   else
      echo "[PASS] tpcc mode=${mode} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   echo "[TIME] tpcc ${mode} elapsed=${elapsed}s" | tee -a "$result_file"
   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Pre-flight
# -----------------------------------------------------------------------------
preflight() {
   if [[ ! -e "$CXL_DAX_DEVICE" ]]; then
      echo "[ERROR] dax device $CXL_DAX_DEVICE not found"; return 1
   fi
   for build_dir in "$BUILD_DIR_WT" "$BUILD_DIR_RO"; do
      for wl in "${YCSB_WORKLOADS[@]}"; do
         if [[ ! -x "$build_dir/experiment_1_ycsb_${wl}" ]]; then
            echo "[WARN] binary missing: $build_dir/experiment_1_ycsb_${wl}"
         fi
      done
      if [[ ! -x "$build_dir/tpcc_compare_test" ]]; then
         echo "[WARN] tpcc binary missing: $build_dir/tpcc_compare_test"
      fi
   done
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
main() {
   log_phase "Exp7 Group1: B/C/D + TPC-C (DAX=${CXL_DAX_DEVICE})"
   log_info "timestamp        = ${TIMESTAMP}"
   log_info "result_dir       = ${RESULT_DIR}"
   log_info "ssd_path         = ${SSD_PATH}"
   log_info "cxl_device       = ${CXL_DAX_DEVICE}"
   log_info "workloads        = ${YCSB_WORKLOADS[*]}"
   log_info "total_experiments= ${TOTAL_EXPERIMENTS}"

   preflight

   local OVERALL_START
   OVERALL_START=$(date +%s)

   for theta in "${THETAS[@]}"; do
      log_phase "Theta=${theta}"
      for wl in "${YCSB_WORKLOADS[@]}"; do
         log_phase "YCSB-${wl^^} theta=${theta}"
         for mode in "${MODES[@]}"; do
            run_ycsb "$wl" "$mode" "$theta"
         done
      done
   done

   log_phase "TPC-C"
   for mode in "${MODES[@]}"; do
      run_tpcc "$mode"
   done

   local OVERALL_SEC OVERALL_HR
   OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
   OVERALL_HR=$(awk "BEGIN {printf \"%.2f\", $OVERALL_SEC / 3600.0}")

   log_phase "EXP7 GROUP1 (B/C/D + TPC-C) COMPLETE"
   log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_HR} hr)"
   log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / ${TOTAL_EXPERIMENTS}"

   if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
      echo ""
      echo "FAILED TESTS:"
      for t in "${FAILED_TESTS[@]}"; do
         echo "  - $t"
      done
   fi

   echo ""
   echo "Results in: $RESULT_DIR"
}

main 2>&1 | tee "$MASTER_LOG"
