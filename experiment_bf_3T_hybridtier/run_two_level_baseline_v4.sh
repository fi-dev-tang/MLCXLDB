#!/usr/bin/env bash

set -uo pipefail

# =============================================================================
# Two-level optimization baseline runner — v4 (hot-path micro-opts)
#
# Optimizations under test (vs v2):
#   1. Transparent string_view lookup (zero per-find std::string ctor)
#   2. BuildPrefixedKey helper + optional dt_id bypass on RC keys
#      - YCSB (single-table)  : --rc_skip_dt_id_prefix=true   → saves 2B + memcpy
#      - TPC-C (multi-table)  : --rc_skip_dt_id_prefix=false  → keep dt_id (else collides)
#   3. SeqLock retry max tunable via --rc_seqlock_retry_max
#
# Plus a separate D/E variant that rebalances the DRAM budget toward the buffer
# pool (BP=0.375 + RC=0.125 instead of BP=0.125 + RC=0.375) — for these two
# workloads RC_HR ≈ 0% so the RC capacity is wasted; the rebalance hypothesis is
# that the extra BP cuts SSD_miss enough to flip rank vs hybridtier.
#
# Workload matrix:
#   Pass A (standard, BP=0.125+RC=0.375): YCSB A B C D E F + TPC-C
#   Pass B (rebalanced, BP=0.375+RC=0.125): YCSB D E only
#
# Config otherwise identical to run_two_level_baseline.sh so summaries diff
# cleanly. Pass B logs are tagged with `_rebal` suffix in the result dir.
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/frontend"
SSD_PATH="/data1/zhizhi.tyf/cxl_test_tmp/cxl_test_comparison"
CXL_DAX_DEVICE="/dev/dax0.1"

# Bump on each optimization round.
BASELINE_TAG="${BASELINE_TAG:-v4}"

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR="$SCRIPT_DIR/bench_two_level_${BASELINE_TAG}_${TIMESTAMP}"
mkdir -p "$RESULT_DIR"

# -----------------------------------------------------------------------------
# Common config — keep identical to run_two_level_baseline.sh for fair diff
# -----------------------------------------------------------------------------
WORKING_SET_GIB=2.0
PAYLOAD_SIZE_BYTES=100
CXL_GIB=2.0
ZIPF_THETA=0.99

# Pass A: standard DRAM split (BP 0.125 + RC 0.375)
DRAM_BP_STD=0.125
DRAM_RC_STD=0.375

# Pass B: rebalanced for D/E (BP 0.375 + RC 0.125)
DRAM_BP_REBAL=0.375
DRAM_RC_REBAL=0.125

WORKER_THREADS=8
PP_THREADS=1
CXL_PP_THREADS=1
TWO_LEVEL_ADMISSION_THREADS=2
FORWARD_EPOCH_THREAD=1
SIEVE_EVICTION_THREAD=1
RECORD_CACHE_PROMOTE_THREAD=4

# v4 SeqLock retry cap. Lower = bail to CXL sooner under writer contention.
# Default 8 matches v3-and-earlier behavior. Override via CLI:
#   RC_SEQLOCK_RETRY_MAX=4 ./run_two_level_baseline_v4.sh
RC_SEQLOCK_RETRY_MAX="${RC_SEQLOCK_RETRY_MAX:-8}"

WARMUP_LOOKUPS=20000000
MEASURE_LOOKUPS=30000000
PROGRESS_INTERVAL=5000000

TPCC_WAREHOUSE_COUNT=10
TPCC_WARMUP_SECONDS=20
TPCC_MEASURE_SECONDS=40

COOLDOWN_SECONDS=10

