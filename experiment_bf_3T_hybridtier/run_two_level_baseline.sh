#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# Two-level optimization baseline runner
#
# Only runs two_level across YCSB A-F + TPC-C (7 cases). The first run
# (BASELINE_TAG=v0) snapshots current performance; future optimization
# rounds bump BASELINE_TAG (v1, v2, ...) and diff against v0 via the
# generated summary CSVs.
#
# Goal: drive every workload toward rank #1 vs the three baselines from
# bench_20260525_233324/.
#
# Config is intentionally identical to run_medium_benchmark.sh so this
# baseline is directly comparable to bench_20260525_233324/.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

# Bump on each optimization round: v0 (baseline) -> v1 -> v2 -> ...
# Override on CLI: BASELINE_TAG=v1 ./run_two_level_baseline.sh
BASELINE_TAG="${BASELINE_TAG:-v0}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_two_level_${BASELINE_TAG}_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Configuration — keep identical to run_medium_benchmark.sh
# -----------------------------------------------------------------------------
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99

# DRAM (two_level): BP 0.125 + RC 0.375 = 0.5 GiB
DRAM_BP_TWO_LEVEL=0.125
DRAM_RC_TWO_LEVEL=0.375

WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40

COOLDOWN_SECONDS=10

YCSB_WORKLOADS=(a b c d e f)
TOTAL_EXPERIMENTS=$(( ${#YCSB_WORKLOADS[@]} + 1 ))
CURRENT_EXPERIMENT=0
PASS_COUNT=0
FAIL_COUNT=0
declare -a FAILED_TESTS=()

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

two_level_flags() {
   # NOTE: page_cms col_num is auto-derived in the backend Wrapper ctor from
   # (cxl_gib + dram_bp + dram_rc); no extra flag needed here. v1 optimization
   # over the fixed 1M-col baseline. See DEBUG_PLAN v1 notes.
   echo --test_admission_mode=two_level \
        --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
        --cxl_dax_device_path="$CXL_DAX_DEVICE" \
        --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
        --delay_admission_recordcache_threads_start=true \
        --dram_buffer_pool_gib="$DRAM_BP_TWO_LEVEL" \
        --dram_recordcache_gib="$DRAM_RC_TWO_LEVEL" \
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD"
}

# -----------------------------------------------------------------------------
# run_ycsb
# -----------------------------------------------------------------------------
run_ycsb() {
   local wl="$1"
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}_two_level.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}:MISSING_BINARY")
      return 1
   fi

   local warmup="$WARMUP_LOOKUPS"
   case "$wl" in d|e) warmup=10000000 ;; esac

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl} (two_level, tag=${BASELINE_TAG})"

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
      $(two_level_flags) \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}:exit${exit_code}")
   else
      echo "[PASS] ycsb_${wl} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# run_tpcc
# -----------------------------------------------------------------------------
run_tpcc() {
   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/tpcc_compare_test"
   local result_file="$RESULT_DIR/tpcc_two_level.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:MISSING_BINARY")
      return 1
   fi

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc (two_level, tag=${BASELINE_TAG})"

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
      $(two_level_flags) \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] tpcc exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("tpcc:exit${exit_code}")
   else
      echo "[PASS] tpcc (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# extract_summary — parse logs into two CSVs (YCSB + TPC-C) for easy diff
