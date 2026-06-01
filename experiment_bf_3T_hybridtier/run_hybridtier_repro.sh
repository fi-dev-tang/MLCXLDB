#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# HybridTier (ASPLOS '25) repro run.
#
# 2026-05-24 fix pass — validates two changes:
#   D6 (ycsb_c/f admission-thread init race, was 600s timeout core dump):
#     fixed by eager-initializing the baseline policy in
#     TwoLevelAdmissionControlWrapper ctor so the first CBF/MomentumSketch
#     malloc no longer races with CPUCounters::registerThread.
#   D7 (ycsb_d EnsureFailed @ 76% warmup, LeanStoreAdapter.hpp@105):
#     fixed by rewriting HybridTierASPLOS2025AdmissionPolicy to feed the
#     shared DramHotPageCandidates pool instead of maintaining a private
#     promotion_candidates_ map (mirrors BFTreeAdmissionPolicy pattern).
#   Algorithm (CBF frequency + MomentumSketch + 4-Grid) preserved.
#
# Mirrors run_3t_repro.sh / run_bf_repro.sh but:
#   - admission mode      = hybried-tier-asplos2025   (preserve original typo)
#   - CXL DAX device      = /dev/dax0.3   (3T: dax0.1, bf-tree: dax0.2)
#   - SSD backing file    = cxl_test_comparison_ht
#   - extra flags         = --two_level_admission_threads
#                           --delay_admission_recordcache_threads_start
#
# Safe to run concurrently with run_3t_repro.sh and run_bf_repro.sh —
# different DAX device, different SSD file, separate process trees.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison_ht"
CXL_DAX_DEVICE="/dev/dax0.3"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_hybridtier_repro_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# Same workload config as run_3t_repro.sh / run_bf_repro.sh so results stay comparable.
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99
DRAM_BP_BASELINE=0.5
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000
TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40
COOLDOWN_SECONDS=10

# Per-binary wall-clock caps.
# Post-fix expectations (from approved plan):
#   a: 0.30-0.45 Mqps (was 0.37, regression check)
#   b: 0.35-0.50 Mqps (was 0.44, regression check)
#   c: ~0.40 Mqps    (D6 fix target — was 600s timeout)
#   d: 0.10-0.20 Mqps (D7 fix target — was EnsureFailed @ 76%)
#   e: 0.04-0.07 Mqps (was 0.057, regression check)
#   f: ~0.30 Mqps    (D6 fix target — was 600s timeout)
#   tpcc: 50K-65K TPS (was 57974, regression check)
# 40M ops at 0.10 Mqps ≈ 400s; 40M at 0.04 Mqps ≈ 1000s.
PER_TEST_TIMEOUT=600
PER_TEST_TIMEOUT_YCSB_A=600
PER_TEST_TIMEOUT_YCSB_C=600    # D6 retest — fix should resolve init race
PER_TEST_TIMEOUT_YCSB_D=900    # D7 retest — expected 0.10-0.20 Mqps
PER_TEST_TIMEOUT_YCSB_E=1500
PER_TEST_TIMEOUT_YCSB_F=600    # D6 retest — fix should resolve init race

TEST_PAIRS=(
   "a:hybried-tier-asplos2025"
   "b:hybried-tier-asplos2025"
   "c:hybried-tier-asplos2025"
   "d:hybried-tier-asplos2025"
   "e:hybried-tier-asplos2025"
   "f:hybried-tier-asplos2025"
   "tpcc:hybried-tier-asplos2025"
)

log_phase() {
   echo ""
   echo "============================================================"
   echo "  $1"
   echo "  $(date '+%Y-%m-%d %H:%M:%S')"
   echo "============================================================"
}
log_info() { echo "[INFO] $(date '+%H:%M:%S') $1"; }

TOTAL_EXPERIMENTS=${#TEST_PAIRS[@]}
CURRENT_EXPERIMENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

mode_flags_for_ht() {
   echo "--test_admission_mode=hybried-tier-asplos2025"
   echo "--cxl_tiering_enabled=true"
   echo "--cxl_gib=$CXL_GIB"
   echo "--cxl_dax_device_path=$CXL_DAX_DEVICE"
   echo "--pp_threads=$PP_THREADS"
   echo "--cxl_pp_threads=$CXL_PP_THREADS"
   echo "--two_level_admission_threads=$TWO_LEVEL_ADMISSION_THREADS"
   echo "--delay_admission_recordcache_threads_start=true"
   echo "--dram_buffer_pool_gib=$DRAM_BP_BASELINE"
   echo "--dram_recordcache_gib=0.0"
}

run_ycsb() {
   local wl="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_hybried-tier-asplos2025.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}:MISSING_BINARY")
      return 1
   fi

   local warmup="$WARMUP_LOOKUPS"
   case "$wl" in d|e) warmup=10000000 ;; esac

   local wl_timeout="$PER_TEST_TIMEOUT"
   case "$wl" in
      a) wl_timeout="$PER_TEST_TIMEOUT_YCSB_A" ;;
      c) wl_timeout="$PER_TEST_TIMEOUT_YCSB_C" ;;
      d) wl_timeout="$PER_TEST_TIMEOUT_YCSB_D" ;;
      e) wl_timeout="$PER_TEST_TIMEOUT_YCSB_E" ;;
      f) wl_timeout="$PER_TEST_TIMEOUT_YCSB_F" ;;
   esac

   readarray -t mode_flags < <(mode_flags_for_ht)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=hybridtier (timeout=${wl_timeout}s)"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   timeout --signal=KILL "${wl_timeout}" "$binary" \
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

   if [[ "$exit_code" -eq 137 ]]; then
      echo "[HANG] ycsb_${wl} killed after ${elapsed}s — inspect tail of $result_file"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}:HANG")
   elif [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}:exit${exit_code}")
   else
      echo "[PASS] ycsb_${wl} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

run_tpcc() {
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/tpcc_compare_test"
   local result_file="$RESULT_DIR/tpcc_hybried-tier-asplos2025.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:MISSING_BINARY")
      return 1
   fi

   readarray -t mode_flags < <(mode_flags_for_ht)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc mode=hybridtier (timeout=${PER_TEST_TIMEOUT}s)"

   local start_ts exit_code=0
   start_ts=$(date +%s)

   timeout --signal=KILL "${PER_TEST_TIMEOUT}" "$binary" \
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

   if [[ "$exit_code" -eq 137 ]]; then
      echo "[HANG] tpcc killed after ${elapsed}s — inspect tail of $result_file"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:HANG")
   elif [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] tpcc exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:exit${exit_code}")
   else
      echo "[PASS] tpcc (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

log_phase "HybridTier Repro Benchmark (parallel-safe with 3T / bf-tree)"
log_info "result_dir = $RESULT_DIR"
log_info "ssd_path   = $SSD_PATH"
log_info "cxl_device = $CXL_DAX_DEVICE"
log_info "total      = ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)
for pair in "${TEST_PAIRS[@]}"; do
   wl="${pair%%:*}"
   if [[ "$wl" == "tpcc" ]]; then
      log_phase "TPC-C / hybridtier"
      run_tpcc
   else
      log_phase "YCSB-${wl^^} / hybridtier"
      run_ycsb "$wl"
   fi
done

OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

log_phase "DONE"
log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_MIN} min)"
log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / ${TOTAL_EXPERIMENTS}"

if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
   echo ""
   echo "FAILED:"
   for t in "${FAILED_TESTS[@]}"; do echo "  - $t"; done
fi

echo ""
echo "Results in: $RESULT_DIR"
ls -1 "$RESULT_DIR"