YCSB_WORKLOADS=(a b c d e f)
REBAL_WORKLOADS=(d e)
TOTAL_EXPERIMENTS=$(( ${#YCSB_WORKLOADS[@]} + 1 + ${#REBAL_WORKLOADS[@]} ))
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

# two_level_flags <skip_dt_id_prefix:bool> <dram_bp> <dram_rc>
two_level_flags() {
   local skip_prefix="$1"
   local dram_bp="$2"
   local dram_rc="$3"

   echo --test_admission_mode=two_level \
        --cxl_tiering_enabled=true --cxl_gib="$CXL_GIB" \
        --cxl_dax_device_path="$CXL_DAX_DEVICE" \
        --pp_threads="$PP_THREADS" --cxl_pp_threads="$CXL_PP_THREADS" \
        --two_level_admission_threads="$TWO_LEVEL_ADMISSION_THREADS" \
        --delay_admission_recordcache_threads_start=true \
        --dram_buffer_pool_gib="$dram_bp" \
        --dram_recordcache_gib="$dram_rc" \
        --forward_epoch_thread="$FORWARD_EPOCH_THREAD" \
        --sieve_eviction_thread="$SIEVE_EVICTION_THREAD" \
        --record_cache_promote_thread="$RECORD_CACHE_PROMOTE_THREAD" \
        --rc_skip_dt_id_prefix="$skip_prefix" \
        --rc_seqlock_retry_max="$RC_SEQLOCK_RETRY_MAX"
}

# -----------------------------------------------------------------------------
# run_ycsb <workload> <variant_tag> <skip_prefix> <dram_bp> <dram_rc>
#   variant_tag is appended to the log filename (e.g. "" or "_rebal")
# -----------------------------------------------------------------------------
run_ycsb() {
   local wl="$1"
   local variant_tag="$2"
   local skip_prefix="$3"
   local dram_bp="$4"
   local dram_rc="$5"

   CURRENT_EXPERIMENT=$((CURRENT_EXPERIMENT + 1))

   local binary="$BUILD_DIR/experiment_1_ycsb_${wl}"
   local result_file="$RESULT_DIR/ycsb_${wl}${variant_tag}_two_level.log"

   if [[ ! -x "$binary" ]]; then
      echo "[ERROR] binary not found: $binary"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}${variant_tag}:MISSING_BINARY")
      return 1
   fi

   local warmup="$WARMUP_LOOKUPS"
   case "$wl" in d|e) warmup=10000000 ;; esac

   rm -f "$SSD_PATH"

   echo ""
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] ycsb_${wl}${variant_tag} (two_level, tag=${BASELINE_TAG}, skip_prefix=${skip_prefix}, BP=${dram_bp}+RC=${dram_rc})"

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
      $(two_level_flags "$skip_prefix" "$dram_bp" "$dram_rc") \
      2>&1 | tee "$result_file" || exit_code=$?

   local elapsed=$(( $(date +%s) - start_ts ))

   if [[ "$exit_code" -ne 0 ]]; then
      echo "[FAIL] ycsb_${wl}${variant_tag} exit=$exit_code (${elapsed}s)"
      FAIL_COUNT=$((FAIL_COUNT + 1))
      FAILED_TESTS+=("ycsb_${wl}${variant_tag}:exit${exit_code}")
   else
      echo "[PASS] ycsb_${wl}${variant_tag} (${elapsed}s)"
      PASS_COUNT=$((PASS_COUNT + 1))
   fi

   sleep "$COOLDOWN_SECONDS"
}

# -----------------------------------------------------------------------------
# run_tpcc — always uses skip_prefix=false (multi-table)
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
   log_info "[$CURRENT_EXPERIMENT/$TOTAL_EXPERIMENTS] tpcc (two_level, tag=${BASELINE_TAG}, skip_prefix=false [multi-table])"

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
      $(two_level_flags false "$DRAM_BP_STD" "$DRAM_RC_STD") \
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
# extract_summary — parse logs into CSVs. Two YCSB tables (std + rebal) + tpcc.
# -----------------------------------------------------------------------------
parse_ycsb_log() {
   local log="$1"
   local row_tag="$2"
   local wl="$3"
   if [[ ! -f "$log" ]]; then
      echo "${row_tag},${wl},MISSING,,,,,,,,"
      return
   fi
   local line
   line=$(grep -E "^.*mode=" "$log" | tail -1 || true)
   if [[ -z "$line" ]]; then
      echo "${row_tag},${wl},NO_SUMMARY_LINE,,,,,,,,"
      return
   fi
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
   echo "${row_tag},${wl},${mqps},${avg},${p95},${p99},${rc},${bp},${cxl},${ssd},${ab}"
}