# -----------------------------------------------------------------------------
extract_summary() {
   local ycsb_csv="$RESULT_DIR/summary_ycsb.csv"
   local tpcc_csv="$RESULT_DIR/summary_tpcc.csv"

   # YCSB summary: parse the final `mode=...` line in each ycsb log.
   {
      echo "tag,workload,mqps,avg_us,p95_us,p99_us,rc_hr_pct,bp_hr_pct,cxl_hr_pct,ssd_miss_pct,aborts"
      for wl in "${YCSB_WORKLOADS[@]}"; do
         local log="$RESULT_DIR/ycsb_${wl}_two_level.log"
         [[ -f "$log" ]] || { echo "${BASELINE_TAG},${wl},MISSING,,,,,,,,"; continue; }
         local line
         line=$(grep -E "^.*mode=" "$log" | tail -1 || true)
         if [[ -z "$line" ]]; then
            echo "${BASELINE_TAG},${wl},NO_SUMMARY_LINE,,,,,,,,"
            continue
         fi
         # Field extraction (avoid awk subshell scope issues by using grep -oP).
         local mqps avg p95 p99 rc bp cxl ssd ab
         mqps=$(echo "$line" | grep -oP 'Mqps=\K[0-9.]+' || echo "")
         avg=$( echo "$line" | grep -oP 'avg_us=\K[0-9.]+' || echo "")
         p95=$( echo "$line" | grep -oP 'p95_us=\K[0-9.]+' || echo "")
         p99=$( echo "$line" | grep -oP 'p99_us=\K[0-9.]+' || echo "")
         rc=$(  echo "$line" | grep -oP 'RC_HR=\K[0-9.]+' || echo "")
         bp=$(  echo "$line" | grep -oP 'DRAM_HR=\K[0-9.]+' || echo "")
         cxl=$( echo "$line" | grep -oP 'CXL_HR=\K[0-9.]+' || echo "")
         ssd=$( echo "$line" | grep -oP 'SSD_miss=\K[0-9.]+' || echo "")
         ab=$(  echo "$line" | grep -oP 'aborts=\K[0-9]+' || echo "")
         echo "${BASELINE_TAG},${wl},${mqps},${avg},${p95},${p99},${rc},${bp},${cxl},${ssd},${ab}"
      done
   } > "$ycsb_csv"

   # TPC-C summary: parse the Final Summary block of tpcc log.
   {
      echo "tag,tps,abort_rate_pct,rc_hr_pct,dram_bp_pct,cxl_bp_pct,ssd_miss_pct,promotions,demotions,evictions"
      local log="$RESULT_DIR/tpcc_two_level.log"
      if [[ ! -f "$log" ]]; then
         echo "${BASELINE_TAG},MISSING,,,,,,,,"
      else
         local tps abort rc dram cxl ssd promo demo evict
         tps=$(   grep -oP '^TPS:\s+\K[0-9.]+'                     "$log" | tail -1 || echo "")
         abort=$( grep -oP '^Abort rate:\s+\K[0-9.]+'               "$log" | tail -1 || echo "")
         rc=$(    grep -oP 'Record cache hit/miss:.*\(\K[0-9.]+'    "$log" | tail -1 || echo "")
         dram=$(  grep -oP '^DRAM BP hits:\s+[0-9]+\s+\(\K[0-9.]+'  "$log" | tail -1 || echo "")
         cxl=$(   grep -oP '^CXL BP hits:\s+[0-9]+\s+\(\K[0-9.]+'   "$log" | tail -1 || echo "")
         ssd=$(   grep -oP '^SSD misses:\s+[0-9]+\s+\(\K[0-9.]+'    "$log" | tail -1 || echo "")
         promo=$( grep -oP '^CXL->DRAM promotions:\s+\K[0-9]+'      "$log" | tail -1 || echo "")
         demo=$(  grep -oP '^DRAM->CXL demotions:\s+\K[0-9]+'       "$log" | tail -1 || echo "")
         evict=$( grep -oP '^Evictions \(to SSD\):\s+\K[0-9]+'      "$log" | tail -1 || echo "")
         echo "${BASELINE_TAG},${tps},${abort},${rc},${dram},${cxl},${ssd},${promo},${demo},${evict}"
      fi
   } > "$tpcc_csv"

   echo ""
   log_info "summary CSVs:"
   echo "  $ycsb_csv"
   echo "  $tpcc_csv"
   echo ""
   echo "--- YCSB summary ---"
   column -s, -t < "$ycsb_csv"
   echo ""
   echo "--- TPC-C summary ---"
   column -s, -t < "$tpcc_csv"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
log_phase "Two-level optimization baseline (tag=${BASELINE_TAG})"
log_info "result_dir       = $RESULT_DIR"
log_info "baseline_tag     = ${BASELINE_TAG}"
log_info "working_set      = ${WORKING_SET_GIB} GiB (payload=${PAYLOAD_SIZE_BYTES}B)"
log_info "cxl_gib          = ${CXL_GIB}"
log_info "dram (two_level) = BP ${DRAM_BP_TWO_LEVEL} + RC ${DRAM_RC_TWO_LEVEL} GiB"
log_info "zipf_theta       = ${ZIPF_THETA}"
log_info "worker_threads   = ${WORKER_THREADS}"
log_info "ycsb: warmup=${WARMUP_LOOKUPS} measure=${MEASURE_LOOKUPS} (D/E warmup=10M)"
log_info "tpcc: wh=${TPCC_WAREHOUSE_COUNT} warmup=${TPCC_WARMUP_SECONDS}s measure=${TPCC_MEASURE_SECONDS}s"
log_info "total_experiments= ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)

for wl in "${YCSB_WORKLOADS[@]}"; do
   log_phase "YCSB-${wl^^}"
   run_ycsb "$wl"
done

log_phase "TPC-C"
run_tpcc

OVERALL_SEC=$(( $(date +%s) - OVERALL_START ))
OVERALL_MIN=$(awk "BEGIN {printf \"%.1f\", $OVERALL_SEC / 60.0}")

log_phase "BENCHMARK COMPLETE (tag=${BASELINE_TAG})"
log_info "total elapsed = ${OVERALL_SEC}s (${OVERALL_MIN} min)"
log_info "pass=${PASS_COUNT} fail=${FAIL_COUNT} / ${TOTAL_EXPERIMENTS}"

if [[ ${#FAILED_TESTS[@]} -gt 0 ]]; then
   echo ""
   echo "FAILED TESTS:"
   for t in "${FAILED_TESTS[@]}"; do
      echo "  - $t"
   done
fi

extract_summary

echo ""
echo "Results in: $RESULT_DIR"
echo ""
echo "To compare against this baseline after an optimization round:"
echo "  BASELINE_TAG=v1 $0    # then diff summary_*.csv"
