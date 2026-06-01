#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# SSD-involved benchmark: two_level vs bf-tree vs 3T vs hybridtier
#
# Difference vs run_medium_benchmark.sh:
#   - WORKING_SET_GIB = 3.0  (was 2.0)  → DRAM+CXL covers 83% (was 125%)
#   - All other YCSB knobs unchanged
#   - TPC-C: see notes below
#
# Rationale:
#   In the original setup, working_set (2.0) <= CXL (2.0), so the entire dataset
#   fits in the DRAM+CXL tier and SSD is never touched (bf-tree SSD_miss=0%, our
#   SSD_miss=0.49%). That makes the bench essentially in-memory across tiers,
#   which underestimates the cost of CXL-page eviction to SSD — exactly the
#   regime where deferred write-back (bf-tree) is supposed to win big over
#   write-through (us).
#
#   Bumping WORKING_SET_GIB to 3.0 forces ~17% of the dataset to live on SSD,
#   pulling SSD I/O into the steady-state path so we can measure the real
#   write-back vs write-through tradeoff (and validate our paper claim).
#
# TPC-C note:
#   wh=10 produces ~1 GiB dataset, which still fits 100% in DRAM+CXL. To make
#   TPC-C see SSD pressure as well, set TPCC_WAREHOUSE_COUNT=30 (~3 GiB).
#   The default below KEEPS wh=10 to honor "only adjust working_set, others
#   unchanged" — flip the variable below if you want TPC-C SSD-involved too.
#
# Covers: YCSB A/B/C/D/E/F + TPC-C, all 4 modes = 28 experiments
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison_ssd_involved"
# Use dax0.2 (dax0.1 is held by the currently-running in-memory benchmark).
# All dax0.* devices are 128 GiB, more than enough for CXL_GIB=2.0.
CXL_DAX_DEVICE="/dev/dax0.2"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_ssd_involved_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
# >>> ONLY THIS LINE DIFFERS from run_medium_benchmark.sh <<<
WORKING_SET_GIB=3.0     # was 2.0 — pushes SSD into the steady-state path
# Coverage check: DRAM(0.5) + CXL(2.0) = 2.5 GiB
#                 coverage = 2.5 / 3.0 = 83.3%   (was 125%, 100% in-memory)
#                 → ~17% of dataset lives on SSD

PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99

# Memory: total DRAM budget = 0.5 GiB
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

# Workload: 20M warmup + 30M measure
# NOTE: with WS=3 GiB and Zipf 0.99 the warmup may take longer than 2 min
# (SSD I/O), but it's still the right number for the measure phase. If runtime
# becomes a concern, drop warmup to 10M (Zipf 0.99 converges fast anyway).
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

# TPC-C — see header note. wh=10 keeps TPC-C in-memory (DRAM+CXL covers 250%).
# Flip to 30 if you want TPC-C SSD-involved.
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
   # Keep the same v4 split as run_medium_benchmark.sh.
   local bp_gib="$DRAM_BP_TWO_LEVEL"
   local rc_gib="$DRAM_RC_TWO_LEVEL"
   case "$wl" in d|e) bp_gib=0.375; rc_gib=0.125 ;; esac

   # Fresh SSD per test — sharing $SSD_PATH across 28 tests poisons later runs.
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
log_phase "SSD-involved Benchmark: 4 modes x (YCSB A-F + TPC-C)"
log_info "result_dir       = $RESULT_DIR"
log_info "working_set      = ${WORKING_SET_GIB} GiB (payload=${PAYLOAD_SIZE_BYTES}B)"
log_info "cxl_gib          = ${CXL_GIB}"
log_info "dram (two_level) = BP ${DRAM_BP_TWO_LEVEL} + RC ${DRAM_RC_TWO_LEVEL} GiB"
log_info "dram (baselines) = BP ${DRAM_BP_BASELINE} GiB"
log_info "tier coverage    = $(awk "BEGIN {printf \"%.1f%%\", (${CXL_GIB} + ${DRAM_BP_BASELINE}) / ${WORKING_SET_GIB} * 100}") of working_set (>=80% target)"
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
