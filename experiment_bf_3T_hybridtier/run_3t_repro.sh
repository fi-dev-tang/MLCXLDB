#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# 3T-only repro run for hang debugging.
#
# Binary has [3T-ADAPTER-INIT] / [registerBTreeVI] / [BTreeGeneric::create]
# prints already compiled in — re-running the full 3T matrix in batch mode is
# the easiest way to make the flaky hang resurface in a log we can read.
#
# Per-binary timeout=300s: when 3T hangs, the binary is auto-killed and the
# script moves on instead of stalling forever. No tmux babysitting needed.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_3t_repro_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# Same config as run_medium_benchmark_debug1.sh.
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

# Per-binary wall-clock cap. Healthy 3T runs finish well under 300s; any
# binary still alive at the cap is the hang we're hunting.
PER_TEST_TIMEOUT=300
# ycsb_d (insert-heavy / latest-key) is the 3T "performance soft spot": eviction
# is genuinely slow because the hot tree fills faster than the down-migration
# clock can drain it. 600s is enough for the workload to finish on this hardware.
PER_TEST_TIMEOUT_YCSB_D=1800
# ycsb_e 同理：scan 工作负载在 hot+cold 双树合并下稳定 ~0.14 Mqps，
# 30M measure 单算就要 ~210s，加上 warmup 10M ~70s 共 ~280s，逼近 300s。
PER_TEST_TIMEOUT_YCSB_E=1200

TEST_PAIRS=(
   "a:tiered-indexing-zxj"
   "b:tiered-indexing-zxj"
   "c:tiered-indexing-zxj"
   "d:tiered-indexing-zxj"
   "e:tiered-indexing-zxj"
   "f:tiered-indexing-zxj"
   "tpcc:tiered-indexing-zxj"
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

mode_flags_for_3t() {
   echo "--test_admission_mode=tiered-indexing-zxj"
   echo "--cxl_tiering_enabled=true"
   echo "--cxl_gib=$CXL_GIB"
   echo "--cxl_dax_device_path=$CXL_DAX_DEVICE"
   echo "--pp_threads=$PP_THREADS"
   echo "--cxl_pp_threads=$CXL_PP_THREADS"
   echo "--dram_buffer_pool_gib=$DRAM_BP_BASELINE"
   echo "--dram_recordcache_gib=0.0"
   echo "--vi_fremove=true"
}

run_ycsb() {
   local wl="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_tiered-indexing-zxj.log"

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
      d) wl_timeout="$PER_TEST_TIMEOUT_YCSB_D" ;;
      e) wl_timeout="$PER_TEST_TIMEOUT_YCSB_E" ;;
   esac

   readarray -t mode_flags < <(mode_flags_for_3t)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} mode=3T (timeout=${wl_timeout}s)"

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
   local result_file="$RESULT_DIR/tpcc_tiered-indexing-zxj.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:MISSING_BINARY")
      return 1
   fi

   readarray -t mode_flags < <(mode_flags_for_3t)

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc mode=3T (timeout=${PER_TEST_TIMEOUT}s)"

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

log_phase "3T Repro Benchmark (instrumented binary, per-test timeout=${PER_TEST_TIMEOUT}s)"
log_info "result_dir = $RESULT_DIR"
log_info "total      = ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)
for pair in "${TEST_PAIRS[@]}"; do
   wl="${pair%%:*}"
   if [[ "$wl" == "tpcc" ]]; then
      log_phase "TPC-C / 3T"
      run_tpcc
   else
      log_phase "YCSB-${wl^^} / 3T"
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