extract_summary() {
   local ycsb_csv="$RESULT_DIR/summary_ycsb.csv"
   local ycsb_rebal_csv="$RESULT_DIR/summary_ycsb_rebal.csv"
   local tpcc_csv="$RESULT_DIR/summary_tpcc.csv"

   {
      echo "tag,workload,mqps,avg_us,p95_us,p99_us,rc_hr_pct,bp_hr_pct,cxl_hr_pct,ssd_miss_pct,aborts"
      for wl in "${YCSB_WORKLOADS[@]}"; do
         parse_ycsb_log "$RESULT_DIR/ycsb_${wl}_two_level.log" "${BASELINE_TAG}" "${wl}"
      done
   } > "$ycsb_csv"

   {
      echo "tag,workload,mqps,avg_us,p95_us,p99_us,rc_hr_pct,bp_hr_pct,cxl_hr_pct,ssd_miss_pct,aborts"
      for wl in "${REBAL_WORKLOADS[@]}"; do
         parse_ycsb_log "$RESULT_DIR/ycsb_${wl}_rebal_two_level.log" "${BASELINE_TAG}_rebal" "${wl}"
      done
   } > "$ycsb_rebal_csv"

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
   echo "  $ycsb_rebal_csv"
   echo "  $tpcc_csv"
   echo ""
   echo "--- YCSB summary (standard DRAM split) ---"
   column -s, -t < "$ycsb_csv"
   echo ""
   echo "--- YCSB summary (rebalanced DRAM split: BP=${DRAM_BP_REBAL} + RC=${DRAM_RC_REBAL}) ---"
   column -s, -t < "$ycsb_rebal_csv"
   echo ""
   echo "--- TPC-C summary ---"
   column -s, -t < "$tpcc_csv"
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
log_phase "Two-level optimization baseline v4 (tag=${BASELINE_TAG})"
log_info "result_dir              = $RESULT_DIR"
log_info "baseline_tag            = ${BASELINE_TAG}"
log_info "working_set             = ${WORKING_SET_GIB} GiB (payload=${PAYLOAD_SIZE_BYTES}B)"
log_info "cxl_gib                 = ${CXL_GIB}"
log_info "dram std                = BP ${DRAM_BP_STD} + RC ${DRAM_RC_STD} GiB"
log_info "dram rebal (D/E only)   = BP ${DRAM_BP_REBAL} + RC ${DRAM_RC_REBAL} GiB"
log_info "zipf_theta              = ${ZIPF_THETA}"
log_info "worker_threads          = ${WORKER_THREADS}"
log_info "rc_seqlock_retry_max    = ${RC_SEQLOCK_RETRY_MAX}"
log_info "rc_skip_dt_id_prefix    = true (YCSB)  /  false (TPC-C)"
log_info "ycsb: warmup=${WARMUP_LOOKUPS} measure=${MEASURE_LOOKUPS} (D/E warmup=10M)"
log_info "tpcc: wh=${TPCC_WAREHOUSE_COUNT} warmup=${TPCC_WARMUP_SECONDS}s measure=${TPCC_MEASURE_SECONDS}s"
log_info "total_experiments       = ${TOTAL_EXPERIMENTS}"

OVERALL_START=$(date +%s)

# Pass A: standard DRAM split, all 6 YCSB + TPC-C
for wl in "${YCSB_WORKLOADS[@]}"; do
   log_phase "YCSB-${wl^^} (standard DRAM)"
   run_ycsb "$wl" "" true "$DRAM_BP_STD" "$DRAM_RC_STD"
done

log_phase "TPC-C"
run_tpcc

# Pass B: rebalanced DRAM (BP up, RC down) for D and E only
for wl in "${REBAL_WORKLOADS[@]}"; do
   log_phase "YCSB-${wl^^} (rebalanced DRAM: BP=${DRAM_BP_REBAL} + RC=${DRAM_RC_REBAL})"
   run_ycsb "$wl" "_rebal" true "$DRAM_BP_REBAL" "$DRAM_RC_REBAL"
done

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
echo "To compare against v2 baseline:"
echo "  diff <(tail -n +2 bench_two_level_v2_*/summary_ycsb.csv) <(tail -n +2 $RESULT_DIR/summary_ycsb.csv)"
