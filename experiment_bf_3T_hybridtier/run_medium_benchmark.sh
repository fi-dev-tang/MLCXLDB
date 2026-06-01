#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# Medium-scale benchmark: two_level vs bf-tree vs 3T vs hybridtier
#
# Scaled for ~1-2 hour total runtime. Sufficient for performance comparison
# while not requiring overnight runs.
#
# Covers: YCSB A/B/C/D/E/F + TPC-C, all 4 modes = 28 experiments
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Configuration (Bf-Tree paper scale: ~2 GiB dataset, 10% DRAM ratio)
# -----------------------------------------------------------------------------
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99

# Memory: total DRAM budget = 0.5 GiB (~25% of working set)
# two_level splits: BP 0.125 + RC 0.375
# baselines: all to BP 0.5
DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.375
DRAM_BP_BASELINE=0.5

# Threading
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

# Workload: 20M warmup + 30M measure (~2 min per test at ~250K ops/s)
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

# TPC-C
TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40

COOLDOWN_SECONDS=10

# Modes and workloads
MODES=(two_level bf-tree tiered-indexing-zxj hybried-tier-asplos2025)
YCSB_WORKLOADS=(a b c d e f)

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

TOTAL_EXPERIMENTS=$(( ${#YCSB_WORKLOADS[@]} * ${#MODES[@]} + ${#MODES[@]} ))
CURRENT_EXPERIMENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

# -----------------------------------------------------------------------------
# run_ycsb
# -----------------------------------------------------------------------------
run_ycsb() {
   local wl="$1"
   local mode="$2"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_${mode}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}_${mode}:MISSING_BINARY")
      return 1
   fi

   local warmup="$WARMUP_LOOKUPS"
   # Scan-heavy workloads (D/E): reduce warmup
   case "$wl" in d|e) warmup=10000000 ;; esac

   # two_level per-workload DRAM split (BP+RC=0.5 GiB unchanged).
   # D/E: RC_HR<25% in v4 — RC is dead weight, give BP the budget
   #      (rebal sweep showed +35.2% / +51.7% ops/s, SSD_miss halved).
   # A/B/C/F/TPC-C: RC_HR>=49% — RC pulling its weight, keep current split.
   local bp_gib="$DRAM_BP_TWO_LEVEL"
   local rc_gib="$DRAM_RC_TWO_LEVEL"
   case "$wl" in d|e) bp_gib=0.375; rc_gib=0.125 ;; esac

   # Fresh SSD per test — sharing $SSD_PATH across 28 tests poisons later
   # runs (e.g. tpcc two_level hit update1 NOT_FOUND because earlier YCSB
   # tests left btree state on disk). Binary will recreate on next load.
   rm -f "$SSD_PATH"

   local mode_flags=()
   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
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
         mode_flags=(
            --test_admission_mode=bf-tree
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
            --vi_fremove=true
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=${mode}"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   "$binary" \
      --test_zipf_theta="$ZIPF_THETA" \
      --test_working_set_gib="$WORKING_SET_GIB" \
      --test_payload_size_bytes="$PAYLOAD_SIZE_BYTES" \
      --test_warmup_lookups="$warmup" \
      --test_measure_lookups="$MEASURE_LOOKUPS" \
      --test_progress_interval="$PROGRESS_INTERVAL" \
      --worker_threads="$WORKER_THREADS" \
      --vi=true --wal=true --trunc=true \
      --ssd_path="$SSD_PATH" \
      "${mode_flags[@]}" \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl} mode=${mode} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}_${mode}:exit${exit_code}")
   else
      echo "[PASS] ycsb_${wl} mode=${mode} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# run_tpcc
# -----------------------------------------------------------------------------
run_tpcc() {
   local mode="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/tpcc_compare_test"
   local result_file="$RESULT_DIR/tpcc_${mode}.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc_${mode}:MISSING_BINARY")
      return 1
   fi

   # Fresh SSD per test — see comment in run_ycsb.
   rm -f "$SSD_PATH"

   local mode_flags=()
   case "$mode" in
      two_level)
         mode_flags=(
            --test_admission_mode=two_level
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
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
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
      tiered-indexing-zxj)
         mode_flags=(
            --test_admission_mode=tiered-indexing-zxj
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
            --vi_fremove=true
         )
         ;;
      hybried-tier-asplos2025)
         mode_flags=(
            --test_admission_mode=hybried-tier-asplos2025
            --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB"
            --cxl_dax_device_path="$CXL_DAX_DEVICE"
            --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS"
            --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS"
            --delay_admission_recordcache_threads_start=true
            --dram_buffer_pool_gib="$DRAM_BP_BASELINE"
            --dram_recordcache_gib=0.0
         )
         ;;
   esac

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
      "${mode_flags[@]}" \
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

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
log_phase "Medium Benchmark: 4 modes x (YCSB A-F + TPC-C)"
log_info "result_dir       = $RESULT_DIR"
log_info "working_set      = ${WORKING_SET_GIB} GiB (payload=${PAYLOAD_SIZE_BYTES}B)"
log_info "cxl_gib          = ${CXL_GIB}"
log_info "dram (two_level) = BP ${DRAM_BP_TWO_LEVEL} + RC ${DRAM_RC_TWO_LEVEL} GiB"
log_info "dram (baselines) = BP ${DRAM_BP_BASELINE} GiB"
log_info "zipf_theta       = ${ZIPF_THETA}"
log_info "worker_threads   = ${WORKER_THREADS}"
log_info "ycsb: warmup=${WARMUP_LOOKUPS} measure=${MEASURE_LOOKUPS}"
log_info "tpcc: wh=${TPCC_WAREHOUSE_COUNT} warmup=${TPCC_WARMUP_SECONDS}s measure=${TPCC_MEASURE_SECONDS}s"
log_info "total_experiments= ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)

for wl in "${YCSB_WORKLOADS[@]}"; do
   log_phase "YCSB-${wl^^}"
   for mode in "${MODES[@]}"; do
      run_ycsb "$wl" "$mode"
   done
done

log_phase "TPC-C"
for mode in "${MODES[@]}"; do
   run_tpcc "$mode"
done

# --- Summary ---
OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

log_phase "BENCHMARK COMPLETE"
log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_MIN} min)"
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
ls -1 "$RESULT_DIR"
