#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# Bf-tree repro run for parallel debugging alongside the 3T repro.
#
# 2026-05-24 retest pass — validates the 32-way sharded BfTreeAdapter
# refactor. Prior repro (bench_20260523_184519) used a single global mutex
# on the mini-page cache; this run exercises the new sharded layout with
# alignas(64) shards to remove false sharing. Expected:
#   - ycsb_b/c/d/e/f: PASS at <= prior throughput (correctness > perf)
#   - ycsb_a: still flaky (D2 — 5/23 deadlock + 5/24 heap-corrupt at 98%
#     warmup, two independent bugs not yet root-caused; kept in matrix for
#     known-issue documentation)
#   - tpcc:  PASS at >= prior throughput (less contention)
#
# Mirrors run_3t_repro.sh but:
#   - admission mode      = bf-tree            (was tiered-indexing-zxj)
#   - CXL DAX device      = /dev/dax0.2        (3T uses dax0.1)
#   - SSD backing file    = cxl_test_comparison_bf  (3T uses cxl_test_comparison)
#   - timeouts            = much larger (bf-tree is genuinely 10-20x slower than 3T)
#   - no vi_fremove       (3T-specific tombstone workaround, unsafe elsewhere)
#
# Safe to run *concurrently* with run_hybridtier_repro.sh / run_3t_repro.sh
# — different DAX device, different SSD file, separate process trees.
# Per-test timeout kills any wedged binary so the matrix keeps moving.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison_bf"
CXL_DAX_DEVICE="/dev/dax0.2"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_bf_repro_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# Same workload config as run_3t_repro.sh so results stay comparable.
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99
DRAM_BP_BASELINE=0.5
WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000
TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40
COOLDOWN_SECONDS=10

# Per-binary wall-clock caps.
# bf-tree on YCSB tops out around 0.025-0.077 Mqps on this hardware (see
# bench_20260523_184519/ycsb_*_bf-tree.log), so 20M warmup + 30M measure
# at 0.03 Mqps ≈ 1700 s. 2400s gives a 40% margin for the slower workloads.
PER_TEST_TIMEOUT=900
PER_TEST_TIMEOUT_YCSB_A=2400
PER_TEST_TIMEOUT_YCSB_B=2400
PER_TEST_TIMEOUT_YCSB_C=2400
PER_TEST_TIMEOUT_YCSB_D=2400
PER_TEST_TIMEOUT_YCSB_E=1800
PER_TEST_TIMEOUT_YCSB_F=2400

TEST_PAIRS=(
   "a:bf-tree"
   "b:bf-tree"
   "c:bf-tree"
   "d:bf-tree"
   "e:bf-tree"
   "f:bf-tree"
   "tpcc:bf-tree"
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

mode_flags_for_bf() {
   echo "--test_admission_mode=bf-tree"
   echo "--cxl_tiering_enabled=true"
   echo "--cxl_gib=$CXL_GIB"
   echo "--cxl_dax_device_path=$CXL_DAX_DEVICE"
   echo "--pp_threads=$PP_THREADS"
   echo "--cxl_pp_threads=$CXL_PP_THREADS"
   echo "--dram_buffer_pool_gib=$DRAM_BP_BASELINE"
   echo "--dram_recordcache_gib=0.0"
}

run_ycsb() {
   local wl="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_bf-tree.log"

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
      b) wl_timeout="$PER_TEST_TIMEOUT_YCSB_B" ;;
      c) wl_timeout="$PER_TEST_TIMEOUT_YCSB_C" ;;
      d) wl_timeout="$PER_TEST_TIMEOUT_YCSB_D" ;;
      e) wl_timeout="$PER_TEST_TIMEOUT_YCSB_E" ;;
      f) wl_timeout="$PER_TEST_TIMEOUT_YCSB_F" ;;
   esac

   readarray -t mode_flags < <(mode_flags_for_bf)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=bf-tree (timeout=${wl_timeout}s)"

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
   local result_file="$RESULT_DIR/tpcc_bf-tree.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:MISSING_BINARY")
      return 1
   fi

   readarray -t mode_flags < <(mode_flags_for_bf)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc mode=bf-tree (timeout=${PER_TEST_TIMEOUT}s)"

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

log_phase "bf-tree Repro Benchmark (parallel-safe with run_3t_repro.sh)"
log_info "result_dir = $RESULT_DIR"
log_info "ssd_path   = $SSD_PATH"
log_info "cxl_device = $CXL_DAX_DEVICE"
log_info "total      = ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)
for pair in "${TEST_PAIRS[@]}"; do
   wl="${pair%%:*}"
   if [[ "$wl" == "tpcc" ]]; then
      log_phase "TPC-C / bf-tree"
      run_tpcc
   else
      log_phase "YCSB-${wl^^} / bf-tree"
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
